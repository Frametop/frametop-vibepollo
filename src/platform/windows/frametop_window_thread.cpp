/**
 * @file src/platform/windows/frametop_window_thread.cpp
 * @brief The Frametop window role's own thread (see frametop_window_thread.h).
 */
#include "frametop_window_thread.h"

#include "src/logging.h"

#include <windows.h>
#include <objbase.h>

#include <thread>

namespace platf::frametop_windows {
  using clock = std::chrono::steady_clock;
  using namespace std::literals;
  constexpr auto kFirstTick = 2s;  // before every() has said how often

  void window_thread_t::every(std::chrono::milliseconds interval, std::function<void()> f) {
    std::lock_guard guard {queue_lock};
    ensure_started();
    tick_interval = interval;
    tick = std::move(f);
  }

  void window_thread_t::ensure_started() {
    if (running) return;
    running = true;
    std::thread([this] { loop(); }).detach();
  }

  void window_thread_t::bind_desktop() {
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

  void window_thread_t::loop() {
    // Physical pixels whatever the monitor's scale, as capture and input use.
    using set_context_t = void *(WINAPI *) (void *);
    if (const auto set_context = reinterpret_cast<set_context_t>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetThreadDpiAwarenessContext"))) {
      set_context(reinterpret_cast<void *>(static_cast<intptr_t>(-4)));  // per monitor v2
    }
    // Multithreaded COM makes no window, so the thread can still change desktops.
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    auto next_tick = clock::now() + kFirstTick;
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

  window_thread_t &window_thread() {
    static auto *thread = new window_thread_t;  // never destroyed: at-exit release may still use it
    return *thread;
  }
}  // namespace platf::frametop_windows
