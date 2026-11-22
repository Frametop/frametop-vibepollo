/**
 * @file src/frametop_monitors.cpp
 * @brief Frametop's plan for the host's physical monitors (see the header).
 */
#include "frametop_monitors.h"

#include <algorithm>

namespace frametop_monitors {
  namespace {
    using json = nlohmann::json;

    const monitor_t *find(const std::vector<monitor_t> &monitors, const std::string &id) {
      const auto it = std::find_if(monitors.begin(), monitors.end(), [&](const monitor_t &m) {
        return m.id == id;
      });
      return it == monitors.end() ? nullptr : &*it;
    }

    std::string describe(const video_mode_t &mode) {
      const unsigned whole = mode.refresh_mhz / 1000, part = mode.refresh_mhz % 1000;
      std::string hz = std::to_string(whole);
      if (part) {
        std::string frac = std::to_string(part);
        frac.insert(0, 3 - frac.size(), '0');
        while (!frac.empty() && frac.back() == '0') frac.pop_back();
        hz += "." + frac;
      }
      return std::to_string(mode.width) + "x" + std::to_string(mode.height) + "@" + hz;
    }

    std::string label(const monitor_t &m) {
      return m.name.empty() ? m.id : m.name;
    }

    // How many outputs stay on once `targets` are made so.
    int on_after(const inventory_t &now, const std::vector<target_t> &targets) {
      int on = now.other_outputs;
      for (const auto &m : now.monitors) {
        const auto t = std::find_if(targets.begin(), targets.end(), [&](const target_t &x) {
          return x.id == m.id;
        });
        on += (t == targets.end() ? m.on : t->on) ? 1 : 0;
      }
      return on;
    }
  }  // namespace

  bool same_mode(const video_mode_t &a, const video_mode_t &b) {
    const auto diff = a.refresh_mhz > b.refresh_mhz ? a.refresh_mhz - b.refresh_mhz : b.refresh_mhz - a.refresh_mhz;
    return a.width == b.width && a.height == b.height && diff <= 500;
  }

  json to_json(const video_mode_t &mode) {
    return {{"width", mode.width}, {"height", mode.height}, {"refresh_mhz", mode.refresh_mhz}};
  }

  std::optional<video_mode_t> mode_from_json(const json &value) {
    if (!value.is_object()) return std::nullopt;
    for (const char *key : {"width", "height", "refresh_mhz"}) {
      if (!value.contains(key) || !value[key].is_number_unsigned()) return std::nullopt;
    }
    video_mode_t mode {value["width"].get<unsigned>(), value["height"].get<unsigned>(), value["refresh_mhz"].get<unsigned>()};
    if (!mode.width || !mode.height || mode.width > 16384 || mode.height > 16384 || mode.refresh_mhz > 1000000) {
      return std::nullopt;
    }
    return mode;
  }

  json to_json(const monitor_t &m) {
    json modes = json::array();
    for (const auto &mode : m.modes) modes.push_back(to_json(mode));
    return {{"id", m.id}, {"name", m.name}, {"on", m.on}, {"primary", m.primary},
            {"mode", m.mode ? to_json(*m.mode) : json(nullptr)}, {"modes", std::move(modes)}, {"place", m.place}};
  }

  std::optional<monitor_t> monitor_from_json(const json &value) {
    if (!value.is_object() || !value.contains("id") || !value["id"].is_string()) return std::nullopt;
    monitor_t m;
    m.id = value["id"].get<std::string>();
    if (value.contains("name") && value["name"].is_string()) m.name = value["name"].get<std::string>();
    m.on = value.value("on", false) == true;
    m.primary = value.value("primary", false) == true;
    if (value.contains("mode")) m.mode = mode_from_json(value["mode"]);
    if (value.contains("modes") && value["modes"].is_array()) {
      for (const auto &mode : value["modes"]) {
        if (const auto parsed = mode_from_json(mode)) m.modes.push_back(*parsed);
      }
    }
    if (value.contains("place") && value["place"].is_object()) m.place = value["place"];
    return m;
  }

  std::optional<plan_t> plan_from_json(const json &value, std::string *error) {
    if (!value.is_object()) {
      if (error) *error = "monitors must be an object of monitor ids";
      return std::nullopt;
    }
    plan_t plan;
    for (const auto &[id, wish] : value.items()) {
      if (id.empty() || id.size() > 512 || !wish.is_object() || !wish.contains("on") || !wish["on"].is_boolean()) {
        if (error) *error = "each monitor needs \"on\": true or false";
        return std::nullopt;
      }
      wish_t w;
      w.on = wish["on"].get<bool>();
      if (wish.contains("mode") && !wish["mode"].is_null()) {
        w.mode = mode_from_json(wish["mode"]);
        if (!w.mode) {
          if (error) *error = "a mode is {\"width\", \"height\", \"refresh_mhz\"}";
          return std::nullopt;
        }
      }
      if (!w.on) w.mode.reset();  // an off monitor has no mode to be in
      plan[id] = w;
    }
    return plan;
  }

  json to_json(const plan_t &plan) {
    json out = json::object();
    for (const auto &[id, wish] : plan) {
      out[id] = {{"on", wish.on}, {"mode", wish.mode ? to_json(*wish.mode) : json(nullptr)}};
    }
    return out;
  }

  std::string validate(const plan_t &plan, const inventory_t &now) {
    int on = now.other_outputs;
    for (const auto &m : now.monitors) {
      const auto wish = plan.find(m.id);
      on += (wish == plan.end() ? m.on : wish->second.on) ? 1 : 0;
    }
    for (const auto &[id, wish] : plan) {
      const auto *m = find(now.monitors, id);
      if (!m) return "There's no monitor " + id;
      if (wish.mode && std::none_of(m->modes.begin(), m->modes.end(), [&](const video_mode_t &listed) {
            return same_mode(listed, *wish.mode);
          })) {
        return label(*m) + " has no " + describe(*wish.mode) + " mode";
      }
    }
    if (on == 0) return "Keep one display on: this would turn every display off";
    return {};
  }

  std::vector<target_t> plan_targets(const plan_t &plan, const inventory_t &now, const std::vector<monitor_t> &saved,
                                     std::string *kept_on) {
    std::vector<target_t> targets;
    for (const auto &[id, wish] : plan) {
      const auto *m = find(now.monitors, id);
      if (!m) continue;  // unplugged since
      const auto *was = find(saved, id);
      const bool was_on = was && was->on;
      if (!wish.on) {
        if (m->on) targets.push_back({id, false, std::nullopt, json::object()});
      } else if (!m->on) {
        targets.push_back({id, true, wish.mode ? wish.mode : was_on ? was->mode : std::nullopt,
                           was_on ? was->place : json::object()});
      } else if (wish.mode && (!m->mode || !same_mode(*m->mode, *wish.mode))) {
        targets.push_back({id, true, wish.mode, m->place});
      }
    }
    if (on_after(now, targets) == 0) {
      // Never every output off: the primary monitor stays (else the first one turned off).
      auto keep = std::find_if(targets.begin(), targets.end(), [&](const target_t &t) {
        const auto *m = find(now.monitors, t.id);
        return !t.on && m && m->primary;
      });
      if (keep == targets.end()) {
        keep = std::find_if(targets.begin(), targets.end(), [](const target_t &t) {
          return !t.on;
        });
      }
      if (keep != targets.end()) {
        if (kept_on) *kept_on = label(*find(now.monitors, keep->id));
        targets.erase(keep);
      }
    }
    return targets;
  }

  bool still_as_set(const monitor_t &now, const wish_t &set) {
    return now.on == set.on && (!set.on || !set.mode || (now.mode && same_mode(*now.mode, *set.mode)));
  }

  std::vector<target_t> restore_targets(const std::vector<std::string> &ids, const std::vector<monitor_t> &saved,
                                        const std::map<std::string, wish_t> &set, const inventory_t &now) {
    std::vector<target_t> targets;
    for (const auto &id : ids) {
      const auto *was = find(saved, id);
      const auto *m = find(now.monitors, id);
      const auto made = set.find(id);
      if (!was || !m || made == set.end() || !still_as_set(*m, made->second)) continue;  // theirs stays
      if (!was->on) {
        if (m->on) targets.push_back({id, false, std::nullopt, json::object()});
      } else if (!m->on || (was->mode && (!m->mode || !same_mode(*m->mode, *was->mode)))) {
        targets.push_back({id, true, was->mode, was->place});
      }
    }
    return targets;
  }

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
    store_.save({{"version", 1}, {"plan", to_json(plan_)}, {"saved", std::move(saved)}, {"set", std::move(set)}});
  }

  bool manager_t::start() {
    std::lock_guard lock {mutex_};
    plan_.clear();
    saved_.clear();
    set_.clear();
    const auto state = store_.load();
    if (!state || !state->is_object()) return true;
    if (state->contains("plan")) {
      if (auto plan = plan_from_json((*state)["plan"], nullptr)) plan_ = std::move(*plan);
    }
    if (state->contains("saved") && (*state)["saved"].is_array()) {
      for (const auto &m : (*state)["saved"]) {
        if (auto parsed = monitor_from_json(m)) saved_.push_back(std::move(*parsed));
      }
    }
    if (state->contains("set") && (*state)["set"].is_object()) {
      if (auto set = plan_from_json((*state)["set"], nullptr)) set_ = std::move(*set);
    }
    if (set_.empty()) {
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
      ++generation_;  // a pending grace no longer ends it
      if (!plan_.empty() || !set_.empty()) apply_locked();
    }
    return std::shared_ptr<void>(this, [](manager_t *self) {
      self->release();
    });
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
    if (set_.empty()) saved_.clear();
    // Monitors this changed that the plan no longer names go back as they were.
    std::vector<std::string> dropped;
    for (const auto &[id, wish] : set_) {
      if (!plan_.contains(id)) dropped.push_back(id);
    }
    auto targets = restore_targets(dropped, saved_, set_, *now);
    std::string kept_on;
    const auto planned = plan_targets(plan_, *now, saved_, &kept_on);
    targets.insert(targets.end(), planned.begin(), planned.end());
    last_error_ = kept_on.empty() ? std::string {} : kept_on + " stays on: the desktop needs one display";
    if (!kept_on.empty()) log(last_error_);
    // Each monitor is saved as it is just before this first changes it.
    for (const auto &t : planned) {
      if (set_.contains(t.id)) continue;
      const auto *m = find(now->monitors, t.id);
      std::erase_if(saved_, [&](const monitor_t &x) {
        return x.id == t.id;
      });
      saved_.push_back(*m);
      set_[t.id] = {t.on, t.mode};  // in the store before the change, for a crash
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

  bool manager_t::restore_all_locked() {
    if (set_.empty()) {
      saved_.clear();
      save_locked();
      return true;
    }
    const auto now = platform_.inventory();
    if (!now) {
      log("couldn't read the monitors to put them back");
      return false;
    }
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
    std::string error;
    auto plan = plan_from_json(body["monitors"], &error);
    if (!plan) return error;
    std::lock_guard lock {mutex_};
    const auto now = platform_.inventory();
    if (!now) return "couldn't read the monitors";
    if (error = validate(*plan, *now); !error.empty()) return error;
    plan_ = std::move(*plan);
    save_locked();
    if (holders_ > 0 || !set_.empty()) apply_locked();
    return {};
  }

  json manager_t::state() {
    std::lock_guard lock {mutex_};
    const auto now = platform_.inventory();
    json monitors = json::array();
    if (now) {
      for (const auto &m : now->monitors) {
        auto entry = to_json(m);
        entry.erase("place");
        const auto wish = plan_.find(m.id);
        entry["wish"] = wish == plan_.end() ? json(nullptr) :
                                              json {{"on", wish->second.on},
                                                    {"mode", wish->second.mode ? to_json(*wish->second.mode) : json(nullptr)}};
        entry["changed"] = set_.contains(m.id);
        monitors.push_back(std::move(entry));
      }
    }
    return {{"backend", platform_.backend()},
            {"monitors", std::move(monitors)},
            {"other_outputs", now ? now->other_outputs : 0},
            {"streams", holders_},
            {"applies", holders_ > 0 || !set_.empty()},
            {"grace_seconds", std::chrono::duration_cast<std::chrono::seconds>(grace_).count()},
            {"error", now ? last_error_ : std::string {"couldn't read the monitors"}}};
  }
}  // namespace frametop_monitors
