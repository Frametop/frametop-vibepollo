// vibepollo-mic-source: a Frametop client's microphone as "Frametop Microphone" in the desktop
// user's sound server (PulseAudio, or PipeWire's pipewire-pulse), for Vibepollo's system service
// (src/platform/linux/frametop_mic.cpp). The session broker runs it as the desktop user
// (vibepollo-session-exec audio-mic-source), with the host's end of a pipe as stdin: 16-bit
// little-endian mono at 48 kHz.
//
// It adds the source with module-pipe-source, whose pipe the sound server makes in
// $XDG_RUNTIME_DIR (this helper can't write there: the broker's sandbox makes it read-only), says
// "ready" on stdout, and copies stdin into that pipe. When the pipe is full (no app records, or not
// fast enough) the audio goes, so it never piles up as delay. It ends, and the source with it, when
// stdin ends (the host stopped playing), on SIGTERM, or when the sound server or the source goes.

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pulse/context.h>
#include <pulse/error.h>
#include <pulse/introspect.h>
#include <pulse/mainloop.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SOURCE_NAME "frametop_mic"
#define FIFO_NAME "vibepollo-frametop-mic.fifo"
// The pipe holds at most this much: 85 ms of 16-bit mono at 48 kHz.
#define PIPE_BYTES 8192

static volatile sig_atomic_t stopping;

static void stop(int signal_number) {
  (void) signal_number;
  stopping = 1;
}

static void say(const char *format, ...) __attribute__((format(printf, 1, 2)));

static void say(const char *format, ...) {
  va_list arguments;
  va_start(arguments, format);
  fputs("vibepollo-mic-source: ", stderr);
  vfprintf(stderr, format, arguments);
  fputc('\n', stderr);
  va_end(arguments);
}

static pa_mainloop *loop;
static pa_context *context;

// Runs the main loop until the operation ends. False when the connection failed meanwhile.
static bool finish(pa_operation *operation) {
  if (!operation) return false;
  bool good = true;
  while (pa_operation_get_state(operation) == PA_OPERATION_RUNNING) {
    if (pa_mainloop_iterate(loop, 1, NULL) < 0) {
      pa_operation_cancel(operation);
      good = false;
      break;
    }
  }
  pa_operation_unref(operation);
  return good;
}

static bool connect_server(void) {
  loop = pa_mainloop_new();
  context = loop ? pa_context_new(pa_mainloop_get_api(loop), "vibepollo-frametop-mic") : NULL;
  // Never start a sound server of its own: the desktop's is the one apps record from.
  if (!context || pa_context_connect(context, NULL, PA_CONTEXT_NOAUTOSPAWN, NULL) < 0) return false;
  for (;;) {
    const pa_context_state_t state = pa_context_get_state(context);
    if (state == PA_CONTEXT_READY) return true;
    if (!PA_CONTEXT_IS_GOOD(state) || pa_mainloop_iterate(loop, 1, NULL) < 0) return false;
  }
}

static void found_source(pa_context *unused, const pa_source_info *info, int eol, void *data) {
  (void) unused;
  if (info && !eol) *(uint32_t *) data = info->owner_module;
}

static void loaded(pa_context *unused, uint32_t index, void *data) {
  (void) unused;
  *(uint32_t *) data = index;
}

static void unloaded(pa_context *unused, int success, void *data) {
  (void) unused;
  (void) success;
  (void) data;
}

static uint32_t source_module(void) {
  uint32_t index = PA_INVALID_INDEX;
  finish(pa_context_get_source_info_by_name(context, SOURCE_NAME, found_source, &index));
  return index;
}

static void unload(uint32_t index) {
  if (index != PA_INVALID_INDEX) finish(pa_context_unload_module(context, index, unloaded, NULL));
}

// Only a pipe of this user's: anything else at that name isn't ours to write to.
static bool our_fifo(const char *path, bool missing_ok) {
  struct stat status;
  if (lstat(path, &status)) return missing_ok && errno == ENOENT;
  return S_ISFIFO(status.st_mode) && status.st_uid == getuid();
}

// Copies stdin into the pipe until one of them ends. True when stdin ended or we were stopped.
static bool copy(int fifo) {
  unsigned char buffer[PIPE_BUF];
  size_t kept = 0;  // a sample's first byte, when a read ended between its two
  while (!stopping) {
    struct pollfd input = {.fd = STDIN_FILENO, .events = POLLIN};
    const int polled = poll(&input, 1, 500);
    if (polled < 0 && errno != EINTR) return false;
    // The server's messages (and its going away) are read here too.
    if (pa_mainloop_iterate(loop, 0, NULL) < 0 || pa_context_get_state(context) != PA_CONTEXT_READY) {
      say("the sound server went away");
      return false;
    }
    if (polled <= 0) continue;
    const ssize_t got = read(STDIN_FILENO, buffer + kept, sizeof buffer - kept);
    if (got < 0 && errno == EINTR) continue;
    if (got < 0) return false;
    if (!got) return true;  // the host stopped
    const size_t total = kept + (size_t) got;
    const size_t whole = total & ~(size_t) 1;
    // Up to PIPE_BUF bytes go in whole or not at all. Full: these go.
    if (whole && write(fifo, buffer, whole) < 0 && errno != EAGAIN) {
      if (errno == EPIPE) say("Frametop Microphone went away");
      else say("couldn't write to Frametop Microphone: %s", strerror(errno));
      return false;
    }
    kept = total - whole;
    if (kept) buffer[0] = buffer[whole];
  }
  return true;
}

int main(int argc, char **argv) {
  (void) argv;
  if (argc != 1) return 2;
  struct sigaction ignore = {.sa_handler = SIG_IGN};
  struct sigaction ending = {.sa_handler = stop};  // no SA_RESTART: poll and read return
  sigemptyset(&ignore.sa_mask);
  sigemptyset(&ending.sa_mask);
  if (sigaction(SIGPIPE, &ignore, NULL) || sigaction(SIGTERM, &ending, NULL) || sigaction(SIGINT, &ending, NULL) ||
      sigaction(SIGHUP, &ending, NULL)) {
    return 1;
  }

  const char *runtime = getenv("XDG_RUNTIME_DIR");
  char path[PATH_MAX];
  if (!runtime || runtime[0] != '/' || snprintf(path, sizeof path, "%s/%s", runtime, FIFO_NAME) >= (int) sizeof path) {
    say("no XDG_RUNTIME_DIR");
    return 1;
  }
  // The sound server makes the pipe, or uses one left by an earlier run.
  if (!our_fifo(path, true)) {
    say("%s is something else: not using it", path);
    return 1;
  }
  if (!connect_server()) {
    say("couldn't reach the desktop's sound server: %s", context ? pa_strerror(pa_context_errno(context)) : "out of memory");
    return 1;
  }
  // A source left behind (a crash, or Vibepollo run as the user) would hold the name.
  unload(source_module());

  // priority.session keeps PipeWire from making it the default microphone.
  char arguments[PATH_MAX + 256];
  snprintf(arguments, sizeof arguments,
           "source_name=" SOURCE_NAME " file=%s format=s16le rate=48000 channels=1"
           " source_properties=\"device.description='Frametop Microphone' priority.session=1\"",
           path);
  uint32_t module = PA_INVALID_INDEX;
  finish(pa_context_load_module(context, "module-pipe-source", arguments, loaded, &module));
  if (module == PA_INVALID_INDEX) {
    say("couldn't add Frametop Microphone: %s", pa_strerror(pa_context_errno(context)));
    return 1;
  }

  int status = 1;
  // The module has the pipe open for reading, so this doesn't wait.
  const int fifo = our_fifo(path, false) ? open(path, O_WRONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC) : -1;
  if (fifo < 0) {
    say("couldn't open Frametop Microphone's pipe: %s", strerror(errno));
  } else {
    (void) fcntl(fifo, F_SETPIPE_SZ, PIPE_BYTES);
    if (write(STDOUT_FILENO, "ready\n", 6) == 6 && copy(fifo)) status = 0;
    close(fifo);
  }
  unload(module);
  pa_context_disconnect(context);
  pa_context_unref(context);
  pa_mainloop_free(loop);
  return status;
}
