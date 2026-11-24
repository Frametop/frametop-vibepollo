/**
 * @file src/platform/linux/desktop_apps.cpp
 * @brief This host's installed desktop apps (see desktop_apps.h).
 */
#include "desktop_apps.h"
#include "app_window_class.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

namespace platf::hyprland_windows::catalog {
  namespace fs = std::filesystem;
  using clock = std::chrono::steady_clock;

  namespace {
    std::vector<fs::path> application_dirs() {
      std::vector<fs::path> dirs;
      const char *home = std::getenv("HOME");
      const char *data_home = std::getenv("XDG_DATA_HOME");
      if (data_home && *data_home) dirs.emplace_back(fs::path(data_home) / "applications");
      else if (home && *home) dirs.emplace_back(fs::path(home) / ".local/share/applications");
      const char *data_dirs = std::getenv("XDG_DATA_DIRS");
      std::stringstream list {data_dirs && *data_dirs ? data_dirs : "/usr/local/share:/usr/share"};
      for (std::string dir; std::getline(list, dir, ':');) {
        if (!dir.empty()) dirs.emplace_back(fs::path(dir) / "applications");
      }
      return dirs;
    }

    // The Exec line without its field codes (the desktop entry spec's %f, %u, ...).
    std::string strip_field_codes(const std::string &exec) {
      std::string out;
      for (size_t i = 0; i < exec.size(); ++i) {
        if (exec[i] != '%' || i + 1 == exec.size()) {
          out += exec[i];
          continue;
        }
        if (exec[++i] == '%') out += '%';
      }
      return out;
    }
  }  // namespace

  std::vector<desktop_app_t> desktop_apps() {
    std::vector<desktop_app_t> apps;
    std::set<std::string> seen;  // the first directory with an id wins, as the spec says
    for (const auto &dir : application_dirs()) {
      std::error_code ec;
      for (const auto &entry : fs::directory_iterator(dir, ec)) {
        const auto id = entry.path().filename().string();
        if (entry.path().extension() != ".desktop" || !seen.insert(id).second) continue;
        std::ifstream file {entry.path()};
        desktop_app_t app {id, {}, {}, {}, {}};
        bool in_entry = false, hide = false, application = false;
        for (std::string line; std::getline(file, line);) {
          if (!line.empty() && line[0] == '[') {
            in_entry = line == "[Desktop Entry]";
            continue;
          }
          if (!in_entry) continue;
          const auto eq = line.find('=');
          if (eq == std::string::npos) continue;
          const auto trim = [](std::string t) {  // "Key = value" is allowed too
            const auto first = t.find_first_not_of(" \t"), last = t.find_last_not_of(" \t\r");
            return first == std::string::npos ? std::string {} : t.substr(first, last - first + 1);
          };
          const auto key = trim(line.substr(0, eq)), value = trim(line.substr(eq + 1));
          if (key == "Name") app.name = value;
          else if (key == "Icon") app.icon = value;
          else if (key == "Exec") app.exec = strip_field_codes(value);
          else if (key == "StartupWMClass") app.wm_class = value;
          else if (key == "Type") application = value == "Application";
          else if ((key == "NoDisplay" || key == "Hidden" || key == "Terminal") && value == "true") hide = true;
        }
        if (application && !hide && !app.name.empty() && !app.exec.empty()) apps.push_back(std::move(app));
      }
    }
    std::sort(apps.begin(), apps.end(), [](const auto &a, const auto &b) { return a.name < b.name; });
    return apps;
  }

  const std::map<std::string, std::string> &app_classes() {
    static std::map<std::string, std::string> cache;
    static clock::time_point read_at;
    if (!cache.empty() && clock::now() - read_at < std::chrono::seconds(30)) return cache;
    const auto apps = desktop_apps();
    std::map<std::string, std::string> by_wm, by_stem, by_exec;
    for (const auto &a : apps) {
      if (!a.wm_class.empty()) by_wm.emplace(window_class_key(a.wm_class), a.id);
      const auto stem = a.id.size() > 8 && a.id.ends_with(".desktop") ? a.id.substr(0, a.id.size() - 8) : a.id;
      by_stem.emplace(lower(stem), a.id);
      for (const auto &cls : app_window_classes(a.wm_class, a.id, a.exec)) by_exec.emplace(cls, a.id);
    }
    cache = by_wm;
    cache.insert(by_stem.begin(), by_stem.end());
    cache.insert(by_exec.begin(), by_exec.end());
    read_at = clock::now();
    return cache;
  }
}  // namespace platf::hyprland_windows::catalog
