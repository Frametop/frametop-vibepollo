/**
 * @file src/platform/linux/app_window_class.h
 * @brief Frametop windows: which window classes a desktop app's windows have.
 */
#pragma once

#include <algorithm>
#include <cctype>
#include <set>
#include <sstream>
#include <string>

namespace platf::hyprland_windows {
  inline std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
  }

  /**
   * The classes a desktop app's windows may have: its StartupWMClass, its desktop file's name,
   * and the name of each program on its Exec line (not options or VAR=value). A browser that's
   * already running hands a new launch to its first process, whose window then ignores the
   * launch's workspace rule; its class still says whose it is.
   */
  inline std::set<std::string> app_window_classes(const std::string &startup_wm_class, const std::string &id,
                                                  const std::string &exec) {
    std::set<std::string> out;
    if (!startup_wm_class.empty()) out.insert(lower(startup_wm_class));
    const auto stem = id.size() > 8 && id.ends_with(".desktop") ? id.substr(0, id.size() - 8) : id;
    if (!stem.empty()) out.insert(lower(stem));
    std::istringstream words {exec};
    for (std::string w; words >> w;) {
      if (w.front() == '"' || w.front() == '\'') w.erase(0, 1);
      if (!w.empty() && (w.back() == '"' || w.back() == '\'')) w.pop_back();
      if (w.empty() || w.front() == '-' || w.find('=') != std::string::npos) continue;
      const auto slash = w.rfind('/');
      out.insert(lower(slash == std::string::npos ? w : w.substr(slash + 1)));
    }
    for (const auto *launcher : {"uwsm", "uwsm-app", "app", "env", "flatpak", "run", "sh", "bash", "exec", "setsid"}) {
      out.erase(launcher);
    }
    return out;
  }
}  // namespace platf::hyprland_windows
