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
  /// Hyprland and KScreen fractions (59.951 Hz). In a wish, a refresh of 0 asks for a size:
  /// resolve_sizes picks the mode.
  struct video_mode_t {
    unsigned width = 0;
    unsigned height = 0;
    unsigned refresh_mhz = 0;
  };

  /// Same size, and refresh rates within 10 mHz of each other (Hyprland lists 239.76 Hz for the
  /// 239.761 it reports).
  bool same_mode(const video_mode_t &a, const video_mode_t &b);

  /// Scales within 0.001 of each other (Hyprland reports 1.25 as 1.250000).
  bool same_scale(double a, double b);

  /// The scales a plan may ask for: 50% to 400%.
  constexpr double min_scale = 0.5;
  constexpr double max_scale = 4.0;

  struct monitor_t {
    std::string id;  ///< as /api/display-devices names it: a Windows device id, a connector name
    std::string name;
    bool on = false;
    bool primary = false;
    std::optional<video_mode_t> mode;  ///< its mode while on
    std::vector<video_mode_t> modes;  ///< the modes it takes (none known while it's off on Windows)
    /// Turned a quarter: its modes are listed unrotated, so a stream of it is height x width.
    bool sideways = false;
    /// Its scale while on, where the platform can set it (Hyprland, KScreen); none on Windows.
    std::optional<double> scale;
    /// The scales it takes in its mode now (none: its scale can't be set here, or it's off).
    std::vector<double> scales;
    /// It takes a mode it doesn't list too (Hyprland makes a custom mode for any size).
    bool any_mode = false;
    /// The platform's own placement (position, scale, rotation...), to put it back as it was.
    nlohmann::json place = nlohmann::json::object();
  };

  struct inventory_t {
    std::vector<monitor_t> monitors;
    /// Active outputs that aren't physical monitors (virtual displays, window outputs). They
    /// don't count as a display kept on: each goes when its stream does.
    int other_outputs = 0;
  };

  /// What one monitor should be: on or off, and on, in `mode` and at `scale` (none: its own,
  /// as it was).
  struct wish_t {
    bool on = true;
    std::optional<video_mode_t> mode;
    std::optional<double> scale;
  };

  using plan_t = std::map<std::string, wish_t>;

  /// One monitor made so by platform_t::apply. `place` is where it goes when it comes on (from
  /// the saved state), or empty for wherever the platform puts it.
  struct target_t {
    std::string id;
    bool on = true;
    std::optional<video_mode_t> mode;
    nlohmann::json place = nlohmann::json::object();
    std::optional<double> scale;  ///< its scale, over the one in `place` (none: place's)
  };

  const monitor_t *find_monitor(const std::vector<monitor_t> &monitors, const std::string &id);

  nlohmann::json to_json(const video_mode_t &mode);
  std::optional<video_mode_t> mode_from_json(const nlohmann::json &value);
  nlohmann::json to_json(const monitor_t &monitor);
  std::optional<monitor_t> monitor_from_json(const nlohmann::json &value);

  /// {"ID": {"on": bool, "mode": {...} | null, "scale": number | null}}; nullopt with *error
  /// for anything else.
  std::optional<plan_t> plan_from_json(const nlohmann::json &value, std::string *error);
  nlohmann::json to_json(const plan_t &plan);

  /// What it is now, as a wish: on in its mode and scale, or off.
  wish_t as_it_is(const monitor_t &m);

  /// The plan with each size it asks for (a mode with refresh 0) made a mode: none (its own)
  /// when it's the size of its own mode (as `saved` has it, else as it is), else a mode it lists
  /// in that size (its own refresh if it has it, else the fastest), else, where it takes any
  /// mode, that size at 60 Hz. A size it can't take stays as asked, for validate to refuse.
  plan_t resolve_sizes(plan_t plan, const inventory_t &now, const std::vector<monitor_t> &saved);

  /// The plan with every monitor of `now` it doesn't name turned off (a plan with others_off:
  /// Frametop's Remote Displays name only the monitors that have a display there).
  plan_t with_others_off(plan_t plan, const inventory_t &now);

  /// Why the plan can't be taken ("" if it can): every monitor it names is one of these, each
  /// mode is one that monitor lists (if it lists any and takes no other), each size is
  /// resolved (resolve_sizes), a scale is asked only of one on that
  /// lists scales (or off: it's tried when it comes on), and a physical monitor stays on. `going_back`: monitors
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

  /// The targets that `after` (the monitors once `targets` were applied) doesn't have: a monitor
  /// asked into a mode that is on in another size. A desktop may take a mode and fall back to
  /// another when the monitor or its driver refuses it (Hyprland: a custom mode taller than a
  /// laptop's panel, or any custom mode on NVIDIA).
  std::vector<target_t> not_taken(const std::vector<target_t> &targets, const inventory_t &after);

  /// The sizes (width and height) each monitor didn't go into when asked, by monitor id.
  using refused_t = std::map<std::string, std::vector<video_mode_t>>;

  /// The plan with each mode of a size its monitor refused taken out, so it stays in its own
  /// mode and isn't asked again. *said: "NAME can't be WxH" for the first one ("" none).
  plan_t without_refused(plan_t plan, const inventory_t &now, const refused_t &refused, std::string *said = nullptr);

  /// Whether `now` is still as this left it (`set`): only then is it put back.
  bool still_as_set(const monitor_t &now, const wish_t &set);

  /// The changes that put each monitor in `ids` back as `saved` had it, where it's still as
  /// this left it (`set`).
  std::vector<target_t> restore_targets(const std::vector<std::string> &ids, const std::vector<monitor_t> &saved,
                                        const std::map<std::string, wish_t> &set, const inventory_t &now);
}  // namespace frametop_monitors
