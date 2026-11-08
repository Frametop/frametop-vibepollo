/**
 * @file tests/unit/platform/linux/test_window_pick.cpp
 * @brief Frametop windows: the window at a point on a monitor, and where a dropped one goes.
 */
#include "../../../../src/platform/linux/window_pick.h"

#include <cstdio>

static int failed = 0;
#define CHECK(c) \
  do { \
    if (!(c)) std::printf("not so: %s\n", #c), ++failed; \
  } while (0)

int main() {
  using namespace platf::hyprland_windows;
  using P = std::pair<int, int>;
  const monitor_rect_t dp2 {"DP-2", 0, 1440, 5120, 1440, 1, 0, true, false};
  const monitor_rect_t edp {"eDP-2", 1536, 2880, 2560, 1600, 1.25, 0, false, false};

  // Fractions of a monitor, in layout coordinates; eDP-2 at 1.25 is 2048x1280 there.
  CHECK((monitor_point(dp2, 0, 0) == P {0, 1440}));
  CHECK((monitor_point(dp2, 0.5, 0.5) == P {2560, 2160}));
  CHECK((monitor_point(dp2, 1, 1) == P {5119, 2879}));  // the last pixel, not past it
  CHECK((monitor_point(edp, 0.5, 0.5) == P {1536 + 1024, 2880 + 640}));
  CHECK((monitor_point(dp2, -1, 2) == P {0, 2879}));

  // Two tiled windows side by side on workspace 1, a floating one over both, workspace 2 hidden.
  const std::vector<client_rect_t> ws {
    {"0xa", 0, 1440, 2560, 1440, 1, false, false, 3},
    {"0xb", 2560, 1440, 2560, 1440, 1, false, false, 1},
    {"0xf", 2000, 1600, 1200, 800, 1, true, false, 5},
    {"0xh", 0, 1440, 5120, 1440, 2, false, false, 0},
  };
  const auto at = [&](std::vector<int> shown, int x, int y) {
    const auto *c = window_at(ws, shown, x, y);
    return c ? c->address : std::string("-");
  };
  CHECK(at({1}, 100, 1500) == "0xa");
  CHECK(at({1}, 5000, 2800) == "0xb");
  CHECK(at({1}, 2100, 1700) == "0xf");  // floating over tiled, though focused longer ago
  CHECK(at({1}, 2560, 1500) == "0xb");  // the left edge is its own
  CHECK(at({2}, 100, 1500) == "0xh");
  CHECK(at({3}, 100, 1500) == "-");     // no window on the shown workspace
  CHECK(at({1}, 100, 100) == "-");      // above the monitor
  // A special workspace shown over workspace 1: its window first, and through its gaps the other.
  auto special = ws;
  special.push_back({"0xs", 1000, 1500, 1000, 600, -98, false, false, 4});
  const auto *s = window_at(special, {-98, 1}, 1500, 1800);
  CHECK(s && s->address == "0xs");
  s = window_at(special, {-98, 1}, 4000, 1800);
  CHECK(s && s->address == "0xb");
  // A fullscreen window covers the floating one; of two floating ones, the one focused last.
  auto full = ws;
  full.push_back({"0xF", 0, 1440, 5120, 1440, 1, false, true, 9});
  s = window_at(full, {1}, 2100, 1700);
  CHECK(s && s->address == "0xF");
  auto two = ws;
  two.push_back({"0xg", 2100, 1650, 400, 300, 1, true, false, 2});
  s = window_at(two, {1}, 2200, 1700);
  CHECK(s && s->address == "0xg");

  // Dropped at the middle of DP-2, held by its title bar's middle: centred under that point.
  CHECK((drop_origin(dp2, 2560, 2160, 0.5, 0.02, 1000, 600) == P {2060, 2148}));
  // Near a corner: all of it on the monitor.
  CHECK((drop_origin(dp2, 10, 2870, 0.5, 0.5, 1000, 600) == P {0, 2280}));
  // Bigger than the monitor: its top left at the monitor's.
  CHECK((drop_origin(edp, 2000, 3000, 0.5, 0.5, 3000, 2000) == P {1536, 2880}));
  std::puts(failed ? "window pick: FAILED" : "window pick: ok");
  return failed ? 1 : 0;
}
