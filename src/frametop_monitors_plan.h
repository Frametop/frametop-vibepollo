/**
 * @file src/frametop_monitors_plan.h
 * @brief Frametop's monitor plan as data: the monitors, the plan, their JSON, and the rules
 *        that turn a plan into changes (and back). No state and no I/O: the manager
 *        (frametop_monitors.h) keeps the state, a platform_t makes the changes.
 */
#pragma once

#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace frametop_monitors {
  /// A display mode. The refresh rate is in millihertz: Windows lists whole hertz,
  /// Hyprland and KScreen fractions (59.951 Hz).
  struct video_mode_t {
    unsigned width = 0;
    unsigned height = 0;
    unsigned refresh_mhz = 0;
  };

  /// Same size, and refresh rates within 10 mHz of each other (Hyprland lists 239.76 Hz for the
  /// 239.761 it reports).
  bool same_mode(const video_mode_t &a, const video_mode_t &b);

  struct monitor_t {
    std::string id;  ///< as /api/display-devices names it: a Windows device id, a connector name
    std::string name;
    bool on = false;
    bool primary = false;
    std::optional<video_mode_t> mode;  ///< its mode while on
    std::vector<video_mode_t> modes;  ///< the modes it takes (none known while it's off on Windows)
    /// Turned a quarter: its modes are listed unrotated, so a stream of it is height x width.
    bool sideways = false;
    /// The platform's own placement (position, scale, rotation...), to put it back as it was.
    nlohmann::json place = nlohmann::json::object();
  };

  struct inventory_t {
    std::vector<monitor_t> monitors;
    /// Active outputs that aren't physical monitors (virtual displays, window outputs). They
    /// don't count as a display kept on: each goes when its stream does.
    int other_outputs = 0;
  };

  /// What one monitor should be: on or off, and on, in `mode` (none: its own or as it was).
  struct wish_t {
    bool on = true;
    std::optional<video_mode_t> mode;
  };

  using plan_t = std::map<std::string, wish_t>;

  /// One monitor made so by platform_t::apply. `place` is where it goes when it comes on (from
  /// the saved state), or empty for wherever the platform puts it.
  struct target_t {
    std::string id;
    bool on = true;
    std::optional<video_mode_t> mode;
    nlohmann::json place = nlohmann::json::object();
  };

  const monitor_t *find_monitor(const std::vector<monitor_t> &monitors, const std::string &id);

  nlohmann::json to_json(const video_mode_t &mode);
  std::optional<video_mode_t> mode_from_json(const nlohmann::json &value);
  nlohmann::json to_json(const monitor_t &monitor);
  std::optional<monitor_t> monitor_from_json(const nlohmann::json &value);

  /// {"ID": {"on": bool, "mode": {...} | null}}; nullopt with *error for anything else.
  std::optional<plan_t> plan_from_json(const nlohmann::json &value, std::string *error);
  nlohmann::json to_json(const plan_t &plan);

  /// Why the plan can't be taken ("" if it can): every monitor it names is one of these, each
  /// mode is one that monitor lists, and a physical monitor stays on. `going_back`: monitors
  /// the plan no longer names that go back as they were saved (on or off).
  std::string validate(const plan_t &plan, const inventory_t &now, const std::vector<monitor_t> &going_back = {});

  /// If `targets` would leave no physical monitor on, takes out the change that turns the
  /// primary monitor off (else the first one turned off) and names it in *kept_on.
  void keep_one_on(std::vector<target_t> &targets, const inventory_t &now, std::string *kept_on);

  /// The changes that make `now` match the plan. Monitors it names that `saved` has, coming
  /// back on, go back to their saved place and mode. If the plan would leave nothing on, the
  /// primary monitor (else the first) stays on and *kept_on names it.
  std::vector<target_t> plan_targets(const plan_t &plan, const inventory_t &now, const std::vector<monitor_t> &saved,
                                     std::string *kept_on = nullptr);

  /// Whether `now` is still as this left it (`set`): only then is it put back.
  bool still_as_set(const monitor_t &now, const wish_t &set);

  /// The changes that put each monitor in `ids` back as `saved` had it, where it's still as
  /// this left it (`set`).
  std::vector<target_t> restore_targets(const std::vector<std::string> &ids, const std::vector<monitor_t> &saved,
                                        const std::map<std::string, wish_t> &set, const inventory_t &now);
}  // namespace frametop_monitors
