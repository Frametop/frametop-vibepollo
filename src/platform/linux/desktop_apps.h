/**
 * @file src/platform/linux/desktop_apps.h
 * @brief This host's installed desktop apps, from the XDG desktop entries a launch can name.
 *
 * Read from $XDG_DATA_HOME/applications and each $XDG_DATA_DIRS one; the first directory with an
 * id wins, as the spec says. Hidden, NoDisplay and Terminal entries are left out.
 */
#pragma once

#include <map>
#include <string>
#include <vector>

namespace platf::hyprland_windows::catalog {
  struct desktop_app_t {
    std::string id, name, icon, exec, wm_class;
  };

  /** The installed apps, sorted by name. Read fresh on every call. */
  std::vector<desktop_app_t> desktop_apps();

  /**
   * Window class (window_class_key) -> the desktop app it belongs to. A StartupWMClass says it best,
   * then the desktop file's name, then a program on the Exec line. Read again every 30 s.
   */
  const std::map<std::string, std::string> &app_classes();
}  // namespace platf::hyprland_windows::catalog
