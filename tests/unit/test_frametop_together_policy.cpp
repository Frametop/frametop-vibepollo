#include "../tests_common.h"

#include "src/frametop_together_policy.h"

namespace {
  using namespace frametop_together::policy;
  using namespace std::chrono_literals;

  const instant_t T0 = instant_t {} + 1h;

  step_t does(act_e act, int kbps = 0, int pictures = 0) {
    return {act, {kbps, pictures}};
  }

  /// A panel that has a window and is watched: its capture runs at what a panel starts at.
  panel_t running_panel() {
    panel_t panel;
    panel.window(true);
    panel.heard('W', 1, T0);
    return panel;
  }
}  // namespace

TEST(FrametopTogetherPolicy, RunsOnlyWatchedAndWithAWindow) {
  panel_t first;
  EXPECT_EQ(first.window(true), does(act_e::none));
  EXPECT_FALSE(first.running());
  EXPECT_EQ(first.heard('W', 1, T0), does(act_e::start, 2000, 10));
  EXPECT_TRUE(first.running());
  EXPECT_TRUE(first.watched());
  EXPECT_EQ(first.run(), (run_t {2000, 10}));
  EXPECT_EQ(first.window(true), does(act_e::none));  // (it has one, and runs)

  panel_t second;
  EXPECT_EQ(second.heard('W', 1, T0), does(act_e::none));
  EXPECT_TRUE(second.watched());
  EXPECT_FALSE(second.running());
  EXPECT_EQ(second.window(true), does(act_e::start, 2000, 10));
}

TEST(FrametopTogetherPolicy, StopsWhenNobodyWatchesOrItsWindowGoes) {
  auto unwatched = running_panel();
  EXPECT_EQ(unwatched.heard('W', 0, T0), does(act_e::stop));
  EXPECT_FALSE(unwatched.running());
  EXPECT_FALSE(unwatched.watched());
  EXPECT_EQ(unwatched.heard('W', 0, T0), does(act_e::none));
  EXPECT_EQ(unwatched.heard('W', 1, T0), does(act_e::start, 2000, 10));

  auto windowless = running_panel();
  EXPECT_EQ(windowless.window(false), does(act_e::stop));
  EXPECT_EQ(windowless.window(false), does(act_e::none));
  EXPECT_EQ(windowless.heard('W', 1, T0), does(act_e::none));
  EXPECT_EQ(windowless.window(true), does(act_e::start, 2000, 10));
}

TEST(FrametopTogetherPolicy, StartsAtWhatTheHeadsetSaidLast) {
  panel_t panel;
  panel.window(true);
  EXPECT_EQ(panel.heard('R', 5'000'000, T0), does(act_e::none));
  EXPECT_EQ(panel.heard('F', 60, T0), does(act_e::none));
  EXPECT_EQ(panel.heard('P', 45, T0), does(act_e::none));
  EXPECT_EQ(panel.heard('K', 0, T0), does(act_e::none));
  EXPECT_EQ(panel.heard('W', 1, T0), does(act_e::start, 5000, 45));
}

TEST(FrametopTogetherPolicy, KeepsWhatItRunsAtWithinBounds) {
  const auto started_at = [](std::uint32_t bits, std::uint32_t full, std::uint32_t pictures) {
    panel_t panel;
    panel.window(true);
    panel.heard('R', bits, T0);
    panel.heard('F', full, T0);
    panel.heard('P', pictures, T0);
    return panel.heard('W', 1, T0).run;
  };
  EXPECT_EQ(started_at(1, 30, 10), (run_t {100, 10}));
  EXPECT_EQ(started_at(99'999, 30, 10), (run_t {100, 10}));
  EXPECT_EQ(started_at(100'000'000, 30, 10), (run_t {100'000, 10}));
  EXPECT_EQ(started_at(4'000'000'000u, 30, 10), (run_t {100'000, 10}));
  EXPECT_EQ(started_at(2'000'000, 30, 60), (run_t {2000, 30}));  // never more than the full rate
  EXPECT_EQ(started_at(2'000'000, 15, 30), (run_t {2000, 15}));
  EXPECT_EQ(started_at(2'000'000, 4'000'000'000u, 4'000'000'000u), (run_t {2000, 60}));
  EXPECT_EQ(started_at(2'000'000, 30, 0), (run_t {2000, 1}));
  EXPECT_EQ(started_at(2'000'000, 0, 30), (run_t {2000, 1}));
}

TEST(FrametopTogetherPolicy, AKeyframeForTheAskingAndForANewViewer) {
  auto panel = running_panel();
  EXPECT_EQ(panel.heard('K', 0, T0), does(act_e::keyframe));
  EXPECT_EQ(panel.heard('W', 1, T0), does(act_e::keyframe));
  EXPECT_TRUE(panel.running());
  panel.heard('W', 0, T0);
  EXPECT_EQ(panel.heard('K', 0, T0), does(act_e::none));
  EXPECT_EQ(panel.heard('Q', 1, T0), does(act_e::none));
}

TEST(FrametopTogetherPolicy, TellsTheRunningCaptureEachNewBitrate) {
  auto panel = running_panel();
  EXPECT_EQ(panel.heard('R', 3'000'000, T0), does(act_e::bitrate, 3000, 10));
  EXPECT_EQ(panel.run(), (run_t {3000, 10}));
  EXPECT_EQ(panel.heard('R', 3'000'000, T0), does(act_e::none));
  EXPECT_EQ(panel.heard('R', 3'000'999, T0), does(act_e::none));  // (the same in kbit/s)
  EXPECT_EQ(panel.heard('R', 50'000, T0), does(act_e::bitrate, 100, 10));
  EXPECT_EQ(panel.heard('R', 1, T0), does(act_e::none));  // (the least already)
  EXPECT_EQ(panel.heard('R', 8'000'000, T0), does(act_e::bitrate, 8000, 10));
  EXPECT_TRUE(panel.running());

  // A capture that starts later starts at it.
  panel.heard('W', 0, T0);
  EXPECT_EQ(panel.heard('R', 5'000'000, T0), does(act_e::none));
  EXPECT_EQ(panel.heard('W', 1, T0), does(act_e::start, 5000, 10));
}

TEST(FrametopTogetherPolicy, MorePicturesAtOnceFewerOnceThatHasStood) {
  auto panel = running_panel();
  EXPECT_EQ(panel.heard('P', 30, T0), does(act_e::restart, 2000, 30));
  EXPECT_EQ(panel.heard('P', 30, T0), does(act_e::none));

  EXPECT_EQ(panel.heard('P', 10, T0 + 1s), does(act_e::none));
  EXPECT_EQ(panel.run(), (run_t {2000, 30}));
  EXPECT_EQ(panel.tick(T0 + 5999ms), does(act_e::none));
  EXPECT_EQ(panel.tick(T0 + 6s), does(act_e::restart, 2000, 10));
  EXPECT_EQ(panel.tick(T0 + 20s), does(act_e::none));

  // Fewer still while it waits doesn't start the wait over.
  panel.heard('P', 30, T0 + 30s);
  EXPECT_EQ(panel.heard('P', 20, T0 + 31s), does(act_e::none));
  EXPECT_EQ(panel.heard('F', 5, T0 + 34s), does(act_e::none));
  EXPECT_EQ(panel.tick(T0 + 36s), does(act_e::restart, 2000, 5));
  EXPECT_EQ(panel.heard('F', 30, T0 + 37s), does(act_e::restart, 2000, 20));
}

TEST(FrametopTogetherPolicy, AGlanceAwayStartsNoEncoderOver) {
  auto panel = running_panel();
  panel.heard('P', 30, T0);
  EXPECT_EQ(panel.heard('P', 10, T0 + 1s), does(act_e::none));
  EXPECT_EQ(panel.heard('P', 30, T0 + 4s), does(act_e::none));
  EXPECT_EQ(panel.tick(T0 + 60s), does(act_e::none));
  EXPECT_EQ(panel.run(), (run_t {2000, 30}));

  // Another bitrate in the meantime changes nothing of that.
  EXPECT_EQ(panel.heard('P', 10, T0 + 61s), does(act_e::none));
  EXPECT_EQ(panel.heard('R', 8'000'000, T0 + 62s), does(act_e::bitrate, 8000, 30));
  EXPECT_EQ(panel.tick(T0 + 65999ms), does(act_e::none));
  EXPECT_EQ(panel.tick(T0 + 66s), does(act_e::restart, 8000, 10));
  EXPECT_EQ(panel.tick(T0 + 120s), does(act_e::none));

  // Nor after it stopped.
  panel.heard('P', 30, T0 + 121s);
  panel.heard('P', 10, T0 + 122s);
  panel.heard('W', 0, T0 + 123s);
  EXPECT_EQ(panel.tick(T0 + 200s), does(act_e::none));
  EXPECT_FALSE(panel.running());
}

TEST(FrametopTogetherPolicy, ACaptureThatEndedIsTriedAgainLaterWhileWatched) {
  auto panel = running_panel();
  panel.ended(T0);
  EXPECT_FALSE(panel.running());
  EXPECT_EQ(panel.heard('W', 1, T0 + 1s), does(act_e::none));  // not at once, whoever asks
  EXPECT_EQ(panel.heard('K', 0, T0 + 1s), does(act_e::none));
  EXPECT_EQ(panel.heard('R', 8'000'000, T0 + 1s), does(act_e::none));
  EXPECT_EQ(panel.heard('P', 30, T0 + 1s), does(act_e::none));
  EXPECT_EQ(panel.tick(T0 + 1999ms), does(act_e::none));
  EXPECT_EQ(panel.tick(T0 + 2s), does(act_e::start, 8000, 30));
  EXPECT_TRUE(panel.running());
  EXPECT_EQ(panel.tick(T0 + 10s), does(act_e::none));
}

TEST(FrametopTogetherPolicy, NothingIsTriedAgainForAPanelNobodyWatches) {
  auto unwatched = running_panel();
  unwatched.ended(T0);
  EXPECT_EQ(unwatched.heard('W', 0, T0 + 1s), does(act_e::none));
  EXPECT_EQ(unwatched.tick(T0 + 10s), does(act_e::none));
  EXPECT_EQ(unwatched.heard('W', 1, T0 + 11s), does(act_e::start, 2000, 10));  // (and no wait is left over)

  auto glanced = running_panel();
  glanced.ended(T0);
  glanced.heard('W', 0, T0 + 500ms);
  EXPECT_EQ(glanced.heard('W', 1, T0 + 1s), does(act_e::start, 2000, 10));  // (nor with no tick between)

  auto windowless = running_panel();
  windowless.ended(T0);
  EXPECT_EQ(windowless.window(false), does(act_e::none));
  EXPECT_EQ(windowless.tick(T0 + 10s), does(act_e::none));
  EXPECT_EQ(windowless.window(true), does(act_e::start, 2000, 10));

  // A capture that was stopped didn't end by itself.
  auto stopped = running_panel();
  stopped.heard('W', 0, T0);
  stopped.ended(T0);
  EXPECT_EQ(stopped.heard('W', 1, T0), does(act_e::start, 2000, 10));
}

TEST(FrametopTogetherPolicy, EncodesAtTheOutputsSizeOrTheNearestBelowAFrameTakes) {
  EXPECT_EQ(encode_size(1280, 720), (extent_t {1280, 720}));
  EXPECT_EQ(encode_size(1920, 1080), (extent_t {1920, 1080}));
  EXPECT_EQ(encode_size(2560, 1440), (extent_t {2560, 1440}));
  EXPECT_EQ(encode_size(3840, 2160), (extent_t {3840, 2160}));
  EXPECT_EQ(encode_size(128, 128), (extent_t {128, 128}));
  EXPECT_EQ(encode_size(4096, 4096), (extent_t {4096, 4096}));
  EXPECT_EQ(encode_size(1366, 768), (extent_t {1280, 720}));
  EXPECT_EQ(encode_size(1279, 720), (extent_t {1152, 648}));
  EXPECT_EQ(encode_size(255, 255), (extent_t {128, 128}));
  EXPECT_EQ(encode_size(1280, 724), (extent_t {1280, 728}));  // (the height to the nearest 8)
  EXPECT_EQ(encode_size(1280, 723), (extent_t {1280, 720}));
}

TEST(FrametopTogetherPolicy, KeepsAnEncodeWithinTheRoomsBounds) {
  EXPECT_FALSE(encode_size(127, 720));
  EXPECT_FALSE(encode_size(720, 127));
  EXPECT_FALSE(encode_size(0, 0));
  EXPECT_FALSE(encode_size(-1280, -720));
  EXPECT_EQ(encode_size(8192, 4096), (extent_t {4096, 2048}));
  EXPECT_EQ(encode_size(5000, 200), (extent_t {4096, 160}));
  EXPECT_EQ(encode_size(1000, 8000), (extent_t {512, 4096}));
  EXPECT_EQ(encode_size(4096, 130), (extent_t {4096, 128}));
  EXPECT_EQ(encode_size(200, 4096), (extent_t {128, 2624}));
  EXPECT_EQ(encode_size(129, 8000), (extent_t {128, 4096}));
  for (int width = 128; width <= 9000; width += 37) {
    for (int height = 128; height <= 9000; height += 411) {
      const auto size = encode_size(width, height);
      ASSERT_TRUE(size) << width << 'x' << height;
      EXPECT_EQ(size->width % WIDTH_STEP, 0) << width << 'x' << height;
      EXPECT_EQ(size->height % HEIGHT_STEP, 0) << width << 'x' << height;
      EXPECT_TRUE(size->width >= MIN_SIDE && size->width <= MAX_SIDE) << width << 'x' << height;
      EXPECT_TRUE(size->height >= MIN_SIDE && size->height <= MAX_SIDE) << width << 'x' << height;
      EXPECT_LE(size->width, width) << width << 'x' << height;
    }
  }
}
