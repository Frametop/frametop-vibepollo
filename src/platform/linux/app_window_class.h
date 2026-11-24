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
   * A window class as classes are compared: in lower case, and a web app's (a Chromium browser's
   * --app window, BROWSER-HOST__PATH-PROFILE) without its browser. A desktop file says
   * chrome-discord.com__..., and Brave's window of it is brave-discord.com__...
   */
  inline std::string window_class_key(const std::string &window_class) {
    auto key = lower(window_class);
    const auto dash = key.find('-');
    if (dash == std::string::npos || dash == 0) return key;
    if (!std::all_of(key.begin(), key.begin() + dash, [](unsigned char c) { return std::isalpha(c); })) return key;
    const auto host_end = key.find("__", dash + 1);
    if (host_end == std::string::npos) return key;
    if (key.substr(dash + 1, host_end - dash - 1).find('.') == std::string::npos) return key;  // no host's name
    return "web-app" + key.substr(dash);
  }

  /**
   * The classes (window_class_key) a desktop app's windows may have: its StartupWMClass, its desktop file's name,
   * and the name of each program on its Exec line (not options or VAR=value). A browser that's
   * already running hands a new launch to its first process, whose window then ignores the
   * launch's workspace rule; its class still says whose it is.
   */
  inline std::set<std::string> app_window_classes(const std::string &startup_wm_class, const std::string &id,
                                                  const std::string &exec) {
    std::set<std::string> out;
    if (!startup_wm_class.empty()) out.insert(window_class_key(startup_wm_class));
    const auto stem = id.size() > 8 && id.ends_with(".desktop") ? id.substr(0, id.size() - 8) : id;
    if (!stem.empty()) out.insert(lower(stem));
    std::istringstream words {exec};
    for (std::string w; words >> w;) {
      if (w.front() == '"' || w.front() == '\'') w.erase(0, 1);
      if (!w.empty() && (w.back() == '"' || w.back() == '\'')) w.pop_back();
      if (w.empty() || w.front() == '-' || w.find('=') != std::string::npos) continue;
      const auto slash = w.rfind('/');
      const auto name = slash == std::string::npos ? w : w.substr(slash + 1);
      if (!name.empty()) out.insert(lower(name));  // a web app's URL may end in its slash
    }
    for (const auto *launcher : {"uwsm", "uwsm-app", "app", "env", "flatpak", "run", "sh", "bash", "exec", "setsid"}) {
      out.erase(launcher);
    }
    return out;
  }
}  // namespace platf::hyprland_windows
