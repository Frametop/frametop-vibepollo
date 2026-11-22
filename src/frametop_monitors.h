/**
 * @file src/frametop_monitors.h
 * @brief Frametop: the host's physical monitors, turned on or off and set to a mode from
 *        Frametop's Remote Displays page, for as long as Frametop streams from this host.
 *
 * Frametop keeps a plan here: for each monitor it names, on or off, and the mode it should be
 * in. The plan applies while at least one Frametop stream runs (each holds a lease), and for a
 * grace period after the last one ends; then every monitor this changed goes back to how it
 * was, unless someone has changed it since. Before the first change, every monitor's state is
 * saved through the store, so a restart after a crash puts them back too (start()).
 *
 * The operating system's side is an interface (platform_t), so the component tests drive the
 * bookkeeping with a fake one; the store and the grace timer are injected the same way.
 */
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
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

  /// Same size, and refresh rates within half a hertz of each other.
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

  struct platform_t {
    virtual ~platform_t() = default;
    /// "windows", "hyprland", "kscreen".
    virtual std::string backend() const = 0;
    /// Every physical monitor, on or off; nullopt when they can't be read now.
    virtual std::optional<inventory_t> inventory() = 0;
    /// Makes each target so, leaving every other monitor as it is. Monitors come on before any
    /// go off, so the desktop never runs out of outputs. "" when done, else why not.
    virtual std::string apply(const std::vector<target_t> &targets, const inventory_t &now) = 0;
  };

  /// Where the plan and the saved state are kept between runs.
  struct store_t {
    virtual ~store_t() = default;
    virtual std::optional<nlohmann::json> load() = 0;
    virtual void save(const nlohmann::json &state) = 0;
  };

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

  class manager_t {
  public:
    using schedule_fn = std::function<void(std::chrono::milliseconds, std::function<void()>)>;
    using log_fn = std::function<void(const std::string &)>;

    /// After the last stream ends, the plan holds this long, so a reconnect doesn't flash the
    /// monitors back on.
    static constexpr std::chrono::milliseconds default_grace {std::chrono::minutes {2}};
    /// After a stream ends while others go on, the monitors are checked again this long after
    /// (Windows can bring its saved layout back when a virtual display goes).
    static constexpr std::chrono::milliseconds recheck_after {std::chrono::seconds {3}};

    manager_t(platform_t &platform, store_t &store, schedule_fn schedule, std::chrono::milliseconds grace = default_grace,
              log_fn log = {});

    /// Reads the store: the plan, and puts back what a previous run left changed. False while
    /// that couldn't be done yet (the caller may try again).
    bool start();

    /// A Frametop stream: the plan applies while any hold lives, and for the grace after.
    std::shared_ptr<void> hold();

    /// What a stream of the existing display `id` does with the plan.
    enum class display_use_e {
      refuse,  ///< the plan turns it off: no stream of it while Frametop uses this host
      hold_first,  ///< off now, and the plan turns it on: hold before looking it up
      hold_after,  ///< anything else: hold once it's found, so a display that isn't there
                   ///< (a retrying panel) never applies the plan
    };
    display_use_e display_use(const std::string &id);

    /// Replaces the plan ({"monitors": {...}}); it applies now if a stream holds it. "" when
    /// taken, else why not (nothing changed).
    std::string set_plan(const nlohmann::json &body);

    /// The monitors as they are, the plan, and whether it applies now.
    nlohmann::json state();

    int holders() const;

  private:
    void log(const std::string &line) const;
    void release();
    /// After the grace, puts the monitors back unless a stream came (or another call moved
    /// the generation); tries again while that fails.
    void restore_later_locked(std::uint64_t generation);
    void save_locked();
    /// Applies the plan to the monitors as they are now; snapshots them first.
    void apply_locked();
    /// Puts back every monitor this changed; false if some couldn't be (kept for later).
    bool restore_all_locked();

    platform_t &platform_;
    store_t &store_;
    schedule_fn schedule_;
    std::chrono::milliseconds grace_;
    log_fn log_;

    mutable std::mutex mutex_;
    plan_t plan_;
    std::vector<monitor_t> saved_;  // before the first change; empty when nothing is changed
    std::map<std::string, wish_t> set_;  // what this made each changed monitor
    int holders_ = 0;
    bool loaded_ = false;  // the store read (start())
    bool held_ = false;  // a stream came since this started: it owns what's changed
    std::uint64_t generation_ = 0;  // a pending grace or recheck is stale once this moves
    std::string last_error_;
  };
}  // namespace frametop_monitors
