// vibepollo-host-input: lends this computer's keyboards and mice to one Frametop client
// (Frametop's docs/host-input.md). Socket-activated by vibepollo-host-input.socket
// (SOCK_SEQPACKET, Accept=no); only the machine host (the vibepollo user) may connect, and it
// serves one connection, then exits.
//
// From the host, one message per packet:
//   engage    a lease: keyboards and mice are taken until VIBEPOLLO_HOST_INPUT_LEASE_MS after
//             the last one
//   release   give them back now, and lift an escape
//   drop      give them back now, but keep an escape (the client went, or its lease lapsed)
// To the host:
//   state engaged | state released REASON     on every change
//   done                                       after each release and drop, so the host knows
//                                              everything before it belongs to the lease that ended
//   ev k|m T C V [T C V]...                   events of one device report for the Frame's host
//                                              keyboard (k) or mouse (m), ending with 0 0 0
//
// While engaged, every keyboard, mouse and touchpad that isn't a uinput device is grabbed
// (EVIOCGRAB), so the desktop sees nothing from it, and only once no key is held on it. Devices
// are found from sysfs, and only those taken are opened. A touchpad goes through libinput (when
// built with it), which grabs it and turns it into mouse records: its accelerated motion, its
// buttons (with tap to click) and its finger scrolling, in 120ths and whole notches. Touchscreens
// stay here. Its settings: VIBEPOLLO_HOST_INPUT_TOUCHPAD (vibepollo-host-input-policy.h).
// Release reasons: lease (no engage in time), host (release or drop), escape (both Shifts and Escape on
// this computer, the Shifts on any keyboards), stalled (the host stopped reading), closed. After
// escape, engage is refused until the host sends release (the Frame's headset came off, or its
// switch went off). When the kernel drops a device's events (SYN_DROPPED), the keys that came up
// meanwhile come up on the Frame (EVIOCGKEY), and ones that went down stay here. Keys and buttons
// that went to the Frame come up there when the devices go back. Volume, media and brightness keys
// stay here: a taken keyboard's are typed again on a uinput device of our own. Key codes are never
// logged.
//
// The host can't read input without taking it: while it has a lease, this computer's keyboard and
// mouse visibly stop working here.

#define _GNU_SOURCE

#include "vibepollo-host-input-policy.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>

#ifdef VIBEPOLLO_HOST_INPUT_LIBINPUT
  #include <libinput.h>
#endif

#define BITS(n) (((n) + sizeof(unsigned long) * 8 - 1) / (sizeof(unsigned long) * 8))

struct device {
  int fd;  // a touchpad's is only for EVIOCGKEY and unplugging: libinput reads its own
  dev_t rdev;
  bool grabbed;
  bool touchpad;
  bool pad_failed;  // libinput couldn't add it: left alone until it is plugged in again
  char name[64];
  char path[32];
  unsigned long held[BITS(KEY_CNT)];  // keys and buttons that went down to the Frame from here
  unsigned long kept[BITS(KEY_CNT)];  // kept keys it has down on our own device
  struct vibepollo_host_input_escape escape;  // its Shifts
#ifdef VIBEPOLLO_HOST_INPUT_LIBINPUT
  struct libinput_device *pad;  // while grabbed
  double rest_x, rest_y;  // motion under a count
  struct vibepollo_host_input_scroll scroll_v, scroll_h;
#endif
};

static struct device devices[VIBEPOLLO_HOST_INPUT_MAX_DEVICES];
static size_t device_count;
static int host = -1;
static int keep = -1;  // our uinput device for the kept keys
static struct vibepollo_host_input_lease lease;
static int64_t rescan_at;
static bool stalled;  // a message couldn't go: the host stopped reading

static void say(const char *format, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *format, ...) {
  va_list arguments;
  va_start(arguments, format);
  fputs("vibepollo-host-input: ", stderr);
  vfprintf(stderr, format, arguments);
  fputc('\n', stderr);
  va_end(arguments);
}

static int64_t now_ms(void) {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return (int64_t) now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

// Nothing here blocks on the host, so this computer's keyboard, mouse and escape hotkey never
// wait on it. States and "done" must reach it, in order: they wait in this queue, sent again as
// the host reads (main loop). Events may be lost, but only while engaged, and then the lease ends
// (stalled): its "state released" follows them, so the Frame lets go of every key.
#define CONTROL_MAX 32
static char control[CONTROL_MAX][64];
static size_t control_first, control_count;

static void host_ends(const char *why) {
  say("%s: the connection ends", why);
  close(host);
  host = -1;
}

// True when nothing is owed.
static bool flush_control(void) {
  while (control_count && host >= 0) {
    const char *text = control[control_first];
    if (send(host, text, strlen(text), MSG_NOSIGNAL | MSG_DONTWAIT) < 0) {
      if (errno != EAGAIN && errno != EWOULDBLOCK) host_ends("the host went away");
      return false;
    }
    control_first = (control_first + 1) % CONTROL_MAX;
    control_count--;
  }
  return control_count == 0;
}

static void send_control(const char *text) {
  if (host < 0) return;
  if (control_count == CONTROL_MAX) return host_ends("the host stopped reading");
  snprintf(control[(control_first + control_count) % CONTROL_MAX], sizeof control[0], "%s", text);
  control_count++;
  flush_control();
}

static void send_events(const char *text) {
  if (host < 0) return;
  if (flush_control() && send(host, text, strlen(text), MSG_NOSIGNAL | MSG_DONTWAIT) >= 0) return;
  if (host < 0) return;
  if (control_count == 0 && errno != EAGAIN && errno != EWOULDBLOCK) return host_ends("the host went away");
  if (!stalled) say("the host stopped reading");
  stalled = true;
}

// One message of events for the Frame's host keyboard or mouse.
struct batch {
  char kind;
  size_t count;
  char text[16 + VIBEPOLLO_HOST_INPUT_MAX_BATCH * 24];
  size_t length;
};

static void batch_start(struct batch *batch, char kind) {
  batch->kind = kind;
  batch->count = 0;
  batch->length = (size_t) snprintf(batch->text, sizeof batch->text, "ev %c", kind);
}

static void batch_send(struct batch *batch) {
  if (!batch->count) return;
  batch->length += (size_t) snprintf(batch->text + batch->length, sizeof batch->text - batch->length, " 0 0 0");
  send_events(batch->text);
  batch_start(batch, batch->kind);
}

static void batch_add(struct batch *batch, uint16_t type, uint16_t code, int32_t value) {
  if (batch->count == VIBEPOLLO_HOST_INPUT_MAX_BATCH - 1) batch_send(batch);
  batch->length += (size_t) snprintf(batch->text + batch->length, sizeof batch->text - batch->length,
                                     " %u %u %d", (unsigned) type, (unsigned) code, (int) value);
  batch->count++;
}

static bool held(const struct device *device, unsigned int code) {
  return vibepollo_host_input_bit(device->held, BITS(KEY_CNT), code);
}

static void set_bit(unsigned long *bits, unsigned int code, bool on) {
  const size_t per = sizeof(unsigned long) * 8;
  if (on) bits[code / per] |= 1UL << (code % per);
  else bits[code / per] &= ~(1UL << (code % per));
}

static void set_held(struct device *device, unsigned int code, bool down) {
  set_bit(device->held, code, down);
}

static void type_kept(uint16_t code, int32_t value);

// What went down from this device and isn't in down now (NULL: nothing is) comes up: on the
// Frame, or on our own device for the kept keys.
static void lift_except(struct device *device, const unsigned long *down, struct batch *keys, struct batch *mouse) {
  static const unsigned long none[BITS(KEY_CNT)];
  unsigned long lost[BITS(KEY_CNT)], kept_lost[BITS(KEY_CNT)];
  vibepollo_host_input_lost_keys(device->held, down ? down : none, lost, BITS(KEY_CNT));
  vibepollo_host_input_lost_keys(device->kept, down ? down : none, kept_lost, BITS(KEY_CNT));
  for (unsigned int code = 1; code <= KEY_MAX; ++code) {
    if (vibepollo_host_input_bit(lost, BITS(KEY_CNT), code)) {
      set_held(device, code, false);
      const bool to_mouse = vibepollo_host_input_route_event(EV_KEY, (uint16_t) code, 0) == VIBEPOLLO_HOST_INPUT_MOUSE;
      batch_add(to_mouse ? mouse : keys, EV_KEY, (uint16_t) code, 0);
    }
    if (vibepollo_host_input_bit(kept_lost, BITS(KEY_CNT), code)) {
      set_bit(device->kept, code, false);
      type_kept((uint16_t) code, 0);
    }
  }
}

// Everything that went down from this device comes up (or one unplugged while held stays down).
static void lift(struct device *device) {
  struct batch keys, mouse;
  batch_start(&keys, 'k');
  batch_start(&mouse, 'm');
  lift_except(device, NULL, &keys, &mouse);
  batch_send(&keys);
  batch_send(&mouse);
}

#ifdef VIBEPOLLO_HOST_INPUT_LIBINPUT
static void remove_pad(struct device *device);
#endif

static void close_device(size_t index) {
  struct device *device = &devices[index];
  lift(device);
#ifdef VIBEPOLLO_HOST_INPUT_LIBINPUT
  if (device->pad) remove_pad(device);
#endif
  if (device->grabbed && !device->touchpad) (void) ioctl(device->fd, EVIOCGRAB, 0);
  close(device->fd);
  devices[index] = devices[--device_count];
}

// Both Shifts down, on any taken keyboards, and (with escape) Escape on any too.
static bool escape_held(bool escape) {
  bool left = false, right = false, esc = !escape;
  for (size_t i = 0; i < device_count; ++i)
    left |= devices[i].escape.left_shift, right |= devices[i].escape.right_shift, esc |= devices[i].escape.escape;
  return left && right && esc;
}

// The escape hotkey from this device's key: both Shifts down, on any taken keyboards, and Escape.
static bool escape_pressed(struct device *device, const struct input_event *event) {
  if (vibepollo_host_input_escape_step(&device->escape, event->type, event->code, event->value)) return true;
  return event->type == EV_KEY && event->code == KEY_ESC && event->value == 1 && escape_held(false);
}

// The devices go back, and the kept keys still down on our own device come up. The lease
// already says released.
static void give_back(const char *reason) {
  while (device_count) close_device(device_count - 1);
  char text[64];
  snprintf(text, sizeof text, "state released %s", reason);
  send_control(text);
  say("released: %s", reason);
}

static bool any_key_down(int fd) {
  unsigned long keys[BITS(KEY_CNT)] = {0};
  if (ioctl(fd, EVIOCGKEY(sizeof keys), keys) < 0) return false;
  for (size_t i = 0; i < BITS(KEY_CNT); ++i)
    if (keys[i]) return true;
  return false;
}

#ifdef VIBEPOLLO_HOST_INPUT_LIBINPUT
static bool add_pad(struct device *device);
#endif

// Takes a device once nothing is held on it, so no key stays down on either side.
static void try_grab(struct device *device) {
  if (device->grabbed || any_key_down(device->fd)) return;
#ifdef VIBEPOLLO_HOST_INPUT_LIBINPUT
  if (device->touchpad) {
    if (device->pad_failed || !add_pad(device)) return;
    if (any_key_down(device->fd)) {
      remove_pad(device);
      return;
    }
    device->grabbed = true;
    say("took %s", device->name);
    return;
  }
#endif
  if (ioctl(device->fd, EVIOCGRAB, 1) < 0) return;
  // A key that went down between the look and the grab would come up only here: try later.
  if (any_key_down(device->fd)) {
    (void) ioctl(device->fd, EVIOCGRAB, 0);
    return;
  }
  device->grabbed = true;
  // Events from before the grab went to the desktop too.
  struct input_event events[64];
  while (read(device->fd, events, sizeof events) > 0) {}
  say("took %s", device->name);
}

// A uinput device (ours, the host's passthrough devices, any other). Bluetooth keyboards and mice
// come through uhid, under /devices/virtual/misc/uhid, and are taken.
static bool is_virtual(const char *node) {
  char link[PATH_MAX], resolved[PATH_MAX];
  snprintf(link, sizeof link, "/sys/class/input/%s/device", node);
  if (!realpath(link, resolved)) return true;  // can't tell: leave it alone
  return strstr(resolved, "/devices/virtual/input/") != NULL;
}

// One of a node's sysfs bitmaps (capabilities/ev, capabilities/key, properties...).
static bool sysfs_bits(const char *node, const char *file, unsigned long *bits, size_t count) {
  char path[PATH_MAX], text[4096];
  snprintf(path, sizeof path, "/sys/class/input/%s/device/%s", node, file);
  const int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return false;
  const ssize_t n = read(fd, text, sizeof text - 1);
  close(fd);
  if (n <= 0) return false;
  text[n] = 0;
  return vibepollo_host_input_parse_bits(text, bits, count);
}

static void scan(void) {
  DIR *dir = opendir("/dev/input");
  if (!dir) return;
  struct dirent *entry;
  while ((entry = readdir(dir)) && device_count < VIBEPOLLO_HOST_INPUT_MAX_DEVICES) {
    // A longer name wouldn't fit the device's path.
    if (strncmp(entry->d_name, "event", 5) != 0 || strlen(entry->d_name) > 16) continue;
    char path[PATH_MAX];
    snprintf(path, sizeof path, "/dev/input/%s", entry->d_name);
    struct stat st;
    if (stat(path, &st) < 0 || !S_ISCHR(st.st_mode)) continue;
    bool known = false;
    for (size_t i = 0; i < device_count && !known; ++i) known = devices[i].rdev == st.st_rdev;
    if (known || is_virtual(entry->d_name)) continue;
    // From sysfs, so the nodes that aren't taken are never opened (see RESCAN_MS).
    unsigned long types[BITS(EV_CNT)], keys[BITS(KEY_CNT)], rel[BITS(REL_CNT)], abs[BITS(ABS_CNT)],
      props[BITS(INPUT_PROP_CNT)];
    if (!sysfs_bits(entry->d_name, "capabilities/ev", types, BITS(EV_CNT))) continue;
    if (!sysfs_bits(entry->d_name, "capabilities/key", keys, BITS(KEY_CNT))) memset(keys, 0, sizeof keys);
    if (!sysfs_bits(entry->d_name, "capabilities/rel", rel, BITS(REL_CNT))) memset(rel, 0, sizeof rel);
    if (!sysfs_bits(entry->d_name, "capabilities/abs", abs, BITS(ABS_CNT))) memset(abs, 0, sizeof abs);
    if (!sysfs_bits(entry->d_name, "properties", props, BITS(INPUT_PROP_CNT))) memset(props, 0, sizeof props);
    const struct vibepollo_host_input_caps caps = {
      .virtual_device = false,
      .keys_letters = vibepollo_host_input_bit(keys, BITS(KEY_CNT), KEY_A) &&
                      vibepollo_host_input_bit(keys, BITS(KEY_CNT), KEY_Z) &&
                      vibepollo_host_input_bit(keys, BITS(KEY_CNT), KEY_SPACE) &&
                      vibepollo_host_input_bit(keys, BITS(KEY_CNT), KEY_ENTER),
      .rel_xy = vibepollo_host_input_bit(rel, BITS(REL_CNT), REL_X) && vibepollo_host_input_bit(rel, BITS(REL_CNT), REL_Y),
      .btn_left = vibepollo_host_input_bit(keys, BITS(KEY_CNT), BTN_LEFT),
      .abs_mt = vibepollo_host_input_bit(types, BITS(EV_CNT), EV_ABS) &&
                vibepollo_host_input_bit(abs, BITS(ABS_CNT), ABS_MT_POSITION_X),
      .btn_tool_finger = vibepollo_host_input_bit(keys, BITS(KEY_CNT), BTN_TOOL_FINGER),
      .direct = vibepollo_host_input_bit(props, BITS(INPUT_PROP_CNT), INPUT_PROP_DIRECT),
    };
    const enum vibepollo_host_input_kind kind = vibepollo_host_input_kind_of(&caps);
#ifdef VIBEPOLLO_HOST_INPUT_LIBINPUT
    if (kind == VIBEPOLLO_HOST_INPUT_SKIP) continue;
    if (kind == VIBEPOLLO_HOST_INPUT_TOUCHPAD) {
      // libinput waits (up to 2 s, reading nothing else) for udev to finish with a new device, so
      // a touchpad is taken from the rescan after udev is done.
      char ready[64];
      snprintf(ready, sizeof ready, "/run/udev/data/c%u:%u", major(st.st_rdev), minor(st.st_rdev));
      if (access(ready, F_OK) < 0) continue;
    }
#else
    if (kind != VIBEPOLLO_HOST_INPUT_EVDEV) continue;
#endif
    const int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) continue;
    struct device *device = &devices[device_count++];
    memset(device, 0, sizeof *device);
    device->fd = fd;
    device->rdev = st.st_rdev;
    device->touchpad = kind == VIBEPOLLO_HOST_INPUT_TOUCHPAD;
    snprintf(device->path, sizeof device->path, "/dev/input/%s", entry->d_name);
    if (ioctl(fd, EVIOCGNAME(sizeof device->name - 1), device->name) < 0) strcpy(device->name, "?");
    for (char *c = device->name; *c; ++c)
      if ((unsigned char) *c < 0x20 || (unsigned char) *c > 0x7e) *c = '?';
  }
  closedir(dir);
}

static void open_keep(void) {
  if (keep >= 0) return;
  const int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0) {
    say("no /dev/uinput (%s): volume and media keys of a taken keyboard are dropped", strerror(errno));
    return;
  }
  bool ok = ioctl(fd, UI_SET_EVBIT, EV_KEY) == 0;
  for (unsigned int code = 1; ok && code <= KEY_MAX; ++code)
    if (vibepollo_host_input_kept_key(code)) ok = ioctl(fd, UI_SET_KEYBIT, code) == 0;
  struct uinput_setup setup = {.id = {.bustype = BUS_VIRTUAL, .vendor = 0x1209, .product = 0x3004}};
  snprintf(setup.name, sizeof setup.name, "Vibepollo host input media keys");
  if (!ok || ioctl(fd, UI_DEV_SETUP, &setup) < 0 || ioctl(fd, UI_DEV_CREATE) < 0) {
    say("no device for volume and media keys (%s): a taken keyboard's are dropped", strerror(errno));
    close(fd);
    return;
  }
  keep = fd;
}

static void type_kept(uint16_t code, int32_t value) {
  if (keep < 0) return;
  struct input_event events[2] = {{.type = EV_KEY, .code = code, .value = value}, {.type = EV_SYN, .code = SYN_REPORT}};
  if (write(keep, events, sizeof events) < 0) {}
}

static void take(void) {
  open_keep();
  scan();
  for (size_t i = 0; i < device_count; ++i) try_grab(&devices[i]);
  rescan_at = now_ms() + VIBEPOLLO_HOST_INPUT_RESCAN_MS;
  send_control("state engaged");
  say("engaged");
}

// After the kernel dropped events (SYN_DROPPED): what is still queued is older than any look at
// the keys, so it goes too, and then the keys that came up meanwhile come up on the Frame (or on
// our own device, the kept ones) and the Shifts are as they are now. A key that went down
// meanwhile stays here, as one held at the grab does. True when the escape is held now (its
// Escape press may have been among the lost events).
static bool resync(struct device *device, struct batch *keys, struct batch *mouse) {
  struct input_event stale[64];
  while (read(device->fd, stale, sizeof stale) > 0) {}
  unsigned long down[BITS(KEY_CNT)] = {0};
  if (ioctl(device->fd, EVIOCGKEY(sizeof down), down) < 0) memset(down, 0, sizeof down);  // gone: all up
  lift_except(device, down, keys, mouse);
  device->escape.left_shift = vibepollo_host_input_bit(down, BITS(KEY_CNT), KEY_LEFTSHIFT);
  device->escape.right_shift = vibepollo_host_input_bit(down, BITS(KEY_CNT), KEY_RIGHTSHIFT);
  device->escape.escape = vibepollo_host_input_bit(down, BITS(KEY_CNT), KEY_ESC);
  say("%s dropped events: caught up", device->name);
  return escape_held(true);  // the parts may be on different keyboards, each caught up in turn
}

#ifdef VIBEPOLLO_HOST_INPUT_LIBINPUT
// Touchpads, through libinput's path backend: it opens them through us, grabbed.
static struct libinput *pads;
static struct vibepollo_host_input_touchpad pad_settings;

static int open_pad(const char *path, int flags, void *data) {
  (void) flags, (void) data;
  const int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) return -errno;
  if (ioctl(fd, EVIOCGRAB, 1) < 0) {
    const int error = errno;
    close(fd);
    return -error;
  }
  return fd;
}

static void close_pad(int fd, void *data) {
  (void) data;
  (void) ioctl(fd, EVIOCGRAB, 0);
  close(fd);
}

static const struct libinput_interface pad_interface = {.open_restricted = open_pad, .close_restricted = close_pad};

static bool add_pad(struct device *device) {
  if (!pads) {
    const char *settings = getenv("VIBEPOLLO_HOST_INPUT_TOUCHPAD");
    if (!vibepollo_host_input_parse_touchpad(settings, &pad_settings))
      say("VIBEPOLLO_HOST_INPUT_TOUCHPAD not understood: the defaults apply");
    pads = libinput_path_create_context(&pad_interface, NULL);
    if (!pads) {
      say("no libinput: touchpads stay here");
      for (size_t i = 0; i < device_count; ++i) devices[i].pad_failed = true;
      return false;
    }
  }
  device->pad = libinput_path_add_device(pads, device->path);
  if (!device->pad) {
    say("%s: libinput can't read it, so it stays here", device->name);
    device->pad_failed = true;
    return false;
  }
  struct libinput_device *pad = libinput_device_ref(device->pad);  // still ours if it's unplugged
  (void) libinput_device_config_tap_set_enabled(pad, pad_settings.tap ? LIBINPUT_CONFIG_TAP_ENABLED : LIBINPUT_CONFIG_TAP_DISABLED);
  if (libinput_device_config_scroll_has_natural_scroll(pad))
    (void) libinput_device_config_scroll_set_natural_scroll_enabled(pad, pad_settings.natural);
  const uint32_t methods = libinput_device_config_click_get_methods(pad);
  const enum libinput_config_click_method method =
    pad_settings.clickfinger ? LIBINPUT_CONFIG_CLICK_METHOD_CLICKFINGER : LIBINPUT_CONFIG_CLICK_METHOD_BUTTON_AREAS;
  if (methods & method) (void) libinput_device_config_click_set_method(pad, method);
  device->rest_x = device->rest_y = 0;
  device->scroll_v = device->scroll_h = (struct vibepollo_host_input_scroll) {0};
  return true;
}

static void pad_events(void);

static void remove_pad(struct device *device) {
  libinput_path_remove_device(device->pad);
  libinput_device_unref(device->pad);
  device->pad = NULL;
  device->grabbed = false;
  pad_events();  // what libinput says about it now has no device, and is dropped
}

static struct device *pad_device(struct libinput_device *pad) {
  for (size_t i = 0; i < device_count; ++i)
    if (devices[i].pad == pad) return &devices[i];
  return NULL;
}

static void pad_scroll(struct batch *mouse, struct libinput_event_pointer *pointer, struct device *device) {
  const struct {
    enum libinput_pointer_axis axis;
    struct vibepollo_host_input_scroll *scroll;
    uint16_t hi_res, notch;
    int sign;  // the wheel's up is libinput's negative
  } axes[] = {
    {LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL, &device->scroll_v, REL_WHEEL_HI_RES, REL_WHEEL, -1},
    {LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL, &device->scroll_h, REL_HWHEEL_HI_RES, REL_HWHEEL, 1},
  };
  for (size_t i = 0; i < 2; ++i) {
    if (!libinput_event_pointer_has_axis(pointer, axes[i].axis)) continue;
    int32_t notches;
    const int32_t hi_res = vibepollo_host_input_scroll_step(
      axes[i].scroll, libinput_event_pointer_get_scroll_value(pointer, axes[i].axis), pad_settings.scroll, &notches
    );
    if (hi_res) batch_add(mouse, EV_REL, axes[i].hi_res, axes[i].sign * hi_res);
    if (notches) batch_add(mouse, EV_REL, axes[i].notch, axes[i].sign * notches);
  }
}

// libinput's events, each one report to the Frame's host mouse.
static void pad_events(void) {
  if (!pads) return;
  (void) libinput_dispatch(pads);
  struct libinput_event *event;
  while ((event = libinput_get_event(pads))) {
    const enum libinput_event_type type = libinput_event_get_type(event);
    const bool wanted = type == LIBINPUT_EVENT_POINTER_MOTION || type == LIBINPUT_EVENT_POINTER_BUTTON ||
                        type == LIBINPUT_EVENT_POINTER_SCROLL_FINGER;  // not gestures, nor the older axis twin
    struct device *device = wanted ? pad_device(libinput_event_get_device(event)) : NULL;
    if (device && device->grabbed) {
      struct libinput_event_pointer *pointer = libinput_event_get_pointer_event(event);
      struct batch mouse;
      batch_start(&mouse, 'm');
      switch (type) {
        case LIBINPUT_EVENT_POINTER_MOTION: {
          device->rest_x += libinput_event_pointer_get_dx(pointer);
          device->rest_y += libinput_event_pointer_get_dy(pointer);
          const int32_t x = vibepollo_host_input_carry(&device->rest_x), y = vibepollo_host_input_carry(&device->rest_y);
          if (x) batch_add(&mouse, EV_REL, REL_X, x);
          if (y) batch_add(&mouse, EV_REL, REL_Y, y);
          break;
        }
        case LIBINPUT_EVENT_POINTER_BUTTON: {
          const uint32_t code = libinput_event_pointer_get_button(pointer);
          const bool down = libinput_event_pointer_get_button_state(pointer) == LIBINPUT_BUTTON_STATE_PRESSED;
          if (code > KEY_MAX || vibepollo_host_input_route_event(EV_KEY, (uint16_t) code, down) != VIBEPOLLO_HOST_INPUT_MOUSE) break;
          if (!down && !held(device, code)) break;
          set_held(device, code, down);
          batch_add(&mouse, EV_KEY, (uint16_t) code, down);
          break;
        }
        case LIBINPUT_EVENT_POINTER_SCROLL_FINGER:
          pad_scroll(&mouse, pointer, device);
          break;
        default:
          break;
      }
      batch_send(&mouse);
    }
    libinput_event_destroy(event);
  }
}
#endif

// One device's events. False when the device went away.
static bool read_device(struct device *device) {
  struct input_event events[64];
  struct batch keys, mouse;
  batch_start(&keys, 'k');
  batch_start(&mouse, 'm');
  for (bool more = true; more;) {
    const ssize_t n = read(device->fd, events, sizeof events);
    if (n < 0 && errno == EAGAIN) break;
    if (n <= 0) return false;
    for (size_t i = 0; i < (size_t) n / sizeof events[0]; ++i) {
      const struct input_event *event = &events[i];
      if (event->type == EV_SYN && event->code == SYN_DROPPED) {
        batch_send(&keys);
        batch_send(&mouse);
        const bool escape = resync(device, &keys, &mouse);  // the rest of events[] is stale too
        batch_send(&keys);
        batch_send(&mouse);
        if (escape) {
          vibepollo_host_input_lease_escape(&lease);
          say("this computer took its keyboard and mouse back (both Shifts and Escape)");
          give_back("escape");
          return true;
        }
        more = false;
        break;
      }
      if (event->type == EV_SYN && event->code == SYN_REPORT) {
        batch_send(&keys);
        batch_send(&mouse);
        continue;
      }
      if (escape_pressed(device, event)) {
        batch_send(&keys);
        batch_send(&mouse);
        vibepollo_host_input_lease_escape(&lease);
        say("this computer took its keyboard and mouse back (both Shifts and Escape)");
        give_back("escape");  // closes every device, this one too
        return true;
      }
      switch (vibepollo_host_input_route_event(event->type, event->code, event->value)) {
        case VIBEPOLLO_HOST_INPUT_KEEP:
          if (event->value == 0 || event->value == 1) set_bit(device->kept, event->code, event->value == 1);
          type_kept(event->code, event->value);
          break;
        case VIBEPOLLO_HOST_INPUT_KEYBOARD:
        case VIBEPOLLO_HOST_INPUT_MOUSE: {
          const bool mouse_event = vibepollo_host_input_route_event(event->type, event->code, event->value) ==
                                   VIBEPOLLO_HOST_INPUT_MOUSE;
          if (event->type == EV_KEY) {
            // A release of a key that went down before the grab stays here.
            if (!event->value && !held(device, event->code)) break;
            set_held(device, event->code, event->value != 0);
          }
          batch_add(mouse_event ? &mouse : &keys, event->type, event->code, event->value);
          break;
        }
        case VIBEPOLLO_HOST_INPUT_DROP:
          break;
      }
    }
  }
  batch_send(&keys);
  batch_send(&mouse);
  return true;
}

// One message from the host. False when the connection ends.
static bool serve_request(void) {
  char request[64];
  const ssize_t n = recv(host, request, sizeof request, MSG_DONTWAIT);
  if (n < 0 && (errno == EAGAIN || errno == EINTR)) return true;
  if (n <= 0) return false;
  const enum vibepollo_host_input_request parsed = vibepollo_host_input_parse_request(request, (size_t) n);
  if (parsed == VIBEPOLLO_HOST_INPUT_REQUEST_INVALID) {
    say("an unknown request: the connection ends");
    return false;
  }
  switch (vibepollo_host_input_lease_request(&lease, parsed, now_ms())) {
    case VIBEPOLLO_HOST_INPUT_TAKE:
      take();
      break;
    case VIBEPOLLO_HOST_INPUT_GIVE_BACK:
      give_back("host");
      break;
    case VIBEPOLLO_HOST_INPUT_STILL_ESCAPED:
      send_control("state released escape");
      break;
    case VIBEPOLLO_HOST_INPUT_STAY:
      break;
  }
  if (parsed != VIBEPOLLO_HOST_INPUT_REQUEST_ENGAGE) send_control("done");
  return true;
}

static uid_t host_uid(void) {
  const struct passwd *user = getpwnam("vibepollo");
  return user ? user->pw_uid : (uid_t) -1;
}

int main(void) {
  signal(SIGPIPE, SIG_IGN);
  // systemd's socket: fd 3, for this process only.
  const char *pid = getenv("LISTEN_PID"), *fds = getenv("LISTEN_FDS");
  if (!pid || !fds || strtol(pid, NULL, 10) != getpid() || strcmp(fds, "1") != 0) {
    say("start me through vibepollo-host-input.socket");
    return 2;
  }
  const uid_t expected = host_uid();
  if (expected == (uid_t) -1) {
    say("no vibepollo user");
    return 1;
  }
  host = accept4(3, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
  if (host < 0) {
    say("accept: %s", strerror(errno));
    return 1;
  }
  struct ucred peer;
  socklen_t peer_length = sizeof peer;
  if (getsockopt(host, SOL_SOCKET, SO_PEERCRED, &peer, &peer_length) < 0 || peer.uid != expected) {
    say("a connection from uid %d refused: only the machine host may lend this computer's input",
        peer_length == sizeof peer ? (int) peer.uid : -1);
    return 1;
  }

  while (host >= 0) {
    struct pollfd polls[2 + VIBEPOLLO_HOST_INPUT_MAX_DEVICES];
    polls[0] = (struct pollfd) {.fd = host, .events = POLLIN | (control_count ? POLLOUT : 0)};
    for (size_t i = 0; i < device_count; ++i) {
      const bool read_here = devices[i].grabbed && !devices[i].touchpad;
      polls[1 + i] = (struct pollfd) {.fd = devices[i].fd, .events = read_here ? POLLIN : 0};
    }
    int timeout = -1;
    if (lease.engaged) {
      const int64_t now = now_ms();
      int64_t next = lease.until < rescan_at ? lease.until : rescan_at;
      for (size_t i = 0; i < device_count; ++i)
        if (!devices[i].grabbed && !devices[i].pad_failed && now + 100 < next) next = now + 100;  // waiting for its keys to come up
      timeout = next > now ? (int) (next - now) : 0;
    }
    const nfds_t count = 1 + device_count;
    polls[count] = (struct pollfd) {.fd = -1};
#ifdef VIBEPOLLO_HOST_INPUT_LIBINPUT
    if (pads) polls[count] = (struct pollfd) {.fd = libinput_get_fd(pads), .events = POLLIN};
#endif
    if (poll(polls, count + 1, timeout) < 0 && errno != EINTR) break;
#ifdef VIBEPOLLO_HOST_INPUT_LIBINPUT
    if (polls[count].revents & POLLIN) pad_events();
#endif

    if (polls[0].revents & POLLOUT) flush_control();
    if (polls[0].revents & POLLIN) {
      if (!serve_request()) break;
    } else if (polls[0].revents & (POLLHUP | POLLERR)) {
      break;
    }
    if (host < 0) break;
    // A device that went away (or the batch above released everything) leaves the list.
    for (size_t i = device_count; i-- > 0;) {
      // Polled this round, and still open (an escape releases them all).
      if (i + 1 >= count || i >= device_count) continue;
      if (polls[1 + i].revents & (POLLERR | POLLHUP | POLLNVAL)) {
        say("%s went away", devices[i].name);
        close_device(i);
      } else if ((polls[1 + i].revents & POLLIN) && !read_device(&devices[i])) {
        say("%s went away", devices[i].name);
        close_device(i);
      }
    }
    if (stalled) {
      stalled = false;
      if (lease.engaged) {
        lease.engaged = false;
        give_back("stalled");
      }
    }
    if (!lease.engaged) continue;
    const int64_t now = now_ms();
    if (vibepollo_host_input_lease_lapsed(&lease, now)) {
      give_back("lease");
      continue;
    }
    for (size_t i = 0; i < device_count; ++i) try_grab(&devices[i]);
    if (now >= rescan_at) {
      const size_t before = device_count;
      scan();
      for (size_t i = before; i < device_count; ++i) try_grab(&devices[i]);
      rescan_at = now + VIBEPOLLO_HOST_INPUT_RESCAN_MS;
    }
  }
  if (lease.engaged) {
    lease.engaged = false;
    give_back("closed");
  }
  if (keep >= 0) {
    (void) ioctl(keep, UI_DEV_DESTROY);
    close(keep);
  }
#ifdef VIBEPOLLO_HOST_INPUT_LIBINPUT
  if (pads) libinput_unref(pads);
#endif
  return 0;
}
