/**
 * @file src/platform/linux/kms_outputs.h
 * @brief The scanout size and refresh of the outputs KMS capture can open.
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace platf {
  struct kms_output_t {
    std::string name;  ///< The capture name, as display_names() lists it.
    int width {};
    int height {};
    std::uint32_t refresh_millihz {};  ///< Zero when the CRTC has no valid mode.
  };

  /// The active outputs KMS capture can open, refreshed now, in display_names() order.
  std::vector<kms_output_t> kms_outputs();

  namespace kms {
    /// A DRM mode's refresh rate in millihertz, from its pixel clock in kHz and its totals.
    /// It truncates, and ignores vscan, which modern modes leave at 0 or 1.
    constexpr std::uint32_t refresh_millihz(std::uint32_t clock_khz, std::uint32_t htotal, std::uint32_t vtotal, bool interlaced, bool double_scan) {
      if (!clock_khz || !htotal || !vtotal) return 0;
      auto millihz = static_cast<std::uint64_t>(clock_khz) * 1000000ULL / (static_cast<std::uint64_t>(htotal) * vtotal);
      if (interlaced) millihz *= 2;
      if (double_scan) millihz /= 2;
      return millihz > UINT32_MAX ? 0 : static_cast<std::uint32_t>(millihz);
    }
  }  // namespace kms
}  // namespace platf
