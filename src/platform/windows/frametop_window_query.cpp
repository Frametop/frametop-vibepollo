/**
 * @file src/platform/windows/frametop_window_query.cpp
 * @brief What Windows says about app windows and monitors (see frametop_window_query.h).
 */
#include "frametop_window_query.h"

#include "src/platform/windows/frametop_vdesktop.h"
#include "src/platform/windows/misc.h"
#include "src/platform/windows/utf_utils.h"
#include "src/video.h"

#include <dwmapi.h>
#include <shobjidl.h>
#include <appmodel.h>
#include <propidl.h>
#include <propsys.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <thread>

using namespace std::literals;

namespace platf::frametop_windows::win32 {
  namespace vdesktop = frametop_vdesktop;

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

  std::string title_of_utf8(HWND hwnd) {
    return utf_utils::to_utf8(title_of(hwnd));
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

  // A window's app id (AUMID), the one Store apps are named by: the window's own (a Store
  // app's frame window says whose it is), else its packaged process's. Empty for others.
  std::string aumid_of(HWND hwnd) {
    // PKEY_AppUserModel_ID, here so no property-key library is needed.
    static constexpr PROPERTYKEY app_id_key {{0x9F4C2855, 0x9F79, 0x4B39, {0xA8, 0xD0, 0xE1, 0xD4, 0x2D, 0xE1, 0xD5, 0xF3}}, 5};
    std::wstring out;
    IPropertyStore *store = nullptr;
    if (SUCCEEDED(SHGetPropertyStoreForWindow(hwnd, IID_PPV_ARGS(&store))) && store) {
      PROPVARIANT value;
      PropVariantInit(&value);
      if (SUCCEEDED(store->GetValue(app_id_key, &value)) && value.vt == VT_LPWSTR && value.pwszVal) out = value.pwszVal;
      PropVariantClear(&value);
      store->Release();
    }
    if (out.empty()) {
      if (const auto process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid_of(hwnd))) {
        wchar_t id[130] {};  // APPLICATION_USER_MODEL_ID_MAX_LENGTH
        UINT32 size = UINT32(std::size(id));
        if (GetApplicationUserModelId(process, &size, id) == ERROR_SUCCESS) out = id;
        CloseHandle(process);
      }
    }
    return utf_utils::to_utf8(out);
  }

  bool cloaked(HWND hwnd) {
    DWORD value = 0;
    return SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &value, sizeof(value))) && value != 0;
  }

  // How a window comes back without taking the focus from what the user is doing now.
  UINT quietly(UINT show) {
    switch (show) {
      case SW_SHOWMINIMIZED:
      case SW_MINIMIZE:
        return SW_SHOWMINNOACTIVE;
      case SW_SHOWMAXIMIZED:
        return SW_SHOWMAXIMIZED;  // Windows has no maximize that keeps the focus elsewhere
      default:
        return SW_SHOWNOACTIVATE;
    }
  }

  // A window Alt+Tab would list on its desktop: not a tool window, not owned by another
  // (unless it asks to be on the taskbar), and titled.
  bool app_like(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd) || !IsWindowVisible(hwnd)) return false;
    const auto ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    const bool on_taskbar = (ex & WS_EX_APPWINDOW) != 0;
    if ((ex & WS_EX_TOOLWINDOW) && !on_taskbar) return false;
    if (GetWindow(hwnd, GW_OWNER) && !on_taskbar) return false;
    if ((ex & WS_EX_NOACTIVATE) && !on_taskbar) return false;
    if (GetWindowTextLengthW(hwnd) == 0) return false;
    static const std::set<std::wstring> shell {L"Progman", L"WorkerW", L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd", L"Windows.UI.Core.CoreWindow"};
    return !shell.contains(class_of(hwnd));
  }

  // An app window on the desktop shown, as Alt+Tab lists them: shown (or minimized), not cloaked.
  bool app_window(HWND hwnd) {
    return app_like(hwnd) && !cloaked(hwnd);
  }

  // The shell's desktop calls as the signed-in user: Vibepollo runs as SYSTEM, the shell as them.
  bool as_user(const std::function<void()> &job) {
    if (!is_running_as_system()) {
      job();
      return true;
    }
    const HANDLE token = retrieve_users_token(false);
    if (!token) return false;
    const bool ran = !impersonate_current_user(token, job);
    CloseHandle(token);
    return ran;
  }

  // An app window on another virtual desktop: opening it brings it here.
  bool away_window(HWND hwnd) {
    return app_like(hwnd) && vdesktop::shell_cloaked(hwnd) && !vdesktop::elsewhere({hwnd}, as_user).empty();
  }

  // The app windows: the desktop shown's, and with every_desktop the other virtual desktops' too,
  // as the Hyprland side lists every workspace's.
  listed_t app_windows(bool every_desktop) {
    struct found_t {
      std::vector<HWND> windows, maybe_away;
      bool every_desktop;
    } found {{}, {}, every_desktop};
    EnumWindows(
      [](HWND hwnd, LPARAM param) -> BOOL {
        auto *f = reinterpret_cast<found_t *>(param);
        if (app_window(hwnd)) {
          f->windows.push_back(hwnd);
        } else if (f->every_desktop && app_like(hwnd) && vdesktop::shell_cloaked(hwnd)) {
          f->windows.push_back(hwnd);
          f->maybe_away.push_back(hwnd);
        }
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&found)
    );
    listed_t out;
    for (const auto hwnd : vdesktop::elsewhere(found.maybe_away, as_user)) out.away.insert(hwnd);
    const std::set<HWND> maybe(found.maybe_away.begin(), found.maybe_away.end());
    for (const auto hwnd : found.windows) {
      if (!maybe.contains(hwnd) || out.away.contains(hwnd)) out.windows.push_back(hwnd);
    }
    return out;
  }

  // After a move between desktops, the shell uncloaks the window a moment later.
  bool await_uncloaked(HWND hwnd) {
    for (int i = 0; i < 40 && cloaked(hwnd); ++i) std::this_thread::sleep_for(25ms);
    return !cloaked(hwnd);
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
}  // namespace platf::frametop_windows::win32
