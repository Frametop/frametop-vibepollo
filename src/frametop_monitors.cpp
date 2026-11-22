/**
 * @file src/frametop_monitors.cpp
 * @brief Frametop's plan for the host's physical monitors: the manager (see the header).
 */
#include "frametop_monitors.h"

#include <cctype>

#include <algorithm>

namespace frametop_monitors {
  namespace {
    using json = nlohmann::json;

    const monitor_t *find(const std::vector<monitor_t> &monitors, const std::string &id) {
      return find_monitor(monitors, id);
    }
  }  // namespace

  manager_t::manager_t(platform_t &platform, store_t &store, schedule_fn schedule, std::chrono::milliseconds grace,
                       log_fn log):
      platform_ {platform},
      store_ {store},
      schedule_ {std::move(schedule)},
      grace_ {grace},
      log_ {std::move(log)} {}

  void manager_t::log(const std::string &line) const {
    if (log_) log_(line);
  }

  int manager_t::holders() const {
    std::lock_guard lock {mutex_};
    return holders_;
  }

  void manager_t::save_locked() {
    json saved = json::array();
    for (const auto &m : saved_) saved.push_back(to_json(m));
    json set = json::object();
    for (const auto &[id, wish] : set_) set[id] = {{"on", wish.on}, {"mode", wish.mode ? to_json(*wish.mode) : json(nullptr)}};
    json pending = json::object();
    for (const auto &[id, wish] : pending_) pending[id] = {{"on", wish.on}, {"mode", wish.mode ? to_json(*wish.mode) : json(nullptr)}};
    store_.save({{"version", 1}, {"plan", to_json(plan_)}, {"others_off", others_off_}, {"saved", std::move(saved)}, {"set", std::move(set)}, {"pending", std::move(pending)}});
  }

  void manager_t::settle_pending_locked(const inventory_t &now) {
    for (const auto &[id, wish] : pending_) {
      const auto *m = find(now.monitors, id);
      const auto *was = find(saved_, id);
      if (!m) continue;
      if (still_as_set(*m, wish)) {
        set_[id] = wish;
      } else if (was && !set_.contains(id) && !still_as_set(*m, {was->on, was->mode})) {
        set_[id] = {m->on, m->on ? m->mode : std::nullopt};  // part of it took (on, not yet in its mode)
      }
    }
    pending_.clear();
  }

  bool manager_t::start() {
    std::lock_guard lock {mutex_};
    if (!loaded_) {
      loaded_ = true;
      if (const auto state = store_.load(); state && state->is_object()) {
        if (state->contains("plan")) {
          if (auto plan = plan_from_json((*state)["plan"], nullptr)) plan_ = std::move(*plan);
        }
        others_off_ = state->value("others_off", false) == true;
        if (state->contains("saved") && (*state)["saved"].is_array()) {
          for (const auto &m : (*state)["saved"]) {
            if (auto parsed = monitor_from_json(m)) saved_.push_back(std::move(*parsed));
          }
        }
        if (state->contains("set") && (*state)["set"].is_object()) {
          if (auto set = plan_from_json((*state)["set"], nullptr)) set_ = std::move(*set);
        }
        if (state->contains("pending") && (*state)["pending"].is_object()) {
          if (auto pending = plan_from_json((*state)["pending"], nullptr)) pending_ = std::move(*pending);
        }
      }
    }
    // A stream that came since owns what is changed now: its release (and grace) puts it back.
    if (held_) return true;
    if (set_.empty() && pending_.empty()) {
      saved_.clear();
      return true;
    }
    log("putting back the monitors a previous run changed");
    return restore_all_locked();
  }

  std::shared_ptr<void> manager_t::hold() {
    {
      std::lock_guard lock {mutex_};
      ++holders_;
      held_ = true;
      ++generation_;  // a pending grace no longer ends it
      if (!plan_.empty() || others_off_ || !set_.empty()) apply_locked();
    }
    return std::shared_ptr<void>(this, [](manager_t *self) {
      self->release();
    });
  }

  manager_t::display_use_e manager_t::display_use(const std::string &id) {
    const auto same = [&](const std::string &other) {
      return id.size() == other.size() && std::equal(id.begin(), id.end(), other.begin(), [](unsigned char a, unsigned char b) {
               return std::tolower(a) == std::tolower(b);
             });
    };
    std::lock_guard lock {mutex_};
    const auto now = platform_.inventory();
    if (!now) return display_use_e::hold_after;
    const auto m = std::find_if(now->monitors.begin(), now->monitors.end(), [&](const monitor_t &x) {
      return same(x.id);
    });
    if (m == now->monitors.end()) return display_use_e::hold_after;
    const auto plan = plan_for(*now);
    const auto wish = plan.find(m->id);
    if (wish == plan.end()) return display_use_e::hold_after;
    if (wish->second.on) return m->on ? display_use_e::hold_after : display_use_e::hold_first;
    // Off in the plan: refused unless it stays on, as the one the desktop needs.
    if (!m->on) return display_use_e::refuse;
    const auto targets = targets_locked(plan, *now, nullptr);
    return std::any_of(targets.begin(), targets.end(), [&](const target_t &t) {
             return t.id == m->id && !t.on;
           }) ?
             display_use_e::refuse :
             display_use_e::hold_after;
  }

  void manager_t::release() {
    std::lock_guard lock {mutex_};
    holders_ = std::max(0, holders_ - 1);
    const auto generation = ++generation_;
    if (set_.empty() || !schedule_) return;
    if (holders_ == 0) {
      log("the last Frametop stream ended: the monitors go back in " +
          std::to_string(std::chrono::duration_cast<std::chrono::seconds>(grace_).count()) + " s");
      restore_later_locked(generation);
    } else {
      schedule_(recheck_after, [this, generation] {
        std::lock_guard lock {mutex_};
        if (generation == generation_ && holders_ > 0) apply_locked();
      });
    }
  }

  void manager_t::restore_later_locked(std::uint64_t generation) {
    // Tries again a grace apart until the monitors are back or a stream starts again.
    schedule_(grace_, [this, generation] {
      std::lock_guard lock {mutex_};
      if (generation != generation_ || holders_ > 0) return;
      if (!restore_all_locked() && schedule_) restore_later_locked(++generation_);
    });
  }

  void manager_t::apply_locked() {
    const auto now = platform_.inventory();
    if (!now) {
      last_error_ = "couldn't read the monitors";
      log(last_error_);
      return;
    }
    settle_pending_locked(*now);
    if (set_.empty()) saved_.clear();
    std::string kept_on;
    const auto plan = plan_for(*now);
    const auto targets = targets_locked(plan, *now, &kept_on);
    last_error_ = kept_on.empty() ? std::string {} : kept_on + " stays on: the desktop needs one display";
    if (!kept_on.empty()) log(last_error_);
    // Each monitor is saved as it is just before this first changes it, and what it's being
    // made is in the store (pending) before the change, for a crash.
    for (const auto &t : targets) {
      if (!plan.contains(t.id)) continue;  // going back: set_ has it already
      if (!set_.contains(t.id)) {
        const auto *m = find(now->monitors, t.id);
        std::erase_if(saved_, [&](const monitor_t &x) {
          return x.id == t.id;
        });
        saved_.push_back(*m);
      }
      pending_[t.id] = {t.on, t.mode};
    }
    if (targets.empty()) {
      if (set_.empty()) saved_.clear();
      save_locked();
      return;
    }
    save_locked();
    if (const auto error = platform_.apply(targets, *now); !error.empty()) {
      last_error_ = error;
      log("couldn't change the monitors: " + error);
    }
    for (const auto &[id, wish] : pending_) set_[id] = wish;
    pending_.clear();
    // What each one is now, for the check that it's still as this left it. One that is as it
    // was saved (the change didn't take, or it went back) has nothing left to put back.
    if (const auto after = platform_.inventory()) {
      for (const auto &t : targets) {
        const auto *m = find(after->monitors, t.id);
        const auto *was = find(saved_, t.id);
        if (!m || !set_.contains(t.id)) continue;
        if (was && was->on == m->on && (!m->on || !was->mode || (m->mode && same_mode(*was->mode, *m->mode)))) {
          set_.erase(t.id);
        } else {
          set_[t.id] = {m->on, m->on ? m->mode : std::nullopt};
        }
      }
    }
    std::erase_if(saved_, [&](const monitor_t &x) {
      return !set_.contains(x.id);
    });
    save_locked();
  }

  plan_t manager_t::plan_for(const inventory_t &now) const {
    return others_off_ ? with_others_off(plan_, now) : plan_;
  }

  std::vector<target_t> manager_t::targets_locked(const plan_t &plan, const inventory_t &now, std::string *kept_on) const {
    // Monitors this changed that the plan no longer names go back as they were.
    std::vector<std::string> dropped;
    for (const auto &[id, wish] : set_) {
      if (!plan.contains(id)) dropped.push_back(id);
    }
    auto targets = restore_targets(dropped, saved_, set_, now);
    std::string kept;
    const auto planned = plan_targets(plan, now, saved_, &kept);
    targets.insert(targets.end(), planned.begin(), planned.end());
    if (kept.empty()) keep_one_on(targets, now, &kept);  // with the ones going back too
    if (kept_on) *kept_on = kept;
    return targets;
  }

  bool manager_t::restore_all_locked() {
    if (set_.empty() && pending_.empty()) {
      saved_.clear();
      save_locked();
      return true;
    }
    const auto now = platform_.inventory();
    if (!now) {
      log("couldn't read the monitors to put them back");
      return false;
    }
    settle_pending_locked(*now);
    std::vector<std::string> ids;
    for (const auto &[id, wish] : set_) ids.push_back(id);
    const auto targets = restore_targets(ids, saved_, set_, *now);
    if (!targets.empty()) {
      if (const auto error = platform_.apply(targets, *now); !error.empty()) {
        last_error_ = error;
        log("couldn't put the monitors back: " + error);
        return false;
      }
    }
    saved_.clear();
    set_.clear();
    last_error_.clear();
    save_locked();
    return true;
  }

  std::string manager_t::set_plan(const json &body) {
    if (!body.is_object() || !body.contains("monitors")) return "the body is {\"monitors\": {...}}";
    if (body.contains("others_off") && !body["others_off"].is_boolean()) return "others_off is true or false";
    const bool others_off = body.value("others_off", false);
    std::string error;
    auto plan = plan_from_json(body["monitors"], &error);
    if (!plan) return error;
    std::lock_guard lock {mutex_};
    const auto now = platform_.inventory();
    if (!now) return "couldn't read the monitors";
    const auto whole = others_off ? with_others_off(*plan, *now) : *plan;
    std::vector<monitor_t> going_back;
    for (const auto &[id, wish] : set_) {
      const auto *was = find(saved_, id);
      if (!whole.contains(id) && was) going_back.push_back(*was);
    }
    if (error = validate(whole, *now, going_back); !error.empty()) return error;
    plan_ = std::move(*plan);
    others_off_ = others_off;
    save_locked();
    if (holders_ > 0 || !set_.empty()) apply_locked();
    return {};
  }

  json manager_t::state() {
    std::lock_guard lock {mutex_};
    const auto now = platform_.inventory();
    json monitors = json::array();
    if (now) {
      const auto plan = plan_for(*now);
      for (const auto &m : now->monitors) {
        auto entry = to_json(m);
        entry.erase("place");
        const auto wish = plan.find(m.id);
        entry["wish"] = wish == plan.end() ? json(nullptr) :
                                              json {{"on", wish->second.on},
                                                    {"mode", wish->second.mode ? to_json(*wish->second.mode) : json(nullptr)}};
        entry["changed"] = set_.contains(m.id);
        // Off, its rotation isn't listed: it comes back on as it was saved.
        if (const auto *was = find(saved_, m.id); !m.on && was) entry["sideways"] = was->sideways;
        monitors.push_back(std::move(entry));
      }
    }
    return {{"backend", platform_.backend()},
            {"monitors", std::move(monitors)},
            {"other_outputs", now ? now->other_outputs : 0},
            {"others_off", others_off_},
            {"streams", holders_},
            {"applies", holders_ > 0 || !set_.empty()},
            {"grace_seconds", std::chrono::duration_cast<std::chrono::seconds>(grace_).count()},
            {"error", now ? last_error_ : std::string {"couldn't read the monitors"}}};
  }
}  // namespace frametop_monitors
