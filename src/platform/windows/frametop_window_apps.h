/**
 * @file src/platform/windows/frametop_window_apps.h
 * @brief The apps a Frametop window panel can launch (frametop_windows.cpp): the Start menu's
 * shortcuts and the Store apps no shortcut names, each read on a thread of its own now and then,
 * so neither the window thread nor its lock waits on them.
 */
#pragma once

#include <optional>
#include <string>
#include <vector>

namespace platf::frametop_windows::catalog {
  struct app_t {
    std::string id, name;
    std::wstring link;  // the .lnk
    std::wstring exe;  // its target, lower case, if it's an .exe
  };

  // Reads the shortcuts again in the background when they're more than a minute old.
  void refresh_shortcuts();
  // The shortcuts read last (refreshing them when they're old), waiting up to 8 s for the
  // first read when `wait`. Never under the window role's lock with `wait`.
  std::vector<app_t> shortcut_apps(bool wait);

  // A Store app's id: an AUMID (Family_publisher!App).
  bool is_aumid(const std::string &id);
  // Lists Store apps again in the background, when they're older than ten minutes.
  void refresh_store_apps();
  bool has_store_apps();
  // Store apps listed (or tried) once, waiting up to 8 s for the first list.
  void await_store_apps();
  bool store_app(const std::string &id);

  // Shortcuts, then Store apps no shortcut has the name of. Doesn't wait: callers that want
  // the first lists await them first.
  std::vector<app_t> all_apps();
  // The id of the app whose shortcut starts `exe` (lower case), if one does.
  std::optional<std::string> app_of(const std::vector<app_t> &apps, const std::wstring &exe);
}  // namespace platf::frametop_windows::catalog
