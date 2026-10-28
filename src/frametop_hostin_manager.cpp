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

  bool manager_t::push(const void *session, std::string payload) {
    auto &queue = queues_[session];
    if (queue.size() < MAX_QUEUED) {
      queue.push_back(std::move(payload));
      return true;
    }
    if (session != owner_) return false;
    // Whatever is queued may hold a key-up: give the input back rather than lose one, and say so
    // in place of the queue, so the client lets go of every key it holds.
    log("a client's control stream fell behind: this computer's input goes back");
    queue.clear();
    give_back("drop", "overflow");
    queue.push_back(policy::state_payload(false, "overflow"));
    return false;
  }

  void manager_t::tell(const void *session, bool engaged, std::string_view reason) {
    if (push(session, policy::state_payload(engaged, reason)) && engaged) {
      push(session, policy::hello_payload(policy::KEYBOARD | policy::MOUSE, host_name_));
    }
  }

  void manager_t::refuse(const void *session, const char *reason, clock::time_point now) {
    auto [refusal, fresh] = refused_.try_emplace(session, refusal_t {reason, now});
    if (!fresh) {
      if (refusal->second.reason == reason && now - refusal->second.at < REPORT_INTERVAL) return;
      fresh = refusal->second.reason != reason;
      refusal->second = {reason, now};
    }
    if (fresh) log(std::string("a client's lease refused: ") + reason);
    tell(session, false, reason);
  }

  void manager_t::give_back(const char *request, std::string reason) {
    if (!service_.send(request)) return reset_owner(std::move(reason));
    // A connection that went with requests unanswered answers none of them.
    if (unanswered_on_ != service_.connection()) unanswered_ = 0, unanswered_on_ = service_.connection();
    ++unanswered_;
    reset_owner(std::move(reason));
  }

  void manager_t::reset_owner(std::string reason) {
    owner_ = nullptr;
    engaged_ = false;
    reason_ = std::move(reason);
  }

  void manager_t::lose_permission(const void *session) {
    give_back("drop", "permission");
    queues_.erase(session);  // nothing it already had queued goes either
    log("a client lost its keyboard and mouse permission: this computer's input goes back");
  }

  void manager_t::lease(const void *session, bool permitted, std::string_view payload, clock::time_point now) {
    const auto lease = policy::parse_lease(payload);
    if (!lease) return;
    std::lock_guard lock {mutex_};
    if (owner_ && now >= until_) expire();
    if (!permitted) {
      // Its permission can go while it holds the input.
      if (owner_ == session) lose_permission(session);
      return refuse(session, "permission", now);
    }
    if (lease->action != policy::lease_t::engage) {
      if (owner_ && owner_ != session) return;  // not its to give back
      // A release with nobody holding the input still lifts an escape left after a session ended.
      const bool release = lease->action == policy::lease_t::release;
      if (owner_) log("a client gave this computer's keyboard and mouse back");
      if (owner_ || release) give_back(release ? "release" : "drop", "host");
      return tell(session, false, "host");
    }
    if (owner_ && owner_ != session) return refuse(session, "busy", now);
    if (owner_ != session) {
      owner_ = session;
      engaged_ = false;
      reason_ = "lease";
      next_report_ = now + REPORT_INTERVAL;  // the service's answer tells it first
      log("a client asks for this computer's keyboard and mouse");
    }
    until_ = now + lease->duration;
    if (!service_.send("engage")) {
      owner_ = nullptr;
      engaged_ = false;
      refuse(session, service_.unavailable(), now);
    }
  }

  void manager_t::expire() {
    // The client may have asked for less than the service's own lease: end it here too.
    const void *const session = owner_;
    give_back("drop", "lease");
    log("a client's lease on this computer's keyboard and mouse ran out");
    tell(session, false, "lease");
  }

  void manager_t::session_end(const void *session) {
    std::lock_guard lock {mutex_};
    queues_.erase(session);
    refused_.erase(session);
    // Give the input back now, but keep an escape: only the client lifts that.
    if (owner_ == session) give_back("drop", "lease");
  }

  void manager_t::service_message(std::uint64_t connection, std::string_view message) {
    std::lock_guard lock {mutex_};
    if (connection != service_.connection()) return;
    if (unanswered_on_ != connection) unanswered_ = 0;
    if (message == "done") {
      if (unanswered_) --unanswered_;
      return;
    }
    if (unanswered_) return;  // the last lease's
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

  std::vector<std::string> manager_t::take(const void *session, bool permitted, clock::time_point now) {
    std::lock_guard lock {mutex_};
    if (owner_ && now >= until_) expire();
    if (owner_ == session && !permitted) {
      lose_permission(session);
      refuse(session, "permission", now);
    }
    if (owner_ && now >= next_report_) {
      tell(owner_, engaged_, reason_);
      next_report_ = now + REPORT_INTERVAL;
    }
    const auto found = queues_.find(session);
    if (found == queues_.end()) return {};
    std::vector<std::string> out(std::make_move_iterator(found->second.begin()), std::make_move_iterator(found->second.end()));
    queues_.erase(found);
    return out;
  }

  bool manager_t::active() const {
    std::lock_guard lock {mutex_};
    return owner_ != nullptr;
  }
}  // namespace frametop_hostin
