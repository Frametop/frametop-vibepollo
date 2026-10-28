/**
 * @file src/frametop_hostin_manager.cpp
 * @brief Frametop host input's bookkeeping (see the header).
 */
#include "frametop_hostin_manager.h"

#include "frametop_hostin_policy.h"

namespace frametop_hostin {
  manager_t::manager_t(service_t &service, std::string host_name, std::function<void(const std::string &)> log):
      service_ {service},
      host_name_ {std::move(host_name)},
      log_ {std::move(log)} {}

  void manager_t::log(const std::string &line) const {
    if (log_) log_(line);
  }

  void manager_t::push(const void *session, std::string payload) {
    auto &queue = queues_[session];
    if (queue.size() >= MAX_QUEUED) {
      if (!overflow_logged_) log("a client's control stream fell behind: host input dropped");
      overflow_logged_ = true;
      return;
    }
    queue.push_back(std::move(payload));
  }

  void manager_t::tell(const void *session, bool engaged, std::string_view reason) {
    push(session, policy::state_payload(engaged, reason));
    if (engaged) push(session, policy::hello_payload(policy::KEYBOARD | policy::MOUSE, host_name_));
  }

  void manager_t::refuse(const void *session, const char *reason, clock::time_point now) {
    auto [at, fresh] = refused_at_.try_emplace(session, now);
    if (!fresh && now - at->second < REPORT_INTERVAL) return;
    at->second = now;
    if (fresh) log(std::string("a client's lease refused: ") + reason);
    tell(session, false, reason);
  }

  void manager_t::lease(const void *session, bool permitted, std::string_view payload, clock::time_point now) {
    const auto lease = policy::parse_lease(payload);
    if (!lease) return;
    std::lock_guard lock {mutex_};
    if (!permitted) {
      // Its permission can go while it holds the input.
      if (owner_ == session) {
        service_.send("drop");
        owner_ = nullptr;
        engaged_ = false;
        log("a client lost its keyboard and mouse permission: this computer's input goes back");
      }
      return refuse(session, "permission", now);
    }
    if (!lease->engage) {
      if (owner_ != session) return;
      service_.send("release");
      owner_ = nullptr;
      engaged_ = false;
      reason_ = "host";
      log("a client gave this computer's keyboard and mouse back");
      return tell(session, false, "host");
    }
    if (owner_ && owner_ != session && now < until_) return refuse(session, "busy", now);
    if (owner_ != session) {
      if (owner_) expire();
      owner_ = session;
      refused_at_.erase(session);
      engaged_ = false;
      reason_ = "lease";
      next_report_ = now + REPORT_INTERVAL;  // the service's answer tells it first
      log("a client asks for this computer's keyboard and mouse");
    }
    until_ = now + lease->duration;
    if (!service_.send("engage")) {
      const char *why = service_.unavailable();
      owner_ = nullptr;
      engaged_ = false;
      refuse(session, why, now);
    }
  }

  void manager_t::expire() {
    // The client may have asked for less than the service's own lease: end it here too.
    service_.send("drop");
    log("a client's lease on this computer's keyboard and mouse ran out");
    tell(owner_, false, "lease");
    owner_ = nullptr;
    engaged_ = false;
    reason_ = "lease";
  }

  void manager_t::session_end(const void *session) {
    std::lock_guard lock {mutex_};
    queues_.erase(session);
    refused_at_.erase(session);
    if (owner_ != session) return;
    // Give the input back now, but keep an escape: only the client lifts that.
    service_.send("drop");
    owner_ = nullptr;
    engaged_ = false;
  }

  void manager_t::service_message(std::uint64_t connection, std::string_view message) {
    std::lock_guard lock {mutex_};
    if (connection != service_.connection()) return;
    if (const auto state = policy::parse_service_state(message)) {
      // The service answers every engage while escaped: tell the owner only what changed.
      const bool changed = state->engaged != engaged_ || (!state->engaged && state->reason != reason_);
      engaged_ = state->engaged;
      reason_ = state->engaged ? "" : state->reason;
      if (owner_ && changed) tell(owner_, engaged_, reason_);
      return;
    }
    if (!owner_ || !engaged_) return;
    if (auto events = policy::events_payload(message)) push(owner_, std::move(*events));
  }

  void manager_t::service_gone(std::uint64_t connection) {
    std::lock_guard lock {mutex_};
    if (connection != service_.connection()) return;
    if (!engaged_ && !owner_) return;
    engaged_ = false;
    reason_ = "service";
    if (owner_) tell(owner_, false, reason_);
  }

  std::vector<std::string> manager_t::take(const void *session, clock::time_point now) {
    std::lock_guard lock {mutex_};
    if (owner_ && now >= until_) expire();
    if (owner_ && now >= next_report_) {
      tell(owner_, engaged_, reason_);
      next_report_ = now + REPORT_INTERVAL;
    }
    const auto found = queues_.find(session);
    if (found == queues_.end()) return {};
    std::vector<std::string> out(std::make_move_iterator(found->second.begin()), std::make_move_iterator(found->second.end()));
    queues_.erase(found);
    overflow_logged_ = false;
    return out;
  }

  bool manager_t::active() const {
    std::lock_guard lock {mutex_};
    return owner_ != nullptr;
  }
}  // namespace frametop_hostin
