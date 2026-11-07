/**
 * @file tests/unit/platform/linux/test_app_window_class.cpp
 * @brief Frametop windows: a launched app's window found by its class.
 */
#include "../../../../src/platform/linux/app_window_class.h"

#include <cstdio>

static int failed = 0;
#define CHECK(c) \
  do { \
    if (!(c)) std::printf("not so: %s\n", #c), ++failed; \
  } while (0)

int main() {
  using platf::hyprland_windows::app_window_classes;
  using S = std::set<std::string>;
  // Brave Origin: its StartupWMClass, its file's name and its program are one.
  CHECK((app_window_classes("brave-origin", "brave-origin.desktop", "brave-origin ") == S {"brave-origin"}));
  // omarchy's launchers: uwsm and its words aren't the app.
  CHECK((app_window_classes("", "Signal.desktop", "uwsm app -- /usr/bin/signal-desktop --ozone ") ==
          S {"signal", "signal-desktop"}));
  // env and its VAR=value are not the app either; a quoted program loses its quotes.
  CHECK((app_window_classes("", "x.desktop", "env FOO=1 \"/opt/x/x-bin\"") == S {"x", "x-bin"}));
  CHECK((app_window_classes("org.gnome.Calculator", "org.gnome.Calculator.desktop", "gnome-calculator") ==
          S {"org.gnome.calculator", "gnome-calculator"}));
  std::puts(failed ? "app window classes: FAILED" : "app window classes: ok");
  return failed ? 1 : 0;
}
