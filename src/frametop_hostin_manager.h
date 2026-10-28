/**
 * @file src/frametop_hostin_manager.h
 * @brief Frametop host input's bookkeeping: which client holds this computer's input, until
 *        when, and what each client is told. The capture service behind it is an interface,
 *        so the component tests can drive it.
 */
#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace frametop_hostin {
  class manager_t {
  public:
    using clock = std::chrono::steady_clock;

    /// The capture service. Calls come with the manager's lock held.
    struct service_t {
      virtual ~service_t() = default;
      /// "engage", "release" (also lifts an escape) or "drop" (gives the input back, keeps
      /// an escape). False when there's no service now.
      virtual bool send(std::string_view request) = 0;
      /// Why there's none: "service" (it couldn't be reached) or "unsupported".
      virtual const char *unavailable() const = 0;
      /// The connection messages come from now; a reader of an older one is ignored.
      virtual std::uint64_t connection() const = 0;
    };

    /// Queued payloads per session; past this, new ones are dropped.
    static constexpr std::size_t MAX_QUEUED = 512;
    /// A client is told the state this often, and about a refusal at most this often.
    static constexpr std::chrono::seconds REPORT_INTERVAL {1};

    manager_t(service_t &service, std::string host_name, std::function<void(const std::string &)> log = {});

    void lease(const void *session, bool permitted, std::string_view payload, clock::time_point now);
    void session_end(const void *session);
    /// A message from the capture service, on that connection's reader thread.
    void service_message(std::uint64_t connection, std::string_view message);
    /// That connection to the capture service closed.
    void service_gone(std::uint64_t connection);
    /// Payloads for this session; first lets a lapsed lease go and sends the periodic state.
    std::vector<std::string> take(const void *session, clock::time_point now);
    bool active() const;

  private:
    void push(const void *session, std::string payload);
    void tell(const void *session, bool engaged, std::string_view reason);
    void refuse(const void *session, const char *reason, clock::time_point now);
    /// The owner's lease lapsed: give the input back and tell it.
    void expire();
    void log(const std::string &line) const;

    service_t &service_;
    const std::string host_name_;
    const std::function<void(const std::string &)> log_;
    mutable std::mutex mutex_;
    const void *owner_ = nullptr;
    clock::time_point until_ {};
    bool engaged_ = false;
    std::string reason_ = "lease";
    clock::time_point next_report_ {};
    std::map<const void *, std::deque<std::string>> queues_;
    std::map<const void *, clock::time_point> refused_at_;
    bool overflow_logged_ = false;
  };
}  // namespace frametop_hostin
