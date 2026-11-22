/**
 * @file src/platform/linux/frametop_monitors_hyprland.h
 * @brief Frametop's monitor plan on Hyprland (0.55+, Lua config): the monitors from
 *        `j/monitors all`, and each change as an `hl.monitor` rule.
 *
 * Hyprland merges a rule into the output's earlier ones (tested on 0.56.2), so the user's own
 * settings (vrr, bit depth, colour management) stay. A rule here names mode, position, scale
 * and transform, and "disabled = false" (an output stays disabled until a rule says so).
 */
#pragma once

#include "src/frametop_monitors.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace platf::frametop_monitors_hyprland {
  using frametop_monitors::inventory_t;
  using frametop_monitors::target_t;

  /// One rule: an output turned off, or on as these say. Each field is checked text, the same
  /// words the session broker accepts (rule_is_safe) and puts in the Lua.
  struct rule_t {
    std::string output;
    bool on = true;
    std::string mode = "preferred";  ///< "WxH@HZ" (HZ with up to 3 decimals) or "preferred"
    std::string position = "auto";  ///< "XxY" or "auto"
    std::string scale = "auto";  ///< a decimal or "auto"
    int transform = 0;  ///< 0-7
  };

  /// Whether an output name is one Hyprland could have and a rule may name.
  bool output_name_is_safe(const std::string &name);
  bool rule_is_safe(const rule_t &rule);

  /// The rule as Lua for `hyprctl eval`; empty when it isn't safe.
  std::string lua_rule(const rule_t &rule);

  /// The monitors in `j/monitors all`, without the outputs `is_virtual` names (counted in
  /// other_outputs while on). nullopt when it isn't Hyprland's list.
  std::optional<inventory_t> parse_monitors(const nlohmann::json &monitors,
                                            const std::function<bool(const std::string &)> &is_virtual);

  /// The rule that makes `target` so, keeping what `now` has for whatever the target leaves out.
  rule_t rule_for(const target_t &target, const inventory_t &now);

  /// How this reaches Hyprland: its socket from the user's session, or the session broker from
  /// the machine service.
  struct link_t {
    virtual ~link_t() = default;
    /// `j/monitors all`, or nullopt.
    virtual std::optional<nlohmann::json> monitors() = 0;
    /// "" when Hyprland took the rule, else why not.
    virtual std::string apply(const rule_t &rule) = 0;
  };

  /// Outputs no monitor stands behind: Hyprland's headless and fallback ones, and Frametop
  /// windows' (FT-W1...).
  bool hyprland_made(const std::string &name);

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
}  // namespace platf::frametop_monitors_hyprland
