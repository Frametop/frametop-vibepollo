/**
 * @file tests/unit/platform/linux/test_hypr_scale.cpp
 * @brief The scales Hyprland takes for a mode.
 */
#include "../../../../src/platform/linux/hypr_scale.h"

#include <cstdio>

static int failed = 0;
#define CHECK(c) \
  do { \
    if (!(c)) std::printf("not so: %s\n", #c), ++failed; \
  } while (0)

int main() {
  using platf::hyprland_scale::fit;
  using platf::hyprland_scale::text;
  using platf::hyprland_scale::whole_at;
  CHECK(whole_at(1280, 800, 1));
  CHECK(whole_at(1280, 800, 1.25));
  CHECK(!whole_at(1280, 800, 1.5));  // 853.33 wide: Hyprland refuses it
  CHECK(whole_at(1278, 804, 1.5));  // what Frametop makes the panel for 150%
  CHECK(fit(1280, 800, 1.6) == 1.6);
  CHECK(fit(1280, 800, 1.5) == 1.6);  // the nearest it takes
  CHECK(fit(1278, 804, 1.5) == 1.5);
  CHECK(fit(1281, 801, 2) == 1 || whole_at(1281, 801, fit(1281, 801, 2)));  // always one it takes
  CHECK(text(1) == "1");
  CHECK(text(1.25) == "1.25");
  CHECK(text(200 / 120.0) == "1.666667");
  CHECK(text(0) == "auto");
  if (!failed) std::puts("ok");
  return failed != 0;
}
