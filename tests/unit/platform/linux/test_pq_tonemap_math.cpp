/** Transfer math of the PQ to SDR tone map (CPU mirror of PqToSdr.frag). */
#include "../../../tests_common.h"
#include "src/platform/linux/pq_tonemap_math.h"

namespace math = egl::pq_tonemap_math;

namespace {
  math::rgb_t grey(double encoded) {
    return {encoded, encoded, encoded};
  }

  double srgb_decode(double encoded) {
    return encoded <= 0.04045 ? encoded / 12.92 : std::pow((encoded + 0.055) / 1.055, 2.4);
  }
}  // namespace

TEST(PqTonemapMath, PqCurveRoundTrips) {
  for (const double nits : {0.0, 0.1, 1.0, 100.0, 203.0, 290.0, 1000.0, 10000.0}) {
    EXPECT_NEAR(math::pq_to_nits(math::nits_to_pq(nits)), nits, nits * 1e-6 + 1e-6);
  }
  EXPECT_NEAR(math::nits_to_pq(10000.0), 1.0, 1e-9);
}

TEST(PqTonemapMath, SdrWhiteMapsToWhite) {
  for (const double channel : math::tonemap(grey(math::nits_to_pq(290.0)), 290.0)) {
    EXPECT_NEAR(channel, 1.0, 1e-3);
  }
}

TEST(PqTonemapMath, BlackStaysBlack) {
  for (const double channel : math::tonemap(grey(0.0), 290.0)) {
    EXPECT_NEAR(channel, 0.0, 1e-9);
  }
}

TEST(PqTonemapMath, SdrGreysRoundTrip) {
  // sRGB greys the compositor showed with 290-nit SDR white.
  for (const double encoded : {0.05, 0.25, 0.5, 0.75, 0.95}) {
    const double nits = srgb_decode(encoded) * 290.0;
    for (const double channel : math::tonemap(grey(math::nits_to_pq(nits)), 290.0)) {
      EXPECT_NEAR(channel, encoded, 2e-3) << encoded;
    }
  }
}

TEST(PqTonemapMath, HighlightsStopAtWhite) {
  for (const double nits : {300.0, 1000.0, 10000.0}) {
    for (const double channel : math::tonemap(grey(math::nits_to_pq(nits)), 290.0)) {
      EXPECT_NEAR(channel, 1.0, 1e-3) << nits;
    }
  }
}

TEST(PqTonemapMath, SaturatedColourKeepsItsHue) {
  // A BT.2020-encoded red highlight past SDR white: channels scale together.
  const double r = math::nits_to_pq(1000.0 * 0.6274);
  const double g = math::nits_to_pq(1000.0 * 0.0691);
  const double b = math::nits_to_pq(1000.0 * 0.0164);
  const auto rgb = math::tonemap({r, g, b}, 290.0);
  EXPECT_GT(rgb[0], 0.9);
  EXPECT_LT(rgb[1], 0.1);
  EXPECT_LT(rgb[2], 0.1);
}
