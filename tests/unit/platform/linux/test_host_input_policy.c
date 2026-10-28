// vibepollo-host-input's policy: which devices it takes, where events go, the escape hotkey
// and the requests it accepts.
#include "../../../../packaging/linux/vibepollo-host-input-policy.h"

#undef NDEBUG
#include <assert.h>

#define BITS_FOR_TEST 8

static void takes_keyboards_mice_and_touchpads_only(void) {
  const struct vibepollo_host_input_caps keyboard = {.keys_letters = true};
  const struct vibepollo_host_input_caps mouse = {.rel_xy = true, .btn_left = true};
  const struct vibepollo_host_input_caps touchpad = {.btn_left = true, .abs_mt = true, .btn_tool_finger = true};
  const struct vibepollo_host_input_caps touchpad_with_rel = {.rel_xy = true, .btn_left = true, .abs_mt = true, .btn_tool_finger = true};
  const struct vibepollo_host_input_caps touchscreen = {.btn_left = true, .abs_mt = true, .btn_tool_finger = true, .direct = true};
  const struct vibepollo_host_input_caps touch_no_finger = {.rel_xy = true, .btn_left = true, .abs_mt = true};
  const struct vibepollo_host_input_caps virtual_touchpad = {.btn_left = true, .abs_mt = true, .btn_tool_finger = true, .virtual_device = true};
  const struct vibepollo_host_input_caps passthrough = {.keys_letters = true, .virtual_device = true};
  const struct vibepollo_host_input_caps virtual_mouse = {.rel_xy = true, .btn_left = true, .virtual_device = true};
  const struct vibepollo_host_input_caps motion_only = {.rel_xy = true};
  const struct vibepollo_host_input_caps nothing = {0};
  const struct vibepollo_host_input_caps keyboard_with_mouse = {.keys_letters = true, .rel_xy = true, .btn_left = true};
  const struct vibepollo_host_input_caps touch_keyboard = {.keys_letters = true, .abs_mt = true};
  const struct vibepollo_host_input_caps keyboard_touchpad = {.keys_letters = true, .btn_left = true, .abs_mt = true, .btn_tool_finger = true};
  const struct vibepollo_host_input_caps no_button_pad = {.abs_mt = true, .btn_tool_finger = true};
  assert(vibepollo_host_input_kind_of(&keyboard_touchpad) == VIBEPOLLO_HOST_INPUT_SKIP);  // its keys, the escape's too
  assert(vibepollo_host_input_kind_of(&no_button_pad) == VIBEPOLLO_HOST_INPUT_SKIP);
  assert(vibepollo_host_input_kind_of(&keyboard) == VIBEPOLLO_HOST_INPUT_EVDEV);
  assert(vibepollo_host_input_kind_of(&mouse) == VIBEPOLLO_HOST_INPUT_EVDEV);
  assert(vibepollo_host_input_kind_of(&touchpad) == VIBEPOLLO_HOST_INPUT_TOUCHPAD);
  assert(vibepollo_host_input_kind_of(&touchpad_with_rel) == VIBEPOLLO_HOST_INPUT_TOUCHPAD);  // never read raw
  assert(vibepollo_host_input_kind_of(&touchscreen) == VIBEPOLLO_HOST_INPUT_SKIP);
  assert(vibepollo_host_input_kind_of(&touch_no_finger) == VIBEPOLLO_HOST_INPUT_SKIP);
  assert(vibepollo_host_input_kind_of(&virtual_touchpad) == VIBEPOLLO_HOST_INPUT_SKIP);
  assert(vibepollo_host_input_kind_of(&passthrough) == VIBEPOLLO_HOST_INPUT_SKIP);  // the host's own devices never loop back
  assert(vibepollo_host_input_kind_of(&virtual_mouse) == VIBEPOLLO_HOST_INPUT_SKIP);
  assert(vibepollo_host_input_kind_of(&motion_only) == VIBEPOLLO_HOST_INPUT_SKIP);
  assert(vibepollo_host_input_kind_of(&nothing) == VIBEPOLLO_HOST_INPUT_SKIP);
  assert(vibepollo_host_input_kind_of(&keyboard_with_mouse) == VIBEPOLLO_HOST_INPUT_EVDEV);
  assert(vibepollo_host_input_kind_of(&touch_keyboard) == VIBEPOLLO_HOST_INPUT_SKIP);
}

static void parses_sysfs_bitmaps(void) {
  const size_t per = sizeof(unsigned long) * 8;
  unsigned long bits[BITS_FOR_TEST];
  assert(vibepollo_host_input_parse_bits("1b\n", bits, BITS_FOR_TEST));
  assert(bits[0] == 0x1b && bits[1] == 0);
  // The highest word first: "e520 10000 0 0 0 0" is a touchpad's key bits.
  assert(vibepollo_host_input_parse_bits("e520 10000 0 0 0 0\n", bits, BITS_FOR_TEST));
  assert(bits[5] == 0xe520 && bits[4] == 0x10000 && bits[0] == 0);
  if (per == 64) {
    assert(vibepollo_host_input_bit(bits, BITS_FOR_TEST, BTN_TOOL_FINGER));
    assert(vibepollo_host_input_bit(bits, BITS_FOR_TEST, BTN_LEFT));
    assert(!vibepollo_host_input_bit(bits, BITS_FOR_TEST, KEY_A));
  }
  // Words past the array are left out; garbage and empty files aren't bitmaps.
  assert(vibepollo_host_input_parse_bits("ff 0 1", bits, 2));
  assert(bits[0] == 1 && bits[1] == 0);
  assert(!vibepollo_host_input_parse_bits("", bits, 2));
  assert(!vibepollo_host_input_parse_bits("\n", bits, 2));
  assert(!vibepollo_host_input_parse_bits("1z 0", bits, 2));
  assert(bits[0] == 0 && bits[1] == 0);
  assert(vibepollo_host_input_parse_bits("0\n", bits, 2) && bits[0] == 0);  // an empty set
}

static void finds_keys_lost_in_a_drop(void) {
  const unsigned long held[2] = {0x6, 0x1}, down[2] = {0x4, 0x3};
  unsigned long lost[2];
  vibepollo_host_input_lost_keys(held, down, lost, 2);
  assert(lost[0] == 0x2);  // held, now up: it comes up on the Frame
  assert(lost[1] == 0);  // down meanwhile, never sent: stays here
}

static void reads_touchpad_settings(void) {
  struct vibepollo_host_input_touchpad settings;
  assert(vibepollo_host_input_parse_touchpad(NULL, &settings));
  assert(settings.tap && !settings.natural && !settings.clickfinger && settings.scroll == 1.0);
  assert(vibepollo_host_input_parse_touchpad("  tap=off natural=on click=fingers scroll=0.4 ", &settings));
  assert(!settings.tap && settings.natural && settings.clickfinger && settings.scroll == 0.4);
  assert(vibepollo_host_input_parse_touchpad("click=areas", &settings) && !settings.clickfinger);
  assert(vibepollo_host_input_parse_touchpad("tap=off\tscroll=2\n", &settings) && !settings.tap && settings.scroll == 2);
  const char *bad[] = {"tap=maybe", "scroll=0", "scroll=11", "scroll=x", "scroll=", "speed=2", "tap=onn",
                       "averyveryveryverylongwordthatdoesnotfit=1"};
  for (size_t i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
    assert(!vibepollo_host_input_parse_touchpad(bad[i], &settings));
    assert(settings.tap && !settings.natural && settings.scroll == 1.0);  // the defaults
  }
  assert(!vibepollo_host_input_parse_touchpad("natural=on bogus", &settings) && !settings.natural);
}

static void scrolls_in_120ths_and_notches(void) {
  struct vibepollo_host_input_scroll scroll = {0};
  int32_t notches;
  // 15 is a notch: 120ths, with whole notches as they add up.
  assert(vibepollo_host_input_scroll_step(&scroll, 15, 1.0, &notches) == 120 && notches == 1);
  assert(vibepollo_host_input_scroll_step(&scroll, 7.5, 1.0, &notches) == 60 && notches == 0);
  assert(vibepollo_host_input_scroll_step(&scroll, 7.5, 1.0, &notches) == 60 && notches == 1);
  // Fractions carry: 1/16 is 0.5 of a 120th.
  int32_t total = 0;
  for (int i = 0; i < 10; ++i) total += vibepollo_host_input_scroll_step(&scroll, 0.0625, 1.0, &notches);
  assert(total == 5);
  // The factor scales it; 0 ends the scroll and forgets what was toward a notch, and under a 120th.
  assert(vibepollo_host_input_scroll_step(&scroll, 0, 1.0, &notches) == 0 && notches == 0);
  assert(vibepollo_host_input_scroll_step(&scroll, 0.0625, 1.0, &notches) == 0);  // 0.5: under one
  assert(vibepollo_host_input_scroll_step(&scroll, 0, 1.0, &notches) == 0);
  assert(vibepollo_host_input_scroll_step(&scroll, 0.0625, 1.0, &notches) == 0);  // not 1: the half went
  assert(vibepollo_host_input_scroll_step(&scroll, 0, 1.0, &notches) == 0);
  assert(vibepollo_host_input_scroll_step(&scroll, 15, 0.4, &notches) == 48 && notches == 0);
  assert(vibepollo_host_input_scroll_step(&scroll, 0, 1.0, &notches) == 0);
  assert(vibepollo_host_input_scroll_step(&scroll, 10, 0.4, &notches) == 32 && notches == 0);
  // Down and up: negatives work the same way.
  assert(vibepollo_host_input_scroll_step(&scroll, 0, 1.0, &notches) == 0);
  assert(vibepollo_host_input_scroll_step(&scroll, -30, 1.0, &notches) == -240 && notches == -2);
  // A change of direction counts back from where it was.
  assert(vibepollo_host_input_scroll_step(&scroll, 0, 1.0, &notches) == 0);
  assert(vibepollo_host_input_scroll_step(&scroll, 7.5, 1.0, &notches) == 60 && notches == 0);
  assert(vibepollo_host_input_scroll_step(&scroll, -15, 1.0, &notches) == -120 && notches == 0);
  assert(scroll.partial == -60);
  assert(vibepollo_host_input_scroll_step(&scroll, -0.0625, 1.0, &notches) == 0 && scroll.rest == -0.5);
  assert(vibepollo_host_input_scroll_step(&scroll, -0.0625, 1.0, &notches) == -1);
}

static void carries_motion_under_a_count(void) {
  double rest = 0;
  assert(vibepollo_host_input_carry(&rest) == 0);
  rest += 0.75;
  assert(vibepollo_host_input_carry(&rest) == 0 && rest == 0.75);
  rest += 0.75;
  assert(vibepollo_host_input_carry(&rest) == 1 && rest == 0.5);
  rest = -2.25;
  assert(vibepollo_host_input_carry(&rest) == -2 && rest == -0.25);
}

static void routes_events(void) {
  assert(vibepollo_host_input_route_event(EV_KEY, KEY_A, 1) == VIBEPOLLO_HOST_INPUT_KEYBOARD);
  assert(vibepollo_host_input_route_event(EV_KEY, KEY_A, 0) == VIBEPOLLO_HOST_INPUT_KEYBOARD);
  assert(vibepollo_host_input_route_event(EV_KEY, KEY_A, 2) == VIBEPOLLO_HOST_INPUT_DROP);  // repeat
  assert(vibepollo_host_input_route_event(EV_KEY, KEY_F24, 1) == VIBEPOLLO_HOST_INPUT_KEYBOARD);
  assert(vibepollo_host_input_route_event(EV_KEY, BTN_LEFT, 1) == VIBEPOLLO_HOST_INPUT_MOUSE);
  assert(vibepollo_host_input_route_event(EV_KEY, BTN_TASK, 0) == VIBEPOLLO_HOST_INPUT_MOUSE);
  const uint16_t buttons[] = {BTN_RIGHT, BTN_MIDDLE, BTN_SIDE, BTN_EXTRA, BTN_FORWARD, BTN_BACK};
  for (size_t i = 0; i < sizeof buttons / sizeof buttons[0]; ++i)
    assert(vibepollo_host_input_route_event(EV_KEY, buttons[i], 1) == VIBEPOLLO_HOST_INPUT_MOUSE);
  // The button ranges' edges: BTN_MISC up to KEY_OK isn't a keyboard's.
  assert(vibepollo_host_input_route_event(EV_KEY, BTN_MISC - 1, 1) == VIBEPOLLO_HOST_INPUT_KEYBOARD);
  assert(vibepollo_host_input_route_event(EV_KEY, BTN_MISC, 1) == VIBEPOLLO_HOST_INPUT_DROP);
  assert(vibepollo_host_input_route_event(EV_KEY, BTN_MOUSE - 1, 1) == VIBEPOLLO_HOST_INPUT_DROP);
  assert(vibepollo_host_input_route_event(EV_KEY, BTN_TASK + 1, 1) == VIBEPOLLO_HOST_INPUT_DROP);
  assert(vibepollo_host_input_route_event(EV_KEY, KEY_OK - 1, 1) == VIBEPOLLO_HOST_INPUT_DROP);
  assert(vibepollo_host_input_route_event(EV_KEY, KEY_OK, 1) == VIBEPOLLO_HOST_INPUT_KEYBOARD);
  assert(vibepollo_host_input_route_event(EV_KEY, BTN_TRIGGER_HAPPY - 1, 1) == VIBEPOLLO_HOST_INPUT_KEYBOARD);
  assert(vibepollo_host_input_route_event(EV_KEY, BTN_SOUTH, 1) == VIBEPOLLO_HOST_INPUT_DROP);
  assert(vibepollo_host_input_route_event(EV_KEY, BTN_TOUCH, 1) == VIBEPOLLO_HOST_INPUT_DROP);
  assert(vibepollo_host_input_route_event(EV_KEY, BTN_TRIGGER_HAPPY1, 1) == VIBEPOLLO_HOST_INPUT_DROP);
  assert(vibepollo_host_input_route_event(EV_KEY, 0, 1) == VIBEPOLLO_HOST_INPUT_DROP);
  assert(vibepollo_host_input_route_event(EV_KEY, KEY_MAX + 1, 1) == VIBEPOLLO_HOST_INPUT_DROP);
  // Volume, media and brightness stay here, repeats too.
  assert(vibepollo_host_input_route_event(EV_KEY, KEY_VOLUMEUP, 1) == VIBEPOLLO_HOST_INPUT_KEEP);
  assert(vibepollo_host_input_route_event(EV_KEY, KEY_VOLUMEUP, 2) == VIBEPOLLO_HOST_INPUT_KEEP);
  assert(vibepollo_host_input_route_event(EV_KEY, KEY_BRIGHTNESSDOWN, 1) == VIBEPOLLO_HOST_INPUT_KEEP);
  assert(vibepollo_host_input_route_event(EV_KEY, KEY_PLAYPAUSE, 0) == VIBEPOLLO_HOST_INPUT_KEEP);
  const uint16_t kept[] = {KEY_MUTE, KEY_VOLUMEDOWN, KEY_VOLUMEUP, KEY_MICMUTE, KEY_PLAYPAUSE, KEY_PLAYCD,
                           KEY_PAUSECD, KEY_STOPCD, KEY_NEXTSONG, KEY_PREVIOUSSONG, KEY_FASTFORWARD, KEY_REWIND,
                           KEY_BRIGHTNESSDOWN, KEY_BRIGHTNESSUP, KEY_KBDILLUMDOWN, KEY_KBDILLUMUP, KEY_KBDILLUMTOGGLE};
  unsigned kept_count = 0;
  for (unsigned code = 1; code <= KEY_MAX; ++code) kept_count += vibepollo_host_input_kept_key(code);
  assert(kept_count == sizeof kept / sizeof kept[0]);  // these and no others
  for (size_t i = 0; i < sizeof kept / sizeof kept[0]; ++i) assert(vibepollo_host_input_kept_key(kept[i]));
  assert(vibepollo_host_input_route_event(EV_REL, REL_X, -3) == VIBEPOLLO_HOST_INPUT_MOUSE);
  assert(vibepollo_host_input_route_event(EV_REL, REL_WHEEL_HI_RES, 120) == VIBEPOLLO_HOST_INPUT_MOUSE);
  assert(vibepollo_host_input_route_event(EV_REL, REL_DIAL, 1) == VIBEPOLLO_HOST_INPUT_DROP);
  assert(vibepollo_host_input_route_event(EV_ABS, ABS_X, 1) == VIBEPOLLO_HOST_INPUT_DROP);
  assert(vibepollo_host_input_route_event(EV_MSC, MSC_SCAN, 30) == VIBEPOLLO_HOST_INPUT_DROP);
  assert(vibepollo_host_input_route_event(EV_LED, LED_CAPSL, 1) == VIBEPOLLO_HOST_INPUT_DROP);
}

static void escape_needs_both_shifts(void) {
  struct vibepollo_host_input_escape escape = {0};
  assert(!vibepollo_host_input_escape_step(&escape, EV_KEY, KEY_ESC, 1));
  assert(!vibepollo_host_input_escape_step(&escape, EV_KEY, KEY_LEFTSHIFT, 1));
  assert(!vibepollo_host_input_escape_step(&escape, EV_KEY, KEY_ESC, 1));  // one Shift
  assert(!vibepollo_host_input_escape_step(&escape, EV_KEY, KEY_RIGHTSHIFT, 1));
  assert(!vibepollo_host_input_escape_step(&escape, EV_KEY, KEY_ESC, 2));  // a repeat isn't a press
  assert(!vibepollo_host_input_escape_step(&escape, EV_REL, KEY_ESC, 1));
  assert(vibepollo_host_input_escape_step(&escape, EV_KEY, KEY_ESC, 1));
  assert(!vibepollo_host_input_escape_step(&escape, EV_KEY, KEY_RIGHTSHIFT, 0));
  assert(!vibepollo_host_input_escape_step(&escape, EV_KEY, KEY_ESC, 1));  // released one
  assert(!vibepollo_host_input_escape_step(&escape, EV_KEY, KEY_RIGHTSHIFT, 1));
  assert(vibepollo_host_input_escape_step(&escape, EV_KEY, KEY_ESC, 1));  // both again
  assert(!vibepollo_host_input_escape_step(&escape, EV_KEY, KEY_LEFTSHIFT, 0));
  assert(!vibepollo_host_input_escape_step(&escape, EV_KEY, KEY_ESC, 1));
}

static void leases_and_the_escape_latch(void) {
  struct vibepollo_host_input_lease lease = {0};
  assert(vibepollo_host_input_lease_request(&lease, VIBEPOLLO_HOST_INPUT_REQUEST_ENGAGE, 1000) == VIBEPOLLO_HOST_INPUT_TAKE);
  assert(lease.engaged && lease.until == 1000 + VIBEPOLLO_HOST_INPUT_LEASE_MS);
  // A renewal keeps them and moves the end.
  assert(vibepollo_host_input_lease_request(&lease, VIBEPOLLO_HOST_INPUT_REQUEST_ENGAGE, 1500) == VIBEPOLLO_HOST_INPUT_STAY);
  assert(lease.until == 1500 + VIBEPOLLO_HOST_INPUT_LEASE_MS);
  assert(!vibepollo_host_input_lease_lapsed(&lease, 1500 + VIBEPOLLO_HOST_INPUT_LEASE_MS - 1));
  assert(vibepollo_host_input_lease_lapsed(&lease, 1500 + VIBEPOLLO_HOST_INPUT_LEASE_MS));
  assert(!lease.engaged);
  assert(!vibepollo_host_input_lease_lapsed(&lease, 99999));  // once

  // Release and drop give them back; with nothing taken they do nothing.
  assert(vibepollo_host_input_lease_request(&lease, VIBEPOLLO_HOST_INPUT_REQUEST_ENGAGE, 5000) == VIBEPOLLO_HOST_INPUT_TAKE);
  assert(vibepollo_host_input_lease_request(&lease, VIBEPOLLO_HOST_INPUT_REQUEST_DROP, 5100) == VIBEPOLLO_HOST_INPUT_GIVE_BACK);
  assert(vibepollo_host_input_lease_request(&lease, VIBEPOLLO_HOST_INPUT_REQUEST_DROP, 5200) == VIBEPOLLO_HOST_INPUT_STAY);
  assert(vibepollo_host_input_lease_request(&lease, VIBEPOLLO_HOST_INPUT_REQUEST_RELEASE, 5300) == VIBEPOLLO_HOST_INPUT_STAY);

  // The escape: refused until a release, which drop isn't.
  assert(vibepollo_host_input_lease_request(&lease, VIBEPOLLO_HOST_INPUT_REQUEST_ENGAGE, 6000) == VIBEPOLLO_HOST_INPUT_TAKE);
  vibepollo_host_input_lease_escape(&lease);
  assert(!lease.engaged && lease.escaped);
  assert(!vibepollo_host_input_lease_lapsed(&lease, 99999));
  assert(vibepollo_host_input_lease_request(&lease, VIBEPOLLO_HOST_INPUT_REQUEST_ENGAGE, 6100) == VIBEPOLLO_HOST_INPUT_STILL_ESCAPED);
  assert(!lease.engaged);
  assert(vibepollo_host_input_lease_request(&lease, VIBEPOLLO_HOST_INPUT_REQUEST_DROP, 6200) == VIBEPOLLO_HOST_INPUT_STAY);
  assert(vibepollo_host_input_lease_request(&lease, VIBEPOLLO_HOST_INPUT_REQUEST_ENGAGE, 6300) == VIBEPOLLO_HOST_INPUT_STILL_ESCAPED);
  assert(vibepollo_host_input_lease_request(&lease, VIBEPOLLO_HOST_INPUT_REQUEST_RELEASE, 6400) == VIBEPOLLO_HOST_INPUT_STAY);
  assert(!lease.escaped);
  assert(vibepollo_host_input_lease_request(&lease, VIBEPOLLO_HOST_INPUT_REQUEST_ENGAGE, 6500) == VIBEPOLLO_HOST_INPUT_TAKE);
}

static void accepts_exact_requests(void) {
  assert(vibepollo_host_input_parse_request("engage", 6) == VIBEPOLLO_HOST_INPUT_REQUEST_ENGAGE);
  assert(vibepollo_host_input_parse_request("release", 7) == VIBEPOLLO_HOST_INPUT_REQUEST_RELEASE);
  assert(vibepollo_host_input_parse_request("drop", 4) == VIBEPOLLO_HOST_INPUT_REQUEST_DROP);
  assert(vibepollo_host_input_parse_request("engage\n", 7) == VIBEPOLLO_HOST_INPUT_REQUEST_INVALID);
  assert(vibepollo_host_input_parse_request("engag", 5) == VIBEPOLLO_HOST_INPUT_REQUEST_INVALID);
  assert(vibepollo_host_input_parse_request("dropx", 5) == VIBEPOLLO_HOST_INPUT_REQUEST_INVALID);
  assert(vibepollo_host_input_parse_request("", 0) == VIBEPOLLO_HOST_INPUT_REQUEST_INVALID);
}

static void reads_capability_bits(void) {
  unsigned long bits[2] = {0};
  const size_t per = sizeof(unsigned long) * 8;
  bits[1] = 1UL << 3;
  assert(vibepollo_host_input_bit(bits, 2, (unsigned) per + 3));
  assert(!vibepollo_host_input_bit(bits, 2, (unsigned) per + 4));
  assert(!vibepollo_host_input_bit(bits, 1, (unsigned) per + 3));  // past the array
}

int main(void) {
  takes_keyboards_mice_and_touchpads_only();
  parses_sysfs_bitmaps();
  finds_keys_lost_in_a_drop();
  reads_touchpad_settings();
  scrolls_in_120ths_and_notches();
  carries_motion_under_a_count();
  routes_events();
  escape_needs_both_shifts();
  accepts_exact_requests();
  leases_and_the_escape_latch();
  reads_capability_bits();
  return 0;
}
