/**
 * @file src/platform/linux/frametop_monitors_kscreen.h
 * @brief Frametop's monitor plan on KDE Plasma: the monitors from `kscreen-doctor -j`, and each
 *        change as kscreen-doctor arguments (the ones the session broker's display-apply takes).
 *
 * KScreen saves what it applies as the desktop's configuration, so the plan's journal is what
 * puts a monitor back after a crash; a reboot alone doesn't.
 */
#pragma once

#include "src/frametop_monitors.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace platf::frametop_monitors_kscreen {
  using frametop_monitors::inventory_t;
  using frametop_monitors::target_t;

  /// The connected monitors in `kscreen-doctor -j`, without the outputs `is_virtual` names
  /// (counted in other_outputs while on). nullopt when it isn't KScreen's answer.
  std::optional<inventory_t> parse_outputs(const nlohmann::json &configuration,
                                           const std::function<bool(const std::string &)> &is_virtual);

  /// kscreen-doctor arguments that make every target so, those coming on first; empty with
  /// *error when one can't be (no such output, or no mode id for the mode).
  std::vector<std::string> arguments_for(const std::vector<target_t> &targets, const nlohmann::json &configuration,
                                         std::string *error);

  /// How this reaches KScreen: kscreen-doctor directly, or through the session broker.
  struct link_t {
    virtual ~link_t() = default;
    virtual std::optional<nlohmann::json> query() = 0;
    /// "" when applied, else why not.
    virtual std::string apply(const std::vector<std::string> &arguments) = 0;
  };

  class platform_t final: public frametop_monitors::platform_t {
  public:
    platform_t(std::unique_ptr<link_t> link, std::function<bool(const std::string &)> is_virtual);
    std::string backend() const override;
    std::optional<inventory_t> inventory() override;
    std::string apply(const std::vector<target_t> &targets, const inventory_t &now) override;

  private:
    std::unique_ptr<link_t> link_;
    std::function<bool(const std::string &)> is_virtual_;
  };
}  // namespace platf::frametop_monitors_kscreen
