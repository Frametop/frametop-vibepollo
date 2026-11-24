/**
 * @file src/platform/linux/hypr_scale.h
 * @brief The scales Hyprland takes for a mode, and how a rule spells one.
 */
#pragma once

#include <cmath>
#include <cstdio>
#include <string>

namespace platf::hyprland_scale {
  // Hyprland takes a scale that makes a whole-pixel desktop size, as it is or at the nearest
  // 120th (the fractional scale protocol's step: 1.666667 is 200/120). Any other it refuses for
  // its default scale and says the config is wrong. This is its own check, doubles compared
  // exactly, so a scale that passes here passes there.
  inline bool whole_at(int width, int height, double scale) {
    const auto whole = [&](double by) {
      const double w = width / by, h = height / by;
      return w == std::round(w) && h == std::round(h);
    };
    return whole(scale) || whole(std::round(scale * 120.0) / 120.0);
  }

  // The asked scale if the mode takes it, else the nearest 120th it does (1 takes every mode).
  inline double fit(int width, int height, double scale) {
    if (whole_at(width, height, scale)) return scale;
    const int asked = static_cast<int>(std::lround(scale * 120));
    for (int off = 1; off < 120; ++off) {
      for (const int steps : {asked - off, asked + off}) {
        if (steps >= 60 && steps <= 480 && whole_at(width, height, steps / 120.0)) return steps / 120.0;
      }
    }
    return 1.0;
  }

  // A scale as a rule has it: "1", "1.25", "1.666667"; "auto" for one out of range.
  inline std::string text(double scale) {
    if (!(scale > 0) || scale >= 100) return "auto";
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%.6f", scale);
    std::string out = buffer;
    while (out.back() == '0') out.pop_back();
    if (out.back() == '.') out.pop_back();
    return out;
  }
}  // namespace platf::hyprland_scale
