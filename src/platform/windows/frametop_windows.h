/**
 * @file src/platform/windows/frametop_windows.h
 * @brief Frametop windows on a Windows desktop: one app window per Frametop panel (DEE-348).
 *
 * The Windows side of the window role the Hyprland one (src/platform/linux/hyprland_windows.h)
 * has, with the same calls, so /api/frametop/windows answers alike on both. Opening a window
 * names it as an output of its own, "window:0x<HWND>" (frametop_window_target.h), sizes it to
 * the panel and keeps it where it is on its monitor. A Frametop display stream of that output
 * captures the window alone with Windows Graphics Capture, and its input goes to the window's
 * rectangle, raising the window first. Releasing it puts the window back as it was. An output
 * no stream captures for a minute (the Frame left, or it never came) is released, and so is
 * one whose window closed.
 */
#pragma once

#include <nlohmann/json.hpp>

#include <optional>
#include <string>

namespace platf::frametop_windows {
  /** The interactive desktop has windows to list (a user is signed in). */
  bool available();

  /**
   * {"windows": [...], "outputs": [...], "locked": bool}: every app window (as Alt+Tab shows
   * them), the window outputs open now, and whether the session is locked.
   */
  nlohmann::json list();

  /** Start menu apps a launch can name: [{"id", "name", "icon"}]. */
  nlohmann::json apps();

  /**
   * Makes the window (its address, 0x...) an output of its own, its capture width x height
   * (0: the window's own size; at most its monitor's work area). Returns {"output", "width",
   * "height", "address"} or nullopt with *error set.
   */
  std::optional<nlohmann::json> open(const std::string &address, int width, int height, std::string *error);

  /** Starts a Start menu app (an id from apps()) and opens its first new window. */
  std::optional<nlohmann::json> launch(const std::string &app_id, int width, int height, std::string *error);

  /** Resizes a window output's window: its capture restarts at the new size. */
  bool resize(const std::string &output, int width, int height, std::string *error);

  /**
   * Where a released window goes instead of back where it came from: the monitor (a device id
   * or \\.\DISPLAYn) a window panel was dropped on in Frametop, at u, v of the way across and
   * down it (0 to 1), held at fx, fy of the window.
   */
  struct drop_t {
    std::string monitor;
    double u = 0.5, v = 0.5, fx = 0.5, fy = 0.5;
  };

  /** Puts a window back (named by its output or its address): as it was, or onto `drop`'s monitor. */
  bool release(const std::string &output_or_address, std::string *error, const drop_t *drop = nullptr);

  /**
   * The window shown at u, v of the way across and down a monitor (0 to 1): {"address",
   * "class", "title", "width", "height", "grab": [fx, fy]}, or nullopt with *error set.
   */
  std::optional<nlohmann::json> at(const std::string &monitor, double u, double v, std::string *error);

  /** Puts every window back ("Bring windows back"). Returns how many. */
  int release_all();

  /** Starts the watcher (grace period, closed windows) and puts back windows a crash left out. */
  void start();

  /** A window output open now (a display-role launch may stream it). */
  bool hosted(const std::string &output);

  /**
   * Before input goes to a window output's stream: raises its window to the front, so a click
   * lands on it and keys go to it. Does nothing for other outputs.
   */
  void raise_for_input(const std::string &output);
}  // namespace platf::frametop_windows
