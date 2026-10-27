/**
 * @file tests/unit/platform/linux/test_kmsgrab_selection.cpp
 * @brief Tests for stable Linux KMS output naming and selection.
 */
#include "../../../tests_common.h"

#include <cstddef>
#include <limits>
#include <src/platform/linux/kms_outputs.h>
#include <src/platform/linux/kmsgrab_pacing.h>
#include <src/platform/linux/kmsgrab_selection.h>
#include <src/platform/linux/output_layout.h>
#include <vibeshine_drm_uapi.h>

namespace selection = platf::kms::selection;

TEST(KmsgrabSelection, DerivesModeRefreshFromPixelClock) {
  using platf::kms::refresh_millihz;

  EXPECT_EQ(refresh_millihz(148500, 2200, 1125, false, false), 60000U);  // 1080p60
  EXPECT_EQ(refresh_millihz(148352, 2200, 1125, false, false), 59940U);  // 1080p59.94
  EXPECT_EQ(refresh_millihz(74250, 2200, 1125, true, false), 60000U);  // 1080i: two fields per frame
  EXPECT_EQ(refresh_millihz(50350, 800, 525, false, true), 59940U);  // double scan halves it
  EXPECT_EQ(refresh_millihz(0, 2200, 1125, false, false), 0U);
  EXPECT_EQ(refresh_millihz(148500, 0, 1125, false, false), 0U);
  EXPECT_EQ(refresh_millihz(148500, 2200, 0, false, false), 0U);
}

TEST(KmsgrabSelection, RejectsInconsistentPresentationResponses) {
  using platf::kms::pacing::classify_response;
  using response_e = platf::kms::pacing::presentation_response_e;

  EXPECT_EQ(classify_response(VIBESHINE_DRM_PRESENT_CHANGED, VIBESHINE_DRM_PRESENT_CHANGED, VIBESHINE_DRM_PRESENT_TIMEOUT, VIBESHINE_DRM_PRESENT_PENDING, 5, 6), response_e::changed);
  EXPECT_EQ(classify_response(VIBESHINE_DRM_PRESENT_CHANGED, VIBESHINE_DRM_PRESENT_CHANGED, VIBESHINE_DRM_PRESENT_TIMEOUT, VIBESHINE_DRM_PRESENT_PENDING, 5, 12), response_e::changed);
  EXPECT_EQ(classify_response(VIBESHINE_DRM_PRESENT_CHANGED | VIBESHINE_DRM_PRESENT_PENDING, VIBESHINE_DRM_PRESENT_CHANGED, VIBESHINE_DRM_PRESENT_TIMEOUT, VIBESHINE_DRM_PRESENT_PENDING, 5, 6), response_e::changed);
  EXPECT_EQ(classify_response(VIBESHINE_DRM_PRESENT_TIMEOUT, VIBESHINE_DRM_PRESENT_CHANGED, VIBESHINE_DRM_PRESENT_TIMEOUT, VIBESHINE_DRM_PRESENT_PENDING, 5, 5), response_e::timeout);
  EXPECT_EQ(classify_response(VIBESHINE_DRM_PRESENT_TIMEOUT | VIBESHINE_DRM_PRESENT_PENDING, VIBESHINE_DRM_PRESENT_CHANGED, VIBESHINE_DRM_PRESENT_TIMEOUT, VIBESHINE_DRM_PRESENT_PENDING, 5, 5), response_e::timeout);

  EXPECT_EQ(classify_response(0, VIBESHINE_DRM_PRESENT_CHANGED, VIBESHINE_DRM_PRESENT_TIMEOUT, VIBESHINE_DRM_PRESENT_PENDING, 5, 5), response_e::invalid);
  EXPECT_EQ(classify_response(VIBESHINE_DRM_PRESENT_PENDING, VIBESHINE_DRM_PRESENT_CHANGED, VIBESHINE_DRM_PRESENT_TIMEOUT, VIBESHINE_DRM_PRESENT_PENDING, 5, 5), response_e::invalid);
  EXPECT_EQ(classify_response(VIBESHINE_DRM_PRESENT_CHANGED | VIBESHINE_DRM_PRESENT_TIMEOUT, VIBESHINE_DRM_PRESENT_CHANGED, VIBESHINE_DRM_PRESENT_TIMEOUT, VIBESHINE_DRM_PRESENT_PENDING, 5, 6), response_e::invalid);
  EXPECT_EQ(classify_response(1U << 31, VIBESHINE_DRM_PRESENT_CHANGED, VIBESHINE_DRM_PRESENT_TIMEOUT, VIBESHINE_DRM_PRESENT_PENDING, 5, 6), response_e::invalid);
  EXPECT_EQ(classify_response(VIBESHINE_DRM_PRESENT_CHANGED, VIBESHINE_DRM_PRESENT_CHANGED, VIBESHINE_DRM_PRESENT_TIMEOUT, VIBESHINE_DRM_PRESENT_PENDING, 5, 5), response_e::invalid);
  EXPECT_EQ(classify_response(VIBESHINE_DRM_PRESENT_CHANGED, VIBESHINE_DRM_PRESENT_CHANGED, VIBESHINE_DRM_PRESENT_TIMEOUT, VIBESHINE_DRM_PRESENT_PENDING, 5, 4), response_e::invalid);
  EXPECT_EQ(classify_response(VIBESHINE_DRM_PRESENT_TIMEOUT, VIBESHINE_DRM_PRESENT_CHANGED, VIBESHINE_DRM_PRESENT_TIMEOUT, VIBESHINE_DRM_PRESENT_PENDING, 5, 6), response_e::invalid);
}

TEST(KmsgrabSelection, BoundsPresentationIoctlErrorHandling) {
  using error_e = platf::kms::pacing::presentation_ioctl_error_e;
  using platf::kms::pacing::classify_ioctl_error;

  EXPECT_EQ(classify_ioctl_error(EINTR, true), error_e::retry);
  EXPECT_EQ(classify_ioctl_error(EAGAIN, true), error_e::retry);
  EXPECT_EQ(classify_ioctl_error(EBUSY, true), error_e::transient_timeout);
  EXPECT_EQ(classify_ioctl_error(ENOTTY, true), error_e::unsupported);

  EXPECT_EQ(classify_ioctl_error(EINTR, false), error_e::unsupported);
  EXPECT_EQ(classify_ioctl_error(EAGAIN, false), error_e::unsupported);
  EXPECT_EQ(classify_ioctl_error(EBUSY, false), error_e::unsupported);
}

TEST(KmsgrabSelection, RetainsCaptureAcrossPendingCommitTransitions) {
  using namespace std::chrono_literals;
  using response_e = platf::kms::pacing::presentation_response_e;
  using clock_t = platf::kms::pacing::clock_t;

  platf::kms::pacing::presentation_latch_t latch;
  const auto start = clock_t::time_point {1s};

  latch.observe_response(response_e::changed, true, start);
  EXPECT_TRUE(latch.state_pending());
  EXPECT_TRUE(latch.capture_ready());
  EXPECT_FALSE(latch.pending_timed_out(start + 99ms, 100ms));
  EXPECT_TRUE(latch.pending_timed_out(start + 100ms, 100ms));

  latch.observe_response(response_e::timeout, false, start + 100ms);
  EXPECT_FALSE(latch.state_pending());
  EXPECT_TRUE(latch.capture_ready());

  latch.mark_delivered(latch.capture_generation());
  EXPECT_FALSE(latch.capture_ready());
}

TEST(KmsgrabSelection, CompletedPendingChangesRefreshTheirHangDeadline) {
  using namespace std::chrono_literals;
  using response_e = platf::kms::pacing::presentation_response_e;
  using clock_t = platf::kms::pacing::clock_t;

  platf::kms::pacing::presentation_latch_t latch;
  const auto start = clock_t::time_point {1s};

  for (int elapsed_ms = 0; elapsed_ms <= 96; elapsed_ms += 16) {
    latch.observe_response(response_e::changed, true, start + std::chrono::milliseconds {elapsed_ms});
  }
  EXPECT_FALSE(latch.pending_timed_out(start + 195ms, 100ms));
  EXPECT_TRUE(latch.pending_timed_out(start + 196ms, 100ms));
}

TEST(KmsgrabSelection, PendingPresentationUsesAHangDeadlineInsteadOfTheOldFallbackWindow) {
  using namespace std::chrono_literals;
  using response_e = platf::kms::pacing::presentation_response_e;
  using clock_t = platf::kms::pacing::clock_t;

  platf::kms::pacing::presentation_latch_t latch;
  const auto start = clock_t::time_point {1s};
  latch.observe_response(response_e::changed, true, start);

  EXPECT_FALSE(latch.pending_timed_out(start + 500ms, platf::kms::pacing::PRESENT_PENDING_HANG_TIMEOUT));
  EXPECT_TRUE(latch.pending_timed_out(start + 5s, platf::kms::pacing::PRESENT_PENDING_HANG_TIMEOUT));
}

TEST(KmsgrabSelection, RequiredPresentationModeNeverEnablesFixedRateFallback) {
  platf::kms::pacing::presentation_mode_t required_mode {true};
  EXPECT_FALSE(required_mode.event_capture_enabled());
  EXPECT_FALSE(required_mode.fixed_rate_allowed());

  required_mode.activate();
  EXPECT_TRUE(required_mode.event_capture_enabled());
  EXPECT_FALSE(required_mode.fixed_rate_allowed());

  required_mode.deactivate();
  EXPECT_FALSE(required_mode.event_capture_enabled());
  EXPECT_FALSE(required_mode.fixed_rate_allowed());

  platf::kms::pacing::presentation_mode_t ordinary_kms;
  EXPECT_FALSE(ordinary_kms.event_capture_enabled());
  EXPECT_TRUE(ordinary_kms.fixed_rate_allowed());
}

TEST(KmsgrabSelection, UsesTheFractionalClientFrameInterval) {
  using namespace std::chrono_literals;

  EXPECT_EQ(platf::kms::pacing::interval_from_frame_rate(60'000, 1'001), 16'683'333ns);
  EXPECT_EQ(platf::kms::pacing::interval_from_frame_rate(120'000, 1'001), 8'341'666ns);
  EXPECT_EQ(platf::kms::pacing::interval_from_frame_rate(120, 1), 8'333'333ns);
  EXPECT_EQ(platf::kms::pacing::interval_from_frame_rate(0, 1), 0ns);
  EXPECT_EQ(platf::kms::pacing::interval_from_frame_rate(120, 0), 0ns);
}

TEST(KmsgrabSelection, UnevenVrrPresentationsSpendOneFrameOfPhaseCredit) {
  using namespace std::chrono_literals;
  using clock_t = platf::kms::pacing::clock_t;

  platf::kms::pacing::presentation_rate_limiter_t limiter;
  limiter.set_interval(10ms);

  const auto first = clock_t::time_point {1s};
  EXPECT_EQ(limiter.next_delivery(first), first);
  limiter.mark_delivered(first);
  // The short half can arrive first. The initial one-frame credit preserves it.
  EXPECT_EQ(limiter.next_delivery(first + 4ms), first + 4ms);
  limiter.mark_delivered(first + 4ms);

  // The following long half replenishes both its own token and enough phase
  // credit for the next short half. Every source presentation remains immediate.
  EXPECT_EQ(limiter.next_delivery(first + 20ms), first + 20ms);
  limiter.mark_delivered(first + 20ms);
  EXPECT_EQ(limiter.next_delivery(first + 24ms), first + 24ms);
  limiter.mark_delivered(first + 24ms);
  EXPECT_EQ(limiter.next_delivery(first + 40ms), first + 40ms);

  // The reverse phase (long half first) is source-locked as well.
  limiter.reset();
  limiter.mark_delivered(first);
  EXPECT_EQ(limiter.next_delivery(first + 16ms), first + 16ms);
  limiter.mark_delivered(first + 16ms);
  EXPECT_EQ(limiter.next_delivery(first + 20ms), first + 20ms);
}

TEST(KmsgrabSelection, StartupPhaseCreditIsBoundedToOneFrame) {
  using namespace std::chrono_literals;
  using clock_t = platf::kms::pacing::clock_t;

  platf::kms::pacing::presentation_rate_limiter_t limiter;
  limiter.set_interval(10ms);

  const auto first = clock_t::time_point {1s};
  limiter.mark_delivered(first);
  EXPECT_EQ(limiter.next_delivery(first), first);
  limiter.mark_delivered(first);
  EXPECT_EQ(limiter.next_delivery(first), first + 10ms);
}

TEST(KmsgrabSelection, ReportsCreditAndDeadlineWithoutChangingLimiterState) {
  using namespace std::chrono_literals;
  using clock_t = platf::kms::pacing::clock_t;

  platf::kms::pacing::presentation_rate_limiter_t limiter;
  limiter.set_interval(10ms);

  const auto first = clock_t::time_point {1s};
  limiter.mark_delivered(first);
  limiter.mark_delivered(first + 4ms);

  const auto state = limiter.diagnostic_state(first + 5ms);
  EXPECT_EQ(state.interval, 10ms);
  EXPECT_EQ(state.stored_credit, 4ms);
  EXPECT_EQ(state.available_credit, 5ms);
  ASSERT_TRUE(state.last_delivery);
  EXPECT_EQ(*state.last_delivery, first + 4ms);
  EXPECT_EQ(state.next_delivery, first + 10ms);
  EXPECT_EQ(limiter.next_delivery(first + 5ms), state.next_delivery);
}

TEST(KmsgrabSelection, SustainedOversupplyStaysWithinTheOneFrameBurstAllowance) {
  using namespace std::chrono_literals;
  using clock_t = platf::kms::pacing::clock_t;

  platf::kms::pacing::presentation_rate_limiter_t limiter;
  limiter.set_interval(10ms);

  const auto first = clock_t::time_point {1s};
  auto delivered = first;
  limiter.mark_delivered(delivered);

  // The one-frame phase allowance can absorb finite jitter, but a source that
  // remains 5% fast must spend it and converge to the physical 100 Hz ceiling.
  for (int frame = 1; frame <= 200; ++frame) {
    const auto source_arrival = first + frame * 9500us;
    const auto capture_wake = std::max(source_arrival, delivered);
    delivered = limiter.next_delivery(capture_wake);
    limiter.mark_delivered(delivered);

    // One saved frame is the complete burst allowance. Every later delivery
    // must remain behind the corresponding long-run client-rate bound.
    EXPECT_GE(delivered - first, (frame - 1) * 10ms);
  }

  EXPECT_GE(delivered - first, 1990ms);
  EXPECT_LE(delivered - first, 2000ms);
}

TEST(KmsgrabSelection, TwoIntervalsAreTheExplicitStallBoundary) {
  using namespace std::chrono_literals;
  using clock_t = platf::kms::pacing::clock_t;

  const auto first = clock_t::time_point {1s};
  platf::kms::pacing::presentation_rate_limiter_t limiter;
  limiter.set_interval(10ms);

  // Exhaust the startup credit, then arrive one microsecond before the stall
  // threshold. The remainder still pays for the adjacent presentation.
  limiter.mark_delivered(first);
  limiter.mark_delivered(first);
  limiter.mark_delivered(first + 19'999us);
  EXPECT_EQ(limiter.next_delivery(first + 20ms), first + 20ms);

  // At exactly two intervals the source is considered stalled. It resumes
  // immediately but receives no credit for a same-time catch-up frame.
  limiter.reset();
  limiter.mark_delivered(first);
  limiter.mark_delivered(first);
  limiter.mark_delivered(first + 20ms);
  EXPECT_EQ(limiter.next_delivery(first + 20ms), first + 30ms);
}

TEST(KmsgrabSelection, StallResumesImmediatelyWithoutCatchupCredit) {
  using namespace std::chrono_literals;
  using clock_t = platf::kms::pacing::clock_t;

  platf::kms::pacing::presentation_rate_limiter_t limiter;
  limiter.set_interval(10ms);

  const auto first = clock_t::time_point {1s};
  limiter.mark_delivered(first);
  limiter.mark_delivered(first + 4ms);

  const auto resumed = first + 50ms;
  EXPECT_EQ(limiter.next_delivery(resumed), resumed);
  limiter.mark_delivered(resumed);
  EXPECT_EQ(limiter.next_delivery(resumed + 1ms), resumed + 10ms);

  limiter.reset();
  EXPECT_EQ(limiter.next_delivery(resumed + 1ms), resumed + 1ms);
}

TEST(KmsgrabSelection, OlderSnapshotCannotClearNewerPresentation) {
  using namespace std::chrono_literals;
  using response_e = platf::kms::pacing::presentation_response_e;
  using clock_t = platf::kms::pacing::clock_t;

  platf::kms::pacing::presentation_latch_t latch;
  const auto start = clock_t::time_point {1s};

  latch.request_capture();
  const auto snapshot_generation = latch.capture_generation();

  latch.observe_response(response_e::changed, false, start);
  latch.mark_delivered(snapshot_generation);
  EXPECT_TRUE(latch.capture_ready());

  latch.observe_response(response_e::changed, true, start + 1ms);
  EXPECT_TRUE(latch.capture_ready());
  latch.observe_response(response_e::timeout, false, start + 2ms);
  EXPECT_TRUE(latch.capture_ready());

  latch.mark_delivered(latch.capture_generation());
  EXPECT_FALSE(latch.capture_ready());
}

TEST(KmsgrabSelection, RejectsUnsafePresentationTimestamps) {
  using namespace std::chrono_literals;
  using clock_t = platf::kms::pacing::clock_t;

  const auto now = clock_t::time_point {10s};
  EXPECT_EQ(platf::kms::pacing::validate_timestamp(0, now, 1s), std::nullopt);
  EXPECT_EQ(platf::kms::pacing::validate_timestamp(std::numeric_limits<std::uint64_t>::max(), now, 1s), std::nullopt);
  EXPECT_EQ(platf::kms::pacing::validate_timestamp(std::chrono::duration_cast<std::chrono::nanoseconds>(12s).count(), now, 1s), std::nullopt);
  EXPECT_EQ(platf::kms::pacing::validate_timestamp(std::chrono::duration_cast<std::chrono::nanoseconds>(10001ms).count(), now, 0ns), std::nullopt);

  const auto valid_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(9500ms).count();
  EXPECT_EQ(platf::kms::pacing::validate_timestamp(valid_ns, now, 1s), clock_t::time_point {9500ms});
  EXPECT_EQ(platf::kms::pacing::validate_timestamp(valid_ns, now, 0ns, clock_t::time_point {9600ms}), std::nullopt);
  EXPECT_EQ(platf::kms::pacing::validate_timestamp(valid_ns, now, 0ns, clock_t::time_point {9500ms}), clock_t::time_point {9500ms});
}

TEST(KmsgrabSelection, UsesStableVibeshinePresentationAbi) {
  EXPECT_EQ(VIBESHINE_DRM_PRESENT_ABI_VERSION, 1u);
  EXPECT_EQ(VIBESHINE_DRM_PRESENT_MAX_TIMEOUT_MS, 1000u);
  EXPECT_EQ(DRM_VIBESHINE_WAIT_PRESENT, 0u);
  EXPECT_EQ(sizeof(vibeshine_drm_wait_present), 48u);
  EXPECT_EQ(offsetof(vibeshine_drm_wait_present, sequence), 8u);
  EXPECT_EQ(offsetof(vibeshine_drm_wait_present, timestamp_ns), 16u);
  EXPECT_EQ(offsetof(vibeshine_drm_wait_present, timeout_ms), 24u);
  EXPECT_EQ(offsetof(vibeshine_drm_wait_present, reserved), 32u);

  EXPECT_EQ(VIBESHINE_DRM_FRAME_ABI_VERSION, 1u);
  EXPECT_EQ(VIBESHINE_DRM_FRAME_MAX_PLANES, 4u);
  EXPECT_EQ(DRM_VIBESHINE_GET_FRAME, 1u);
  EXPECT_EQ(sizeof(vibeshine_drm_frame), 152u);
  EXPECT_EQ(offsetof(vibeshine_drm_frame, sequence), 8u);
  EXPECT_EQ(offsetof(vibeshine_drm_frame, modifier), 40u);
  EXPECT_EQ(offsetof(vibeshine_drm_frame, dma_buf_fds), 52u);
  EXPECT_EQ(offsetof(vibeshine_drm_frame, sync_file_fds), 100u);
  EXPECT_EQ(offsetof(vibeshine_drm_frame, reserved), 120u);

  EXPECT_EQ(VIBESHINE_DRM_TRACE_ABI_VERSION, 1u);
  EXPECT_EQ(VIBESHINE_DRM_TRACE_MAX_EVENTS, 64u);
  EXPECT_EQ(DRM_VIBESHINE_GET_PRESENT_TRACE, 2u);
  EXPECT_EQ(sizeof(vibeshine_drm_trace_event), 16u);
  EXPECT_EQ(sizeof(vibeshine_drm_present_trace), 1088u);
  EXPECT_EQ(offsetof(vibeshine_drm_present_trace, after_sequence), 8u);
  EXPECT_EQ(offsetof(vibeshine_drm_present_trace, newest_sequence), 16u);
  EXPECT_EQ(offsetof(vibeshine_drm_present_trace, count), 24u);
  EXPECT_EQ(offsetof(vibeshine_drm_present_trace, events), 32u);
  EXPECT_EQ(offsetof(vibeshine_drm_present_trace, reserved), 1056u);
}

TEST(KmsgrabSelection, RecognizesCudaImportableDisplayDrivers) {
  EXPECT_TRUE(selection::driver_supports_cuda_import("nvidia-drm"));
  EXPECT_TRUE(selection::driver_supports_cuda_import("nvidia-drm-extra"));
  EXPECT_TRUE(selection::driver_supports_cuda_import("vibeshine_drm"));

  EXPECT_FALSE(selection::driver_supports_cuda_import("vkms"));
  EXPECT_FALSE(selection::driver_supports_cuda_import("i915"));
  EXPECT_FALSE(selection::driver_supports_cuda_import(""));
  EXPECT_FALSE(selection::driver_is_nvidia("vibeshine_drm"));
  EXPECT_TRUE(selection::driver_requires_direct_import("vibeshine_drm"));
  EXPECT_TRUE(selection::driver_requires_presentation_events("vibeshine_drm"));
  EXPECT_FALSE(selection::driver_requires_direct_import("nvidia-drm"));
  EXPECT_FALSE(selection::driver_requires_presentation_events("nvidia-drm"));
  EXPECT_FALSE(selection::driver_requires_direct_import("vkms"));
  EXPECT_FALSE(selection::driver_requires_presentation_events("vkms"));
}

TEST(KmsgrabSelection, RejectsMissingOrAmbiguousDriverIdentities) {
  EXPECT_FALSE(selection::normalize_driver_name(nullptr, 0));
  EXPECT_FALSE(selection::normalize_driver_name("", 0));

  const char embedded_nul[] {'v', 'k', 'm', 's', '\0', 'x'};
  EXPECT_FALSE(selection::normalize_driver_name(embedded_nul, sizeof(embedded_nul)));

  const std::string oversized(selection::MAX_DRM_DRIVER_NAME_LENGTH + 1, 'x');
  EXPECT_FALSE(selection::normalize_driver_name(oversized.data(), oversized.size()));

  EXPECT_EQ(selection::normalize_driver_name("vibeshine_drm", 13), "vibeshine_drm");
}

TEST(KmsgrabSelection, PrivateDisplayEncodesOnTheSelectedPhysicalRenderer) {
  EXPECT_EQ(selection::render_device_path("vibeshine_drm", "/dev/dri/card1", "", "/dev/dri/renderD128"), "/dev/dri/renderD128");
  // A selected second GPU must survive enumeration of another renderer.
  EXPECT_EQ(selection::render_device_path("vibeshine_drm", "/dev/dri/card2", "/dev/dri/renderD128", "/dev/dri/renderD129"), "/dev/dri/renderD129");
  // An unresolved encoder never falls back to the display-only card.
  EXPECT_TRUE(selection::render_device_path("vibeshine_drm", "/dev/dri/card1", "", "").empty());
}

TEST(KmsgrabSelection, OrdinaryCardsRetainTheirOwnRenderNodeAndPrimaryFallback) {
  for (const auto driver : {"amdgpu", "i915", "nvidia-drm"}) {
    EXPECT_EQ(selection::render_device_path(driver, "/dev/dri/card0", "/dev/dri/renderD128", "/dev/dri/renderD129"), "/dev/dri/renderD128");
    EXPECT_EQ(selection::render_device_path(driver, "/dev/dri/card0", "", "/dev/dri/renderD129"), "/dev/dri/card0");
  }
  // An unrelated virtual driver cannot opt into the managed renderer route.
  EXPECT_EQ(selection::render_device_path("vkms", "/dev/dri/card1", "", "/dev/dri/renderD128"), "/dev/dri/card1");
  EXPECT_EQ(selection::render_device_path("vibeshine_drm-extra", "/dev/dri/card1", "", "/dev/dri/renderD128"), "/dev/dri/card1");
}

TEST(KmsgrabSelection, ParsesOnlyCompleteUnsignedNumericAliases) {
  EXPECT_EQ(selection::parse_numeric_alias("0"), 0u);
  EXPECT_EQ(selection::parse_numeric_alias("17"), 17u);
  EXPECT_EQ(selection::parse_numeric_alias(std::to_string(std::numeric_limits<std::uint32_t>::max())), std::numeric_limits<std::uint32_t>::max());

  EXPECT_FALSE(selection::parse_numeric_alias(""));
  EXPECT_FALSE(selection::parse_numeric_alias("-1"));
  EXPECT_FALSE(selection::parse_numeric_alias("1-HDMI-A-1"));
  EXPECT_FALSE(selection::parse_numeric_alias("4294967296"));
}

TEST(KmsgrabSelection, UsesStableConnectorNamesAndLegacyMonitorOrdinals) {
  const auto monitors = selection::name_monitors({
    {"card1", "HDMI-A-1", 52, 1},
    {"card2", "Virtual-1", 40, 0},
  });

  ASSERT_EQ(monitors.size(), 2u);
  EXPECT_EQ(monitors[0].display_name, "Virtual-1");
  EXPECT_EQ(monitors[1].display_name, "HDMI-A-1");

  const auto stable_match = selection::resolve_named_monitor("virtual-1", monitors);
  ASSERT_TRUE(stable_match);
  EXPECT_EQ(stable_match->card_path, "card2");
  EXPECT_EQ(stable_match->crtc_id, 40u);
  EXPECT_EQ(stable_match->monitor_index, 0u);

  const auto numeric_alias = selection::parse_numeric_alias("1");
  ASSERT_TRUE(numeric_alias);
  EXPECT_EQ(*numeric_alias, monitors[1].monitor.monitor_index);
}

TEST(KmsgrabSelection, QualifiesCrossGpuCollisionsAndRejectsAmbiguousRawName) {
  const auto monitors = selection::name_monitors({
    {"card0", "HDMI-A-1", 10, 0},
    {"card1", "HDMI-A-1", 20, 1},
  });

  ASSERT_EQ(monitors.size(), 2u);
  EXPECT_EQ(monitors[0].display_name, "card0:HDMI-A-1");
  EXPECT_EQ(monitors[1].display_name, "card1:HDMI-A-1");
  EXPECT_FALSE(selection::resolve_named_monitor("HDMI-A-1", monitors));

  const auto match = selection::resolve_named_monitor("CARD1:hdmi-a-1", monitors);
  ASSERT_TRUE(match);
  EXPECT_EQ(match->card_path, "card1");
  EXPECT_EQ(match->crtc_id, 20u);
}

TEST(OutputLayout, MovesTheLayoutToItsBoundingBoxOrigin) {
  // A laptop panel at 1.25 scale below an ultrawide, as Hyprland lays them out.
  const auto layout = platf::linux_display::layout::parse("L\teDP-2\t1536\t2880\t2048\t1280\nL\tDP-2\t0\t1440\t5120\t1440\n");
  ASSERT_TRUE(layout);
  EXPECT_EQ(layout->width, 5120);
  EXPECT_EQ(layout->height, 2720);
  ASSERT_EQ(layout->outputs.size(), 2U);
  EXPECT_EQ(layout->outputs[0], (platf::linux_display::layout::output_rect_t {"eDP-2", 1536, 1440, 2048, 1280}));
  EXPECT_EQ(layout->outputs[1], (platf::linux_display::layout::output_rect_t {"DP-2", 0, 0, 5120, 1440}));
}

TEST(OutputLayout, AcceptsNegativePositions) {
  const auto layout = platf::linux_display::layout::parse("L\tHDMI-A-1\t-1920\t0\t1920\t1080\nL\tDP-1\t0\t0\t2560\t1440\n");
  ASSERT_TRUE(layout);
  EXPECT_EQ(layout->width, 4480);
  EXPECT_EQ(layout->height, 1440);
  EXPECT_EQ(layout->outputs[0].x, 0);
  EXPECT_EQ(layout->outputs[1].x, 1920);
}

TEST(OutputLayout, RejectsTheWholeReplyForAnyMalformedLine) {
  using platf::linux_display::layout::parse;
  EXPECT_FALSE(parse(""));
  EXPECT_FALSE(parse("L\tDP-1\t0\t0\t2560\t1440"));  // unterminated
  EXPECT_FALSE(parse("L\tDP-1\t0\t0\t2560\t1440\nS\tDP-2\t0\t0\t1\t1\n"));
  EXPECT_FALSE(parse("L\tDP 1\t0\t0\t2560\t1440\n"));
  EXPECT_FALSE(parse("L\tDP-1\t0\t0\t0\t1440\n"));
  EXPECT_FALSE(parse("L\tDP-1\t0\t0\t2560\t1440\t7\n"));
  EXPECT_FALSE(parse("L\tDP-1\t0x10\t0\t2560\t1440\n"));
  EXPECT_FALSE(parse("L\tDP-1\t2000000\t0\t2560\t1440\n"));
  EXPECT_FALSE(parse("L\tDP-1\t0\t0\t2560\t1440\nL\tDP-1\t2560\t0\t2560\t1440\n"));
  EXPECT_FALSE(parse(std::string(platf::linux_display::layout::max_reply_bytes + 1, '\n')));
}

TEST(OutputLayout, EnforcesItsLimits) {
  using namespace platf::linux_display::layout;
  EXPECT_FALSE(parse("L\t" + std::string(max_name_length + 1, 'A') + "\t0\t0\t1\t1\n"));
  EXPECT_TRUE(parse("L\t" + std::string(max_name_length, 'A') + "\t0\t0\t1\t1\n"));
  EXPECT_FALSE(parse("L\tDP-1\t0\t0\t" + std::to_string(max_extent + 1) + "\t1\n"));
  EXPECT_FALSE(parse("L\tDP-1\t0\t-2000000\t1\t1\n"));
  EXPECT_FALSE(parse("L\tDP-1\t0\t0\t1\t1\r\n"));
  EXPECT_FALSE(parse("\n"));
  std::string outputs;
  for (std::size_t index = 0; index <= max_outputs; ++index) {
    outputs += "L\tDP-" + std::to_string(index) + "\t0\t0\t1\t1\n";
  }
  EXPECT_FALSE(parse(outputs));
}

namespace {
  struct fake_viewport_t {
    int offset_x = 0, offset_y = 0, width = 0, height = 0, logical_width = 0, logical_height = 0;
    std::string output_name;
  };

  fake_viewport_t scanout(int width, int height) {
    fake_viewport_t viewport;
    viewport.width = width;
    viewport.height = height;
    return viewport;
  }
}  // namespace

TEST(OutputLayout, PlacesEachUniquelyNamedUnrotatedOutput) {
  using namespace platf::linux_display::layout;
  const auto layout = parse(
    "L\teDP-2\t1536\t2880\t2048\t1280\nL\tDP-2\t0\t1440\t5120\t1440\n"
    "L\tHDMI-A-1\t5120\t1440\t1440\t2560\nL\tDP-1\t6560\t1440\t1\t1\nL\tHEADLESS-1\t6561\t1440\t1\t1\n"
  );
  ASSERT_TRUE(layout);
  auto panel = scanout(2560, 1600), wide = scanout(5120, 1440), rotated = scanout(2560, 1440);
  auto first_gpu = scanout(1, 1), second_gpu = scanout(1, 1);
  const std::vector<capture_output_t<fake_viewport_t>> capture {
    {"eDP-2", &panel}, {"DP-2", &wide}, {"HDMI-A-1", &rotated}, {"DP-1", &first_gpu}, {"DP-1", &second_gpu}
  };
  EXPECT_EQ(place(*layout, capture), (std::vector<std::string> {"HDMI-A-1", "DP-1", "HEADLESS-1"}));
  EXPECT_EQ(panel.output_name, "eDP-2");
  EXPECT_EQ(panel.offset_x, 1536);
  EXPECT_EQ(panel.offset_y, 1440);
  EXPECT_EQ(panel.logical_width, 2048);
  EXPECT_EQ(panel.logical_height, 1280);
  EXPECT_EQ(wide.offset_y, 0);
  EXPECT_EQ(wide.logical_width, 5120);
  EXPECT_TRUE(rotated.output_name.empty());  // portrait layout over a landscape scanout
  EXPECT_EQ(rotated.logical_width, 0);
  EXPECT_TRUE(first_gpu.output_name.empty());
  EXPECT_TRUE(second_gpu.output_name.empty());
}
