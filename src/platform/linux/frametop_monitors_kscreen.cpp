/**
 * @file src/platform/linux/frametop_monitors_kscreen.cpp
 * @brief Frametop's monitor plan on KDE Plasma (see the header).
 */
#include "frametop_monitors_kscreen.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <map>

namespace platf::frametop_monitors_kscreen {
  namespace {
    using json = nlohmann::json;
    using frametop_monitors::monitor_t;
    using frametop_monitors::video_mode_t;

    // KScreen serializes Output::Rotation as flags; the doctor takes names.
    const std::map<int, std::string> &rotations() {
      static const std::map<int, std::string> names {
        {1, "none"}, {2, "left"}, {4, "inverted"}, {8, "right"},
        {16, "flipped"}, {32, "flipped90"}, {64, "flipped180"}, {128, "flipped270"},
      };
      return names;
    }

    std::string text(const json &value) {
      if (value.is_string()) return value.get<std::string>();
      if (value.is_number_integer()) return std::to_string(value.get<long long>());
      return {};
    }

    bool word(const std::string &s) {
      return !s.empty() && s.size() <= 64 && std::all_of(s.begin(), s.end(), [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-';
      });
    }

    std::optional<video_mode_t> mode_of(const json &mode) {
      if (!mode.is_object()) return std::nullopt;
      const auto size = mode.value("size", json::object());
      const auto w = size.value("width", 0), h = size.value("height", 0);
      const auto rate = mode.value("refreshRate", 0.0);
      if (w <= 0 || h <= 0 || !(rate > 0) || rate > 1000) return std::nullopt;
      return video_mode_t {static_cast<unsigned>(w), static_cast<unsigned>(h), static_cast<unsigned>(std::lround(rate * 1000))};
    }

    const json *output_named(const json &configuration, const std::string &name) {
      if (!configuration.is_object() || !configuration.contains("outputs") || !configuration["outputs"].is_array()) return nullptr;
      for (const auto &output : configuration["outputs"]) {
        if (output.is_object() && output.value("name", std::string {}) == name) return &output;
      }
      return nullptr;
    }

    // The id of the output's mode that is `wanted`; the closest refresh rate where several are.
    std::optional<std::string> mode_id(const json &output, const video_mode_t &wanted) {
      std::optional<std::string> best;
      unsigned best_off = ~0u;
      for (const auto &mode : output.value("modes", json::array())) {
        const auto m = mode_of(mode);
        const auto id = text(mode.value("id", json {}));
        if (!m || !word(id) || !frametop_monitors::same_mode(*m, wanted)) continue;
        const unsigned off = m->refresh_mhz > wanted.refresh_mhz ? m->refresh_mhz - wanted.refresh_mhz : wanted.refresh_mhz - m->refresh_mhz;
        if (off < best_off) {
          best = id;
          best_off = off;
        }
      }
      return best;
    }

    std::string scale_text(double scale) {
      char buffer[32];
      std::snprintf(buffer, sizeof buffer, "%.4f", scale > 0 && scale < 100 ? scale : 1.0);
      std::string out = buffer;
      while (out.back() == '0') out.pop_back();
      if (out.back() == '.') out.pop_back();
      return out;
    }
  }  // namespace

  std::optional<inventory_t> parse_outputs(const json &configuration, const std::function<bool(const std::string &)> &is_virtual) {
    if (!configuration.is_object() || !configuration.contains("outputs") || !configuration["outputs"].is_array()) {
      return std::nullopt;
    }
    inventory_t out;
    for (const auto &output : configuration["outputs"]) {
      if (!output.is_object() || !output.value("connected", false)) continue;
      const auto name = output.value("name", std::string {});
      if (!word(name)) continue;
      const bool on = output.value("enabled", false);
      if (is_virtual && is_virtual(name)) {
        out.other_outputs += on ? 1 : 0;
        continue;
      }
      monitor_t monitor;
      monitor.id = name;
      monitor.name = name;
      monitor.on = on;
      monitor.primary = on && output.value("priority", 0) == 1;
      const auto current = text(output.value("currentModeId", json {}));
      for (const auto &mode : output.value("modes", json::array())) {
        const auto m = mode_of(mode);
        if (!m) continue;
        if (on && !current.empty() && text(mode.value("id", json {})) == current) monitor.mode = m;
        if (std::none_of(monitor.modes.begin(), monitor.modes.end(), [&](const video_mode_t &x) {
              return x.width == m->width && x.height == m->height && x.refresh_mhz == m->refresh_mhz;
            })) {
          monitor.modes.push_back(*m);
        }
      }
      if (on) {
        const auto pos = output.value("pos", json::object());
        monitor.place = {{"x", pos.value("x", 0)}, {"y", pos.value("y", 0)}, {"scale", output.value("scale", 1.0)},
                         {"rotation", output.value("rotation", 1)}, {"priority", output.value("priority", 0)}};
      }
      out.monitors.push_back(std::move(monitor));
    }
    return out;
  }

  std::vector<std::string> arguments_for(const std::vector<target_t> &targets, const json &configuration, std::string *error) {
    std::vector<std::string> on, off;
    for (const auto &t : targets) {
      const auto *output = word(t.id) ? output_named(configuration, t.id) : nullptr;
      if (!output) {
        if (error) *error = "KScreen has no output " + t.id;
        return {};
      }
      const auto prefix = "output." + t.id + ".";
      if (!t.on) {
        off.push_back(prefix + "disable");
        continue;
      }
      on.push_back(prefix + "enable");
      if (t.mode) {
        const auto id = mode_id(*output, *t.mode);
        if (!id) {
          if (error) *error = t.id + " has no mode id for that mode";
          return {};
        }
        on.push_back(prefix + "mode." + *id);
      }
      const auto &place = t.place;
      if (place.contains("x") && place["x"].is_number_integer() && place.contains("y") && place["y"].is_number_integer()) {
        on.push_back(prefix + "position." + std::to_string(place["x"].get<int>()) + "," + std::to_string(place["y"].get<int>()));
      }
      if (place.contains("scale") && place["scale"].is_number()) on.push_back(prefix + "scale." + scale_text(place["scale"].get<double>()));
      if (place.contains("rotation") && place["rotation"].is_number_integer()) {
        if (const auto r = rotations().find(place["rotation"].get<int>()); r != rotations().end()) {
          on.push_back(prefix + "rotation." + r->second);
        }
      }
      if (place.contains("priority") && place["priority"].is_number_integer() && place["priority"].get<int>() > 0) {
        on.push_back(prefix + "priority." + std::to_string(place["priority"].get<int>()));
      }
    }
    on.insert(on.end(), off.begin(), off.end());
    return on;
  }

  platform_t::platform_t(std::unique_ptr<link_t> link, std::function<bool(const std::string &)> is_virtual):
      link_ {std::move(link)},
      is_virtual_ {std::move(is_virtual)} {}

  std::string platform_t::backend() const {
    return "kscreen";
  }

  std::optional<inventory_t> platform_t::inventory() {
    const auto configuration = link_->query();
    if (!configuration) return std::nullopt;
    return parse_outputs(*configuration, is_virtual_);
  }

  std::string platform_t::apply(const std::vector<target_t> &targets, const inventory_t &) {
    if (targets.empty()) return {};
    const auto configuration = link_->query();
    if (!configuration || !configuration->contains("outputs")) return "couldn't read KScreen's outputs";
    std::string error;
    const auto arguments = arguments_for(targets, *configuration, &error);
    if (arguments.empty()) return error.empty() ? "nothing to change" : error;
    return link_->apply(arguments);
  }
}  // namespace platf::frametop_monitors_kscreen
