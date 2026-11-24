/**
 * @file src/platform/windows/frametop_windows.cpp
 * @brief Frametop windows on a Windows desktop (see frametop_windows.h).
 */
#include "frametop_windows.h"

#include "src/config.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/platform/windows/frametop_vdesktop.h"
#include "src/platform/windows/frametop_window_apps.h"
#include "src/platform/windows/frametop_window_query.h"
#include "src/platform/windows/frametop_window_thread.h"
#include "src/platform/windows/frametop_window_target.h"
#include "src/platform/windows/misc.h"
#include "src/platform/windows/utf_utils.h"
#include "src/nvhttp.h"
#include "src/remote_display_topology.h"
#include "src/remote_session.h"
#include "src/rtsp.h"
#include "src/video.h"

#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem/path.hpp>

#include <windows.h>
#include <dwmapi.h>
#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <appmodel.h>
#include <propidl.h>
#include <propsys.h>
#include <userenv.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <thread>
#include <vector>

using namespace std::literals;
namespace fs = std::filesystem;

namespace platf::frametop_windows {
  namespace {
    namespace target = frametop_window_target;
    namespace vdesktop = frametop_vdesktop;
    using namespace win32;
    using namespace catalog;
    using clock = std::chrono::steady_clock;

    constexpr auto kGrace = 60s;  // no stream of a window output this long: the window goes back
    constexpr auto kWatch = 2s;
    constexpr auto kLaunchWait = 20s;
    constexpr int kMinWidth = 320, kMinHeight = 240;

    struct hosted_t {
      HWND hwnd = nullptr;
      DWORD pid = 0;  // its process: an address a closed window freed may name another one later
      WINDOWPLACEMENT before {sizeof(WINDOWPLACEMENT)};  // where it was, restored on release
      clock::time_point seen;  // last time a stream captured it (or when it opened)
      std::optional<GUID> desktop;  // the virtual desktop it came from, if not the one shown: back there on release
    };

    // Guards hosted_windows. Every desktop call runs on the window thread.
    std::mutex lock;
    std::map<std::string, hosted_t> hosted_windows;
    std::atomic<bool> started {false}, stopping {false};

    // The window it hosts is still there (and its address names no other window since).
    bool still(const hosted_t &h) {
      return IsWindow(h.hwnd) && pid_of(h.hwnd) == h.pid;
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
                       {"pid", h.pid},
                       {"class", utf_utils::to_utf8(class_of(h.hwnd))},
                       {"show", p.showCmd},
                       {"flags", p.flags},
                       {"normal", {p.rcNormalPosition.left, p.rcNormalPosition.top, p.rcNormalPosition.right, p.rcNormalPosition.bottom}},
                       {"min", {p.ptMinPosition.x, p.ptMinPosition.y}},
                       {"max", {p.ptMaxPosition.x, p.ptMaxPosition.y}}};
        if (h.desktop) out[output]["desktop"] = vdesktop::to_string(*h.desktop);
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
      constexpr UINT flags = SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER;
      SetWindowPos(hwnd, nullptr, x - left, y - top, width + left + right, height + top + bottom, flags);
      // A window's invisible frame can change with its size (dialogs do): measure what it drew
      // and correct once, so the capture is the size the panel asked for.
      const auto drawn = target::capture_rect(hwnd);
      if (!drawn || !GetWindowRect(hwnd, &outer)) return;
      const int dw = width - (drawn->right - drawn->left), dh = height - (drawn->bottom - drawn->top);
      const int dx = x - drawn->left, dy = y - drawn->top;
      if (dw || dh || dx || dy) {
        SetWindowPos(hwnd, nullptr, outer.left + dx, outer.top + dy, outer.right - outer.left + dw, outer.bottom - outer.top + dh, flags);
      }
    }

    // Window thread only, under the lock.
    bool put_back(const std::string &output, const drop_t *drop = nullptr) {
      const auto it = hosted_windows.find(output);
      if (it == hosted_windows.end()) return false;
      const auto h = it->second;
      hosted_windows.erase(it);
      save();
      if (!still(h)) return true;
      const auto onto = drop ? find_monitor(drop->monitor) : std::nullopt;
      if (!onto) {
        // Asynchronously: a window that stopped answering mustn't hold up the role.
        auto before = h.before;
        before.showCmd = quietly(before.showCmd);
        before.flags |= WPF_ASYNCWINDOWPLACEMENT;
        SetWindowPlacement(h.hwnd, &before);
        // Not at exit: the shell call may wait, and its log may be gone. The window stays here then.
        const bool home = !h.desktop || stopping || vdesktop::send_back(h.hwnd, *h.desktop, as_user);
        if (!stopping) BOOST_LOG(info) << "Frametop windows: " << output << " back where it was" << (home ? "" : ", on this desktop: its own has gone");
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
      SetWindowPos(h.hwnd, HWND_TOP, x - (frame->left - outer.left), y - (frame->top - outer.top), 0, 0, SWP_NOSIZE | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
      BOOST_LOG(info) << "Frametop windows: " << output << " dropped onto " << drop->monitor;
      return true;
    }

    // The windows' own display: a virtual display, held while any window is out, as a Remote
    // Monitor of its own identity, so streams ending (theirs or others') leave it alone. Windows
    // opened go onto it, out of the way of the PC's own monitors and its user; put back, they
    // return to where they were. Without it (no driver, no room), windows stay on their monitors.
    constexpr std::string_view kDisplayId = "frametop-windows";
    constexpr std::string_view kDisplayLabel = "Frametop windows";
    // A window is sized to fit its monitor, so this caps window panels. Windows scales a bigger
    // virtual display (3840x2160 and 5120x1600 get 150%), which would draw windows 1.5 times
    // bigger than on the PC's own monitors; this size gets 100%.
    constexpr std::string_view kDisplayMode = "2560x1600@60";
    constexpr auto kDisplayIdle = 15s;  // no window out this long: the display goes

    struct reserved_t {
      std::mutex lock;  // taken before `lock`, never under it
      std::uint64_t generation = 0;
      bool held = false;
      std::string output;  // \\.\DISPLAYn
      clock::time_point last_use;
    };

    // Never destroyed: a release can still be running at exit.
    reserved_t &reserved() {
      static auto &it = *new reserved_t;
      return it;
    }

    std::atomic<bool> display_held {false};
    std::atomic<bool> display_releasing {false};  // a release or a recovery is running
    clock::time_point last_hosted;  // under `lock`
    std::string display_output;  // under `lock`: reserved().output, for the watcher
    int display_missing = 0;  // under `lock`: watcher ticks in a row it was missing

    // True when the display went now. One whose release waits on another stream's capture
    // draining counts as let go too: the coordinator finishes it, and a new hold (a newer
    // generation) takes it back.
    bool release_display_locked(reserved_t &r, std::string_view reason) {
      if (!r.held) return false;
      const bool released = remote_session::release_monitor(kDisplayId, r.generation, reason);
      BOOST_LOG(info) << "Frametop windows: their display " << r.output << (released ? " is gone (" : " goes once other streams drain (") << reason << ')';
      r.held = false;
      r.output.clear();
      display_held = false;
      {
        std::lock_guard windows {lock};
        display_output.clear();
      }
      return released;
    }

    // Ours still: there, and the coordinator still gives our hold that output (a display that
    // went away can leave its name to a client's new one). Under r.lock.
    bool still_ours(const reserved_t &r) {
      if (!r.held || r.output.empty() || !find_monitor(r.output)) return false;
      const auto state = remote_session::monitor_runtime_snapshot(kDisplayId, r.generation);
      return state.ready && state.output == r.output;
    }

    // The windows' display, made or kept (off the window thread: it can take seconds).
    // `still_counted`: a display of ours whose release is still draining, which the
    // coordinator counts yet.
    std::optional<std::string> hold_display(bool still_counted = false) {
      if (stopping) return std::nullopt;
      auto &r = reserved();
      std::unique_lock guard {r.lock};
      r.last_use = clock::now();
      if (still_ours(r)) return r.output;
      const bool was_held = r.held;
      if (!was_held) {
        // The last Remote Display slot stays a client's.
        const auto capacity = std::size_t(std::max(0, int(config::video.virtual_display_max_clients)));
        auto used = remote_display_topology::instance().managed_client_identity_count();
        if (still_counted && used > 0) --used;
        if (used + 2 > capacity) {
          BOOST_LOG(info) << "Frametop windows: no Remote Display slot to spare for their display; windows stay on their monitors";
          return std::nullopt;
        }
        ++r.generation;  // a new hold; a display that vanished resumes the same one
      }
      remote_session::monitor_runtime_state_t state;
      const auto deadline = clock::now() + 8s;
      while (true) {
        state = remote_session::activate_or_resume_monitor(kDisplayId, kDisplayLabel, kDisplayMode, false, r.generation);
        if (state.accepted) r.held = display_held = true;
        if (state.ready && !state.output.empty() && find_monitor(state.output)) break;
        if (!state.accepted || clock::now() >= deadline) {
          BOOST_LOG(warning) << "Frametop windows: no display of their own (" << (state.error.empty() ? "not ready in time" : state.error)
                             << "); windows stay on their monitors";
          // Only a hold made here goes: windows already out may still be on it.
          const bool released = !was_held && release_display_locked(r, "not ready");
          guard.unlock();
          if (released) nvhttp::cleanup_after_owned_display_release();
          return std::nullopt;
        }
        std::this_thread::sleep_for(500ms);
      }
      if (r.output != state.output) BOOST_LOG(info) << "Frametop windows: their display is " << state.output;
      r.output = state.output;
      r.last_use = clock::now();
      if (stopping) {  // exiting meanwhile: shutdown's teardown may have run already
        const bool released = release_display_locked(r, "exiting");
        guard.unlock();
        if (released) nvhttp::cleanup_after_owned_display_release();
        return std::nullopt;
      }
      {
        std::lock_guard windows {lock};
        display_output = r.output;
      }
      return r.output;
    }

    // From the watcher: the display goes once no window has been out for kDisplayIdle.
    void release_display_if_idle() try {
      bool released = false;
      if (!stopping) {
        auto &r = reserved();
        std::lock_guard guard {r.lock};
        bool none_out;
        {
          std::lock_guard windows {lock};
          none_out = hosted_windows.empty() && clock::now() - last_hosted > kDisplayIdle;
        }
        if (none_out && clock::now() - r.last_use > kDisplayIdle) released = release_display_locked(r, "no windows out");
      }
      // What streams that ended while it was held left for later (display revert, cleanup).
      if (released) nvhttp::cleanup_after_owned_display_release();
      display_releasing = false;
    } catch (const std::exception &e) {
      BOOST_LOG(warning) << "Frametop windows: letting their display go: " << e.what();
      display_releasing = false;
    }

    // Onto the windows' display, cascaded, unless it's there already. Under the lock.
    void place_on(HWND hwnd, const std::string &display) {
      const auto onto = find_monitor(display);
      if (!onto) return;
      MONITORINFOEXW now {};
      now.cbSize = sizeof(now);
      if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &now) && _wcsicmp(now.szDevice, onto->szDevice) == 0) return;
      const int k = int(hosted_windows.size() + 7) % 8;
      const auto &work = onto->rcWork;
      SetWindowPos(hwnd, nullptr, work.left + 40 * k, work.top + 40 * k, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    }

    // From the watcher: the display went while windows were out on it (a host end-stream, which
    // ends every stream and drops every Remote Display, or the driver). Windows put the windows
    // on the PC's own monitors then. With streams of windows still running, those streamed (or
    // opened, or between reconnects, within the grace) go onto a new display; with none, or for
    // the rest, the windows go back.
    void recover_display() try {
      if (!stopping) {
        bool gone = false, released = false;
        {
          auto &r = reserved();
          std::lock_guard guard {r.lock};
          if (r.held && !still_ours(r)) {  // not made again meanwhile
            gone = true;
            BOOST_LOG(info) << "Frametop windows: their display " << r.output << " went away with windows out";
            released = release_display_locked(r, "went away");  // the next hold is a new one
          }
        }
        if (released) nvhttp::cleanup_after_owned_display_release();
        std::vector<std::string> streamed;
        if (gone) {
          window_thread().run([&] {
            std::lock_guard guard {lock};
            const auto now = clock::now();
            const auto on_display = display_output.empty() ? std::nullopt : find_monitor(display_output);
            bool any_captured = false;
            for (const auto &[output, h] : hosted_windows) any_captured = any_captured || video::output_is_captured(output);
            std::vector<std::string> back;
            for (const auto &[output, h] : hosted_windows) {
              MONITORINFOEXW now_on {};
              now_on.cbSize = sizeof(now_on);
              if (on_display && GetMonitorInfoW(MonitorFromWindow(h.hwnd, MONITOR_DEFAULTTONEAREST), &now_on) &&
                  _wcsicmp(now_on.szDevice, on_display->szDevice) == 0) {
                continue;  // an open meanwhile put it on a new display already
              }
              const bool wanted = any_captured && (video::output_is_captured(output) || now - h.seen < kGrace);
              (wanted ? streamed : back).push_back(output);
            }
            for (const auto &output : back) put_back(output);
            return 0;
          });
        }
        if (const auto display = streamed.empty() ? std::nullopt : hold_display(gone && !released)) {
          window_thread().run([&] {
            std::lock_guard guard {lock};
            for (const auto &output : streamed) {
              if (const auto it = hosted_windows.find(output); it != hosted_windows.end()) place_on(it->second.hwnd, *display);
            }
            return 0;
          });
        }
      }
      display_releasing = false;
    } catch (const std::exception &e) {
      BOOST_LOG(warning) << "Frametop windows: after their display went: " << e.what();
      display_releasing = false;
    }

    std::optional<nlohmann::json> open_locked(HWND hwnd, int width, int height, std::string *error, const std::optional<std::string> &display) {
      // "no window ADDRESS" as the Hyprland side says it: ft-stream drops a panel whose window
      // has gone on that, and only on that.
      if (!IsWindow(hwnd)) {
        *error = "no window " + target::address(hwnd);
        return std::nullopt;
      }
      // Sizing waits for the window's app: one that has stopped answering would hold the role.
      if (IsHungAppWindow(hwnd)) {
        *error = title_of_utf8(hwnd) + " isn't answering";
        return std::nullopt;
      }
      const auto output = target::output_name(hwnd);
      // From another virtual desktop: to this one, where it can be captured, and back on release.
      std::optional<GUID> from;
      if (!hosted_windows.contains(output) && !app_window(hwnd) && away_window(hwnd)) {
        std::string why;
        from = vdesktop::bring_here(hwnd, as_user, &why);
        if (!from) {
          *error = title_of_utf8(hwnd) + " is on another desktop, and " + why;
          return std::nullopt;
        }
        BOOST_LOG(info) << "Frametop windows: " << target::address(hwnd) << " came from desktop " << vdesktop::to_string(*from);
        if (!await_uncloaked(hwnd)) {
          vdesktop::send_back(hwnd, *from, as_user);
          *error = title_of_utf8(hwnd) + " didn't show on this desktop";
          return std::nullopt;
        }
      }
      if (!app_window(hwnd)) {
        if (from) vdesktop::send_back(hwnd, *from, as_user);
        *error = target::address(hwnd) + " isn't an app window";
        return std::nullopt;
      }
      if (!hosted_windows.contains(output)) {
        hosted_t h;
        h.hwnd = hwnd;
        h.pid = pid_of(hwnd);
        h.desktop = from;
        if (!GetWindowPlacement(hwnd, &h.before)) {
          if (from) vdesktop::send_back(hwnd, *from, as_user);
          *error = "Windows didn't say where " + target::address(hwnd) + " is";
          return std::nullopt;
        }
        h.seen = clock::now();
        hosted_windows[output] = h;
      } else {
        hosted_windows[output].seen = clock::now();
      }
      if (IsIconic(hwnd) || IsZoomed(hwnd)) {
        // Restored to its normal size without coming to the front: the desktop's focus stays put.
        WINDOWPLACEMENT p {sizeof(WINDOWPLACEMENT)};
        if (GetWindowPlacement(hwnd, &p)) {
          p.showCmd = SW_SHOWNOACTIVATE;
          p.flags &= ~UINT(WPF_RESTORETOMAXIMIZED);
          SetWindowPlacement(hwnd, &p);
        }
        if (IsIconic(hwnd) || IsZoomed(hwnd)) ShowWindow(hwnd, SW_RESTORE);
      }
      if (display) place_on(hwnd, *display);
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
      return nlohmann::json {{"output", output}, {"width", w}, {"height", h}, {"address", target::address(hwnd)}, {"title", title_of_utf8(hwnd)}};
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

    // A display job off the window thread; the flag clears if it can't start.
    void spawn(void (*job)()) {
      try {
        std::thread(job).detach();
      } catch (const std::exception &e) {
        BOOST_LOG(warning) << "Frametop windows: " << e.what();
        display_releasing = false;
      }
    }

    void watch_once() {
      std::vector<std::string> ended;
      {
        std::lock_guard guard {lock};
        if (stopping) return;
        const auto now = clock::now();
        if (!hosted_windows.empty()) {
          last_hosted = now;
          // Missing twice in a row: a monitor can drop out of the list for a moment while the
          // displays are laid out again.
          const bool missing = display_held && !display_output.empty() && !find_monitor(display_output);
          display_missing = missing ? display_missing + 1 : 0;
          if (display_missing >= 2 && !display_releasing.exchange(true)) {
            display_missing = 0;
            spawn(recover_display);
          }
        } else if (display_held && now - last_hosted > kDisplayIdle && !display_releasing.exchange(true)) {
          spawn(release_display_if_idle);
        }
        if (hosted_windows.empty()) return;
        std::vector<std::string> done;
        for (auto &[output, h] : hosted_windows) {
          if (!still(h)) {
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

    // The display needs nothing here: Vibepollo's shutdown already dropped every Remote Display
    // (notify_monitor_shutdown and the cleanup after it, at the end of nvhttp::start).
    void release_at_exit() {
      stopping = true;  // and the log may be gone by now: put_back says nothing
      auto done = std::make_shared<std::promise<void>>();
      auto finished = done->get_future();
      std::thread([done] {
        try {
          window_thread().run([] {
            std::lock_guard guard {lock};
            std::vector<std::string> outputs;
            for (const auto &[output, h] : hosted_windows) outputs.push_back(output);
            for (const auto &output : outputs) put_back(output);
            return 0;
          });
        } catch (...) {
        }
        done->set_value();
      }).detach();
      finished.wait_for(3s);  // windows left out go back at the next start
    }
  }  // namespace

  bool available() {
    return window_thread().run([] { return !app_windows().windows.empty() || session_locked(); });
  }

  nlohmann::json list() {
    refresh_store_apps();  // when they're old: a Store window is named by its app
    const auto apps = shortcut_apps(true);  // and the others by their shortcut
    return window_thread().run([&apps] {
      std::lock_guard guard {lock};
      std::map<HWND, std::string> output_of;
      for (const auto &[output, h] : hosted_windows) output_of[h.hwnd] = output;
      const auto [windows, away] = app_windows(true);
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
                          // One on another virtual desktop isn't shown: it comes here when opened.
                          {"focus", hwnd == foreground ? 0 : order++},
                          {"floating", !IsZoomed(hwnd)},
                          {"shown", !IsIconic(hwnd) && !away.contains(hwnd)},
                          {"at", {rect.left, rect.top}},
                          {"app", nullptr}};
        if (const auto app = app_of(apps, exe)) {
          w["app"] = *app;
        } else if (const auto id = has_store_apps() ? aumid_of(hwnd) : std::string {}; !id.empty() && store_app(id)) {
          w["app"] = id;  // a Store app's window
        }
        if (const auto it = output_of.find(hwnd); it != output_of.end()) w["output"] = it->second;
        out.push_back(std::move(w));
      }
      nlohmann::json outputs = nlohmann::json::array();
      for (const auto &[output, h] : hosted_windows) outputs.push_back({{"output", output}, {"address", target::address(h.hwnd)}});
      return nlohmann::json {{"windows", out}, {"outputs", outputs}, {"locked", session_locked()}};
    });
  }

  nlohmann::json apps() {
    shortcut_apps(true);  // the first list waits a little for them
    await_store_apps();
    nlohmann::json out = nlohmann::json::array();
    for (const auto &app : all_apps()) out.push_back({{"id", app.id}, {"name", app.name}, {"icon", ""}});
    return out;
  }

  std::optional<nlohmann::json> open(const std::string &address, int width, int height, [[maybe_unused]] double scale, std::string *error) {
    const auto hwnd = target::parse(std::string {target::prefix} + address);
    if (!hwnd || !address.starts_with("0x")) {
      *error = "address must be a window address (0x...)";
      return std::nullopt;
    }
    // The display first, for a window that can come out (not under the lock: it takes time).
    const bool can_open = window_thread().run([&] { return IsWindow(*hwnd) && (app_window(*hwnd) || away_window(*hwnd)) && !IsHungAppWindow(*hwnd); });
    const auto display = can_open ? hold_display() : std::nullopt;
    return window_thread().run([&] {
      std::lock_guard guard {lock};
      return open_locked(*hwnd, width, height, error, display);
    });
  }

  std::optional<nlohmann::json> launch(const std::string &app_id, int width, int height, [[maybe_unused]] double scale, std::string *error) {
    if (app_id.find('!') != std::string::npos) {
      await_store_apps();  // a Store app's AUMID
    } else {
      shortcut_apps(true);
    }
    struct started_t {
      std::optional<app_t> app;
      std::set<HWND> before;
      std::string error;
    };
    auto start_app = window_thread().run([&] {
      started_t s;
      for (const auto &app : all_apps()) {
        if (app.id == app_id) s.app = app;
      }
      if (!s.app) {
        s.error = "no app " + app_id;
        return s;
      }
      for (const auto hwnd : app_windows().windows) s.before.insert(hwnd);
      // Explorer opens the shortcut as the signed-in user would (its arguments, folder, and
      // how it runs), in the user's session.
      boost::filesystem::path working_dir;
      std::error_code ec;
      const auto env = bp::this_process::env();
      const auto what = s.app->link.empty() ? "shell:AppsFolder\\" + s.app->id : utf_utils::to_utf8(s.app->link);
      auto child = run_command(false, false, "explorer.exe \"" + what + "\"", working_dir, env, nullptr, ec, nullptr);
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
    const auto started = clock::now();
    const auto deadline = started + kLaunchWait;
    while (clock::now() < deadline) {
      std::this_thread::sleep_for(250ms);
      const bool any_new = clock::now() - started > 3s;
      const auto found = window_thread().run([&]() -> std::optional<HWND> {
        const auto windows = app_windows().windows;  // a new one opens on the desktop shown
        for (const auto hwnd : windows) {
          if (start_app.before.contains(hwnd)) continue;
          if (!start_app.app->exe.empty() && exe_of(pid_of(hwnd)) == start_app.app->exe) return hwnd;
          if (start_app.app->link.empty() && lower(aumid_of(hwnd)) == lower(start_app.app->id)) return hwnd;  // a Store app
        }
        // A shortcut to a launcher (Update.exe, a console that Terminal takes over) opens its
        // window from another exe, and one Windows can't resolve names none: then the new window
        // in front, once the exe's own is late.
        const auto front = GetForegroundWindow();
        if (any_new && front && !start_app.before.contains(front) && std::find(windows.begin(), windows.end(), front) != windows.end()) return front;
        return std::nullopt;
      });
      if (!found) continue;
      std::this_thread::sleep_for(300ms);  // let it finish placing itself before it's sized
      const auto display = hold_display();
      auto opened = window_thread().run([&] {
        std::lock_guard guard {lock};
        return open_locked(*found, width, height, error, display);
      });
      if (opened) BOOST_LOG(info) << "Frametop windows: launched " << app_id << " as " << (*opened)["address"].get<std::string>();
      return opened;
    }
    *error = start_app.app->name + " didn't open a window in time";
    return std::nullopt;
  }

  std::optional<nlohmann::json> resize(const std::string &output, int width, int height, [[maybe_unused]] double scale, std::string *error) {
    if (width <= 0 || height <= 0) {
      *error = "a resize needs a width and a height";
      return std::nullopt;
    }
    const bool resized = window_thread().run([&] {
      std::lock_guard guard {lock};
      const auto it = hosted_windows.find(output);
      if (it == hosted_windows.end()) {
        *error = "no window output " + output;
        return false;
      }
      if (!still(it->second)) {
        *error = "the window closed";
        return false;
      }
      if (IsHungAppWindow(it->second.hwnd)) {
        *error = title_of_utf8(it->second.hwnd) + " isn't answering";
        return false;
      }
      size_window(it->second.hwnd, width, height);
      return true;
    });
    if (!resized) return std::nullopt;
    return nlohmann::json {{"output", output}};
  }

  bool release(const std::string &output_or_address, std::string *error, const drop_t *drop) {
    std::optional<drop_t> onto;
    if (drop && !drop->monitor.empty()) {
      onto = *drop;
      for (auto *f : {&onto->u, &onto->v, &onto->fx, &onto->fy}) *f = std::isfinite(*f) ? std::clamp(*f, 0.0, 1.0) : 0.5;
    }
    std::string released;
    const bool ok = window_thread().run([&] {
      std::lock_guard guard {lock};
      for (const auto &[output, h] : hosted_windows) {
        if (output == output_or_address || boost::iequals(target::address(h.hwnd), output_or_address)) {
          released = output;  // put_back erases the entry
          return put_back(released, onto ? &*onto : nullptr);
        }
      }
      *error = "no window output for " + output_or_address;
      return false;
    });
    // Back on its desktop, the window is no output any more: a stream of it would send input
    // to where it was. (Linux removes the output, which ends it.)
    if (ok) end_streams({released});
    return ok;
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
    if (stopping) return 0;  // the tray's thread, at exit: release_at_exit has them
    return window_thread().run([] {
      std::lock_guard guard {lock};
      if (stopping) return 0;
      std::vector<std::string> outputs;
      for (const auto &[output, h] : hosted_windows) outputs.push_back(output);
      for (const auto &output : outputs) put_back(output);
      end_streams(outputs);
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
      for (const auto &[output, s] : saved.items()) try {
        const auto hwnd = target::parse(output);
        if (!hwnd || !s.is_object() || !IsWindow(*hwnd)) continue;
        // After a sign-out or a restart, the address may name another window now.
        if (!s.contains("pid") || s.value("pid", DWORD(0)) != pid_of(*hwnd) ||
            s.value("class", std::string {}) != utf_utils::to_utf8(class_of(*hwnd))) continue;
        WINDOWPLACEMENT p {sizeof(WINDOWPLACEMENT)};
        p.showCmd = s.value("show", UINT(SW_SHOWNORMAL));
        p.flags = s.value("flags", UINT(0));
        if (const auto n = s.value("normal", nlohmann::json::array()); n.size() == 4) p.rcNormalPosition = {n[0].get<LONG>(), n[1].get<LONG>(), n[2].get<LONG>(), n[3].get<LONG>()};
        if (const auto mn = s.value("min", nlohmann::json::array()); mn.size() == 2) p.ptMinPosition = {mn[0].get<LONG>(), mn[1].get<LONG>()};
        if (const auto mx = s.value("max", nlohmann::json::array()); mx.size() == 2) p.ptMaxPosition = {mx[0].get<LONG>(), mx[1].get<LONG>()};
        p.showCmd = quietly(p.showCmd);
        p.flags |= WPF_ASYNCWINDOWPLACEMENT;
        if (SetWindowPlacement(*hwnd, &p)) ++back;
        if (const auto desktop = vdesktop::parse(s.value("desktop", std::string {}))) vdesktop::send_back(*hwnd, *desktop, as_user);
      } catch (const std::exception &e) {
        BOOST_LOG(warning) << "Frametop windows: " << output << " in the state file: " << e.what();
      }
      hosted_windows.clear();
      save();
      if (back) BOOST_LOG(info) << "Frametop windows: put back " << back << " left from before";
      return back;
    });
    window_thread().every(std::chrono::duration_cast<std::chrono::milliseconds>(kWatch), watch_once);
    std::atexit(release_at_exit);
    refresh_shortcuts();  // ready by the first app list
    refresh_store_apps();
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
    // In front already, or one of its own windows is (a dialog it opened: clicks go to that).
    const auto front = GetForegroundWindow();
    if (front == *hwnd || (front && GetAncestor(front, GA_ROOTOWNER) == *hwnd) || (last == *hwnd && now - last_at < 250ms)) return;
    last = *hwnd;
    last_at = now;
    if (!hosted(output) || !IsWindow(*hwnd) || IsHungAppWindow(*hwnd) || !syncThreadDesktop()) return;
    if (IsIconic(*hwnd)) ShowWindowAsync(*hwnd, SW_RESTORE);
    // A process may only take the foreground from the one that has it while their input is joined.
    const auto foreground = GetForegroundWindow();
    const auto ours = GetCurrentThreadId();
    const auto theirs = foreground ? GetWindowThreadProcessId(foreground, nullptr) : 0;
    const bool joined = theirs && theirs != ours && !IsHungAppWindow(foreground) && AttachThreadInput(ours, theirs, TRUE);
    SetWindowPos(*hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW | SWP_ASYNCWINDOWPOS);
    SetForegroundWindow(*hwnd);
    if (joined) AttachThreadInput(ours, theirs, FALSE);
  }
}  // namespace platf::frametop_windows
