/**
 * @file src/platform/windows/frametop_window_thread.h
 * @brief The Frametop window role's own thread on the user's Default desktop (frametop_windows.cpp).
 */
#pragma once

#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>

namespace platf::frametop_windows {
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

    // `f` on the thread every `interval`, between jobs.
    void every(std::chrono::milliseconds interval, std::function<void()> f);

  private:
    void ensure_started();  // under queue_lock
    // Vibepollo may start before anyone signs in: until the Default desktop is there, each job
    // tries again.
    void bind_desktop();
    void loop();

    std::mutex queue_lock;
    std::condition_variable wake;
    std::deque<std::function<void()>> queue;
    std::function<void()> tick;
    std::chrono::milliseconds tick_interval {2000};
    bool running = false;  // under queue_lock
    bool bound = false, warned = false;  // window thread only
  };

  // The one window thread. Never destroyed: at-exit release may still use it.
  window_thread_t &window_thread();
}  // namespace platf::frametop_windows
