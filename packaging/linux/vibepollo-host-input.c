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
// While engaged, every keyboard and mouse that isn't a uinput device or a touch surface is
// grabbed (EVIOCGRAB), so the desktop sees nothing from it, and only once no key is held on it.
// Release reasons: lease (no engage in time), host (release or drop), escape (both Shifts and Escape on
// this computer, the Shifts on any keyboards), dropped (the kernel dropped a device's events, so
// which keys are down is unknown), stalled (the host stopped reading), closed. After escape, engage is refused until the host sends release (the
// Frame's headset came off, or its switch went off). Keys and buttons that went to the Frame come
// up there when the devices go back. Volume, media and brightness keys stay here: a taken
// keyboard's are typed again on a uinput device of our own. Key codes are never logged.
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

#define BITS(n) (((n) + sizeof(unsigned long) * 8 - 1) / (sizeof(unsigned long) * 8))

struct device {
  int fd;
  dev_t rdev;
  bool grabbed;
  char name[64];
  unsigned long held[BITS(KEY_CNT)];  // keys and buttons that went down to the Frame from here
  struct vibepollo_host_input_escape escape;  // its Shifts
};

static struct device devices[VIBEPOLLO_HOST_INPUT_MAX_DEVICES];
static size_t device_count;
static int host = -1;
static int keep = -1;  // our uinput device for the kept keys
static struct vibepollo_host_input_lease lease;
static int64_t rescan_at;
static unsigned long kept_held[BITS(KEY_CNT)];  // kept keys down on our own device
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

// Never blocks: a host that stops reading gets its lease ended (main loop), so this computer's
// keyboard, mouse and escape hotkey never wait on it.
static void send_text(const char *text) {
  if (host < 0 || send(host, text, strlen(text), MSG_NOSIGNAL | MSG_DONTWAIT) >= 0) return;
  if (errno == EAGAIN || errno == EWOULDBLOCK) {
    if (!stalled) say("the host stopped reading");
    stalled = true;
    return;
  }
  say("the host went away");
  close(host);
  host = -1;
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
  send_text(batch->text);
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

// What went down to the Frame from this device comes up there.
static void lift(struct device *device) {
  struct batch keys, mouse;
  batch_start(&keys, 'k');
  batch_start(&mouse, 'm');
  for (unsigned int code = 1; code <= KEY_MAX; ++code) {
    if (!held(device, code)) continue;
    set_held(device, code, false);
    const enum vibepollo_host_input_route route = vibepollo_host_input_route_event(EV_KEY, (uint16_t) code, 0);
    batch_add(route == VIBEPOLLO_HOST_INPUT_MOUSE ? &mouse : &keys, EV_KEY, (uint16_t) code, 0);
  }
  batch_send(&keys);
  batch_send(&mouse);
}

static void close_device(size_t index) {
  struct device *device = &devices[index];
  lift(device);
  if (device->grabbed) (void) ioctl(device->fd, EVIOCGRAB, 0);
  close(device->fd);
  devices[index] = devices[--device_count];
}

// The escape hotkey from this device's key: both Shifts down, on any taken keyboards, and Escape.
static bool escape_pressed(struct device *device, const struct input_event *event) {
  if (vibepollo_host_input_escape_step(&device->escape, event->type, event->code, event->value)) return true;
  if (event->type != EV_KEY || event->code != KEY_ESC || event->value != 1) return false;
  bool left = false, right = false;
  for (size_t i = 0; i < device_count; ++i) left |= devices[i].escape.left_shift, right |= devices[i].escape.right_shift;
  return left && right;
}

static void type_kept(uint16_t code, int32_t value);

// The devices go back, and the kept keys still down on our own device come up. The lease
// already says released.
static void give_back(const char *reason) {
  while (device_count) close_device(device_count - 1);
  for (unsigned int code = 1; code <= KEY_MAX; ++code)
    if (vibepollo_host_input_bit(kept_held, BITS(KEY_CNT), code)) type_kept((uint16_t) code, 0);
  char text[64];
  snprintf(text, sizeof text, "state released %s", reason);
  send_text(text);
  say("released: %s", reason);
}

static bool any_key_down(int fd) {
  unsigned long keys[BITS(KEY_CNT)] = {0};
  if (ioctl(fd, EVIOCGKEY(sizeof keys), keys) < 0) return false;
  for (size_t i = 0; i < BITS(KEY_CNT); ++i)
    if (keys[i]) return true;
  return false;
}

// Takes a device once nothing is held on it, so no key stays down on either side.
static void try_grab(struct device *device) {
  if (device->grabbed || any_key_down(device->fd)) return;
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

static void scan(void) {
  DIR *dir = opendir("/dev/input");
  if (!dir) return;
  struct dirent *entry;
  while ((entry = readdir(dir)) && device_count < VIBEPOLLO_HOST_INPUT_MAX_DEVICES) {
    if (strncmp(entry->d_name, "event", 5) != 0) continue;
    char path[PATH_MAX];
    snprintf(path, sizeof path, "/dev/input/%s", entry->d_name);
    struct stat st;
    if (stat(path, &st) < 0 || !S_ISCHR(st.st_mode)) continue;
    bool known = false;
    for (size_t i = 0; i < device_count && !known; ++i) known = devices[i].rdev == st.st_rdev;
    if (known || is_virtual(entry->d_name)) continue;
    const int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) continue;
    unsigned long types[BITS(EV_CNT)] = {0}, keys[BITS(KEY_CNT)] = {0}, rel[BITS(REL_CNT)] = {0},
                  abs[BITS(ABS_CNT)] = {0};
    (void) ioctl(fd, EVIOCGBIT(0, sizeof types), types);
    (void) ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keys), keys);
    (void) ioctl(fd, EVIOCGBIT(EV_REL, sizeof rel), rel);
    (void) ioctl(fd, EVIOCGBIT(EV_ABS, sizeof abs), abs);
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
    };
    if (!vibepollo_host_input_takes(&caps)) {
      close(fd);
      continue;
    }
    struct device *device = &devices[device_count++];
    memset(device, 0, sizeof *device);
    device->fd = fd;
    device->rdev = st.st_rdev;
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
  if (value == 0 || value == 1) set_bit(kept_held, code, value == 1);
  if (keep < 0) return;
  struct input_event events[2] = {{.type = EV_KEY, .code = code, .value = value}, {.type = EV_SYN, .code = SYN_REPORT}};
  if (write(keep, events, sizeof events) < 0) {}
}

static void take(void) {
  open_keep();
  scan();
  for (size_t i = 0; i < device_count; ++i) try_grab(&devices[i]);
  rescan_at = now_ms() + VIBEPOLLO_HOST_INPUT_RESCAN_MS;
  send_text("state engaged");
  say("engaged");
}

// One device's events. False when the device went away.
static bool read_device(struct device *device) {
  struct input_event events[64];
  struct batch keys, mouse;
  batch_start(&keys, 'k');
  batch_start(&mouse, 'm');
  for (;;) {
    const ssize_t n = read(device->fd, events, sizeof events);
    if (n < 0 && errno == EAGAIN) break;
    if (n <= 0) return false;
    for (size_t i = 0; i < (size_t) n / sizeof events[0]; ++i) {
      const struct input_event *event = &events[i];
      if (event->type == EV_SYN && event->code == SYN_REPORT) {
        batch_send(&keys);
        batch_send(&mouse);
        continue;
      }
      if (event->type == EV_SYN && event->code == SYN_DROPPED) {
        // A key-up may be among what was lost: give everything back, so every key comes up on
        // the Frame, and take them again at the next engage, once no key is held.
        lease.engaged = false;
        give_back("dropped");
        return true;
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
      send_text("state released escape");
      break;
    case VIBEPOLLO_HOST_INPUT_STAY:
      break;
  }
  if (parsed != VIBEPOLLO_HOST_INPUT_REQUEST_ENGAGE) send_text("done");
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
    struct pollfd polls[1 + VIBEPOLLO_HOST_INPUT_MAX_DEVICES];
    polls[0] = (struct pollfd) {.fd = host, .events = POLLIN};
    for (size_t i = 0; i < device_count; ++i)
      polls[1 + i] = (struct pollfd) {.fd = devices[i].fd, .events = devices[i].grabbed ? POLLIN : 0};
    int timeout = -1;
    if (lease.engaged) {
      const int64_t now = now_ms();
      int64_t next = lease.until < rescan_at ? lease.until : rescan_at;
      for (size_t i = 0; i < device_count; ++i)
        if (!devices[i].grabbed && now + 100 < next) next = now + 100;  // waiting for its keys to come up
      timeout = next > now ? (int) (next - now) : 0;
    }
    const nfds_t count = 1 + device_count;
    if (poll(polls, count, timeout) < 0 && errno != EINTR) break;

    if (polls[0].revents & POLLIN) {
      if (!serve_request()) break;
    } else if (polls[0].revents & (POLLHUP | POLLERR)) {
      break;
    }
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
  return 0;
}
