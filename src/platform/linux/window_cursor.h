/**
 * @file src/platform/linux/window_cursor.h
 * @brief Frametop windows: the cursor off a window output whose stream ended.
 */
#pragma once

#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace platf::hyprland_windows {
  // A monitor in Hyprland's layout: x, y in layout coordinates, the mode in pixels.
  struct monitor_rect_t {
    std::string name;
    int x = 0, y = 0, width = 0, height = 0;
    double scale = 1;
    int transform = 0;  // wl_output: 1, 3, 5 and 7 turn it a quarter
    bool focused = false;
    bool window_output = false;  // one of ours (FT-W*), nobody sees it on the PC
  };

  inline std::pair<int, int> logical_size(const monitor_rect_t &m) {
    const double s = m.scale > 0 ? m.scale : 1;
    const int w = int(m.width / s), h = int(m.height / s);
    return m.transform % 2 ? std::pair {h, w} : std::pair {w, h};
  }

  // A window output's stream ended while the PC's cursor was on it: with no viewer, the mouse on
  // the PC moves a cursor nobody sees until the window goes back. Where it goes instead, the
  // middle of a monitor on the PC: `home` (the window's monitor before), else the focused one,
  // else the first. Nothing when the cursor is on none of `ended` or the PC has no monitor.
  inline std::optional<std::pair<int, int>> cursor_off(int cx, int cy, const std::vector<monitor_rect_t> &monitors,
                                                       const std::set<std::string> &ended, const std::string &home) {
    bool on_ended = false;
    for (const auto &m : monitors) {
      const auto [w, h] = logical_size(m);
      if (ended.contains(m.name) && cx >= m.x && cx < m.x + w && cy >= m.y && cy < m.y + h) on_ended = true;
    }
    if (!on_ended) return std::nullopt;
    const monitor_rect_t *to = nullptr;
    for (const auto &m : monitors) {
      if (m.window_output) continue;
      if (m.name == home) {
        to = &m;
        break;
      }
      if (!to || (m.focused && !to->focused)) to = &m;
    }
    if (!to) return std::nullopt;
    const auto [w, h] = logical_size(*to);
    return std::pair {to->x + w / 2, to->y + h / 2};
  }
}  // namespace platf::hyprland_windows
