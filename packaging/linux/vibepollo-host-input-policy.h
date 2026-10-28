#pragma once

// Frametop host input (vibepollo-host-input.c): which devices it takes, where each event goes,
// and the escape hotkey. Pure functions, so the component tests can check them.

#include <linux/input-event-codes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// A Frametop lease keeps the devices for this long; the Frame renews it every 500 ms.
#define VIBEPOLLO_HOST_INPUT_LEASE_MS 1500
// Devices are looked for again this often while engaged (hotplug), from sysfs: opening and closing
// an event node can block for tens of milliseconds, longer than a fast mouse's kernel buffer lasts.
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

// A sysfs capabilities or properties file ("120013", "1 0 0 3"): hex words of the kernel's
// unsigned long, the highest first. False if it isn't one; bits past count are left out.
static inline bool vibepollo_host_input_parse_bits(const char *text, unsigned long *bits, size_t count) {
  memset(bits, 0, count * sizeof *bits);
  size_t words = 0;
  for (const char *p = text; *p;) {
    while (*p == ' ') ++p;
    if (!*p || *p == '\n') break;
    char *end;
    (void) strtoul(p, &end, 16);
    if (end == p || (*end && *end != ' ' && *end != '\n')) return false;
    ++words, p = end;
  }
  size_t index = words;
  for (const char *p = text; index;) {
    while (*p == ' ') ++p;
    char *end;
    const unsigned long word = strtoul(p, &end, 16);
    if (--index < count) bits[index] = word;
    p = end;
  }
  return words > 0;
}

// What a device is, from its capability bits, and whether it is a uinput device.
struct vibepollo_host_input_caps {
  bool virtual_device;  // under /devices/virtual/input: uinput, such as the host's own passthrough
                        // devices (uhid ones, Bluetooth keyboards and mice, are real)
  bool keys_letters;  // KEY_A, KEY_Z, KEY_SPACE and KEY_ENTER
  bool rel_xy;  // REL_X and REL_Y
  bool btn_left;
  bool abs_mt;  // a touchpad or touchscreen (ABS_MT_POSITION_X)
  bool btn_tool_finger;  // a touchpad's (BTN_TOOL_FINGER)
  bool direct;  // INPUT_PROP_DIRECT: a touchscreen or tablet, on its screen
};

enum vibepollo_host_input_kind {
  VIBEPOLLO_HOST_INPUT_SKIP = 0,
  VIBEPOLLO_HOST_INPUT_EVDEV,  // a keyboard or mouse, read here
  VIBEPOLLO_HOST_INPUT_TOUCHPAD,  // read through libinput, which turns it into a mouse
};

// Keyboards, mice and touchpads that aren't ours. A touchscreen stays here, and a touch surface
// is never read as a mouse: its relative node (REL_X/REL_Y with BTN_LEFT, no ABS_MT) is one.
static inline enum vibepollo_host_input_kind vibepollo_host_input_kind_of(const struct vibepollo_host_input_caps *caps) {
  if (caps->virtual_device) return VIBEPOLLO_HOST_INPUT_SKIP;
  if (caps->abs_mt) {
    // A keyboard with a touch surface on one node stays here: libinput would read it as a
    // touchpad only, and its keys (the escape's among them) would be lost.
    const bool touchpad = caps->btn_tool_finger && caps->btn_left && !caps->direct && !caps->keys_letters;
    return touchpad ? VIBEPOLLO_HOST_INPUT_TOUCHPAD : VIBEPOLLO_HOST_INPUT_SKIP;
  }
  return caps->keys_letters || (caps->rel_xy && caps->btn_left) ? VIBEPOLLO_HOST_INPUT_EVDEV : VIBEPOLLO_HOST_INPUT_SKIP;
}

// After the kernel dropped a device's events (SYN_DROPPED): which of the keys that went down to
// the Frame are up now (held and not in EVIOCGKEY's), into lost.
static inline void vibepollo_host_input_lost_keys(
  const unsigned long *held, const unsigned long *down, unsigned long *lost, size_t count
) {
  for (size_t i = 0; i < count; ++i) lost[i] = held[i] & ~down[i];
}

// A touchpad's settings (VIBEPOLLO_HOST_INPUT_TOUCHPAD in the service's environment, words
// "tap=on|off natural=on|off click=fingers|areas scroll=FACTOR", apart by spaces, tabs or
// newlines). The defaults are Hyprland's.
struct vibepollo_host_input_touchpad {
  bool tap;  // tap to click, and tap and drag
  bool natural;  // natural scrolling
  bool clickfinger;  // a click's button by its finger count, not where it is pressed
  double scroll;  // scroll speed, 0.05 to 10
};

static inline struct vibepollo_host_input_touchpad vibepollo_host_input_touchpad_defaults(void) {
  return (struct vibepollo_host_input_touchpad) {.tap = true, .natural = false, .clickfinger = false, .scroll = 1.0};
}

// False on a word it doesn't know; *out keeps the defaults then.
static inline bool vibepollo_host_input_parse_touchpad(const char *text, struct vibepollo_host_input_touchpad *out) {
  *out = vibepollo_host_input_touchpad_defaults();
  struct vibepollo_host_input_touchpad parsed = *out;
  for (const char *p = text ? text : ""; *p;) {
    while (*p == ' ' || *p == '\t' || *p == '\n') ++p;
    if (!*p) break;
    size_t length = 0;
    while (p[length] && p[length] != ' ' && p[length] != '\t' && p[length] != '\n') ++length;
    char word[32];
    if (length >= sizeof word) return false;
    memcpy(word, p, length);
    word[length] = 0;
    p += length;
    if (!strcmp(word, "tap=on")) parsed.tap = true;
    else if (!strcmp(word, "tap=off")) parsed.tap = false;
    else if (!strcmp(word, "natural=on")) parsed.natural = true;
    else if (!strcmp(word, "natural=off")) parsed.natural = false;
    else if (!strcmp(word, "click=fingers")) parsed.clickfinger = true;
    else if (!strcmp(word, "click=areas")) parsed.clickfinger = false;
    else if (!strncmp(word, "scroll=", 7)) {
      char *end;
      const double factor = strtod(word + 7, &end);
      if (end == word + 7 || *end || !(factor >= 0.05 && factor <= 10)) return false;
      parsed.scroll = factor;
    } else {
      return false;
    }
  }
  *out = parsed;
  return true;
}

// The whole counts in *rest, which keeps what's under one for the next event.
static inline int32_t vibepollo_host_input_carry(double *rest) {
  const int32_t whole = (int32_t) *rest;  // toward zero
  *rest -= whole;
  return whole;
}

// A touchpad's finger scrolling, from libinput (15 to a wheel notch, as Wayland's axis events), in
// the wheel's 120ths and its whole notches. Its sign is libinput's (down and right positive).
struct vibepollo_host_input_scroll {
  double rest;  // under a 120th
  int32_t partial;  // 120ths toward the next notch
};

// The 120ths for this value; *notches the whole notches it completes. 0 ends the scroll.
static inline int32_t vibepollo_host_input_scroll_step(
  struct vibepollo_host_input_scroll *scroll, double value, double factor, int32_t *notches
) {
  *notches = 0;
  if (value == 0) {
    *scroll = (struct vibepollo_host_input_scroll) {0};
    return 0;
  }
  scroll->rest += value * 8.0 * factor;
  const int32_t whole = vibepollo_host_input_carry(&scroll->rest);
  scroll->partial += whole;
  *notches = scroll->partial / 120;
  scroll->partial -= *notches * 120;
  return whole;
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
  bool escape;  // held, for catching up after dropped events
};

// Follows one key event from a taken keyboard; true when it completes the escape hotkey.
static inline bool vibepollo_host_input_escape_step(
  struct vibepollo_host_input_escape *escape, uint16_t type, uint16_t code, int32_t value
) {
  if (type != EV_KEY) return false;
  if (code == KEY_LEFTSHIFT) escape->left_shift = value != 0;
  else if (code == KEY_RIGHTSHIFT) escape->right_shift = value != 0;
  else if (code == KEY_ESC) {
    escape->escape = value != 0;
    return value == 1 && escape->left_shift && escape->right_shift;
  }
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

// The lease and the escape latch, apart from the devices. While escaped no engage takes them;
// release lifts that, drop doesn't.
struct vibepollo_host_input_lease {
  bool engaged;
  bool escaped;
  int64_t until;  // ms, CLOCK_MONOTONIC
};

enum vibepollo_host_input_step {
  VIBEPOLLO_HOST_INPUT_STAY = 0,  // nothing changes
  VIBEPOLLO_HOST_INPUT_TAKE,  // take the devices, and say "state engaged"
  VIBEPOLLO_HOST_INPUT_GIVE_BACK,  // give them back, and say "state released host"
  VIBEPOLLO_HOST_INPUT_STILL_ESCAPED,  // say "state released escape"
};

static inline enum vibepollo_host_input_step vibepollo_host_input_lease_request(
  struct vibepollo_host_input_lease *lease, enum vibepollo_host_input_request request, int64_t now
) {
  switch (request) {
    case VIBEPOLLO_HOST_INPUT_REQUEST_ENGAGE:
      if (lease->escaped) return VIBEPOLLO_HOST_INPUT_STILL_ESCAPED;
      lease->until = now + VIBEPOLLO_HOST_INPUT_LEASE_MS;
      if (lease->engaged) return VIBEPOLLO_HOST_INPUT_STAY;
      lease->engaged = true;
      return VIBEPOLLO_HOST_INPUT_TAKE;
    case VIBEPOLLO_HOST_INPUT_REQUEST_RELEASE:
      lease->escaped = false;
      // fall through
    case VIBEPOLLO_HOST_INPUT_REQUEST_DROP:
      if (!lease->engaged) return VIBEPOLLO_HOST_INPUT_STAY;
      lease->engaged = false;
      return VIBEPOLLO_HOST_INPUT_GIVE_BACK;
    default:
      return VIBEPOLLO_HOST_INPUT_STAY;
  }
}

// True when the lease ran out now: give the devices back ("state released lease").
static inline bool vibepollo_host_input_lease_lapsed(struct vibepollo_host_input_lease *lease, int64_t now) {
  if (!lease->engaged || now < lease->until) return false;
  lease->engaged = false;
  return true;
}

// The hotkey: give them back ("state released escape") and refuse engage until a release.
static inline void vibepollo_host_input_lease_escape(struct vibepollo_host_input_lease *lease) {
  lease->engaged = false;
  lease->escaped = true;
}
