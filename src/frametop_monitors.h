/**
 * @file src/frametop_monitors.h
 * @brief Frametop: the host's physical monitors, turned on or off and set to a mode from
 *        Frametop's Remote Displays page, for as long as Frametop streams one of its displays.
 *
 * Frametop keeps a plan here: for each monitor it names, on or off, and the mode it should be
 * in. The plan applies while at least one Frametop display stream runs (each holds a lease), and for a
 * grace period after the last one ends; then every monitor this changed goes back to how it
 * was, unless someone has changed it since. Before the first change, every monitor's state is
 * saved through the store, so a restart after a crash puts them back too (start()).
 *
 * The operating system's side is an interface (platform_t), so the component tests drive the
 * bookkeeping with a fake one; the store and the grace timer are injected the same way.
 */
#pragma once

#include "frametop_monitors_plan.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>

namespace frametop_monitors {
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

  class manager_t {
  public:
    using schedule_fn = std::function<void(std::chrono::milliseconds, std::function<void()>)>;
    using log_fn = std::function<void(const std::string &)>;

    /// After the last stream ends, the plan holds this long, so a reconnect doesn't flash the
    /// monitors back on. Frametop lets go of the plan itself when its user does (an empty plan),
    /// so this is for a stream that was lost: one that comes back later applies the plan again.
    static constexpr std::chrono::milliseconds default_grace {std::chrono::seconds {15}};
    /// After a stream ends while others go on, the monitors are checked again this long after
    /// (Windows can bring its saved layout back when a virtual display goes).
    static constexpr std::chrono::milliseconds recheck_after {std::chrono::seconds {3}};

    manager_t(platform_t &platform, store_t &store, schedule_fn schedule, std::chrono::milliseconds grace = default_grace,
              log_fn log = {});

    /// Reads the store: the plan, and puts back what a previous run left changed. False while
    /// that couldn't be done yet (the caller may try again).
    bool start();

    /// A Frametop display stream: the plan applies while any hold lives, and for the grace after.
    std::shared_ptr<void> hold();

    /// At a clean shutdown, once the streams are gone: puts every monitor back now, not a
    /// grace later (start() puts back what this couldn't). Called once nothing can take a hold
    /// any more (main.cpp: after the HTTP and RTSP threads end); a hold after it applies again.
    void shut_down();

    /// What a stream of the existing display `id` does with the plan.
    enum class display_use_e {
      refuse,  ///< the plan turns it off (and it isn't the one kept on): no stream of it while
               ///< Frametop uses this host
      hold_first,  ///< off now, and the plan turns it on: hold before looking it up
      hold_after,  ///< one of its monitors, or the monitors couldn't be read: hold once it's
                   ///< found, so a display that isn't there (a retrying panel) never applies the plan
      none,  ///< not one of its monitors (a window's output, a virtual display): no hold
    };
    display_use_e display_use(const std::string &id);

    /// Replaces the plan ({"monitors": {...}, "others_off": bool}: with others_off, every
    /// monitor it doesn't name goes off too); it applies now if a stream holds it. "" when
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
    /// What a change cut short by a crash got to: a pending target the monitor is now in, or one
    /// it's partly in (no longer as saved), counts as made, so it goes back; the rest as never made.
    void settle_pending_locked(const inventory_t &now);
    /// The changes that make `now` match `plan` (plan_for), with monitors it no longer names going
    /// back; *kept_on names a monitor that stays on because the desktop needs one.
    std::vector<target_t> targets_locked(const plan_t &plan, const inventory_t &now, std::string *kept_on) const;
    /// The plan for the monitors `now` has: with others_off_, the ones it doesn't name are off,
    /// and a size its monitor refused (refused_) is its own mode. *said names the first of those.
    plan_t plan_for(const inventory_t &now, std::string *said = nullptr) const;
    /// Applies the plan to the monitors as they are now; snapshots them first. A monitor that
    /// didn't go into the size asked is put back as it was, and that size isn't asked of it again.
    void apply_locked();
    /// One pass of apply_locked; true when a monitor refused a size in it (now in refused_).
    bool apply_once_locked();
    /// Puts back every monitor this changed; false if some couldn't be (kept for later).
    bool restore_all_locked();

    platform_t &platform_;
    store_t &store_;
    schedule_fn schedule_;
    std::chrono::milliseconds grace_;
    log_fn log_;

    mutable std::mutex mutex_;
    plan_t plan_;
    bool others_off_ = false;  // monitors the plan doesn't name go off too
    std::vector<monitor_t> before_;  // every monitor as it was when nothing was changed yet, for saved_'s places
    std::vector<monitor_t> saved_;  // before the first change; empty when nothing is changed
    std::map<std::string, wish_t> set_;  // what this made each changed monitor
    std::map<std::string, wish_t> pending_;  // what a change under way is making them (in the store, for a crash)
    refused_t refused_;  // the sizes a monitor didn't go into, this run: not asked of it again
    int holders_ = 0;
    bool loaded_ = false;  // the store read (start())
    bool held_ = false;  // a stream came since this started: it owns what's changed
    std::uint64_t generation_ = 0;  // a pending grace or recheck is stale once this moves
    std::string last_error_;
  };
}  // namespace frametop_monitors
