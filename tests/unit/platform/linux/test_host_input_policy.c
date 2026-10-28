// vibepollo-host-input's policy: which devices it takes, where events go, the escape hotkey
// and the requests it accepts.
#include "../../../../packaging/linux/vibepollo-host-input-policy.h"

#undef NDEBUG
#include <assert.h>

static void takes_keyboards_and_mice_only(void) {
  const struct vibepollo_host_input_caps keyboard = {.keys_letters = true};
  const struct vibepollo_host_input_caps mouse = {.rel_xy = true, .btn_left = true};
  const struct vibepollo_host_input_caps touchpad = {.rel_xy = true, .btn_left = true, .abs_mt = true};
  const struct vibepollo_host_input_caps passthrough = {.keys_letters = true, .virtual_device = true};
  const struct vibepollo_host_input_caps virtual_mouse = {.rel_xy = true, .btn_left = true, .virtual_device = true};
  const struct vibepollo_host_input_caps motion_only = {.rel_xy = true};
  const struct vibepollo_host_input_caps nothing = {0};
  const struct vibepollo_host_input_caps keyboard_with_mouse = {.keys_letters = true, .rel_xy = true, .btn_left = true};
  const struct vibepollo_host_input_caps touch_keyboard = {.keys_letters = true, .abs_mt = true};
  assert(vibepollo_host_input_takes(&keyboard));
  assert(vibepollo_host_input_takes(&mouse));
  assert(!vibepollo_host_input_takes(&touchpad));
  assert(!vibepollo_host_input_takes(&passthrough));  // the host's own devices never loop back
  assert(!vibepollo_host_input_takes(&virtual_mouse));
  assert(!vibepollo_host_input_takes(&motion_only));
  assert(!vibepollo_host_input_takes(&nothing));
  assert(vibepollo_host_input_takes(&keyboard_with_mouse));
  assert(!vibepollo_host_input_takes(&touch_keyboard));
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
  takes_keyboards_and_mice_only();
  routes_events();
  escape_needs_both_shifts();
  accepts_exact_requests();
  leases_and_the_escape_latch();
  reads_capability_bits();
  return 0;
}
