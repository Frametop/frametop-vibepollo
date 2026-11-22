/**
 * @file src/frametop_monitors_plan.cpp
 * @brief Frametop's monitor plan as data (see the header).
 */
#include "frametop_monitors_plan.h"

#include <algorithm>

namespace frametop_monitors {
  namespace {
    using json = nlohmann::json;

    const monitor_t *find(const std::vector<monitor_t> &monitors, const std::string &id) {
      return find_monitor(monitors, id);
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
      int on = 0;
      for (const auto &m : now.monitors) {
        const auto t = std::find_if(targets.begin(), targets.end(), [&](const target_t &x) {
          return x.id == m.id;
        });
        on += (t == targets.end() ? m.on : t->on) ? 1 : 0;
      }
      return on;
    }
  }  // namespace

  const monitor_t *find_monitor(const std::vector<monitor_t> &monitors, const std::string &id) {
    const auto it = std::find_if(monitors.begin(), monitors.end(), [&](const monitor_t &m) {
      return m.id == id;
    });
    return it == monitors.end() ? nullptr : &*it;
  }

  bool same_mode(const video_mode_t &a, const video_mode_t &b) {
    const auto diff = a.refresh_mhz > b.refresh_mhz ? a.refresh_mhz - b.refresh_mhz : b.refresh_mhz - a.refresh_mhz;
    return a.width == b.width && a.height == b.height && diff <= 10;
  }

  bool same_scale(double a, double b) {
    return a - b <= 0.001 && b - a <= 0.001;
  }

  namespace {
    std::optional<double> scale_from_json(const json &value) {
      if (!value.is_number()) return std::nullopt;
      const auto scale = value.get<double>();
      if (!(scale >= min_scale && scale <= max_scale)) return std::nullopt;
      return scale;
    }
  }  // namespace

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
            {"mode", m.mode ? to_json(*m.mode) : json(nullptr)}, {"modes", std::move(modes)}, {"place", m.place},
            {"sideways", m.sideways}, {"scale", m.scale ? json(*m.scale) : json(nullptr)}, {"scales", m.scales},
            {"any_mode", m.any_mode}};
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
    m.sideways = value.value("sideways", false) == true;
    m.any_mode = value.value("any_mode", false) == true;
    if (value.contains("scale")) m.scale = scale_from_json(value["scale"]);
    if (value.contains("scales") && value["scales"].is_array()) {
      for (const auto &scale : value["scales"]) {
        if (const auto parsed = scale_from_json(scale)) m.scales.push_back(*parsed);
      }
    }
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
      if (wish.contains("scale") && !wish["scale"].is_null()) {
        w.scale = scale_from_json(wish["scale"]);
        if (!w.scale) {
          if (error) *error = "a scale is a number from 0.5 to 4";
          return std::nullopt;
        }
      }
      if (!w.on) w.mode.reset(), w.scale.reset();  // an off monitor has no mode or scale to be in
      plan[id] = w;
    }
    return plan;
  }

  json to_json(const plan_t &plan) {
    json out = json::object();
    for (const auto &[id, wish] : plan) {
      out[id] = {{"on", wish.on}, {"mode", wish.mode ? to_json(*wish.mode) : json(nullptr)},
                 {"scale", wish.scale ? json(*wish.scale) : json(nullptr)}};
    }
    return out;
  }

  wish_t as_it_is(const monitor_t &m) {
    if (!m.on) return {false, std::nullopt, std::nullopt};
    return {true, m.mode, m.scale};
  }

  plan_t resolve_sizes(plan_t plan, const inventory_t &now, const std::vector<monitor_t> &saved) {
    for (auto &[id, wish] : plan) {
      if (!wish.mode || wish.mode->refresh_mhz != 0) continue;
      const auto *m = find(now.monitors, id);
      if (!m) continue;
      const auto *was = find(saved, id);
      const auto own = was && was->on ? was->mode : m->on ? m->mode : std::nullopt;
      const auto size = *wish.mode;
      const auto sized = [&](const video_mode_t &x) {
        return x.width == size.width && x.height == size.height;
      };
      if (own && sized(*own)) {
        wish.mode.reset();
        continue;
      }
      std::optional<video_mode_t> best;
      for (const auto &listed : m->modes) {
        if (!sized(listed)) continue;
        if (own && same_mode({size.width, size.height, own->refresh_mhz}, listed)) {
          best = listed;
          break;
        }
        if (!best || listed.refresh_mhz > best->refresh_mhz) best = listed;
      }
      if (best) {
        wish.mode = best;
      } else if (m->any_mode) {
        wish.mode = video_mode_t {size.width, size.height, 60000};
      }
    }
    return plan;
  }

  plan_t with_others_off(plan_t plan, const inventory_t &now) {
    for (const auto &m : now.monitors) {
      if (!plan.contains(m.id)) plan[m.id] = {false, std::nullopt};
    }
    return plan;
  }

  std::string validate(const plan_t &plan, const inventory_t &now, const std::vector<monitor_t> &going_back) {
    int on = 0;
    for (const auto &m : now.monitors) {
      const auto wish = plan.find(m.id);
      const auto *back = find(going_back, m.id);
      on += (wish != plan.end() ? wish->second.on : back ? back->on : m.on) ? 1 : 0;
    }
    for (const auto &[id, wish] : plan) {
      const auto *m = find(now.monitors, id);
      if (!m) return "There's no monitor " + id;
      if (wish.mode && wish.mode->refresh_mhz == 0) {
        return label(*m) + " can't be " + std::to_string(wish.mode->width) + "x" + std::to_string(wish.mode->height);
      }
      // Checked only against a list: Windows lists no modes for a monitor while it's off.
      if (wish.mode && !m->any_mode && !m->modes.empty() && std::none_of(m->modes.begin(), m->modes.end(), [&](const video_mode_t &listed) {
            return same_mode(listed, *wish.mode);
          })) {
        return label(*m) + " has no " + describe(*wish.mode) + " mode";
      }
      if (wish.scale && m->on && m->scales.empty()) return label(*m) + "'s scale can't be set here";
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
                           was_on ? was->place : json::object(), wish.scale});
      } else if ((wish.mode && (!m->mode || !same_mode(*m->mode, *wish.mode))) ||
                 (wish.scale && (!m->scale || !same_scale(*m->scale, *wish.scale)))) {
        targets.push_back({id, true, wish.mode, m->place, wish.scale});
      }
    }
    keep_one_on(targets, now, kept_on);
    return targets;
  }

  void keep_one_on(std::vector<target_t> &targets, const inventory_t &now, std::string *kept_on) {
    if (on_after(now, targets) > 0) return;
    // Never every monitor off: the primary monitor stays (else the first one turned off).
    auto keep = std::find_if(targets.begin(), targets.end(), [&](const target_t &t) {
      const auto *m = find(now.monitors, t.id);
      return !t.on && m && m->on && m->primary;
    });
    if (keep == targets.end()) {
      keep = std::find_if(targets.begin(), targets.end(), [&](const target_t &t) {
        const auto *m = find(now.monitors, t.id);
        return !t.on && m && m->on;
      });
    }
    if (keep != targets.end()) {
      if (kept_on) *kept_on = label(*find(now.monitors, keep->id));
      targets.erase(keep);
    }
  }

  bool still_as_set(const monitor_t &now, const wish_t &set) {
    if (now.on != set.on) return false;
    if (!set.on) return true;
    return (!set.mode || (now.mode && same_mode(*now.mode, *set.mode))) &&
           (!set.scale || (now.scale && same_scale(*now.scale, *set.scale)));
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
      } else if (!still_as_set(*m, as_it_is(*was))) {
        targets.push_back({id, true, was->mode, was->place, was->scale});
      }
    }
    return targets;
  }
}  // namespace frametop_monitors
