#pragma once

// Frametop's monitor plan on Hyprland: the broker's hypr-monitor verb turns one output off,
// or on with a mode, position, scale and transform. The caller sends only those words; the
// broker checks each and writes the Lua itself, so no caller text is ever evaluated as code.
// The C++ side (src/platform/linux/frametop_monitors_hyprland.cpp) writes the same rules.

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static inline bool hypr_monitor_digits(const char *from, const char *to, size_t most) {
  if (to <= from || (size_t) (to - from) > most) return false;
  for (const char *p = from; p < to; ++p) if (*p < '0' || *p > '9') return false;
  return true;
}

// N or N.F, N up to `whole` digits and F up to `fraction`.
static inline bool hypr_monitor_decimal(const char *from, const char *to, size_t whole, size_t fraction) {
  const char *dot = memchr(from, '.', (size_t) (to - from));
  if (!dot) return hypr_monitor_digits(from, to, whole);
  return hypr_monitor_digits(from, dot, whole) && hypr_monitor_digits(dot + 1, to, fraction);
}

static inline bool hypr_monitor_output_is_safe(const char *name) {
  if (!name || !name[0] || strlen(name) > 63) return false;
  if (!((name[0] >= 'A' && name[0] <= 'Z') || (name[0] >= 'a' && name[0] <= 'z') || (name[0] >= '0' && name[0] <= '9'))) return false;
  for (const char *p = name; *p; ++p) {
    if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') ||
          *p == '-' || *p == '_' || *p == '.')) return false;
  }
  return true;
}

// "WxH@HZ" (HZ with up to 3 decimals) or "preferred".
static inline bool hypr_monitor_mode_is_safe(const char *mode) {
  if (!mode || strlen(mode) > 32) return false;
  if (!strcmp(mode, "preferred")) return true;
  const char *end = mode + strlen(mode), *x = strchr(mode, 'x'), *at = strchr(mode, '@');
  return x && at && x < at && hypr_monitor_digits(mode, x, 5) && hypr_monitor_digits(x + 1, at, 5) &&
         hypr_monitor_decimal(at + 1, end, 4, 3);
}

static inline bool hypr_monitor_coordinate(const char *from, const char *to) {
  if (from < to && *from == '-') ++from;
  return hypr_monitor_digits(from, to, 6);
}

// "XxY" or "auto".
static inline bool hypr_monitor_position_is_safe(const char *position) {
  if (!position || strlen(position) > 32) return false;
  if (!strcmp(position, "auto")) return true;
  const char *x = strchr(position, 'x');
  return x && hypr_monitor_coordinate(position, x) && hypr_monitor_coordinate(x + 1, position + strlen(position));
}

// A decimal above zero or "auto".
static inline bool hypr_monitor_scale_is_safe(const char *scale) {
  if (!scale || strlen(scale) > 16) return false;
  if (!strcmp(scale, "auto")) return true;
  return hypr_monitor_decimal(scale, scale + strlen(scale), 2, 6) && strtod(scale, NULL) > 0;
}

static inline bool hypr_monitor_transform_is_safe(const char *transform) {
  return transform && transform[0] >= '0' && transform[0] <= '7' && !transform[1];
}

// argv: {"...", "hypr-monitor", NAME, "off"} or {"...", "hypr-monitor", NAME, MODE, POSITION, SCALE, TRANSFORM}.
static inline bool hypr_monitor_arguments_are_safe(int argc, char **argv) {
  if (!argv || argc < 4 || strcmp(argv[1], "hypr-monitor") || !hypr_monitor_output_is_safe(argv[2])) return false;
  if (argc == 4) return !strcmp(argv[3], "off");
  return argc == 7 && hypr_monitor_mode_is_safe(argv[3]) && hypr_monitor_position_is_safe(argv[4]) &&
         hypr_monitor_scale_is_safe(argv[5]) && hypr_monitor_transform_is_safe(argv[6]);
}

// The rule as Lua for `hyprctl eval`, from arguments hypr_monitor_arguments_are_safe took.
// Hyprland merges a rule into the output's earlier ones, so coming on says "disabled = false".
static inline bool hypr_monitor_lua(int argc, char **argv, char *out, size_t size) {
  if (!hypr_monitor_arguments_are_safe(argc, argv)) return false;
  int written;
  if (argc == 4) {
    written = snprintf(out, size, "hl.monitor({output = \"%s\", disabled = true})", argv[2]);
  } else {
    const bool auto_scale = !strcmp(argv[5], "auto");
    written = snprintf(out, size,
                       "hl.monitor({output = \"%s\", mode = \"%s\", position = \"%s\", scale = %s%s%s, transform = %s, disabled = false})",
                       argv[2], argv[3], argv[4], auto_scale ? "\"" : "", argv[5], auto_scale ? "\"" : "", argv[6]);
  }
  return written > 0 && (size_t) written < size;
}
