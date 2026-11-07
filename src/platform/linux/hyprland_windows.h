/**
 * @file src/platform/linux/hyprland_windows.h
 * @brief Frametop windows on a Hyprland desktop: one app window per headless output.
 *
 * A Vibepollo started in the user's Hyprland session (VIBEPOLLO_SESSION_BACKEND=hyprland-wlr,
 * capture = wlr) streams single windows to Frametop panels. Opening a window gives it its own
 * headless output (FT-W1, FT-W2, ...) far to the right of the real monitors, sized to the panel,
 * and moves the window there fullscreen (the compositor's fullscreen only: the app isn't told,
 * and the desktop's bar and dock stay under it). A Frametop display stream of that output is the
 * window. Releasing it puts the window back on the workspace it came from and removes the output.
 * An output no stream captures for a minute (the Frame left, or it never came) is released, and
 * so is one whose window closed.
 */
#pragma once

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <vector>

namespace platf::hyprland_windows {
  /** The session's Hyprland answers its IPC socket. */
  bool available();

  /**
   * {"windows": [...], "outputs": [...], "locked": bool}: every mapped window, the window outputs
   * open now, and whether the session is locked (window outputs show nothing until it isn't).
   */
  nlohmann::json list();

  /** Installed desktop apps a launch can name: [{"id", "name", "icon"}]. */
  nlohmann::json apps();

  /** A Hyprland output, physical or a window's, for the display list. */
  struct monitor_t {
    std::string name, description;
    int width = 0, height = 0;
    double refresh = 0;  // Hz
    bool window = false;  // a Frametop window's (FT-W1, ...)
  };

  /** Hyprland's outputs as it lists them now. */
  std::vector<monitor_t> monitor_list();

  /**
   * Gives the window (a Hyprland address, 0x...) its own output, width x height (0: the window's
   * own size). Returns {"output", "width", "height", "address"} or nullopt with *error set.
   */
  std::optional<nlohmann::json> open(const std::string &address, int width, int height, std::string *error);

  /** Starts the desktop app (an id from apps()) on a new output and waits for its window. */
  std::optional<nlohmann::json> launch(const std::string &app_id, int width, int height, std::string *error);

  /** Resizes a window output, and with it the window. */
  bool resize(const std::string &output, int width, int height, std::string *error);

  /** Puts a window back (named by its output or its address) and removes its output. */
  bool release(const std::string &output_or_address, std::string *error);

  /** Puts every window back ("Bring windows back"). Returns how many. */
  int release_all();

  /** Starts the watcher (grace period, closed windows) and puts back windows a crash left out. */
  void start();
}  // namespace platf::hyprland_windows
