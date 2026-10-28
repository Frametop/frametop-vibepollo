#pragma once

// Frametop host input (vibepollo-host-input.c): which devices it takes, where each event goes,
// and the escape hotkey. Pure functions, so the component tests can check them.

#include <linux/input-event-codes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// A Frametop lease keeps the devices for this long; the Frame renews it every 500 ms.
#define VIBEPOLLO_HOST_INPUT_LEASE_MS 1500
// Devices are looked for again this often while engaged (hotplug).
#define VIBEPOLLO_HOST_INPUT_RESCAN_MS 1000
#define VIBEPOLLO_HOST_INPUT_MAX_DEVICES 32
// Events in one message to the host; a longer report goes in several.
#define VIBEPOLLO_HOST_INPUT_MAX_BATCH 64

enum vibepollo_host_input_route {
  VIBEPOLLO_HOST_INPUT_DROP = 0,
  VIBEPOLLO_HOST_INPUT_KEYBOARD,  // a key, to the Frame's host keyboard
  VIBEPOLLO_HOST_INPUT_MOUSE,  // a mouse button or relative motion, to its host mouse
  VIBEPOLLO_HOST_INPUT_KEEP,  // stays on this computer: typed again on our own device
};

static inline bool vibepollo_host_input_bit(const unsigned long *bits, size_t count, unsigned int bit) {
  const size_t per = sizeof(unsigned long) * 8;
  return bit / per < count && (bits[bit / per] >> (bit % per)) & 1UL;
}

// What a device is, from its capability bits (EVIOCGBIT), and whether it is a uinput device.
struct vibepollo_host_input_caps {
  bool virtual_device;  // under /devices/virtual: uinput, such as the host's own passthrough devices
  bool keys_letters;  // KEY_A, KEY_Z, KEY_SPACE and KEY_ENTER
  bool rel_xy;  // REL_X and REL_Y
  bool btn_left;
  bool abs_mt;  // a touchpad or touchscreen (ABS_MT_POSITION_X)
};

// A keyboard or a mouse that isn't ours and isn't a touch surface. A touchpad's relative mouse
// node (REL_X/REL_Y with BTN_LEFT) counts as a mouse; its multitouch node stays here.
static inline bool vibepollo_host_input_takes(const struct vibepollo_host_input_caps *caps) {
  if (caps->virtual_device || caps->abs_mt) return false;
  return caps->keys_letters || (caps->rel_xy && caps->btn_left);
}

// Volume, media and brightness keys never go to the Frame.
static inline bool vibepollo_host_input_kept_key(unsigned int code) {
  switch (code) {
    case KEY_MUTE: case KEY_VOLUMEDOWN: case KEY_VOLUMEUP: case KEY_MICMUTE:
    case KEY_PLAYPAUSE: case KEY_PLAYCD: case KEY_PAUSECD: case KEY_STOPCD:
    case KEY_NEXTSONG: case KEY_PREVIOUSSONG: case KEY_FASTFORWARD: case KEY_REWIND:
    case KEY_BRIGHTNESSDOWN: case KEY_BRIGHTNESSUP: case KEY_KBDILLUMDOWN: case KEY_KBDILLUMUP:
    case KEY_KBDILLUMTOGGLE:
      return true;
    default:
      return false;
  }
}

static inline enum vibepollo_host_input_route vibepollo_host_input_route_event(
  uint16_t type, uint16_t code, int32_t value
) {
  if (type == EV_KEY) {
    if (code == 0 || code > KEY_MAX) return VIBEPOLLO_HOST_INPUT_DROP;
    if (vibepollo_host_input_kept_key(code)) return VIBEPOLLO_HOST_INPUT_KEEP;
    // The Frame types its own repeats.
    if (value != 0 && value != 1) return VIBEPOLLO_HOST_INPUT_DROP;
    if (code >= BTN_MOUSE && code <= BTN_TASK) return VIBEPOLLO_HOST_INPUT_MOUSE;
    // Joystick, gamepad and digitizer buttons aren't a keyboard's.
    if (code >= BTN_MISC && code < KEY_OK) return VIBEPOLLO_HOST_INPUT_DROP;
    if (code >= BTN_TRIGGER_HAPPY) return VIBEPOLLO_HOST_INPUT_DROP;
    return VIBEPOLLO_HOST_INPUT_KEYBOARD;
  }
  if (type == EV_REL) {
    switch (code) {
      case REL_X: case REL_Y: case REL_WHEEL: case REL_HWHEEL:
      case REL_WHEEL_HI_RES: case REL_HWHEEL_HI_RES:
        return VIBEPOLLO_HOST_INPUT_MOUSE;
      default:
        return VIBEPOLLO_HOST_INPUT_DROP;
    }
  }
  return VIBEPOLLO_HOST_INPUT_DROP;
}

// Both Shifts held and Escape pressed: this computer takes its keyboard and mouse back.
struct vibepollo_host_input_escape {
  bool left_shift;
  bool right_shift;
};

// Follows one key event from a taken keyboard; true when it completes the escape hotkey.
static inline bool vibepollo_host_input_escape_step(
  struct vibepollo_host_input_escape *escape, uint16_t type, uint16_t code, int32_t value
) {
  if (type != EV_KEY) return false;
  if (code == KEY_LEFTSHIFT) escape->left_shift = value != 0;
  else if (code == KEY_RIGHTSHIFT) escape->right_shift = value != 0;
  else if (code == KEY_ESC && value == 1) return escape->left_shift && escape->right_shift;
  return false;
}

// The host's messages: exactly "engage", "release" or "drop". Anything else ends the connection.
enum vibepollo_host_input_request {
  VIBEPOLLO_HOST_INPUT_REQUEST_INVALID = 0,
  VIBEPOLLO_HOST_INPUT_REQUEST_ENGAGE,
  VIBEPOLLO_HOST_INPUT_REQUEST_RELEASE,
  VIBEPOLLO_HOST_INPUT_REQUEST_DROP,
};

static inline enum vibepollo_host_input_request vibepollo_host_input_parse_request(const char *data, size_t length) {
  if (length == 6 && !memcmp(data, "engage", 6)) return VIBEPOLLO_HOST_INPUT_REQUEST_ENGAGE;
  if (length == 7 && !memcmp(data, "release", 7)) return VIBEPOLLO_HOST_INPUT_REQUEST_RELEASE;
  if (length == 4 && !memcmp(data, "drop", 4)) return VIBEPOLLO_HOST_INPUT_REQUEST_DROP;
  return VIBEPOLLO_HOST_INPUT_REQUEST_INVALID;
}
