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
  assert(vibepollo_host_input_takes(&keyboard));
  assert(vibepollo_host_input_takes(&mouse));
  assert(!vibepollo_host_input_takes(&touchpad));
  assert(!vibepollo_host_input_takes(&passthrough));  // the host's own devices never loop back
  assert(!vibepollo_host_input_takes(&virtual_mouse));
  assert(!vibepollo_host_input_takes(&motion_only));
  assert(!vibepollo_host_input_takes(&nothing));
}

static void routes_events(void) {
  assert(vibepollo_host_input_route_event(EV_KEY, KEY_A, 1) == VIBEPOLLO_HOST_INPUT_KEYBOARD);
  assert(vibepollo_host_input_route_event(EV_KEY, KEY_A, 0) == VIBEPOLLO_HOST_INPUT_KEYBOARD);
  assert(vibepollo_host_input_route_event(EV_KEY, KEY_A, 2) == VIBEPOLLO_HOST_INPUT_DROP);  // repeat
  assert(vibepollo_host_input_route_event(EV_KEY, KEY_F24, 1) == VIBEPOLLO_HOST_INPUT_KEYBOARD);
  assert(vibepollo_host_input_route_event(EV_KEY, BTN_LEFT, 1) == VIBEPOLLO_HOST_INPUT_MOUSE);
  assert(vibepollo_host_input_route_event(EV_KEY, BTN_TASK, 0) == VIBEPOLLO_HOST_INPUT_MOUSE);
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
  reads_capability_bits();
  return 0;
}
