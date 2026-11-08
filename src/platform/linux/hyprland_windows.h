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

  /**
   * Where a released window goes instead of back where it came from: the monitor a window panel
   * was dropped on in Frametop, at u, v of the way across and down it (0 to 1), held at fx, fy of
   * the window. A floating window goes there; a tiled one tiles where the cursor is, there.
   */
  struct drop_t {
    std::string monitor;
    double u = 0.5, v = 0.5, fx = 0.5, fy = 0.5;
  };

  /**
   * Puts a window back (named by its output or its address) and removes its output: where it came
   * from, or onto `drop`'s monitor when it names a real one.
   */
  bool release(const std::string &output_or_address, std::string *error, const drop_t *drop = nullptr);

  /**
   * The window shown at u, v of the way across and down a real monitor (0 to 1):
   * {"address", "class", "title", "width", "height", "grab": [fx, fy]} (grab: where the point
   * is in it, 0 to 1), or nullopt with *error set. It doesn't move the window.
   */
  std::optional<nlohmann::json> at(const std::string &monitor, double u, double v, std::string *error);

  /** Puts every window back ("Bring windows back"). Returns how many. */
  int release_all();

  /** Starts the watcher (grace period, closed windows) and puts back windows a crash left out. */
  void start();
}  // namespace platf::hyprland_windows
