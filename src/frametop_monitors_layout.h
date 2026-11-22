/**
 * @file src/frametop_monitors_layout.h
 * @brief Frametop monitors on Windows: the main display and the places to put back after a
 *        change. No Windows calls, so the component tests check it on any system.
 */
#pragma once

#include "frametop_monitors_plan.h"

#include <map>
#include <string>
#include <vector>

namespace frametop_monitors {
  struct point_t {
    int x = 0;
    int y = 0;
  };

  /// The active displays: each one's place (the main display is at 0,0) and the main display.
  struct layout_t {
    std::map<std::string, point_t> places;
    std::string primary;
  };

  /// What to put back once `targets` are made so, from the layout just before them. A monitor
  /// coming back on goes where it was saved. One that was the main display is the main display
  /// again, and then no places are put back: the others' were noted around another main
  /// display, so they'd overlap. Windows' saved layout (or its own arrangement) places them.
  inline layout_t layout_to_put_back(layout_t before, const std::vector<target_t> &targets) {
    bool main_back = false;
    for (const auto &t : targets) {
      if (!t.on || !t.place.is_object()) continue;
      if (t.place.contains("x") && t.place.contains("y") && t.place["x"].is_number_integer() && t.place["y"].is_number_integer()) {
        before.places[t.id] = {t.place["x"].get<int>(), t.place["y"].get<int>()};
      }
      if (t.place.value("primary", false) == true && before.primary != t.id) {
        before.primary = t.id;
        main_back = true;
      }
    }
    if (main_back) before.places.clear();
    return before;
  }
}  // namespace frametop_monitors
