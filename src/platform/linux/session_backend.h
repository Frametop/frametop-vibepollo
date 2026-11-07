/** @file Display capability boundary selected by the trusted machine session. */
#pragma once

#include <cstdlib>
#include <cstring>

namespace platf::linux_display {
  inline bool hyprland_kms_session() {
    const auto *machine = std::getenv("VIBEPOLLO_MACHINE_HOST");
    const auto *backend = std::getenv("VIBEPOLLO_SESSION_BACKEND");
    return machine && !std::strcmp(machine, "1") && backend && !std::strcmp(backend, "hyprland-kms");
  }

  // Frametop windows: a Vibepollo in the user's Hyprland session itself, capturing
  // with wlr-screencopy, whose displays are Hyprland's outputs (headless ones too).
  inline bool hyprland_wlr_session() {
    // Never the machine service: it runs outside the session, without its Wayland display.
    const auto *machine = std::getenv("VIBEPOLLO_MACHINE_HOST");
    const auto *backend = std::getenv("VIBEPOLLO_SESSION_BACKEND");
    return !(machine && !std::strcmp(machine, "1")) && backend && !std::strcmp(backend, "hyprland-wlr");
  }
}  // namespace platf::linux_display
