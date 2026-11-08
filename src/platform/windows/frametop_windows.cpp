/**
 * @file src/platform/windows/frametop_windows.cpp
 * @brief Frametop windows on a Windows desktop (see frametop_windows.h).
 */
#include "frametop_windows.h"

#include "src/config.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/platform/windows/frametop_window_target.h"
#include "src/platform/windows/misc.h"
#include "src/platform/windows/utf_utils.h"
#include "src/rtsp.h"
#include "src/video.h"

#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem/path.hpp>

#include <windows.h>
#include <dwmapi.h>
#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

using namespace std::literals;
namespace fs = std::filesystem;

namespace platf::frametop_windows {
  namespace {
    namespace target = frametop_window_target;
    using clock = std::chrono::steady_clock;

    constexpr auto kGrace = 60s;  // no stream of a window output this long: the window goes back
    constexpr auto kWatch = 2s;
    constexpr auto kLaunchWait = 20s;
    constexpr int kMinWidth = 320, kMinHeight = 240;

    struct hosted_t {
      HWND hwnd = nullptr;
      WINDOWPLACEMENT before {sizeof(WINDOWPLACEMENT)};  // where it was, restored on release
      clock::time_point seen;  // last time a stream captured it (or when it opened)
    };

    struct app_t {
      std::string id, name;
      std::wstring link;  // the .lnk
      std::wstring exe;  // its target, lower case, if it's an .exe
    };

    // Guards hosted_windows and the app cache. Every desktop call runs on the window thread.
    std::mutex lock;
    std::map<std::string, hosted_t> hosted_windows;
    std::vector<app_t> app_cache;
    clock::time_point app_cache_at;
    std::atomic<bool> started {false}, stopping {false};

    // One thread of ours on the user's Default desktop, where app windows are: HTTP pool threads
    // may have windows of their own (COM), and those can't change desktops.
    class window_thread_t {
    public:
      template<class F>
      auto run(F f) -> decltype(f()) {
        using result_t = decltype(f());
        auto task = std::make_shared<std::packaged_task<result_t()>>(std::move(f));
        auto future = task->get_future();
        {
          std::lock_guard guard {queue_lock};
          ensure_started();
          queue.emplace_back([task] { (*task)(); });
        }
        wake.notify_one();
        return future.get();
      }

      void every(std::chrono::milliseconds interval, std::function<void()> f) {
        std::lock_guard guard {queue_lock};
        ensure_started();
        tick_interval = interval;
        tick = std::move(f);
      }

    private:
      void ensure_started() {
        if (thread.joinable()) return;
        thread = std::thread([this] { loop(); });
        thread.detach();
      }

      // Vibepollo may start before anyone signs in: until the Default desktop is there, each job
      // tries again.
      void bind_desktop() {
        if (bound) return;
        if (const auto desktop = OpenDesktopW(L"Default", 0, FALSE, GENERIC_ALL)) {
          if (SetThreadDesktop(desktop)) {
            bound = true;
            return;
          }
          CloseDesktop(desktop);
        }
        if (!warned) BOOST_LOG(warning) << "Frametop windows: no Default desktop yet [" << GetLastError() << ']';
        warned = true;
      }

      void loop() {
        // Physical pixels whatever the monitor's scale, as capture and input use.
        using set_context_t = void *(WINAPI *) (void *);
        if (const auto set_context = reinterpret_cast<set_context_t>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetThreadDpiAwarenessContext"))) {
          set_context(reinterpret_cast<void *>(static_cast<intptr_t>(-4)));  // per monitor v2
        }
        // Multithreaded COM makes no window, so the thread can still change desktops.
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        auto next_tick = clock::now() + kWatch;
        for (;;) {
          std::function<void()> job;
          {
            std::unique_lock guard {queue_lock};
            wake.wait_until(guard, next_tick, [this] { return !queue.empty(); });
            if (!queue.empty()) {
              job = std::move(queue.front());
              queue.pop_front();
            } else if (clock::now() >= next_tick) {
              next_tick = clock::now() + tick_interval;
              job = tick;
            }
          }
          if (!job) continue;
          bind_desktop();
          try {
            job();
          } catch (const std::exception &e) {
            BOOST_LOG(warning) << "Frametop windows: " << e.what();
          }
        }
      }

      std::mutex queue_lock;
      std::condition_variable wake;
      std::deque<std::function<void()>> queue;
      std::function<void()> tick;
      std::chrono::milliseconds tick_interval {kWatch};
      std::thread thread;
      bool bound = false, warned = false;  // window thread only
    };

    window_thread_t &window_thread() {
      static auto *thread = new window_thread_t;  // never destroyed: at-exit release may still use it
      return *thread;
    }

    std::string lower(std::string s) {
      std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
      return s;
    }

    std::wstring lower(std::wstring s) {
      CharLowerBuffW(s.data(), DWORD(s.size()));
      return s;
    }

    std::wstring class_of(HWND hwnd) {
      wchar_t name[256] {};
      return GetClassNameW(hwnd, name, int(std::size(name))) > 0 ? name : L"";
    }

    std::wstring title_of(HWND hwnd) {
      std::wstring title(size_t(GetWindowTextLengthW(hwnd)) + 1, L'\0');
      title.resize(size_t(std::max(0, GetWindowTextW(hwnd, title.data(), int(title.size())))));
      return title;
    }

    DWORD pid_of(HWND hwnd) {
      DWORD pid = 0;
      GetWindowThreadProcessId(hwnd, &pid);
      return pid;
    }

    std::wstring exe_of(DWORD pid) {
      std::wstring out;
      if (const auto process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        wchar_t path[MAX_PATH * 2] {};
        DWORD size = DWORD(std::size(path));
        if (QueryFullProcessImageNameW(process, 0, path, &size)) out.assign(path, size);
        CloseHandle(process);
      }
      return lower(out);
    }

    bool cloaked(HWND hwnd) {
      DWORD value = 0;
      return SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &value, sizeof(value))) && value != 0;
    }

    // An app window, as Alt+Tab lists them: shown (or minimized), not a tool window, not owned
    // by another (unless it asks to be on the taskbar), not cloaked, and titled.
    bool app_window(HWND hwnd) {
      if (!hwnd || !IsWindow(hwnd) || !IsWindowVisible(hwnd) || cloaked(hwnd)) return false;
      const auto ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
      const bool on_taskbar = (ex & WS_EX_APPWINDOW) != 0;
      if ((ex & WS_EX_TOOLWINDOW) && !on_taskbar) return false;
      if (GetWindow(hwnd, GW_OWNER) && !on_taskbar) return false;
      if ((ex & WS_EX_NOACTIVATE) && !on_taskbar) return false;
      if (GetWindowTextLengthW(hwnd) == 0) return false;
      static const std::set<std::wstring> shell {L"Progman", L"WorkerW", L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd", L"Windows.UI.Core.CoreWindow"};
      return !shell.contains(class_of(hwnd));
    }

    std::vector<HWND> app_windows() {
      std::vector<HWND> out;
      EnumWindows(
        [](HWND hwnd, LPARAM param) -> BOOL {
          if (app_window(hwnd)) reinterpret_cast<std::vector<HWND> *>(param)->push_back(hwnd);
          return TRUE;
        },
        reinterpret_cast<LPARAM>(&out)
      );
      return out;  // front to back
    }

    std::string monitor_name(HMONITOR monitor) {
      MONITORINFOEXW info {};
      info.cbSize = sizeof(info);
      return monitor && GetMonitorInfoW(monitor, &info) ? utf_utils::to_utf8(info.szDevice) : std::string {};
    }

    // A monitor named by its device id (as /api/display-devices lists it) or \\.\DISPLAYn.
    std::optional<MONITORINFOEXW> find_monitor(const std::string &name) {
      if (name.empty()) return std::nullopt;
      std::string display = name;
      if (!name.starts_with("\\\\.\\")) {
        const auto by_device = video::display_name_for_device(name);
        if (!by_device) return std::nullopt;
        display = *by_device;
      }
      struct search_t {
        std::wstring want;
        std::optional<MONITORINFOEXW> found;
      } search {utf_utils::from_utf8(display), std::nullopt};
      EnumDisplayMonitors(
        nullptr,
        nullptr,
        [](HMONITOR monitor, HDC, RECT *, LPARAM param) -> BOOL {
          auto *s = reinterpret_cast<search_t *>(param);
          MONITORINFOEXW info {};
          info.cbSize = sizeof(info);
          if (GetMonitorInfoW(monitor, &info) && _wcsicmp(info.szDevice, s->want.c_str()) == 0) {
            s->found = info;
            return FALSE;
          }
          return TRUE;
        },
        reinterpret_cast<LPARAM>(&search)
      );
      return search.found;
    }

    // Locked (or on the UAC prompt), the input desktop isn't Default: windows show nothing.
    bool session_locked() {
      const auto desktop = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
      if (!desktop) return true;
      wchar_t name[64] {};
      const bool ok = GetUserObjectInformationW(desktop, UOI_NAME, name, sizeof(name), nullptr);
      CloseDesktop(desktop);
      return !ok || _wcsicmp(name, L"Default") != 0;
    }

    fs::path state_path() {
      const fs::path state = config::nvhttp.file_state;
      return (state.has_parent_path() ? state.parent_path() : fs::path {"."}) / "frametop_windows.json";
    }

    void save() {
      nlohmann::json out = nlohmann::json::object();
      for (const auto &[output, h] : hosted_windows) {
        const auto &p = h.before;
        out[output] = {{"address", target::address(h.hwnd)},
                       {"show", p.showCmd},
                       {"flags", p.flags},
                       {"normal", {p.rcNormalPosition.left, p.rcNormalPosition.top, p.rcNormalPosition.right, p.rcNormalPosition.bottom}},
                       {"min", {p.ptMinPosition.x, p.ptMinPosition.y}},
                       {"max", {p.ptMaxPosition.x, p.ptMaxPosition.y}}};
      }
      const auto path = state_path();
      const auto tmp = path.string() + ".tmp";
      {
        std::ofstream file {tmp, std::ios::trunc};
        file << out.dump(2);
        if (!file) return;
      }
      std::error_code ec;
      fs::rename(tmp, path, ec);
    }

    // Sizes a window so its capture (the frame Windows draws, without the shadow) is width x
    // height, inside its monitor's work area so input reaches all of it.
    void size_window(HWND hwnd, int width, int height) {
      RECT outer {};
      const auto frame = target::capture_rect(hwnd);
      if (!frame || !GetWindowRect(hwnd, &outer)) return;
      MONITORINFO info {sizeof(MONITORINFO)};
      if (!GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &info)) return;
      const auto &work = info.rcWork;
      if (width <= 0) width = frame->right - frame->left;
      if (height <= 0) height = frame->bottom - frame->top;
      width = std::clamp(width, kMinWidth, std::max<int>(kMinWidth, work.right - work.left)) & ~1;
      height = std::clamp(height, kMinHeight, std::max<int>(kMinHeight, work.bottom - work.top)) & ~1;
      const int x = std::clamp<int>(frame->left, work.left, std::max<int>(work.left, work.right - width));
      const int y = std::clamp<int>(frame->top, work.top, std::max<int>(work.top, work.bottom - height));
      const int left = frame->left - outer.left, top = frame->top - outer.top;
      const int right = outer.right - frame->right, bottom = outer.bottom - frame->bottom;
      SetWindowPos(hwnd, nullptr, x - left, y - top, width + left + right, height + top + bottom, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    }

    // Window thread only, under the lock.
    bool put_back(const std::string &output, const drop_t *drop = nullptr) {
      const auto it = hosted_windows.find(output);
      if (it == hosted_windows.end()) return false;
      const auto h = it->second;
      hosted_windows.erase(it);
      save();
      if (!IsWindow(h.hwnd)) return true;
      const auto onto = drop ? find_monitor(drop->monitor) : std::nullopt;
      if (!onto) {
        SetWindowPlacement(h.hwnd, &h.before);
        BOOST_LOG(info) << "Frametop windows: " << output << " back where it was";
        return true;
      }
      // Dropped on one of this PC's displays: there, held where it was held.
      const auto frame = target::capture_rect(h.hwnd);
      RECT outer {};
      if (!frame || !GetWindowRect(h.hwnd, &outer)) return true;
      const int w = frame->right - frame->left, ht = frame->bottom - frame->top;
      const auto &work = onto->rcWork;
      const auto &m = onto->rcMonitor;
      int x = int(m.left + drop->u * (m.right - m.left) - drop->fx * w);
      int y = int(m.top + drop->v * (m.bottom - m.top) - drop->fy * ht);
      x = std::clamp<int>(x, work.left, std::max<int>(work.left, work.right - w));
      y = std::clamp<int>(y, work.top, std::max<int>(work.top, work.bottom - ht));
      SetWindowPos(h.hwnd, HWND_TOP, x - (frame->left - outer.left), y - (frame->top - outer.top), 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
      BOOST_LOG(info) << "Frametop windows: " << output << " dropped onto " << drop->monitor;
      return true;
    }

    std::vector<app_t> load_apps() {
      std::vector<std::pair<fs::path, std::string>> roots;  // folder, id prefix
      PWSTR path = nullptr;
      if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_CommonPrograms, 0, nullptr, &path))) roots.emplace_back(path, "");
      CoTaskMemFree(path);
      path = nullptr;
      if (const auto token = retrieve_users_token(false)) {
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Programs, 0, token, &path))) roots.emplace_back(path, "");
        CoTaskMemFree(path);
        CloseHandle(token);
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
          if (!it->is_regular_file(ec) || lower(it->path().extension().wstring()) != L".lnk") continue;
          const auto name = utf_utils::to_utf8(it->path().stem().wstring());
          if (lower(name).find("uninstall") != std::string::npos) continue;
          wchar_t target_path[MAX_PATH * 2] {};
          std::wstring exe;
          if (SUCCEEDED(file->Load(it->path().c_str(), STGM_READ)) &&
              SUCCEEDED(link->GetPath(target_path, int(std::size(target_path)), nullptr, SLGP_RAWPATH))) {
            wchar_t expanded[MAX_PATH * 2] {};
            ExpandEnvironmentStringsW(target_path, expanded, DWORD(std::size(expanded)));
            exe = lower(std::wstring {expanded});
          }
          if (!exe.empty() && !exe.ends_with(L".exe")) continue;  // documents, web links
          auto id = utf_utils::to_utf8(fs::relative(it->path(), root, ec).generic_wstring());
          if (ec || id.empty()) id = name + ".lnk";
          by_id.try_emplace(id, app_t {id, name, it->path().wstring(), exe});  // the machine's first
        }
      }
      file->Release();
      link->Release();
      std::vector<app_t> out;
      for (auto &[id, app] : by_id) out.push_back(std::move(app));
      std::sort(out.begin(), out.end(), [](const auto &a, const auto &b) { return lower(a.name) < lower(b.name); });
      return out;
    }

    // Under the lock, window thread.
    const std::vector<app_t> &cached_apps() {
      if (app_cache.empty() || clock::now() - app_cache_at > 60s) {
        app_cache = load_apps();
        app_cache_at = clock::now();
      }
      return app_cache;
    }

    std::optional<std::string> app_of(const std::wstring &exe) {
      if (exe.empty()) return std::nullopt;
      for (const auto &app : cached_apps()) {
        if (app.exe == exe) return app.id;
      }
      return std::nullopt;
    }

    std::optional<nlohmann::json> open_locked(HWND hwnd, int width, int height, std::string *error) {
      if (!app_window(hwnd)) {
        *error = "no app window " + target::address(hwnd);
        return std::nullopt;
      }
      const auto output = target::output_name(hwnd);
      if (!hosted_windows.contains(output)) {
        hosted_t h;
        h.hwnd = hwnd;
        if (!GetWindowPlacement(hwnd, &h.before)) {
          *error = "Windows didn't say where " + target::address(hwnd) + " is";
          return std::nullopt;
        }
        h.seen = clock::now();
        hosted_windows[output] = h;
      } else {
        hosted_windows[output].seen = clock::now();
      }
      if (IsIconic(hwnd) || IsZoomed(hwnd)) ShowWindow(hwnd, SW_RESTORE);
      size_window(hwnd, width, height);
      save();
      const auto rect = target::capture_rect(hwnd);
      if (!rect) {
        put_back(output);
        *error = "the window closed";
        return std::nullopt;
      }
      const int w = int(rect->right - rect->left) & ~1, h = int(rect->bottom - rect->top) & ~1;
      BOOST_LOG(info) << "Frametop windows: " << target::address(hwnd) << " (" << utf_utils::to_utf8(title_of(hwnd)) << ") is " << output << ", " << w << 'x' << h;
      return nlohmann::json {{"output", output}, {"width", w}, {"height", h}, {"address", target::address(hwnd)}};
    }

    void end_streams(const std::vector<std::string> &outputs) {
      if (outputs.empty()) return;
      std::thread([outputs] {
        for (const auto &output : outputs) {
          for (const auto &client : rtsp_stream::client_uuids_capturing(output)) {
            BOOST_LOG(info) << "Frametop windows: ending the stream of " << output;
            rtsp_stream::disconnect_client_sessions_with_result(client, true);
          }
        }
      }).detach();
    }

    void watch_once() {
      std::vector<std::string> ended;
      {
        std::lock_guard guard {lock};
        if (stopping || hosted_windows.empty()) return;
        const auto now = clock::now();
        std::vector<std::string> done;
        for (auto &[output, h] : hosted_windows) {
          if (!IsWindow(h.hwnd)) {
            BOOST_LOG(info) << "Frametop windows: the window on " << output << " closed";
            done.push_back(output);
            ended.push_back(output);
          } else if (video::output_is_captured(output)) {
            h.seen = now;
          } else if (now - h.seen > kGrace) {
            BOOST_LOG(info) << "Frametop windows: no stream of " << output << " for a minute";
            done.push_back(output);
          }
        }
        for (const auto &output : done) put_back(output);
      }
      end_streams(ended);
    }

    void release_at_exit() {
      stopping = true;
      auto done = std::async(std::launch::async, [] {
        return window_thread().run([] {
          std::lock_guard guard {lock};
          std::vector<std::string> outputs;
          for (const auto &[output, h] : hosted_windows) outputs.push_back(output);
          for (const auto &output : outputs) put_back(output);
          return int(outputs.size());
        });
      });
      done.wait_for(3s);  // windows left out go back at the next start
    }
  }  // namespace

  bool available() {
    return window_thread().run([] { return !app_windows().empty() || session_locked(); });
  }

  nlohmann::json list() {
    return window_thread().run([] {
      std::lock_guard guard {lock};
      std::map<HWND, std::string> output_of;
      for (const auto &[output, h] : hosted_windows) output_of[h.hwnd] = output;
      const auto windows = app_windows();
      const auto foreground = GetForegroundWindow();
      nlohmann::json out = nlohmann::json::array();
      int order = 1;
      for (const auto hwnd : windows) {
        const auto rect = target::capture_rect(hwnd).value_or(RECT {});
        const auto pid = pid_of(hwnd);
        const auto exe = exe_of(pid);
        nlohmann::json w {{"address", target::address(hwnd)},
                          {"class", utf_utils::to_utf8(class_of(hwnd))},
                          {"title", utf_utils::to_utf8(title_of(hwnd))},
                          {"pid", pid},
                          {"xwayland", false},
                          {"width", rect.right - rect.left},
                          {"height", rect.bottom - rect.top},
                          {"workspace", ""},
                          {"monitor", monitor_name(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST))},
                          {"output", nullptr},
                          // For Frametop desktopless mode: which to bring out first (shown, then
                          // the most recently used, 0 = focused now), where it sits, whose app.
                          {"focus", hwnd == foreground ? 0 : order++},
                          {"floating", !IsZoomed(hwnd)},
                          {"shown", !IsIconic(hwnd)},
                          {"at", {rect.left, rect.top}},
                          {"app", nullptr}};
        if (const auto app = app_of(exe)) w["app"] = *app;
        if (const auto it = output_of.find(hwnd); it != output_of.end()) w["output"] = it->second;
        out.push_back(std::move(w));
      }
      nlohmann::json outputs = nlohmann::json::array();
      for (const auto &[output, h] : hosted_windows) outputs.push_back({{"output", output}, {"address", target::address(h.hwnd)}});
      return nlohmann::json {{"windows", out}, {"outputs", outputs}, {"locked", session_locked()}};
    });
  }

  nlohmann::json apps() {
    return window_thread().run([] {
      std::lock_guard guard {lock};
      nlohmann::json out = nlohmann::json::array();
      for (const auto &app : cached_apps()) out.push_back({{"id", app.id}, {"name", app.name}, {"icon", ""}});
      return out;
    });
  }

  std::optional<nlohmann::json> open(const std::string &address, int width, int height, std::string *error) {
    const auto hwnd = target::parse(std::string {target::prefix} + address);
    if (!hwnd || !address.starts_with("0x")) {
      *error = "address must be a window address (0x...)";
      return std::nullopt;
    }
    return window_thread().run([&] {
      std::lock_guard guard {lock};
      return open_locked(*hwnd, width, height, error);
    });
  }

  std::optional<nlohmann::json> launch(const std::string &app_id, int width, int height, std::string *error) {
    struct started_t {
      std::optional<app_t> app;
      std::set<HWND> before;
      std::string error;
    };
    auto start_app = window_thread().run([&] {
      started_t s;
      {
        std::lock_guard guard {lock};
        for (const auto &app : cached_apps()) {
          if (app.id == app_id) s.app = app;
        }
      }
      if (!s.app) {
        s.error = "no app " + app_id;
        return s;
      }
      for (const auto hwnd : app_windows()) s.before.insert(hwnd);
      // Explorer opens the shortcut as the signed-in user would (its arguments, folder, and
      // how it runs), in the user's session.
      boost::filesystem::path working_dir;
      std::error_code ec;
      const auto env = bp::this_process::env();
      auto child = run_command(false, false, "explorer.exe \"" + utf_utils::to_utf8(s.app->link) + "\"", working_dir, env, nullptr, ec, nullptr);
      if (ec) {
        s.error = "couldn't start " + s.app->name + ": " + ec.message();
        return s;
      }
      child.detach();
      return s;
    });
    if (!start_app.error.empty()) {
      *error = start_app.error;
      return std::nullopt;
    }
    const auto deadline = clock::now() + kLaunchWait;
    while (clock::now() < deadline) {
      std::this_thread::sleep_for(250ms);
      const auto found = window_thread().run([&]() -> std::optional<HWND> {
        for (const auto hwnd : app_windows()) {
          if (start_app.before.contains(hwnd)) continue;
          if (start_app.app->exe.empty() || exe_of(pid_of(hwnd)) == start_app.app->exe) return hwnd;
        }
        return std::nullopt;
      });
      if (!found) continue;
      std::this_thread::sleep_for(300ms);  // let it finish placing itself before it's sized
      auto opened = window_thread().run([&] {
        std::lock_guard guard {lock};
        return open_locked(*found, width, height, error);
      });
      if (opened) BOOST_LOG(info) << "Frametop windows: launched " << app_id << " as " << (*opened)["address"].get<std::string>();
      return opened;
    }
    *error = start_app.app->name + " didn't open a window in time";
    return std::nullopt;
  }

  bool resize(const std::string &output, int width, int height, std::string *error) {
    if (width <= 0 || height <= 0) {
      *error = "a resize needs a width and a height";
      return false;
    }
    return window_thread().run([&] {
      std::lock_guard guard {lock};
      const auto it = hosted_windows.find(output);
      if (it == hosted_windows.end()) {
        *error = "no window output " + output;
        return false;
      }
      size_window(it->second.hwnd, width, height);
      return true;
    });
  }

  bool release(const std::string &output_or_address, std::string *error, const drop_t *drop) {
    return window_thread().run([&] {
      std::lock_guard guard {lock};
      for (const auto &[output, h] : hosted_windows) {
        if (output == output_or_address || boost::iequals(target::address(h.hwnd), output_or_address)) {
          const auto name = output;  // put_back erases the entry
          return put_back(name, drop && !drop->monitor.empty() ? drop : nullptr);
        }
      }
      *error = "no window output for " + output_or_address;
      return false;
    });
  }

  std::optional<nlohmann::json> at(const std::string &monitor, double u, double v, std::string *error) {
    if (!(u >= 0 && u <= 1 && v >= 0 && v <= 1)) {
      *error = "x and y must be 0 to 1";
      return std::nullopt;
    }
    return window_thread().run([&]() -> std::optional<nlohmann::json> {
      const auto m = find_monitor(monitor);
      if (!m) {
        *error = "no monitor " + monitor;
        return std::nullopt;
      }
      const auto &r = m->rcMonitor;
      const POINT point {LONG(r.left + std::min(u, 0.9999) * (r.right - r.left)), LONG(r.top + std::min(v, 0.9999) * (r.bottom - r.top))};
      HWND hwnd = GetAncestor(WindowFromPoint(point), GA_ROOT);
      if (hwnd && !app_window(hwnd)) hwnd = GetAncestor(hwnd, GA_ROOTOWNER);
      if (!hwnd || !app_window(hwnd)) {
        *error = "no window there";
        return std::nullopt;
      }
      const auto rect = target::capture_rect(hwnd).value_or(RECT {point.x, point.y, point.x + 1, point.y + 1});
      const double w = std::max<LONG>(1, rect.right - rect.left), h = std::max<LONG>(1, rect.bottom - rect.top);
      return nlohmann::json {{"address", target::address(hwnd)},
                             {"class", utf_utils::to_utf8(class_of(hwnd))},
                             {"title", utf_utils::to_utf8(title_of(hwnd))},
                             {"width", rect.right - rect.left},
                             {"height", rect.bottom - rect.top},
                             {"grab", {std::clamp((point.x - rect.left) / w, 0.0, 1.0), std::clamp((point.y - rect.top) / h, 0.0, 1.0)}}};
    });
  }

  int release_all() {
    return window_thread().run([] {
      std::lock_guard guard {lock};
      std::vector<std::string> outputs;
      for (const auto &[output, h] : hosted_windows) outputs.push_back(output);
      for (const auto &output : outputs) put_back(output);
      return int(outputs.size());
    });
  }

  void start() {
    if (started.exchange(true)) return;
    // Windows a crash left out: back to where they were, before anything new opens.
    window_thread().run([] {
      std::lock_guard guard {lock};
      std::ifstream file {state_path()};
      const auto saved = file ? nlohmann::json::parse(file, nullptr, false) : nlohmann::json {};
      if (!saved.is_object()) return 0;
      int back = 0;
      for (const auto &[output, s] : saved.items()) {
        const auto hwnd = target::parse(output);
        if (!hwnd || !s.is_object() || !IsWindow(*hwnd)) continue;
        WINDOWPLACEMENT p {sizeof(WINDOWPLACEMENT)};
        p.showCmd = s.value("show", UINT(SW_SHOWNORMAL));
        p.flags = s.value("flags", UINT(0));
        if (const auto n = s.value("normal", nlohmann::json::array()); n.size() == 4) p.rcNormalPosition = {n[0].get<LONG>(), n[1].get<LONG>(), n[2].get<LONG>(), n[3].get<LONG>()};
        if (const auto mn = s.value("min", nlohmann::json::array()); mn.size() == 2) p.ptMinPosition = {mn[0].get<LONG>(), mn[1].get<LONG>()};
        if (const auto mx = s.value("max", nlohmann::json::array()); mx.size() == 2) p.ptMaxPosition = {mx[0].get<LONG>(), mx[1].get<LONG>()};
        if (SetWindowPlacement(*hwnd, &p)) ++back;
      }
      hosted_windows.clear();
      save();
      if (back) BOOST_LOG(info) << "Frametop windows: put back " << back << " left from before";
      return back;
    });
    window_thread().every(std::chrono::duration_cast<std::chrono::milliseconds>(kWatch), watch_once);
    std::atexit(release_at_exit);
  }

  bool hosted(const std::string &output) {
    if (!target::is_window_output(output)) return false;
    std::lock_guard guard {lock};
    return hosted_windows.contains(output);
  }

  void raise_for_input(const std::string &output) {
    const auto hwnd = target::parse(output);
    if (!hwnd) return;
    thread_local HWND last = nullptr;
    thread_local clock::time_point last_at;
    const auto now = clock::now();
    if (GetForegroundWindow() == *hwnd || (last == *hwnd && now - last_at < 250ms)) return;
    last = *hwnd;
    last_at = now;
    if (!IsWindow(*hwnd) || !syncThreadDesktop()) return;
    if (IsIconic(*hwnd)) ShowWindow(*hwnd, SW_RESTORE);
    // A process may only take the foreground from the one that has it while their input is joined.
    const auto foreground = GetForegroundWindow();
    const auto ours = GetCurrentThreadId();
    const auto theirs = foreground ? GetWindowThreadProcessId(foreground, nullptr) : 0;
    const bool joined = theirs && theirs != ours && AttachThreadInput(ours, theirs, TRUE);
    SetWindowPos(*hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SetForegroundWindow(*hwnd);
    if (joined) AttachThreadInput(ours, theirs, FALSE);
  }
}  // namespace platf::frametop_windows
