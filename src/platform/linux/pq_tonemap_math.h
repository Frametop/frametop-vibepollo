/**
 * @file src/platform/linux/pq_tonemap_math.h
 * @brief CPU reference for PqToSdr.frag: PQ BT.2020 to SDR sRGB BT.709.
 *
 * The capture path runs the shader. This header mirrors it so the transfer
 * math can be unit tested; keep the two in step.
 */
#pragma once

#include <algorithm>
#include <array>
#include <cmath>

namespace egl::pq_tonemap_math {

  using rgb_t = std::array<double, 3>;

  /** SMPTE ST 2084 EOTF: encoded [0, 1] to absolute nits. */
  [[nodiscard]] inline double pq_to_nits(double encoded) {
    constexpr double m1 = 0.1593017578125;
    constexpr double m2 = 78.84375;
    constexpr double c1 = 0.8359375;
    constexpr double c2 = 18.8515625;
    constexpr double c3 = 18.6875;
    const double p = std::pow(std::clamp(encoded, 0.0, 1.0), 1.0 / m2);
    const double ratio = std::max(p - c1, 0.0) / (c2 - c3 * p);
    return 10000.0 * std::pow(ratio, 1.0 / m1);
  }

  /** SMPTE ST 2084 inverse EOTF: absolute nits to encoded [0, 1]. */
  [[nodiscard]] inline double nits_to_pq(double nits) {
    constexpr double m1 = 0.1593017578125;
    constexpr double m2 = 78.84375;
    constexpr double c1 = 0.8359375;
    constexpr double c2 = 18.8515625;
    constexpr double c3 = 18.6875;
    const double y = std::pow(std::clamp(nits / 10000.0, 0.0, 1.0), m1);
    return std::pow((c1 + c2 * y) / (1.0 + c3 * y), m2);
  }

  [[nodiscard]] inline double srgb_encode(double linear) {
    return linear <= 0.0031308 ? linear * 12.92 : 1.055 * std::pow(linear, 1.0 / 2.4) - 0.055;
  }

  /** One pixel through the same steps as PqToSdr.frag (identity CRTC LUT). */
  [[nodiscard]] inline rgb_t tonemap(const rgb_t &encoded, double sdr_white_nits) {
    rgb_t bt2020;
    for (std::size_t i = 0; i < 3; ++i) {
      bt2020[i] = pq_to_nits(encoded[i]) / sdr_white_nits;
    }
    rgb_t rgb {
      1.6605 * bt2020[0] - 0.5876 * bt2020[1] - 0.0728 * bt2020[2],
      -0.1246 * bt2020[0] + 1.1329 * bt2020[1] - 0.0083 * bt2020[2],
      -0.0182 * bt2020[0] - 0.1006 * bt2020[1] + 1.1187 * bt2020[2],
    };
    for (auto &channel : rgb) {
      channel = std::max(channel, 0.0);
    }
    // SDR content passes through exactly; brighter highlights scale as a whole.
    const double peak = std::max({rgb[0], rgb[1], rgb[2]});
    if (peak > 1.0) {
      for (auto &channel : rgb) {
        channel /= peak;
      }
    }
    for (auto &channel : rgb) {
      channel = srgb_encode(std::clamp(channel, 0.0, 1.0));
    }
    return rgb;
  }

}  // namespace egl::pq_tonemap_math
