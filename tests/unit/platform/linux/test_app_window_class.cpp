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
  using platf::hyprland_windows::window_class_key;
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
  // omarchy's web apps: the file names Chrome's class, the window has its own browser's.
  const auto discord = app_window_classes("chrome-discord.com__channels_@me-Default", "Discord.desktop",
                                          "omarchy-launch-webapp https://discord.com/channels/@me");
  CHECK(discord.contains(window_class_key("brave-discord.com__channels_@me-Default")));
  CHECK(discord.contains(window_class_key("chrome-discord.com__channels_@me-Default")));
  CHECK(discord.contains(window_class_key("msedge-discord.com__channels_@me-Default")));
  // ... but another site's, another profile's, or the browser's own window isn't it.
  CHECK(!discord.contains(window_class_key("brave-web.whatsapp.com__-Default")));
  CHECK(!discord.contains(window_class_key("brave-discord.com__channels_@me-Profile_1")));
  CHECK(!discord.contains(window_class_key("brave-origin")));
  // A URL that ends in its slash adds no empty class (a window without a class isn't the app's).
  const auto whatsapp = app_window_classes("chrome-web.whatsapp.com__-Default", "WhatsApp.desktop",
                                           "omarchy-launch-webapp https://web.whatsapp.com/");
  CHECK(whatsapp.contains(window_class_key("brave-web.whatsapp.com__-Default")));
  CHECK(!whatsapp.contains(window_class_key("")));
  // A host with a dash keeps it; classes that aren't a web app's only lose their case.
  CHECK(window_class_key("brave-my-site.com__-Default") == window_class_key("chrome-my-site.com__-Default"));
  CHECK(window_class_key("brave-my-site.com__-Default") != window_class_key("chrome-other-site.com__-Default"));
  for (const auto *plain : {"Signal", "brave-origin", "org.gnome.Calculator", "com.t3tools.T3Code", "a-b__c",
                            "-x.y__z", "", "FT-Keyboard"}) {
    CHECK(window_class_key(plain) == platf::hyprland_windows::lower(plain));
  }
  std::puts(failed ? "app window classes: FAILED" : "app window classes: ok");
  return failed ? 1 : 0;
}
