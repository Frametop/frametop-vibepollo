/**
 * @file src/frametop_monitors_service.h
 * @brief The host's one Frametop monitor plan (frametop_monitors::manager_t), on this
 *        platform's monitors, kept in frametop_monitors.json beside the state files.
 */
#pragma once

#include "src/frametop_monitors.h"

#include <memory>
#include <nlohmann/json.hpp>
#include <string>

namespace platf {
  /// This platform's monitors (Windows, Hyprland, KDE Plasma); nullptr where there is no way
  /// to change them.
  std::unique_ptr<frametop_monitors::platform_t> make_frametop_monitors();
}  // namespace platf

namespace frametop_monitors::service {
  /// At startup: puts back what a previous run left changed, trying again for a while if the
  /// desktop isn't there yet.
  void start();

  /// A Frametop stream's hold on the plan; nullptr where monitors can't be changed.
  std::shared_ptr<void> hold();

  /// GET /api/frametop/monitors: {"status": true, "backend", "monitors": [...], ...}, or
  /// {"status": false, "error"} where monitors can't be changed.
  nlohmann::json state();

  /// POST /api/frametop/monitors: "" when the plan was taken, else why not.
  std::string set_plan(const nlohmann::json &body);
}  // namespace frametop_monitors::service
