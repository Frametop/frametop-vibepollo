/**
 * @file src/platform/linux/frametop_monitors_hyprland.cpp
 * @brief Frametop's monitor plan on Hyprland (see the header).
 */
#include "frametop_monitors_hyprland.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>

namespace platf::frametop_monitors_hyprland {
  namespace {
    using json = nlohmann::json;
    using frametop_monitors::monitor_t;
    using frametop_monitors::video_mode_t;

    bool digits(const std::string &s, std::size_t from, std::size_t to, std::size_t most) {
      if (to <= from || to - from > most) return false;
      return std::all_of(s.begin() + from, s.begin() + to, [](char c) {
        return c >= '0' && c <= '9';
      });
    }

    // N or N.F, N up to `whole` digits and F up to `fraction`.
    bool decimal(const std::string &s, std::size_t from, std::size_t to, std::size_t whole, std::size_t fraction) {
      const auto dot = s.find('.', from);
      if (dot == std::string::npos || dot >= to) return digits(s, from, to, whole);
      return digits(s, from, dot, whole) && digits(s, dot + 1, to, fraction);
    }

    bool mode_is_safe(const std::string &mode) {
      if (mode == "preferred") return true;
      const auto x = mode.find('x'), at = mode.find('@');
      return x != std::string::npos && at != std::string::npos && x < at && digits(mode, 0, x, 5) &&
             digits(mode, x + 1, at, 5) && decimal(mode, at + 1, mode.size(), 4, 3);
    }

    bool coordinate(const std::string &s, std::size_t from, std::size_t to) {
      if (from < to && s[from] == '-') ++from;
      return digits(s, from, to, 6);
    }

    bool position_is_safe(const std::string &position) {
      if (position == "auto") return true;
      const auto x = position.find('x');
      return x != std::string::npos && coordinate(position, 0, x) && coordinate(position, x + 1, position.size());
    }

    bool scale_is_safe(const std::string &scale) {
      return scale == "auto" || (decimal(scale, 0, scale.size(), 2, 6) && std::strtod(scale.c_str(), nullptr) > 0);
    }

    std::string hz(unsigned refresh_mhz) {
      std::string out = std::to_string(refresh_mhz / 1000);
      if (const auto part = refresh_mhz % 1000) {
        char buffer[8];
        std::snprintf(buffer, sizeof buffer, ".%03u", part);
        std::string frac = buffer;
        while (frac.back() == '0') frac.pop_back();
        out += frac;
      }
      return out;
    }

    std::string mode_text(const video_mode_t &mode) {
      return std::to_string(mode.width) + "x" + std::to_string(mode.height) + "@" + hz(mode.refresh_mhz);
    }

    std::string scale_text(double scale) {
      if (!(scale > 0) || scale >= 100) return "auto";
      char buffer[32];
      std::snprintf(buffer, sizeof buffer, "%.6f", scale);
      std::string out = buffer;
      while (out.back() == '0') out.pop_back();
      if (out.back() == '.') out.pop_back();
      return out;
    }

    // "2560x1600@240.00Hz"
    std::optional<video_mode_t> parse_mode(const std::string &text) {
      unsigned w = 0, h = 0;
      double rate = 0;
      if (std::sscanf(text.c_str(), "%ux%u@%lfHz", &w, &h, &rate) != 3 || !w || !h || !(rate > 0) || rate > 1000) {
        return std::nullopt;
      }
      return video_mode_t {w, h, static_cast<unsigned>(std::lround(rate * 1000))};
    }

    const monitor_t *find(const inventory_t &now, const std::string &id) {
      const auto it = std::find_if(now.monitors.begin(), now.monitors.end(), [&](const monitor_t &m) {
        return m.id == id;
      });
      return it == now.monitors.end() ? nullptr : &*it;
    }

    bool matches(const target_t &t, const inventory_t &now) {
      const auto *m = find(now, t.id);
      if (!m) return true;  // gone: nothing to wait for
      if (m->on != t.on) return false;
      return !t.on || !t.mode || (m->mode && frametop_monitors::same_mode(*m->mode, *t.mode));
    }
  }  // namespace

  bool output_name_is_safe(const std::string &name) {
    if (name.empty() || name.size() > 63 || !std::isalnum(static_cast<unsigned char>(name[0]))) return false;
    return std::all_of(name.begin(), name.end(), [](char c) {
      return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.';
    });
  }

  bool rule_is_safe(const rule_t &rule) {
    return output_name_is_safe(rule.output) &&
           (!rule.on || (mode_is_safe(rule.mode) && position_is_safe(rule.position) && scale_is_safe(rule.scale) &&
                         rule.transform >= 0 && rule.transform <= 7));
  }

  std::string lua_rule(const rule_t &rule) {
    if (!rule_is_safe(rule)) return {};
    // Every field is checked text without quotes or backslashes (rule_is_safe). Hyprland merges
    // a rule into the output's earlier ones, so "disabled = false" has to be said to undo one.
    if (!rule.on) return "hl.monitor({output = \"" + rule.output + "\", disabled = true})";
    const auto scale = rule.scale == "auto" ? std::string {"\"auto\""} : rule.scale;
    return "hl.monitor({output = \"" + rule.output + "\", mode = \"" + rule.mode + "\", position = \"" + rule.position +
           "\", scale = " + scale + ", transform = " + std::to_string(rule.transform) + ", disabled = false})";
  }

  bool hyprland_made(const std::string &name) {
    return name.rfind("HEADLESS-", 0) == 0 || name.rfind("FALLBACK", 0) == 0 || name.rfind("FT-W", 0) == 0;
  }

  std::optional<inventory_t> parse_monitors(const json &monitors, const std::function<bool(const std::string &)> &is_virtual) {
    if (!monitors.is_array()) return std::nullopt;
    inventory_t out;
    for (const auto &m : monitors) {
      if (!m.is_object() || !m.contains("name") || !m["name"].is_string()) return std::nullopt;
      const auto name = m["name"].get<std::string>();
      const bool on = !m.value("disabled", false);
      if (hyprland_made(name) || (is_virtual && is_virtual(name))) {
        out.other_outputs += on ? 1 : 0;
        continue;
      }
      monitor_t monitor;
      monitor.id = name;
      monitor.name = m.value("description", std::string {});
      if (monitor.name.empty()) monitor.name = name;
      monitor.on = on;
      if (m.contains("availableModes") && m["availableModes"].is_array()) {
        for (const auto &text : m["availableModes"]) {
          if (!text.is_string()) continue;
          if (const auto mode = parse_mode(text.get<std::string>());
              mode && std::none_of(monitor.modes.begin(), monitor.modes.end(), [&](const video_mode_t &x) {
                return frametop_monitors::same_mode(x, *mode) && x.refresh_mhz == mode->refresh_mhz;
              })) {
            monitor.modes.push_back(*mode);
          }
        }
      }
      if (on) {
        const auto w = m.value("width", 0), h = m.value("height", 0);
        const auto rate = m.value("refreshRate", 0.0);
        if (w > 0 && h > 0) {
          monitor.mode = video_mode_t {static_cast<unsigned>(w), static_cast<unsigned>(h),
                                       static_cast<unsigned>(std::lround(std::max(0.0, rate) * 1000))};
        }
        monitor.place = {{"x", m.value("x", 0)}, {"y", m.value("y", 0)}, {"scale", m.value("scale", 1.0)},
                         {"transform", m.value("transform", 0)}};
      }
      out.monitors.push_back(std::move(monitor));
    }
    return out;
  }

  rule_t rule_for(const target_t &target, const inventory_t &now) {
    rule_t rule;
    rule.output = target.id;
    rule.on = target.on;
    if (!target.on) return rule;
    const auto *m = find(now, target.id);
    if (target.mode) {
      rule.mode = mode_text(*target.mode);
    } else if (m && m->on && m->mode) {
      rule.mode = mode_text(*m->mode);
    }
    const auto &place = !target.place.empty() ? target.place : m && m->on ? m->place : json::object();
    if (place.contains("x") && place["x"].is_number_integer() && place.contains("y") && place["y"].is_number_integer()) {
      rule.position = std::to_string(place["x"].get<int>()) + "x" + std::to_string(place["y"].get<int>());
    }
    if (place.contains("scale") && place["scale"].is_number()) rule.scale = scale_text(place["scale"].get<double>());
    if (place.contains("transform") && place["transform"].is_number_integer()) {
      rule.transform = std::clamp(place["transform"].get<int>(), 0, 7);
    }
    return rule;
  }

  platform_t::platform_t(std::unique_ptr<link_t> link, std::function<bool(const std::string &)> is_virtual):
      link_ {std::move(link)},
      is_virtual_ {std::move(is_virtual)} {}

  std::string platform_t::backend() const {
    return "hyprland";
  }

  std::optional<inventory_t> platform_t::inventory() {
    const auto monitors = link_->monitors();
    if (!monitors) return std::nullopt;
    return parse_monitors(*monitors, is_virtual_);
  }

  std::string platform_t::apply(const std::vector<target_t> &targets, const inventory_t &now) {
    std::vector<target_t> ordered = targets;
    std::stable_partition(ordered.begin(), ordered.end(), [](const target_t &t) {
      return t.on;
    });
    for (const auto &t : ordered) {
      const auto rule = rule_for(t, now);
      if (!rule_is_safe(rule)) return "Hyprland can't take a rule for " + t.id;
      if (const auto error = link_->apply(rule); !error.empty()) return t.id + ": " + error;
    }
    // Hyprland takes a rule at once and changes the output on its next frame.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds {3};
    while (std::chrono::steady_clock::now() < deadline) {
      const auto after = inventory();
      if (after && std::all_of(ordered.begin(), ordered.end(), [&](const target_t &t) {
            return matches(t, *after);
          })) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds {100});
    }
    return {};
  }
}  // namespace platf::frametop_monitors_hyprland
