/**
 * @file src/platform/windows/frametop_window_apps.cpp
 * @brief The apps a Frametop window panel can launch (see frametop_window_apps.h).
 */
#include "frametop_window_apps.h"

#include "src/config.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/platform/windows/frametop_window_query.h"
#include "src/platform/windows/misc.h"
#include "src/platform/windows/utf_utils.h"

#include <boost/filesystem/path.hpp>

#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <userenv.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>

using namespace std::literals;
namespace fs = std::filesystem;

namespace platf::frametop_windows::catalog {
  using clock = std::chrono::steady_clock;
  using win32::lower;

  namespace {
    // Where Vibepollo keeps its state: the Store app listing is written next to it.
    fs::path files_folder() {
      const fs::path state = config::nvhttp.file_state;
      return state.has_parent_path() ? state.parent_path() : fs::path {"."};
    }
  }  // namespace

  namespace {
  std::vector<app_t> load_apps() {
    std::vector<std::pair<fs::path, std::string>> roots;  // folder, id prefix
    PWSTR path = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_CommonPrograms, 0, nullptr, &path))) roots.emplace_back(path, "");
    CoTaskMemFree(path);
    path = nullptr;
    const HANDLE user_token = retrieve_users_token(false);
    const std::unique_ptr<void, decltype(&CloseHandle)> close_token {user_token, &CloseHandle};
    if (user_token) {
      if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Programs, 0, user_token, &path))) roots.emplace_back(path, "");
      CoTaskMemFree(path);
    }
    IShellLinkW *link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, reinterpret_cast<void **>(&link)))) return {};
    IPersistFile *file = nullptr;
    if (FAILED(link->QueryInterface(IID_IPersistFile, reinterpret_cast<void **>(&file)))) {
      link->Release();
      return {};
    }
    std::map<std::string, app_t> by_id;
    for (const auto &[root, prefix] : roots) {
      std::error_code ec;
      for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (it.depth() == 0 && it->is_directory(ec) && lower(it->path().filename().wstring()) == L"startup") {
          it.disable_recursion_pending();  // what runs at sign-in, not apps to open
          continue;
        }
        if (!it->is_regular_file(ec) || lower(it->path().extension().wstring()) != L".lnk") continue;
        const auto name = utf_utils::to_utf8(it->path().stem().wstring());
        if (lower(name).find("uninstall") != std::string::npos) continue;
        wchar_t target_path[MAX_PATH * 2] {};
        std::wstring exe;
        if (SUCCEEDED(file->Load(it->path().c_str(), STGM_READ)) &&
            SUCCEEDED(link->GetPath(target_path, int(std::size(target_path)), nullptr, SLGP_RAWPATH))) {
          wchar_t expanded[MAX_PATH * 2] {};
          // The user's %LOCALAPPDATA%, not the service's.
          if (!user_token || !ExpandEnvironmentStringsForUserW(user_token, target_path, expanded, DWORD(std::size(expanded)))) {
            ExpandEnvironmentStringsW(target_path, expanded, DWORD(std::size(expanded)));
          }
          exe = lower(std::wstring {expanded});
        }
        if (!exe.empty() && !exe.ends_with(L".exe")) continue;  // documents, web links
        auto id = utf_utils::to_utf8(fs::relative(it->path(), root, ec).generic_wstring());
        if (ec || id.empty()) id = name + ".lnk";
        by_id.try_emplace(id, app_t {id, name, it->path().wstring(), exe});  // the machine's first
      }
    }
    // One entry per app: the same shortcut often sits in a vendor folder and at the top.
    std::set<std::pair<std::string, std::wstring>> seen;
    for (auto it = by_id.begin(); it != by_id.end();) {
      const bool dup = !it->second.exe.empty() && !seen.emplace(lower(it->second.name), it->second.exe).second;
      it = dup ? by_id.erase(it) : std::next(it);
    }
    file->Release();
    link->Release();
    std::vector<app_t> out;
    for (auto &[id, app] : by_id) out.push_back(std::move(app));
    std::sort(out.begin(), out.end(), [](const auto &a, const auto &b) { return lower(a.name) < lower(b.name); });
    return out;
  }

  // Start menu shortcuts, read again each minute on a thread of their own: resolving every
  // shortcut takes a while, and neither the window thread nor `lock` should wait on it.
  struct shortcuts_t {
    std::mutex lock;
    std::condition_variable loaded;
    std::vector<app_t> apps;  // under lock
    std::optional<clock::time_point> at;  // under lock: when they were read
    bool loading = false;  // under lock
  };

  // Never destroyed: a read still running at exit outlives the statics.
  shortcuts_t &shortcuts() {
    static auto &it = *new shortcuts_t;
    return it;
  }

  }  // namespace

  void refresh_shortcuts() {
    {
      std::lock_guard guard {shortcuts().lock};
      if (shortcuts().loading || (shortcuts().at && clock::now() - *shortcuts().at < 60s)) return;
      shortcuts().loading = true;
    }
    std::thread([] {
      std::vector<app_t> loaded;
      const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
      try {
        loaded = load_apps();
      } catch (const std::exception &e) {
        BOOST_LOG(warning) << "Frametop windows: reading the Start menu failed: " << e.what();
      }
      if (com) CoUninitialize();
      {
        std::lock_guard guard {shortcuts().lock};
        // Nothing read (no one signed in yet) keeps the last list and tries again soon.
        const bool failed = loaded.empty();
        if (!failed || !shortcuts().at) shortcuts().apps = std::move(loaded);
        shortcuts().at = failed ? clock::now() - 50s : clock::now();
        shortcuts().loading = false;
      }
      shortcuts().loaded.notify_all();
    }).detach();
  }

  // The shortcuts read last (refreshing them when they're old), waiting up to 8 s for the
  // first read when `wait`. Never under `lock` with `wait`.
  std::vector<app_t> shortcut_apps(bool wait) {
    refresh_shortcuts();
    std::unique_lock guard {shortcuts().lock};
    if (wait) shortcuts().loaded.wait_for(guard, 8s, [] { return shortcuts().at.has_value(); });
    return shortcuts().apps;
  }

  namespace {
  // Store apps (packaged ones): no Start menu shortcut names them, and only the signed-in
  // user's shell lists them, so PowerShell's Get-StartApps runs as the user now and then, off
  // the window thread. Their ids are AUMIDs (Family_publisher!App); Explorer starts them.
  struct store_t {
    std::mutex lock;
    std::condition_variable loaded;
    std::vector<app_t> apps;  // under lock
    std::optional<clock::time_point> at;  // under lock: when they were listed
    bool loading = false;  // under lock
  };

  // Never destroyed: a listing still running at exit outlives the statics.
  store_t &store() {
    static auto &it = *new store_t;
    return it;
  }

  }  // namespace

  bool is_aumid(const std::string &id) {
    const auto bang = id.find('!');
    if (bang == std::string::npos || bang == 0 || bang + 1 == id.size() || id.size() > 300) return false;
    const auto family = id.substr(0, bang);
    const auto underscore = family.rfind('_');
    if (underscore == std::string::npos || family.size() - underscore - 1 != 13) return false;  // the publisher id
    return std::all_of(id.begin(), id.end(), [](unsigned char c) {
      return std::isalnum(c) || c == '.' || c == '_' || c == '-' || c == '!';
    });
  }

  namespace {
  std::vector<app_t> load_store_apps() {
    const auto out_path = files_folder() / "frametop_store_apps.json";
    FILE *file = _wfopen(out_path.c_str(), L"wb");
    if (!file) return {};
    boost::filesystem::path working_dir;
    std::error_code ec;
    auto child = run_command(false, false,
                             // ASCII only (\\u escapes), whatever the console's code page.
                             "powershell.exe -NoProfile -NonInteractive -Command \"$j = Get-StartApps | Select-Object Name, AppID | ConvertTo-Json -Compress; "
                             "[regex]::Replace($j, '[^\\x00-\\x7F]', { '\\u{0:x4}' -f [int][char]$args[0].Value })\"",
                             working_dir, bp::this_process::env(), file, ec, nullptr);
    std::fclose(file);
    if (ec) {
      BOOST_LOG(warning) << "Frametop windows: couldn't list Store apps: " << ec.message();
      return {};
    }
    const auto deadline = clock::now() + 30s;
    while (child.running() && clock::now() < deadline) std::this_thread::sleep_for(100ms);
    if (child.running()) {
      if (const HANDLE process = OpenProcess(PROCESS_TERMINATE, FALSE, DWORD(child.id()))) {
        TerminateProcess(process, 1);
        CloseHandle(process);
      }
      std::error_code wait_ec;
      child.wait(wait_ec);
      BOOST_LOG(warning) << "Frametop windows: listing Store apps took too long";
      return {};
    }
    std::ifstream in {out_path, std::ios::binary};
    std::string text {std::istreambuf_iterator<char> {in}, {}};
    auto listed = nlohmann::json::parse(text, nullptr, false);
    if (listed.is_object()) listed = nlohmann::json::array({listed});  // one app: no array
    std::vector<app_t> out;
    if (!listed.is_array()) return out;
    for (const auto &a : listed) {
      if (!a.is_object() || !a.contains("AppID") || !a["AppID"].is_string() || !a.contains("Name") || !a["Name"].is_string()) continue;
      const auto id = a["AppID"].get<std::string>();
      const auto name = a["Name"].get<std::string>();
      if (!is_aumid(id) || name.empty()) continue;
      out.push_back(app_t {id, name, {}, {}});
    }
    return out;
  }

  }  // namespace

  bool has_store_apps() {
    std::lock_guard guard {store().lock};
    return !store().apps.empty();
  }

  // Store apps listed (or tried) once, waiting up to 8 s for the first list.
  void await_store_apps() {
    refresh_store_apps();
    std::unique_lock guard {store().lock};
    store().loaded.wait_for(guard, 8s, [] { return store().at.has_value(); });
  }

  bool store_app(const std::string &id) {
    std::lock_guard guard {store().lock};
    return std::any_of(store().apps.begin(), store().apps.end(), [&](const auto &app) { return lower(app.id) == lower(id); });
  }

  // Lists Store apps again in the background, when they're older than ten minutes.
  void refresh_store_apps() {
    {
      std::lock_guard guard {store().lock};
      if (store().loading || (store().at && clock::now() - *store().at < 10min)) return;
      store().loading = true;
    }
    std::thread([] {
      std::vector<app_t> loaded;
      try {
        loaded = load_store_apps();
      } catch (const std::exception &e) {
        BOOST_LOG(warning) << "Frametop windows: listing Store apps failed: " << e.what();
      }
      {
        std::lock_guard guard {store().lock};
        // A failure (no one signed in yet) keeps the last list and tries again in a minute.
        const bool failed = loaded.empty();
        if (!failed || !store().at) store().apps = std::move(loaded);
        store().at = failed ? clock::now() - 9min : clock::now();
        store().loading = false;
      }
      store().loaded.notify_all();
    }).detach();
  }

  // Shortcuts, then Store apps no shortcut has the name of. Doesn't wait: callers that want
  // the first lists await them first.
  std::vector<app_t> all_apps() {
    std::vector<app_t> out = shortcut_apps(false);
    std::set<std::string> names;
    for (const auto &app : out) names.insert(lower(app.name));
    std::vector<app_t> listed;
    {
      std::lock_guard guard {store().lock};
      listed = store().apps;
    }
    for (auto &app : listed) {
      if (names.insert(lower(app.name)).second) out.push_back(std::move(app));
    }
    std::sort(out.begin(), out.end(), [](const auto &a, const auto &b) { return lower(a.name) < lower(b.name); });
    return out;
  }

  std::optional<std::string> app_of(const std::vector<app_t> &apps, const std::wstring &exe) {
    if (exe.empty()) return std::nullopt;
    for (const auto &app : apps) {
      if (app.exe == exe) return app.id;
    }
    return std::nullopt;
  }

}  // namespace platf::frametop_windows::catalog
