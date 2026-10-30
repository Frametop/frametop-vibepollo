/**
 * @file src/platform/windows/frametop_hostin.cpp
 * @brief Frametop host input on Windows: lends this computer's keyboard and mouse to one Frametop
 *        client (Frametop's docs/host-input.md), the Linux capture service's protocol and rules
 *        (packaging/linux/vibepollo-host-input.c) in a thread of the host process.
 *
 * While a lease holds, WH_KEYBOARD_LL and WH_MOUSE_LL hooks take the hardware's events (the hook
 * returns 1, so Windows never sees them). Events Sunshine injects itself (the client's clicks and
 * keys) carry the INJECTED flags and pass. Keys go to the client as positional evdev codes; the
 * mouse's motion comes from Raw Input (RIDEV_INPUTSINK on a message-only window), which still
 * gets the counts of moves the hook took, before pointer acceleration. A device with no Raw
 * Input counts (a precision touchpad's pointer, an absolute device) moves by the hook's pointer
 * delta instead. Buttons and the wheel come from the hook. Pen and touch stay here.
 *
 * Hooks belong to one desktop, so the thread moves to the input desktop when it takes the input.
 * On the secure desktop (UAC, Ctrl+Alt+Del) or a locked session, input stays here: "released
 * secure". Ctrl+Alt+Del always reaches Windows.
 *
 * The rules match Linux: keys held at the take stay here, volume and media keys stay here, both
 * Shifts and Escape take the input back until a release, and keys that went to the client come up
 * there when the input goes back and stay taken here until they come up (at most DRAIN_LIMIT).
 * Key codes are never logged.
 */
// standard includes
#include <algorithm>
#include <atomic>
#include <bitset>
#include <chrono>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// platform includes
#include <Windows.h>
#include <wtsapi32.h>

// local includes
#include "frametop_hostin.h"
#include "src/config.h"
#include "src/frametop_hostin_capture.h"
#include "src/platform/common.h"

using namespace std::literals;

namespace platf::frametop_hostin {
  namespace {
    namespace capture = ::frametop_hostin::capture;
    using clock = std::chrono::steady_clock;

    /// The one connection: the thread runs for the life of the process.
    constexpr std::uint64_t CONNECTION = 1;
    /// How often the input desktop and the session's lock are looked at while taken.
    constexpr auto DESKTOP_CHECK = 250ms;
    /// The hook's pointer delta moves the client only when Raw Input gave no counts for this long:
    /// otherwise it's a mouse Raw Input has (its counts arrive just after the hook).
    constexpr auto RAW_QUIET = 100ms;
    constexpr auto HOOK_MOTION_WAIT = 8ms;
    /// Mouse messages Windows makes from pen and touch (GetMessageExtraInfo's signature).
    constexpr ULONG_PTR PEN_TOUCH_MASK = 0xFFFFFF00;
    constexpr ULONG_PTR PEN_TOUCH_SIGNATURE = 0xFF515700;
    /// Events in one lift report, under the client's limit with room for the sync.
    constexpr std::size_t LIFT_CHUNK = 60;

    bool is_button(std::uint16_t code) {
      return code >= capture::BTN_LEFT && code <= capture::BTN_EXTRA;
    }

    /// The console session is locked (its lock screen may still be on the Default desktop).
    bool session_locked() {
      LPWSTR buffer = nullptr;
      DWORD bytes = 0;
      if (!WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, WTSGetActiveConsoleSessionId(), WTSSessionInfoEx, &buffer, &bytes)) {
        return false;
      }
      bool locked = false;
      if (bytes >= sizeof(WTSINFOEXW)) {
        const auto *info = reinterpret_cast<const WTSINFOEXW *>(buffer);
        locked = info->Level == 1 && info->Data.WTSInfoExLevel1.SessionFlags == WTS_SESSIONSTATE_LOCK;
      }
      WTSFreeMemory(buffer);
      return locked;
    }

    /// The desktop that gets the input now, open, when it's the user's ("Default") and the
    /// session isn't locked. Null for the secure desktop, which a user process can't even open.
    HDESK open_user_desktop() {
      if (session_locked()) return nullptr;
      HDESK desktop = OpenInputDesktop(DF_ALLOWOTHERACCOUNTHOOK, FALSE, GENERIC_ALL);
      if (!desktop) return nullptr;
      wchar_t name[64] {};
      if (!GetUserObjectInformationW(desktop, UOI_NAME, name, sizeof name - sizeof name[0], nullptr) || std::wstring_view {name} != L"Default") {
        CloseDesktop(desktop);
        return nullptr;
      }
      return desktop;
    }

    class capture_t;
    capture_t *the_capture = nullptr;  ///< the hooks' and window's, on the capture thread

    LRESULT CALLBACK keyboard_hook(int code, WPARAM wparam, LPARAM lparam);
    LRESULT CALLBACK mouse_hook(int code, WPARAM wparam, LPARAM lparam);
    LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);

    /// The capture thread. Everything but post() runs on it; the hooks only change state and
    /// queue messages, which go to the manager after each round of the message pump, so a hook
    /// never waits on a lock (Windows skips, and then removes, a hook that takes too long).
    class capture_t {
    public:
      explicit capture_t(callbacks_t callbacks):
          callbacks_ {std::move(callbacks)},
          wake_ {CreateEventW(nullptr, FALSE, FALSE, nullptr)} {
      }

      /// A request from the manager, under its lock: queued for the thread.
      bool post(std::string_view request) {
        if (!wake_ || stopped_) return false;
        {
          std::lock_guard lock {requests_mutex_};
          requests_.emplace_back(request);
        }
        std::call_once(started_, [this] {
          std::thread {&capture_t::run, this}.detach();
        });
        SetEvent(wake_);
        return true;
      }

      LRESULT on_key(int code, WPARAM wparam, const KBDLLHOOKSTRUCT &key);
      LRESULT on_mouse(int code, WPARAM wparam, const MSLLHOOKSTRUCT &mouse);
      void on_raw_input(HRAWINPUT input);

    private:
      void run();
      void serve_requests(clock::time_point now);
      void take(clock::time_point now);
      /// keys_ gave the input back already, with these to let go of on the client.
      void finish_give_back(const std::vector<std::uint16_t> &lift, const char *reason, clock::time_point now);
      void give_back(const char *reason, clock::time_point now);
      void refuse(const char *reason);
      enum class hook_e {
        hooked,
        secure,
        failed,
      };

      hook_e hook();
      /// Logged once each time this computer's secure desktop starts keeping the input.
      void secure_now();
      void unhook();
      bool open_window();
      void close_window();
      void flush_motion();
      void add_motion(int dx, int dy);
      void key_event(std::uint16_t code, bool down);
      void wheel_event(std::uint16_t hi_res, std::uint16_t notch_code, capture::wheel_t &wheel, int delta);
      void say(std::string message);
      void deliver();
      void log(const std::string &line) {
        if (callbacks_.log) callbacks_.log(line);
      }

      DWORD wait_ms(clock::time_point now) const;

      const callbacks_t callbacks_;
      const HANDLE wake_;
      std::once_flag started_;
      std::atomic<bool> stopped_ {false};
      std::mutex requests_mutex_;
      std::deque<std::string> requests_;

      // The capture thread's.
      capture::lease_t lease_;
      capture::keys_t keys_;
      capture::wheel_t wheel_;
      capture::wheel_t hwheel_;
      HDESK desktop_ = nullptr;  ///< the one this thread was moved to
      HHOOK keyboard_hook_ = nullptr;
      HHOOK mouse_hook_ = nullptr;
      HWND window_ = nullptr;
      bool raw_input_ = false;
      clock::time_point drain_until_ {};
      clock::time_point next_desktop_check_ {};
      bool escaped_ = false;  ///< the hook saw the escape: give back on this round
      std::vector<std::uint16_t> escape_lift_;
      int motion_x_ = 0, motion_y_ = 0;  ///< Raw Input's counts, not sent yet
      clock::time_point raw_motion_at_ {};
      int hook_x_ = 0, hook_y_ = 0;  ///< the hook's pointer delta, until Raw Input shows it's a mouse
      std::optional<clock::time_point> hook_motion_at_;
      std::vector<std::string> out_;  ///< for the manager, in order
      bool secure_logged_ = false;
      bool hook_failure_logged_ = false;
    };

    void capture_t::run() {
      platf::set_thread_name("frametop-hostin"s);
      SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
      // Hook positions and GetCursorPos in the same (physical) pixels.
      SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
      the_capture = this;
      log("capture thread started");
      for (;;) {
        const auto before = clock::now();
        const DWORD waited = MsgWaitForMultipleObjectsEx(1, &wake_, wait_ms(before), QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (waited == WAIT_FAILED) break;
        // The hooks run in here, and Raw Input's WM_INPUT is dispatched to the window.
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
          DispatchMessageW(&message);
        }
        auto now = clock::now();
        if (escaped_) {
          escaped_ = false;
          lease_.escape();
          log("this computer took its keyboard and mouse back (both Shifts and Escape)");
          finish_give_back(escape_lift_, "escape", now);
          escape_lift_.clear();
        }
        // A mouse whose counts never came: its pointer delta moves the client.
        if (hook_motion_at_ && now - *hook_motion_at_ >= HOOK_MOTION_WAIT) {
          if (keys_.taken()) add_motion(hook_x_, hook_y_);
          hook_x_ = hook_y_ = 0;
          hook_motion_at_.reset();
        }
        serve_requests(now);
        now = clock::now();
        if (lease_.engaged()) {
          if (lease_.lapsed(now)) {
            give_back("lease", now);
          } else if (now >= next_desktop_check_) {
            next_desktop_check_ = now + DESKTOP_CHECK;
            if (HDESK desktop = open_user_desktop()) {
              CloseDesktop(desktop);
            } else {
              lease_.refuse();
              give_back("secure", now);
              secure_now();
            }
          }
        }
        if (!keys_.taken() && (keyboard_hook_ || mouse_hook_) && (!keys_.draining() || now >= drain_until_)) {
          keys_.end_drain();
          unhook();
        }
        flush_motion();
        deliver();
      }
      // Not expected: the thread's wait failed.
      if (lease_.engaged()) give_back("service", clock::now());
      unhook();
      close_window();
      deliver();
      the_capture = nullptr;
      stopped_ = true;
      log("capture thread stopped: "s + std::to_string(GetLastError()));
      if (callbacks_.gone) callbacks_.gone(CONNECTION);
    }

    DWORD capture_t::wait_ms(clock::time_point now) const {
      std::optional<clock::time_point> next;
      const auto sooner = [&](clock::time_point at) {
        if (!next || at < *next) next = at;
      };
      if (lease_.engaged()) {
        sooner(lease_.until());
        sooner(next_desktop_check_);
      }
      if (!keys_.taken() && (keyboard_hook_ || mouse_hook_)) sooner(drain_until_);
      if (hook_motion_at_) sooner(*hook_motion_at_ + HOOK_MOTION_WAIT);
      if (!next) return INFINITE;
      if (*next <= now) return 0;
      return DWORD(std::chrono::ceil<std::chrono::milliseconds>(*next - now).count());
    }

    void capture_t::serve_requests(clock::time_point now) {
      std::deque<std::string> requests;
      {
        std::lock_guard lock {requests_mutex_};
        requests.swap(requests_);
      }
      for (const auto &request : requests) {
        const auto parsed = capture::lease_t::parse(request);
        if (parsed == capture::lease_t::request_e::invalid) continue;  // the manager sends none
        switch (lease_.request(parsed, now)) {
          case capture::lease_t::step_e::take:
            take(now);
            break;
          case capture::lease_t::step_e::give_back:
            give_back("host", now);
            break;
          case capture::lease_t::step_e::still_escaped:
            say("state released escape");
            break;
          case capture::lease_t::step_e::stay:
            break;
        }
        if (parsed != capture::lease_t::request_e::engage) say("done");
      }
    }

    void capture_t::refuse(const char *reason) {
      lease_.refuse();
      say("state released "s + reason);
    }

    void capture_t::secure_now() {
      if (!secure_logged_) log("the secure desktop or the lock screen has the input: it stays here");
      secure_logged_ = true;
    }

    void capture_t::take(clock::time_point now) {
      switch (hook()) {
        case hook_e::hooked:
          break;
        case hook_e::secure:
          // The client asks again every half second: it gets the input once this ends.
          secure_now();
          refuse("secure");
          return;
        case hook_e::failed:
          refuse("service");
          return;
      }
      if (!open_window()) {
        log("Raw Input isn't available: the mouse moves by its pointer delta");
      }
      // Keys and buttons already down here stay here: their ups (and repeats) aren't the client's.
      std::bitset<256> held;
      for (int vk = 1; vk < 256; ++vk) {
        if (GetAsyncKeyState(vk) & 0x8000) held[vk] = true;
      }
      keys_.take(held);
      wheel_.reset();
      hwheel_.reset();
      motion_x_ = motion_y_ = hook_x_ = hook_y_ = 0;
      hook_motion_at_.reset();
      raw_motion_at_ = {};
      next_desktop_check_ = now + DESKTOP_CHECK;
      secure_logged_ = false;
      hook_failure_logged_ = false;
      say("state engaged");
      log("engaged");
    }

    void capture_t::give_back(const char *reason, clock::time_point now) {
      finish_give_back(keys_.give_back(), reason, now);
    }

    void capture_t::finish_give_back(const std::vector<std::uint16_t> &lift, const char *reason, clock::time_point now) {
      flush_motion();
      hook_x_ = hook_y_ = 0;
      hook_motion_at_.reset();
      // What the client holds comes up there, keys and buttons in their own records.
      std::vector<capture::event_t> keys, buttons;
      for (const auto code : lift) {
        (is_button(code) ? buttons : keys).push_back({capture::EV_KEY, code, 0});
      }
      for (auto *events : {&keys, &buttons}) {
        const char kind = events == &keys ? 'k' : 'm';
        for (std::size_t at = 0; at < events->size(); at += LIFT_CHUNK) {
          const auto end = std::min(events->size(), at + LIFT_CHUNK);
          out_.push_back(capture::events_message(kind, {events->begin() + at, events->begin() + end}));
        }
      }
      say("state released "s + reason);
      log("released: "s + reason);
      close_window();
      // The hooks stay while keys that went to the client are still down here.
      drain_until_ = now + capture::DRAIN_LIMIT;
    }

    /// The hooks, on the input desktop.
    capture_t::hook_e capture_t::hook() {
      HDESK desktop = open_user_desktop();
      if (!desktop) return hook_e::secure;
      if (keyboard_hook_ || mouse_hook_) {
        // Still draining, on this desktop: the only one a user's input arrives on.
        CloseDesktop(desktop);
        return hook_e::hooked;
      }
      // A thread with no windows or hooks may move.
      if (SetThreadDesktop(desktop)) {
        if (desktop_ && desktop_ != desktop) CloseDesktop(desktop_);
        desktop_ = desktop;
      } else {
        log("couldn't move to the input desktop: "s + std::to_string(GetLastError()));
        CloseDesktop(desktop);
      }
      const HINSTANCE module = GetModuleHandleW(nullptr);
      keyboard_hook_ = SetWindowsHookExW(WH_KEYBOARD_LL, keyboard_hook, module, 0);
      mouse_hook_ = SetWindowsHookExW(WH_MOUSE_LL, mouse_hook, module, 0);
      if (!keyboard_hook_ || !mouse_hook_) {
        if (!hook_failure_logged_) log("couldn't set the input hooks: "s + std::to_string(GetLastError()));
        hook_failure_logged_ = true;
        unhook();
        return hook_e::failed;
      }
      return hook_e::hooked;
    }

    void capture_t::unhook() {
      if (keyboard_hook_) UnhookWindowsHookEx(keyboard_hook_);
      if (mouse_hook_) UnhookWindowsHookEx(mouse_hook_);
      keyboard_hook_ = mouse_hook_ = nullptr;
    }

    bool capture_t::open_window() {
      if (!window_) {
        static const ATOM window_class = [] {
          WNDCLASSEXW description {};
          description.cbSize = sizeof description;
          description.lpfnWndProc = window_proc;
          description.hInstance = GetModuleHandleW(nullptr);
          description.lpszClassName = L"FrametopHostInput";
          return RegisterClassExW(&description);
        }();
        if (!window_class) return false;
        window_ = CreateWindowExW(0, L"FrametopHostInput", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!window_) return false;
      }
      // Generic desktop, mouse: every mouse's counts, though no window of ours has the focus.
      RAWINPUTDEVICE device {0x01, 0x02, RIDEV_INPUTSINK, window_};
      raw_input_ = RegisterRawInputDevices(&device, 1, sizeof device);
      return raw_input_;
    }

    void capture_t::close_window() {
      if (raw_input_) {
        RAWINPUTDEVICE device {0x01, 0x02, RIDEV_REMOVE, nullptr};
        RegisterRawInputDevices(&device, 1, sizeof device);
        raw_input_ = false;
      }
      // Gone before the thread moves desktops again (a thread with a window can't).
      if (window_) DestroyWindow(window_);
      window_ = nullptr;
    }

    LRESULT capture_t::on_key(int code, WPARAM wparam, const KBDLLHOOKSTRUCT &key) {
      // Sunshine's own (the client's typing) and other software's keys aren't the hardware's.
      if (code != HC_ACTION || (key.flags & LLKHF_INJECTED)) return CallNextHookEx(nullptr, code, wparam, reinterpret_cast<LPARAM>(&key));
      const bool down = !(key.flags & LLKHF_UP);
      const auto evdev = capture::key_code(key.scanCode, key.flags & LLKHF_EXTENDED, key.vkCode);
      if (keys_.escape(evdev, down)) {
        // Taken back now: keys_ routes everything after this as given back.
        escape_lift_ = keys_.give_back();
        escaped_ = true;
      }
      switch (keys_.key(evdev, key.vkCode, down)) {
        case capture::route_e::pass:
          return CallNextHookEx(nullptr, code, wparam, reinterpret_cast<LPARAM>(&key));
        case capture::route_e::forward:
          key_event(evdev, down);
          return 1;
        case capture::route_e::swallow:
          return 1;
      }
      return 1;
    }

    LRESULT capture_t::on_mouse(int code, WPARAM wparam, const MSLLHOOKSTRUCT &mouse) {
      const auto next = [&] {
        return CallNextHookEx(nullptr, code, wparam, reinterpret_cast<LPARAM>(&mouse));
      };
      if (code != HC_ACTION || (mouse.flags & LLMHF_INJECTED)) return next();
      if ((mouse.dwExtraInfo & PEN_TOUCH_MASK) == PEN_TOUCH_SIGNATURE) return next();  // pen and touch stay here
      std::uint16_t button = 0;
      std::uint32_t vk = 0;
      bool down = false;
      switch (wparam) {
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
          button = capture::BTN_LEFT, vk = VK_LBUTTON, down = wparam == WM_LBUTTONDOWN;
          break;
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
          button = capture::BTN_RIGHT, vk = VK_RBUTTON, down = wparam == WM_RBUTTONDOWN;
          break;
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:
          button = capture::BTN_MIDDLE, vk = VK_MBUTTON, down = wparam == WM_MBUTTONDOWN;
          break;
        case WM_XBUTTONDOWN:
        case WM_XBUTTONUP: {
          const bool first = HIWORD(mouse.mouseData) == XBUTTON1;
          button = first ? capture::BTN_SIDE : capture::BTN_EXTRA;
          vk = first ? VK_XBUTTON1 : VK_XBUTTON2;
          down = wparam == WM_XBUTTONDOWN;
          break;
        }
        default:
          break;
      }
      if (button) {
        switch (keys_.key(button, vk, down)) {
          case capture::route_e::pass:
            return next();
          case capture::route_e::forward:
            flush_motion();  // the move before the click lands first
            out_.push_back(capture::events_message('m', {{capture::EV_KEY, button, down ? 1 : 0}}));
            return 1;
          case capture::route_e::swallow:
            return 1;
        }
      }
      if (!keys_.taken()) return next();
      switch (wparam) {
        case WM_MOUSEMOVE: {
          POINT cursor;
          if (!GetCursorPos(&cursor)) break;
          // The hook took every move, so the cursor is where it was: this is the move's delta.
          const auto now = clock::now();
          if (now - raw_motion_at_ < RAW_QUIET) break;  // a mouse: its counts come from Raw Input
          hook_x_ += mouse.pt.x - cursor.x;
          hook_y_ += mouse.pt.y - cursor.y;
          if (!hook_motion_at_) hook_motion_at_ = now;
          break;
        }
        case WM_MOUSEWHEEL:
          wheel_event(capture::REL_WHEEL_HI_RES, capture::REL_WHEEL, wheel_, GET_WHEEL_DELTA_WPARAM(mouse.mouseData));
          break;
        case WM_MOUSEHWHEEL:
          wheel_event(capture::REL_HWHEEL_HI_RES, capture::REL_HWHEEL, hwheel_, GET_WHEEL_DELTA_WPARAM(mouse.mouseData));
          break;
        default:
          break;
      }
      return 1;
    }

    void capture_t::on_raw_input(HRAWINPUT input) {
      RAWINPUT raw;
      UINT size = sizeof raw;
      if (GetRawInputData(input, RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER)) == UINT(-1)) return;
      // No device: injected (Sunshine's own moves) or made up by Windows.
      if (raw.header.dwType != RIM_TYPEMOUSE || !raw.header.hDevice || !keys_.taken()) return;
      const auto &mouse = raw.data.mouse;
      if (mouse.usFlags & MOUSE_MOVE_ABSOLUTE) return;  // a tablet or a remote desktop: the hook's delta
      if (!mouse.lLastX && !mouse.lLastY) return;
      raw_motion_at_ = clock::now();
      // The hook's delta for this move was the same mouse's.
      hook_x_ = hook_y_ = 0;
      hook_motion_at_.reset();
      add_motion(mouse.lLastX, mouse.lLastY);
    }

    void capture_t::add_motion(int dx, int dy) {
      motion_x_ += dx;
      motion_y_ += dy;
    }

    /// Motion so far as one report, before anything that came after it.
    void capture_t::flush_motion() {
      if (!motion_x_ && !motion_y_) return;
      std::vector<capture::event_t> events;
      if (motion_x_) events.push_back({capture::EV_REL, capture::REL_X, motion_x_});
      if (motion_y_) events.push_back({capture::EV_REL, capture::REL_Y, motion_y_});
      out_.push_back(capture::events_message('m', events));
      motion_x_ = motion_y_ = 0;
    }

    void capture_t::key_event(std::uint16_t code, bool down) {
      out_.push_back(capture::events_message('k', {{capture::EV_KEY, code, down ? 1 : 0}}));
    }

    void capture_t::wheel_event(std::uint16_t hi_res, std::uint16_t notch_code, capture::wheel_t &wheel, int delta) {
      if (!delta) return;
      flush_motion();
      std::vector<capture::event_t> events {{capture::EV_REL, hi_res, delta}};
      if (const int notches = wheel.notches(delta)) events.push_back({capture::EV_REL, notch_code, notches});
      out_.push_back(capture::events_message('m', events));
    }

    void capture_t::say(std::string message) {
      out_.push_back(std::move(message));
    }

    void capture_t::deliver() {
      if (out_.empty()) return;
      auto messages = std::move(out_);
      out_.clear();
      if (!callbacks_.message) return;
      for (const auto &message : messages) {
        callbacks_.message(CONNECTION, message);
      }
    }

    LRESULT CALLBACK keyboard_hook(int code, WPARAM wparam, LPARAM lparam) {
      if (!the_capture) return CallNextHookEx(nullptr, code, wparam, lparam);
      return the_capture->on_key(code, wparam, *reinterpret_cast<const KBDLLHOOKSTRUCT *>(lparam));
    }

    LRESULT CALLBACK mouse_hook(int code, WPARAM wparam, LPARAM lparam) {
      if (!the_capture) return CallNextHookEx(nullptr, code, wparam, lparam);
      return the_capture->on_mouse(code, wparam, *reinterpret_cast<const MSLLHOOKSTRUCT *>(lparam));
    }

    LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
      // WM_INPUT goes on to DefWindowProc too, which frees its data.
      if (message == WM_INPUT && the_capture) the_capture->on_raw_input(reinterpret_cast<HRAWINPUT>(lparam));
      return DefWindowProcW(window, message, wparam, lparam);
    }

    /// The manager's view: the thread starts on the first request, when the setting allows it.
    class hook_service_t: public ::frametop_hostin::manager_t::service_t {
    public:
      explicit hook_service_t(callbacks_t callbacks):
          capture_ {new capture_t {std::move(callbacks)}} {
      }

      bool send(std::string_view request) override {
        // Read each time: the setting changes with the config, and "off" says why.
        off_ = !config::input.frametop_host_input;
        if (off_) return false;  // a lease held already lapses within LEASE
        return capture_->post(request);
      }

      const char *unavailable() const override {
        return off_ ? "off" : "service";
      }

      std::uint64_t connection() const override {
        return CONNECTION;
      }

    private:
      // Never destroyed: its thread runs for the life of the process.
      capture_t *capture_;
      bool off_ = false;
    };
  }  // namespace

  std::unique_ptr<::frametop_hostin::manager_t::service_t> make_service(callbacks_t callbacks) {
    return std::make_unique<hook_service_t>(std::move(callbacks));
  }
}  // namespace platf::frametop_hostin
