/**
 * @file tests/unit/platform/linux/test_window_cursor.cpp
 * @brief Frametop windows: the cursor off a window output whose stream ended.
 */
#include "../../../../src/platform/linux/window_cursor.h"

#include <cstdio>

static int failed = 0;
#define CHECK(c) \
  do { \
    if (!(c)) std::printf("not so: %s\n", #c), ++failed; \
  } while (0)

int main() {
  using platf::hyprland_windows::cursor_off;
  using platf::hyprland_windows::monitor_rect_t;
  using P = std::optional<std::pair<int, int>>;
  // oma7: HDMI above, DP-2 the ultrawide, eDP-2 at 1.25 below, FT-W1 far right.
  const std::vector<monitor_rect_t> oma7 {
    {"eDP-2", 1536, 2880, 2560, 1600, 1.25, 0, false, false},
    {"DP-2", 0, 1440, 5120, 1440, 1, 0, true, false},
    {"HDMI-A-1", 420, 0, 3440, 1440, 1, 0, false, false},
    {"FT-W1", 30000, 0, 1682, 1200, 1, 0, false, true},
  };
  // On FT-W1 when its stream ended: to the window's home monitor's middle.
  CHECK((cursor_off(30500, 600, oma7, {"FT-W1"}, "HDMI-A-1") == P {{420 + 1720, 720}}));
  // No home (a launched app): the focused monitor; eDP-2's middle is in logical pixels.
  CHECK((cursor_off(30500, 600, oma7, {"FT-W1"}, "") == P {{2560, 1440 + 720}}));
  CHECK((cursor_off(30500, 600, oma7, {"FT-W1"}, "eDP-2") == P {{1536 + 1024, 2880 + 640}}));
  // A home that is another window output, or gone: the focused one.
  CHECK((cursor_off(30500, 600, oma7, {"FT-W1"}, "FT-W2") == P {{2560, 1440 + 720}}));
  // The cursor on a real monitor, or on a window output whose stream still runs: it stays.
  CHECK((cursor_off(100, 2000, oma7, {"FT-W1"}, "") == P {}));
  CHECK((cursor_off(30500, 600, oma7, {"FT-W2"}, "") == P {}));
  // Its right edge isn't on it.
  CHECK((cursor_off(31682, 600, oma7, {"FT-W1"}, "") == P {}));
  // Only window outputs: nowhere to go.
  CHECK((cursor_off(30500, 600, {oma7[3]}, {"FT-W1"}, "") == P {}));
  // A monitor turned a quarter swaps its size.
  const std::vector<monitor_rect_t> turned {{"DP-1", 0, 0, 2560, 1440, 1, 1, true, false},
                                            {"FT-W1", 30000, 0, 800, 600, 1, 0, false, true}};
  CHECK((cursor_off(30100, 100, turned, {"FT-W1"}, "") == P {{720, 1280}}));
  std::puts(failed ? "window cursor: FAILED" : "window cursor: ok");
  return failed ? 1 : 0;
}
