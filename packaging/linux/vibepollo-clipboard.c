/*
 * Frametop: vibepollo-clipboard, the desktop session's clipboard for Vibepollo's
 * /actions/clipboard (a Frametop client's shared clipboard). A Wayland client of the desktop's
 * compositor through wlr-data-control, which KWin and Hyprland give clients that don't have
 * focus, as a clipboard manager needs.
 *
 * Vibepollo starts it once and keeps it: run as the desktop user, it runs it itself; as the
 * system service, the session broker runs it in the desktop session (vibepollo-session-exec
 * clipboard). Requests come on stdin and replies go on stdout, as
 * vibepollo-clipboard-protocol.h says. What it's given stays on the clipboard, handed to the
 * apps that paste it, until something else is copied or it ends (its stdin closes).
 *
 * It takes text (UTF-8) and PNG images, and leaves alone a copy that a password manager marks
 * as one (x-kde-passwordManagerHint), as clipboard managers do. Nothing of the clipboard's
 * content is ever logged.
 */
#define _GNU_SOURCE

#include "vibepollo-clipboard-protocol.h"
#include "wlr-data-control-unstable-v1.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>

/* How long the app that owns the clipboard gets to hand it over, and a pasting app to take it. */
#define HAND_OVER_MS 3000
#define PASTE_MS 10000
/* How long Vibepollo gets to send a request's content once its header came. */
#define REQUEST_MS 10000

/* The text types taken, best first, and offered. */
static const char *const text_types[] = {"text/plain;charset=utf-8", "UTF8_STRING", "text/plain", "STRING", "TEXT"};
enum { text_type_count = sizeof text_types / sizeof text_types[0] };
static const char png_type[] = "image/png";
static const char secret_hint[] = "x-kde-passwordManagerHint";

static struct wl_display *display;
static struct wl_seat *seat;
static struct zwlr_data_control_manager_v1 *manager;
static struct zwlr_data_control_device_v1 *device;

static int64_t now_ms(void) {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return (int64_t) now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static bool write_all(int fd, const void *data, size_t length) {
  const char *at = data;
  while (length) {
    const ssize_t written = write(fd, at, length);
    if (written < 0 && errno == EINTR) continue;
    if (written <= 0) return false;
    at += written, length -= (size_t) written;
  }
  return true;
}

/* Replies. Vibepollo gone (a failed write) ends the helper. */
static void reply_line(const char *line) {
  if (!write_all(STDOUT_FILENO, line, strlen(line))) _exit(0);
}
static void reply_fail(const char *why) {
  char line[VIBEPOLLO_CLIPBOARD_LINE];
  snprintf(line, sizeof line, "fail %s\n", why);
  reply_line(line);
}
static void reply_ok(const void *data, size_t length) {
  char line[VIBEPOLLO_CLIPBOARD_LINE];
  snprintf(line, sizeof line, "ok %zu\n", length);
  reply_line(line);
  if (length && !write_all(STDOUT_FILENO, data, length)) _exit(0);
}

/* --------------------------------------------------------------------- the clipboard's offer */

struct offer {
  struct zwlr_data_control_offer_v1 *offer;
  int text;  /* index into text_types, text_type_count: no text */
  bool png, secret;
};
static struct offer *selection; /* the clipboard as it stands, NULL when empty */

static void offer_type(void *data, struct zwlr_data_control_offer_v1 *offer, const char *type) {
  (void) offer;
  struct offer *o = data;
  if (!strcmp(type, png_type)) o->png = true;
  else if (!strcmp(type, secret_hint)) o->secret = true;
  for (int i = 0; i < o->text; ++i)
    if (!strcmp(type, text_types[i])) o->text = i;
}
static const struct zwlr_data_control_offer_v1_listener offer_listener = {offer_type};

static void forget(struct offer *o) {
  if (!o) return;
  zwlr_data_control_offer_v1_destroy(o->offer);
  free(o);
}

static void device_offer(void *data, struct zwlr_data_control_device_v1 *d, struct zwlr_data_control_offer_v1 *offer) {
  (void) data, (void) d;
  struct offer *o = calloc(1, sizeof *o);
  if (!o) {
    zwlr_data_control_offer_v1_destroy(offer);
    return;
  }
  o->offer = offer, o->text = text_type_count;
  zwlr_data_control_offer_v1_add_listener(offer, &offer_listener, o);
}
static void device_selection(void *data, struct zwlr_data_control_device_v1 *d, struct zwlr_data_control_offer_v1 *offer) {
  (void) data, (void) d;
  struct offer *next = offer ? zwlr_data_control_offer_v1_get_user_data(offer) : NULL;
  if (selection != next) forget(selection);
  selection = next;
}
static void device_finished(void *data, struct zwlr_data_control_device_v1 *d) {
  (void) data, (void) d;
  fprintf(stderr, "vibepollo-clipboard: the desktop took its clipboard away (its seat went)\n");
  _exit(1);
}
static void device_primary(void *data, struct zwlr_data_control_device_v1 *d, struct zwlr_data_control_offer_v1 *offer) {
  (void) data, (void) d;
  if (offer) forget(zwlr_data_control_offer_v1_get_user_data(offer)); /* not shared */
}
static const struct zwlr_data_control_device_v1_listener device_listener = {device_offer, device_selection, device_finished,
                                                                            device_primary};

/* ---------------------------------------------------------------- what this helper put there */

struct copy {
  struct zwlr_data_control_source_v1 *source;
  char *data;
  size_t length;
  bool image;
};
static struct copy *current; /* on the clipboard now, NULL once something else was copied */

static void source_send(void *data, struct zwlr_data_control_source_v1 *source, const char *type, int32_t fd) {
  (void) source, (void) type;
  const struct copy *c = data;
  /* A child writes it, so an app that reads slowly (a big image) or not at all holds up
   * nothing here. It gets PASTE_MS for all of it. */
  const pid_t child = fork();
  if (child == 0) {
    /* Only the paste is its: Vibepollo's pipes and the desktop's socket stay the helper's. */
    close(STDIN_FILENO), close(STDOUT_FILENO), close(wl_display_get_fd(display));
    const int flags = fcntl(fd, F_GETFL);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    const int64_t until = now_ms() + PASTE_MS;
    for (size_t at = 0; at < c->length;) {
      const int64_t left = until - now_ms();
      struct pollfd p = {fd, POLLOUT, 0};
      if (left <= 0 || poll(&p, 1, (int) left) <= 0) break;
      const ssize_t written = write(fd, c->data + at, c->length - at);
      if (written < 0 && (errno == EINTR || errno == EAGAIN)) continue;
      if (written <= 0) break;
      at += (size_t) written;
    }
    _exit(0);
  }
  close(fd);
}
static void source_cancelled(void *data, struct zwlr_data_control_source_v1 *source) {
  struct copy *c = data;
  if (c == current) current = NULL; /* something else was copied */
  zwlr_data_control_source_v1_destroy(source);
  free(c->data);
  free(c);
}
static const struct zwlr_data_control_source_v1_listener source_listener = {source_send, source_cancelled};

/* ------------------------------------------------------------------------------ requests */

/* Replies with the clipboard's text or image, from the app that has it. */
static void get(bool image) {
  if (current) { /* ours: no app to ask, and this helper can't hand it to itself while it reads */
    if (current->image == image) reply_ok(current->data, current->length);
    else reply_line("none\n");
    return;
  }
  const struct offer *o = selection;
  if (!o || o->secret || (image ? !o->png : o->text == text_type_count)) {
    reply_line("none\n");
    return;
  }
  int fds[2];
  if (pipe2(fds, O_CLOEXEC)) {
    reply_fail("no pipe");
    return;
  }
  zwlr_data_control_offer_v1_receive(o->offer, image ? png_type : text_types[o->text], fds[1]);
  wl_display_flush(display);
  close(fds[1]);
  size_t size = 0, capacity = 65536;
  char *data = malloc(capacity);
  const char *failed = data ? NULL : "out of memory";
  const int64_t until = now_ms() + HAND_OVER_MS;
  while (!failed) {
    const int64_t left = until - now_ms();
    struct pollfd p = {fds[0], POLLIN, 0};
    if (left <= 0 || poll(&p, 1, (int) left) <= 0) {
      failed = "the app that copied it didn't hand it over in time";
      break;
    }
    if (size == capacity) {
      if (capacity > VIBEPOLLO_CLIPBOARD_MAX) {
        failed = "it is over the size limit";
        break;
      }
      /* Never more than one byte past the limit, which tells it's over. */
      const size_t next = capacity * 2 > VIBEPOLLO_CLIPBOARD_MAX ? VIBEPOLLO_CLIPBOARD_MAX + 1 : capacity * 2;
      char *bigger = realloc(data, next);
      if (!bigger) {
        failed = "out of memory";
        break;
      }
      data = bigger, capacity = next;
    }
    const ssize_t got = read(fds[0], data + size, capacity - size);
    if (got < 0 && errno == EINTR) continue;
    if (got < 0) failed = "it couldn't be read";
    if (got <= 0) break;
    size += (size_t) got;
  }
  close(fds[0]);
  if (!failed && size > VIBEPOLLO_CLIPBOARD_MAX) failed = "it is over the size limit";
  if (failed) reply_fail(failed);
  else if (!size || (image && !vibepollo_clipboard_is_png(data, size))) reply_line("none\n");
  else reply_ok(data, size);
  free(data);
}

/* Reads exactly length bytes of a request, giving Vibepollo REQUEST_MS. */
static bool read_exactly(char *data, size_t length) {
  const int64_t until = now_ms() + REQUEST_MS;
  for (size_t at = 0; at < length;) {
    const int64_t left = until - now_ms();
    struct pollfd p = {STDIN_FILENO, POLLIN, 0};
    if (left <= 0 || poll(&p, 1, (int) left) <= 0) return false;
    const ssize_t got = read(STDIN_FILENO, data + at, length - at);
    if (got < 0 && errno == EINTR) continue;
    if (got <= 0) return false;
    at += (size_t) got;
  }
  return true;
}

static void set(bool image, size_t length) {
  struct copy *c = calloc(1, sizeof *c);
  char *data = malloc(length);
  if (!c || !data || !read_exactly(data, length)) {
    /* Out of step with Vibepollo now: it starts a new helper. */
    fprintf(stderr, "vibepollo-clipboard: a request's content didn't come whole\n");
    _exit(1);
  }
  if (image && !vibepollo_clipboard_is_png(data, length)) {
    free(data), free(c);
    reply_fail("that isn't a PNG image");
    return;
  }
  c->data = data, c->length = length, c->image = image;
  c->source = zwlr_data_control_manager_v1_create_data_source(manager);
  zwlr_data_control_source_v1_add_listener(c->source, &source_listener, c);
  if (image) zwlr_data_control_source_v1_offer(c->source, png_type);
  else
    for (int i = 0; i < text_type_count; ++i) zwlr_data_control_source_v1_offer(c->source, text_types[i]);
  /* The one before gets "cancelled" and goes. */
  current = c;
  zwlr_data_control_device_v1_set_selection(device, c->source);
  if (wl_display_roundtrip(display) < 0) {
    reply_fail("the desktop went away");
    _exit(1);
  }
  reply_ok(NULL, 0);
}

/* One request from stdin. False when Vibepollo is gone (stdin ended). */
static bool request(void) {
  char line[VIBEPOLLO_CLIPBOARD_LINE];
  size_t n = 0;
  /* Byte by byte: what follows the line is the request's content, read after. */
  for (;;) {
    const ssize_t got = read(STDIN_FILENO, line + n, 1);
    if (got < 0 && errno == EINTR) continue;
    if (got <= 0) return false;
    if (line[n] == '\n') break;
    if (++n == sizeof line) {
      fprintf(stderr, "vibepollo-clipboard: a request line too long\n");
      _exit(1);
    }
  }
  line[n] = 0;
  bool is_set, image;
  size_t length;
  if (!vibepollo_clipboard_parse_request(line, &is_set, &image, &length)) {
    fprintf(stderr, "vibepollo-clipboard: a request it doesn't know\n");
    _exit(1);
  }
  if (is_set) set(image, length);
  else get(image);
  return true;
}

/* ---------------------------------------------------------------------------------- main */

static void global(void *data, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version) {
  (void) data, (void) version;
  if (!seat && !strcmp(interface, wl_seat_interface.name))
    seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
  else if (!manager && !strcmp(interface, zwlr_data_control_manager_v1_interface.name))
    manager = wl_registry_bind(registry, name, &zwlr_data_control_manager_v1_interface, 1);
}
static void global_remove(void *data, struct wl_registry *registry, uint32_t name) { (void) data, (void) registry, (void) name; }
static const struct wl_registry_listener registry_listener = {global, global_remove};

int main(int argc, char **argv) {
  (void) argv;
  if (argc != 1) {
    fprintf(stderr, "usage: vibepollo-clipboard (requests on stdin, as vibepollo-clipboard-protocol.h says)\n");
    return 2;
  }
  /* Vibepollo gone mid-reply is a write error, not a signal; pasting children reap themselves. */
  signal(SIGPIPE, SIG_IGN);
  signal(SIGCHLD, SIG_IGN);
  display = wl_display_connect(NULL);
  if (!display) {
    reply_fail("no desktop session to connect to (WAYLAND_DISPLAY)");
    return 1;
  }
  struct wl_registry *registry = wl_display_get_registry(display);
  wl_registry_add_listener(registry, &registry_listener, NULL);
  if (wl_display_roundtrip(display) < 0 || !seat || !manager) {
    reply_fail(!manager ? "the desktop has no clipboard for other programs (wlr-data-control)" : "the desktop has no seat");
    return 1;
  }
  device = zwlr_data_control_manager_v1_get_data_device(manager, seat);
  zwlr_data_control_device_v1_add_listener(device, &device_listener, NULL);
  /* The device's first events are the clipboard as it stands. */
  if (wl_display_roundtrip(display) < 0) {
    reply_fail("the desktop went away");
    return 1;
  }
  reply_line("ready\n");

  const int wayland = wl_display_get_fd(display);
  for (;;) {
    while (wl_display_prepare_read(display)) {
      if (wl_display_dispatch_pending(display) < 0) return 1;
    }
    if (wl_display_flush(display) < 0 && errno != EAGAIN) {
      wl_display_cancel_read(display);
      return 1;
    }
    struct pollfd p[2] = {{wayland, POLLIN, 0}, {STDIN_FILENO, POLLIN, 0}};
    if (poll(p, 2, -1) < 0) {
      wl_display_cancel_read(display);
      if (errno == EINTR) continue;
      return 1;
    }
    if (p[0].revents & POLLIN) {
      if (wl_display_read_events(display) < 0) return 1;
    } else {
      wl_display_cancel_read(display);
    }
    if (p[0].revents & (POLLERR | POLLHUP)) return 1;
    if (wl_display_dispatch_pending(display) < 0) return 1;
    if (p[1].revents & (POLLIN | POLLHUP) && !request()) return 0;
  }
}
