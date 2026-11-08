/**
 * @file src/platform/linux/window_pick.h
 * @brief Frametop windows: which window a point on a monitor shows, and where a dropped one goes.
 *
 * Frametop pulls a window off a display panel by the point its pointer holds (Super, the wheel
 * pulled), and drops a window panel back onto a display panel at the point it's let go.
 */
#pragma once

#include "window_cursor.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

namespace platf::hyprland_windows {
  // A mapped window in Hyprland's layout: at and size in layout coordinates.
  struct client_rect_t {
    std::string address;
    int x = 0, y = 0, width = 0, height = 0;
    int workspace = 0;  // its workspace's id
    bool floating = false;
    bool fullscreen = false;  // Hyprland's fullscreen (2), over everything on its monitor
    int focus = 0;  // focusHistoryID: 0 was focused last
  };

  // The layout point u, v of the way across and down a monitor (0 to 1, from its top left).
  inline std::pair<int, int> monitor_point(const monitor_rect_t &m, double u, double v) {
    const auto [w, h] = logical_size(m);
    const auto at = [](double f, int size) {
      return std::clamp(int(std::floor(std::clamp(f, 0.0, 1.0) * size)), 0, std::max(0, size - 1));
    };
    return {m.x + at(u, w), m.y + at(v, h)};
  }

  // The window at layout point x, y, of the workspaces a monitor shows (`shown`: its special
  // workspace first when one is open, drawn over the other). On top: a fullscreen window, then
  // floating ones, then tiled ones, and among equals the one focused last. Null: none there.
  inline const client_rect_t *window_at(const std::vector<client_rect_t> &clients, const std::vector<int> &shown, int x, int y) {
    for (const int ws : shown) {
      const client_rect_t *best = nullptr;
      int best_rank = 3;
      for (const auto &c : clients) {
        if (c.workspace != ws || c.width <= 0 || c.height <= 0) continue;
        if (x < c.x || x >= c.x + c.width || y < c.y || y >= c.y + c.height) continue;
        const int rank = c.fullscreen ? 0 : c.floating ? 1 : 2;
        if (rank < best_rank || (rank == best_rank && c.focus < best->focus)) best = &c, best_rank = rank;
      }
      if (best) return best;
    }
    return nullptr;
  }

  // Where a floating window w x h dropped at layout point x, y goes, held at fx, fy of it (0 to 1,
  // from its top left): its top left, moved so all of it is on the monitor where it fits.
  inline std::pair<int, int> drop_origin(const monitor_rect_t &m, int x, int y, double fx, double fy, int w, int h) {
    const auto [mw, mh] = logical_size(m);
    const int ox = x - int(std::lround(std::clamp(fx, 0.0, 1.0) * w));
    const int oy = y - int(std::lround(std::clamp(fy, 0.0, 1.0) * h));
    return {std::clamp(ox, m.x, std::max(m.x, m.x + mw - w)), std::clamp(oy, m.y, std::max(m.y, m.y + mh - h))};
  }
}  // namespace platf::hyprland_windows
