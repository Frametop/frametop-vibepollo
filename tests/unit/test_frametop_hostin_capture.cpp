#include "../tests_common.h"

#include "src/frametop_hostin_capture.h"
#include "src/frametop_hostin_policy.h"

// Ours, by value: the kernel's header defines the same names as macros.
namespace ours {
  constexpr int key_esc = frametop_hostin::capture::KEY_ESC;
  constexpr int key_leftshift = frametop_hostin::capture::KEY_LEFTSHIFT;
  constexpr int key_rightshift = frametop_hostin::capture::KEY_RIGHTSHIFT;
  constexpr int key_max = frametop_hostin::capture::KEY_MAX;
  constexpr int btn_left = frametop_hostin::capture::BTN_LEFT;
  constexpr int btn_extra = frametop_hostin::capture::BTN_EXTRA;
  constexpr int rel_wheel = frametop_hostin::capture::REL_WHEEL;
  constexpr int rel_hwheel = frametop_hostin::capture::REL_HWHEEL;
  constexpr int rel_wheel_hi_res = frametop_hostin::capture::REL_WHEEL_HI_RES;
  constexpr int rel_hwheel_hi_res = frametop_hostin::capture::REL_HWHEEL_HI_RES;
}  // namespace ours

#if __has_include(<linux/input-event-codes.h>)
  #include <linux/input-event-codes.h>
  #define HAVE_EVDEV_CODES 1
#endif

namespace {
  using namespace frametop_hostin::capture;
  using clock_type = lease_t::clock;

  constexpr std::uint32_t VK_LSHIFT = 0xa0;
  constexpr std::uint32_t VK_RSHIFT = 0xa1;
  constexpr std::uint32_t VK_ESCAPE = 0x1b;
  constexpr std::uint32_t VK_A = 0x41;
  constexpr std::uint32_t VK_VOLUME_UP = 0xaf;
  constexpr std::uint32_t VK_LBUTTON = 0x01;
  constexpr std::uint16_t KEY_A_CODE = 30;
}  // namespace

TEST(FrametopHostInputCapture, MapsScanCodesToEvdev) {
  EXPECT_EQ(key_code(0x01, false, VK_ESCAPE), KEY_ESC);
  EXPECT_EQ(key_code(0x1e, false, VK_A), KEY_A_CODE);
  EXPECT_EQ(key_code(0x2a, false, VK_LSHIFT), KEY_LEFTSHIFT);
  EXPECT_EQ(key_code(0x36, false, VK_RSHIFT), KEY_RIGHTSHIFT);
  EXPECT_EQ(key_code(0x58, false, 0x7b), 88);  // F12
  EXPECT_EQ(key_code(0x1d, true, 0xa3), 97);  // right Ctrl
  EXPECT_EQ(key_code(0x5b, true, 0x5b), 125);  // left Windows key
  EXPECT_EQ(key_code(0x48, true, 0x26), 103);  // Up
  EXPECT_EQ(key_code(0x48, false, 0x68), 72);  // keypad 8: positional, whatever Num Lock says
  EXPECT_EQ(key_code(0x64, false, 0x7c), 183);  // F13
}

TEST(FrametopHostInputCapture, MapsTheKeysWhoseScanCodeSaysTooLittle) {
  EXPECT_EQ(key_code(0x45, false, 0x13), 119);  // Pause
  EXPECT_EQ(key_code(0x45, true, 0x90), 69);  // Num Lock
  EXPECT_EQ(key_code(0x37, true, 0x2c), 99);  // Print Screen
  EXPECT_EQ(key_code(0x54, false, 0x2c), 99);  // Alt+Print Screen
}

TEST(FrametopHostInputCapture, HasNoCodeForFakeShiftsOrUnknownKeys) {
  EXPECT_EQ(key_code(0x2a, true, 0xff), 0);  // E0 2A, sent around Print Screen
  EXPECT_EQ(key_code(0x00, false, 0xff), 0);
  EXPECT_EQ(key_code(0x5a, false, 0xff), 0);
  EXPECT_EQ(key_code(0x1e0, false, 0xff), 0);
}

#ifdef HAVE_EVDEV_CODES
TEST(FrametopHostInputCapture, UsesTheKernelsCodes) {
  EXPECT_EQ(ours::key_esc, KEY_ESC);
  EXPECT_EQ(ours::key_leftshift, KEY_LEFTSHIFT);
  EXPECT_EQ(ours::key_rightshift, KEY_RIGHTSHIFT);
  EXPECT_EQ(ours::key_max, KEY_MAX);
  EXPECT_EQ(ours::btn_left, BTN_LEFT);
  EXPECT_EQ(ours::btn_extra, BTN_EXTRA);
  EXPECT_EQ(ours::rel_wheel, REL_WHEEL);
  EXPECT_EQ(ours::rel_hwheel, REL_HWHEEL);
  EXPECT_EQ(ours::rel_wheel_hi_res, REL_WHEEL_HI_RES);
  EXPECT_EQ(ours::rel_hwheel_hi_res, REL_HWHEEL_HI_RES);
  EXPECT_EQ(key_code(0x46, true, 0x03), KEY_PAUSE);
  EXPECT_EQ(key_code(0x5d, true, 0x5d), KEY_COMPOSE);
  EXPECT_EQ(key_code(0x56, false, 0xe2), KEY_102ND);
  EXPECT_EQ(key_code(0x73, false, 0xc1), KEY_RO);
  EXPECT_EQ(key_code(0x7d, false, 0xdc), KEY_YEN);
  EXPECT_EQ(key_code(0x76, false, 0x87), KEY_F24);
  EXPECT_EQ(key_code(0x6d, true, 0xb5), KEY_MEDIA);
  EXPECT_EQ(key_code(0x20, true, 0xad), KEY_MUTE);
  for (const auto code : {KEY_MUTE, KEY_VOLUMEDOWN, KEY_VOLUMEUP, KEY_MICMUTE, KEY_PLAYPAUSE, KEY_PLAYCD,
                          KEY_PAUSECD, KEY_STOPCD, KEY_NEXTSONG, KEY_PREVIOUSSONG, KEY_FASTFORWARD, KEY_REWIND,
                          KEY_BRIGHTNESSDOWN, KEY_BRIGHTNESSUP, KEY_KBDILLUMDOWN, KEY_KBDILLUMUP, KEY_KBDILLUMTOGGLE}) {
    EXPECT_TRUE(kept_key(std::uint16_t(code))) << code;
  }
}
#endif

TEST(FrametopHostInputCapture, KeepsMediaKeysHere) {
  EXPECT_TRUE(kept_key(113));
  EXPECT_FALSE(kept_key(KEY_A_CODE));
  EXPECT_TRUE(kept_vk(VK_VOLUME_UP));
  EXPECT_FALSE(kept_vk(VK_A));

  keys_t keys;
  keys.take({});
  EXPECT_EQ(keys.key(115, VK_VOLUME_UP, true), route_e::pass);
  EXPECT_EQ(keys.key(0, VK_VOLUME_UP, true), route_e::pass);  // no scan code
}

TEST(FrametopHostInputCapture, ForwardsAKeyOnceAndSwallowsItsRepeats) {
  keys_t keys;
  keys.take({});
  EXPECT_EQ(keys.key(KEY_A_CODE, VK_A, true), route_e::forward);
  EXPECT_EQ(keys.key(KEY_A_CODE, VK_A, true), route_e::swallow);
  EXPECT_EQ(keys.key(KEY_A_CODE, VK_A, false), route_e::forward);
  // An up the client never got a down for stays nowhere.
  EXPECT_EQ(keys.key(KEY_A_CODE, VK_A, false), route_e::swallow);
}

TEST(FrametopHostInputCapture, SwallowsKeysWithNoCode) {
  keys_t keys;
  keys.take({});
  EXPECT_EQ(keys.key(0, 0xff, true), route_e::swallow);
  EXPECT_EQ(keys.key(0, 0xff, false), route_e::swallow);
}

TEST(FrametopHostInputCapture, LeavesKeysHeldAtTheTakeHere) {
  keys_t keys;
  std::bitset<256> held;
  held[VK_A] = true;
  held[VK_LBUTTON] = true;
  keys.take(held);
  EXPECT_EQ(keys.key(KEY_A_CODE, VK_A, true), route_e::pass);  // its repeat
  EXPECT_EQ(keys.key(KEY_A_CODE, VK_A, false), route_e::pass);
  EXPECT_EQ(keys.key(BTN_LEFT, VK_LBUTTON, false), route_e::pass);
  // Pressed again, it's the client's.
  EXPECT_EQ(keys.key(KEY_A_CODE, VK_A, true), route_e::forward);
  EXPECT_EQ(keys.key(BTN_LEFT, VK_LBUTTON, true), route_e::forward);
}

TEST(FrametopHostInputCapture, PassesEverythingWhenNotTaken) {
  keys_t keys;
  EXPECT_EQ(keys.key(KEY_A_CODE, VK_A, true), route_e::pass);
  EXPECT_FALSE(keys.taken());
}

TEST(FrametopHostInputCapture, GivingBackLiftsTheClientsKeysAndDrainsThem) {
  keys_t keys;
  keys.take({});
  keys.key(KEY_A_CODE, VK_A, true);
  keys.key(BTN_LEFT, VK_LBUTTON, true);
  keys.key(KEY_LEFTSHIFT, VK_LSHIFT, true);
  keys.key(KEY_LEFTSHIFT, VK_LSHIFT, false);
  EXPECT_EQ(keys.give_back(), (std::vector<std::uint16_t> {KEY_A_CODE, BTN_LEFT}));
  EXPECT_TRUE(keys.draining());
  // Still down here: its repeats and its up go nowhere, so this computer never types it.
  EXPECT_EQ(keys.key(KEY_A_CODE, VK_A, true), route_e::swallow);
  EXPECT_EQ(keys.key(KEY_A_CODE, VK_A, false), route_e::swallow);
  EXPECT_EQ(keys.key(KEY_A_CODE, VK_A, true), route_e::pass);
  EXPECT_EQ(keys.key(BTN_LEFT, VK_LBUTTON, false), route_e::swallow);
  EXPECT_FALSE(keys.draining());
}

TEST(FrametopHostInputCapture, EndingTheDrainLetsKeysThrough) {
  keys_t keys;
  keys.take({});
  keys.key(KEY_A_CODE, VK_A, true);
  keys.give_back();
  keys.end_drain();
  EXPECT_FALSE(keys.draining());
  EXPECT_EQ(keys.key(KEY_A_CODE, VK_A, true), route_e::pass);
}

TEST(FrametopHostInputCapture, AKeyStillDrainingAtTheNextTakeComesUpNowhere) {
  keys_t keys;
  keys.take({});
  keys.key(KEY_A_CODE, VK_A, true);
  keys.give_back();
  keys.take({});
  EXPECT_EQ(keys.key(KEY_A_CODE, VK_A, true), route_e::swallow);
  EXPECT_EQ(keys.key(KEY_A_CODE, VK_A, false), route_e::swallow);
  EXPECT_EQ(keys.key(KEY_A_CODE, VK_A, true), route_e::forward);
}

TEST(FrametopHostInputCapture, BothShiftsAndEscapeEscape) {
  keys_t keys;
  keys.take({});
  EXPECT_FALSE(keys.escape(KEY_LEFTSHIFT, true));
  EXPECT_FALSE(keys.escape(KEY_ESC, true));  // one Shift isn't enough
  EXPECT_FALSE(keys.escape(KEY_ESC, false));
  EXPECT_FALSE(keys.escape(KEY_RIGHTSHIFT, true));
  EXPECT_TRUE(keys.escape(KEY_ESC, true));
  EXPECT_FALSE(keys.escape(KEY_ESC, true));  // its repeat
  keys.give_back();
  // That Escape stays nowhere until it comes up.
  EXPECT_EQ(keys.key(KEY_ESC, VK_ESCAPE, true), route_e::swallow);
  EXPECT_EQ(keys.key(KEY_ESC, VK_ESCAPE, false), route_e::swallow);
  EXPECT_EQ(keys.key(KEY_ESC, VK_ESCAPE, true), route_e::pass);
}

TEST(FrametopHostInputCapture, NoEscapeWhenNotTaken) {
  keys_t keys;
  keys.escape(KEY_LEFTSHIFT, true);
  keys.escape(KEY_RIGHTSHIFT, true);
  EXPECT_FALSE(keys.escape(KEY_ESC, true));
  EXPECT_EQ(keys.key(KEY_ESC, VK_ESCAPE, true), route_e::pass);
}

TEST(FrametopHostInputCapture, AShiftLetGoBreaksTheEscape) {
  keys_t keys;
  keys.take({});
  keys.escape(KEY_LEFTSHIFT, true);
  keys.escape(KEY_RIGHTSHIFT, true);
  keys.escape(KEY_RIGHTSHIFT, false);
  EXPECT_FALSE(keys.escape(KEY_ESC, true));
}

TEST(FrametopHostInputCapture, CountsWheelNotches) {
  wheel_t wheel;
  EXPECT_EQ(wheel.notches(120), 1);
  EXPECT_EQ(wheel.notches(60), 0);
  EXPECT_EQ(wheel.notches(60), 1);
  EXPECT_EQ(wheel.notches(-30), 0);  // the other way starts again
  EXPECT_EQ(wheel.notches(-90), -1);
  EXPECT_EQ(wheel.notches(-240), -2);
  wheel.notches(100);
  wheel.reset();
  EXPECT_EQ(wheel.notches(20), 0);
}

TEST(FrametopHostInputCapture, ParsesRequests) {
  EXPECT_EQ(lease_t::parse("engage"), lease_t::request_e::engage);
  EXPECT_EQ(lease_t::parse("release"), lease_t::request_e::release);
  EXPECT_EQ(lease_t::parse("drop"), lease_t::request_e::drop);
  EXPECT_EQ(lease_t::parse("engage "), lease_t::request_e::invalid);
}

TEST(FrametopHostInputCapture, LeasesLikeTheLinuxService) {
  lease_t lease;
  const auto t0 = clock_type::now();
  EXPECT_EQ(lease.request(lease_t::request_e::engage, t0), lease_t::step_e::take);
  EXPECT_EQ(lease.request(lease_t::request_e::engage, t0 + std::chrono::milliseconds {500}), lease_t::step_e::stay);
  EXPECT_FALSE(lease.lapsed(t0 + std::chrono::milliseconds {1999}));
  EXPECT_TRUE(lease.lapsed(t0 + std::chrono::milliseconds {2000}));
  EXPECT_FALSE(lease.engaged());
  EXPECT_EQ(lease.request(lease_t::request_e::drop, t0), lease_t::step_e::stay);
  EXPECT_EQ(lease.request(lease_t::request_e::engage, t0), lease_t::step_e::take);
  EXPECT_EQ(lease.request(lease_t::request_e::drop, t0), lease_t::step_e::give_back);
}

TEST(FrametopHostInputCapture, AnEscapeHoldsUntilARelease) {
  lease_t lease;
  const auto t0 = clock_type::now();
  lease.request(lease_t::request_e::engage, t0);
  lease.escape();
  EXPECT_FALSE(lease.engaged());
  EXPECT_EQ(lease.request(lease_t::request_e::engage, t0), lease_t::step_e::still_escaped);
  EXPECT_EQ(lease.request(lease_t::request_e::drop, t0), lease_t::step_e::stay);
  EXPECT_EQ(lease.request(lease_t::request_e::engage, t0), lease_t::step_e::still_escaped);
  EXPECT_EQ(lease.request(lease_t::request_e::release, t0), lease_t::step_e::stay);
  EXPECT_EQ(lease.request(lease_t::request_e::engage, t0), lease_t::step_e::take);
}

TEST(FrametopHostInputCapture, ARefusedTakeAsksAgainOnTheNextEngage) {
  lease_t lease;
  const auto t0 = clock_type::now();
  lease.request(lease_t::request_e::engage, t0);
  lease.refuse();
  EXPECT_EQ(lease.request(lease_t::request_e::engage, t0), lease_t::step_e::take);
}

TEST(FrametopHostInputCapture, ItsMessagesAreOnesTheHostReads) {
  const auto keyboard = events_message('k', {{EV_KEY, KEY_A_CODE, 1}});
  EXPECT_EQ(keyboard, "ev k 1 30 1 0 0 0");
  EXPECT_TRUE(frametop_hostin::policy::events_payload(keyboard));
  const auto mouse = events_message('m', {{EV_REL, REL_X, -3}, {EV_REL, REL_Y, 7}, {EV_KEY, BTN_LEFT, 1}, {EV_REL, REL_WHEEL_HI_RES, -60}, {EV_REL, REL_HWHEEL, 1}});
  EXPECT_TRUE(frametop_hostin::policy::events_payload(mouse));
  // Every code the map makes is a keyboard key the host forwards.
  for (std::uint32_t scan = 0; scan <= 0xff; ++scan) {
    for (const bool extended : {false, true}) {
      const auto code = key_code(scan, extended, 0xff);
      if (code) {
        EXPECT_TRUE(frametop_hostin::policy::events_payload(events_message('k', {{EV_KEY, code, 1}}))) << scan;
      }
    }
  }
}
