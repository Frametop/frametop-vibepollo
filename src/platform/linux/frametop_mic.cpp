/**
 * @file src/platform/linux/frametop_mic.cpp
 * @brief Frametop: a client's microphone plays into a "Frametop Microphone" source, made with
 *        module-pipe-source (PulseAudio or PipeWire) while a client sends it.
 *
 * Run as the desktop user, Vibepollo adds the source itself. As the system service it can't
 * reach the desktop's sound server, so the session broker runs vibepollo-mic-source as the
 * desktop user (vibepollo-session-exec audio-mic-source) and the audio goes to it over a pipe.
 */
#include "src/frametop_mic.h"

#include "src/logging.h"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <pthread.h>
#include <pulse/context.h>
#include <pulse/error.h>
#include <pulse/introspect.h>
#include <poll.h>
#include <pulse/mainloop.h>
#include <spawn.h>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

extern char **environ;

using namespace std::literals;

namespace platf::frametop_mic {
  namespace {
    constexpr auto SOURCE_NAME = "frametop_mic";
    /// The pipe holds at most this much, so audio no app reads yet can't pile up as delay.
    constexpr int PIPE_BYTES = 8192;  // 85 ms of 16-bit mono at 48 kHz
    /// Pipe writes up to PIPE_BUF bytes go in whole or not at all.
    constexpr int CHUNK_SAMPLES = PIPE_BUF / sizeof(std::int16_t);
    constexpr auto SESSION_EXEC = "/usr/libexec/vibeshine/vibepollo-session-exec";
    /// How long the desktop's helper may take to add the source.
    constexpr int READY_MS = 5000;
    /// How long it may take to remove it once its input ends, before it's stopped.
    constexpr int EXIT_MS = 2000;

    /// Samples into a pipe, never waiting: what doesn't fit goes, so no delay piles up.
    class pipe_writer_t {
    protected:
      pipe_writer_t() {
        // A write after the reader goes away raises SIGPIPE, which would end Sunshine. This
        // thread only plays the microphone, so it blocks the signal and handles EPIPE instead.
        sigemptyset(&sigpipe_);
        sigaddset(&sigpipe_, SIGPIPE);
        pthread_sigmask(SIG_BLOCK, &sigpipe_, nullptr);
      }

      /// False when the reader is gone.
      bool write_samples(int fd, const float *samples, int count) {
        buffer_.resize(count);
        for (int i = 0; i < count; ++i) {
          buffer_[i] = static_cast<std::int16_t>(std::lrint(std::clamp(samples[i], -1.0f, 1.0f) * 32767.0f));
        }

        for (int done = 0; done < count; done += CHUNK_SAMPLES) {
          const auto samples_now = std::min(CHUNK_SAMPLES, count - done);
          if (::write(fd, buffer_.data() + done, samples_now * sizeof(std::int16_t)) >= 0) {
            continue;
          }
          if (errno == EAGAIN) {
            return true;  // the pipe is full: no app reads it, or not fast enough. The rest goes.
          }
          if (errno == EPIPE) {
            const timespec now {};
            sigtimedwait(&sigpipe_, nullptr, &now);  // take the SIGPIPE this write raised
          }
          return false;
        }
        return true;
      }

    private:
      sigset_t sigpipe_ {};
      std::vector<std::int16_t> buffer_;
    };

    /// A connection to the sound server that runs each request to the end before returning.
    class pulse_t {
    public:
      ~pulse_t() {
        if (ctx_) {
          pa_context_disconnect(ctx_);
          pa_context_unref(ctx_);
        }
        if (loop_) {
          pa_mainloop_free(loop_);
        }
      }

      bool connect() {
        loop_ = pa_mainloop_new();
        ctx_ = loop_ ? pa_context_new(pa_mainloop_get_api(loop_), "sunshine-frametop-mic") : nullptr;
        if (!ctx_ || pa_context_connect(ctx_, nullptr, PA_CONTEXT_NOFLAGS, nullptr) < 0) {
          return false;
        }
        while (true) {
          const auto state = pa_context_get_state(ctx_);
          if (state == PA_CONTEXT_READY) {
            return true;
          }
          if (!PA_CONTEXT_IS_GOOD(state) || pa_mainloop_iterate(loop_, 1, nullptr) < 0) {
            return false;
          }
        }
      }

      /// The module that made the source called @p name, or PA_INVALID_INDEX.
      std::uint32_t source_module(const char *name) {
        std::uint32_t index = PA_INVALID_INDEX;
        auto found = [](pa_context *, const pa_source_info *info, int eol, void *data) {
          if (info && !eol) {
            *static_cast<std::uint32_t *>(data) = info->owner_module;
          }
        };
        wait(pa_context_get_source_info_by_name(ctx_, name, found, &index));
        return index;
      }

      /// Loads a module, returning its index or PA_INVALID_INDEX.
      std::uint32_t load(const char *module, const std::string &args) {
        std::uint32_t index = PA_INVALID_INDEX;
        auto loaded = [](pa_context *, std::uint32_t i, void *data) {
          *static_cast<std::uint32_t *>(data) = i;
        };
        wait(pa_context_load_module(ctx_, module, args.c_str(), loaded, &index));
        return index;
      }

      void unload(std::uint32_t index) {
        auto done = [](pa_context *, int, void *) {
        };
        wait(pa_context_unload_module(ctx_, index, done, nullptr));
      }

      const char *error() const {
        return pa_strerror(pa_context_errno(ctx_));
      }

    private:
      void wait(pa_operation *op) {
        if (!op) {
          return;
        }
        while (pa_operation_get_state(op) == PA_OPERATION_RUNNING) {
          if (pa_mainloop_iterate(loop_, 1, nullptr) < 0) {
            pa_operation_cancel(op);
            break;
          }
        }
        pa_operation_unref(op);
      }

      pa_mainloop *loop_ = nullptr;
      pa_context *ctx_ = nullptr;
    };

    class pipe_device_t: public device_t, pipe_writer_t {
    public:
      ~pipe_device_t() override {
        if (fd_ >= 0) {
          close(fd_);
        }
        if (module_ != PA_INVALID_INDEX) {
          pulse_.unload(module_);
        }
        if (!fifo_.empty()) {
          unlink(fifo_.c_str());
        }
        if (!dir_.empty()) {
          rmdir(dir_.c_str());
        }
      }

      bool open() {
        const char *runtime = std::getenv("XDG_RUNTIME_DIR");
        std::string dir = (runtime && *runtime ? std::string {runtime} : "/tmp"s) + "/sunshine-frametop-mic-XXXXXX";
        if (!mkdtemp(dir.data())) {
          BOOST_LOG(error) << "Frametop mic: couldn't make a folder for its pipe: "sv << std::strerror(errno);
          return false;
        }
        dir_ = dir;
        if (mkfifo((dir_ + "/pipe").c_str(), 0600) < 0) {
          BOOST_LOG(error) << "Frametop mic: couldn't make its pipe: "sv << std::strerror(errno);
          return false;
        }
        fifo_ = dir_ + "/pipe";

        if (!pulse_.connect()) {
          BOOST_LOG(warning) << "Frametop mic: couldn't reach the sound server"sv;
          return false;
        }

        // A source left behind by a crash would hold the name.
        if (const auto stale = pulse_.source_module(SOURCE_NAME); stale != PA_INVALID_INDEX) {
          pulse_.unload(stale);
        }

        // priority.session keeps PipeWire from making it the default microphone.
        module_ = pulse_.load(
          "module-pipe-source",
          "source_name="s + SOURCE_NAME + " file=" + fifo_ +
            " format=s16le rate=48000 channels=1"
            " source_properties=\"device.description='Frametop Microphone' priority.session=1\""
        );
        if (module_ == PA_INVALID_INDEX) {
          BOOST_LOG(warning) << "Frametop mic: couldn't add Frametop Microphone: "sv << pulse_.error();
          return false;
        }

        // The module has the pipe open for reading, so this doesn't wait.
        fd_ = ::open(fifo_.c_str(), O_WRONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd_ < 0) {
          BOOST_LOG(warning) << "Frametop mic: couldn't open Frametop Microphone's pipe: "sv << std::strerror(errno);
          return false;
        }
        fcntl(fd_, F_SETPIPE_SZ, PIPE_BYTES);
        return true;
      }

      bool write(const float *samples, int count) override {
        return write_samples(fd_, samples, count);
      }

    private:
      pulse_t pulse_;
      std::string dir_;
      std::string fifo_;
      std::uint32_t module_ = PA_INVALID_INDEX;
      int fd_ = -1;
    };

    /// The source in the desktop user's sound server, made by vibepollo-mic-source there: this
    /// writes into its stdin, a pipe the session broker hands it.
    class session_device_t: public device_t, pipe_writer_t {
    public:
      ~session_device_t() override {
        if (fd_ >= 0) {
          close(fd_);  // its input ends: it removes the source and exits
        }
        if (pid_ > 0) {
          stop();
        }
      }

      /// Empty when it's ready, else why not.
      std::string open() {
        int input[2] = {-1, -1};
        int ready[2] = {-1, -1};
        if (pipe2(input, O_CLOEXEC) < 0) {
          return "couldn't make its pipe: "s + std::strerror(errno);
        }
        if (pipe2(ready, O_CLOEXEC) < 0) {
          const auto why = "couldn't make its pipe: "s + std::strerror(errno);
          close(input[0]);
          close(input[1]);
          return why;
        }

        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_adddup2(&actions, input[0], STDIN_FILENO);
        posix_spawn_file_actions_adddup2(&actions, ready[1], STDOUT_FILENO);
        // This thread blocks SIGPIPE; session-exec gets the signals as they'd be.
        posix_spawnattr_t attributes;
        posix_spawnattr_init(&attributes);
        sigset_t none;
        sigemptyset(&none);
        posix_spawnattr_setsigmask(&attributes, &none);
        posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSIGMASK);
        char *const arguments[] = {const_cast<char *>(SESSION_EXEC), const_cast<char *>("audio-mic-source"), nullptr};
        const int spawned = posix_spawn(&pid_, SESSION_EXEC, &actions, &attributes, arguments, environ);
        posix_spawn_file_actions_destroy(&actions);
        posix_spawnattr_destroy(&attributes);
        close(input[0]);
        close(ready[1]);
        if (spawned) {
          pid_ = -1;
          close(input[1]);
          close(ready[0]);
          return "couldn't start "s + SESSION_EXEC + ": " + std::strerror(spawned);
        }

        const bool is_ready = wait_ready(ready[0]);
        close(ready[0]);
        if (!is_ready) {
          close(input[1]);
          return "the desktop session didn't add it (Vibepollo's log has vibepollo-mic-source's reason)";
        }
        fd_ = input[1];
        fcntl(fd_, F_SETFL, fcntl(fd_, F_GETFL) | O_NONBLOCK);
        fcntl(fd_, F_SETPIPE_SZ, PIPE_BYTES);
        return {};
      }

      bool write(const float *samples, int count) override {
        return write_samples(fd_, samples, count);
      }

    private:
      /// The helper says "ready" once the source is there; anything else, or nothing, isn't.
      static bool wait_ready(int fd) {
        std::string said;
        for (int waited = 0; waited < READY_MS; waited += 100) {
          pollfd ready {fd, POLLIN, 0};
          const int polled = poll(&ready, 1, 100);
          if (polled < 0 && errno != EINTR) {
            return false;
          }
          if (polled <= 0) {
            continue;
          }
          char buffer[16];
          const auto got = read(fd, buffer, sizeof buffer);
          if (got <= 0) {
            return false;  // it ended
          }
          said.append(buffer, got);
          if (said.find('\n') != std::string::npos) {
            return said == "ready\n";
          }
          if (said.size() > 64) {
            return false;
          }
        }
        return false;
      }

      bool reaped(int within_ms) {
        for (int waited = 0;; waited += 20) {
          const auto done = waitpid(pid_, nullptr, WNOHANG);
          if (done == pid_ || (done < 0 && errno == ECHILD)) {
            return true;
          }
          if (waited >= within_ms) {
            return false;
          }
          const timespec pause {0, 20'000'000};
          nanosleep(&pause, nullptr);
        }
      }

      /// session-exec ends when the helper does; else stopping it makes the broker stop the helper.
      void stop() {
        if (reaped(EXIT_MS)) {
          return;
        }
        kill(pid_, SIGTERM);
        if (!reaped(EXIT_MS)) {
          kill(pid_, SIGKILL);
          waitpid(pid_, nullptr, 0);
        }
      }

      pid_t pid_ = -1;
      int fd_ = -1;
    };
  }  // namespace

  std::unique_ptr<device_t> open_device() {
    // As a system service, Vibepollo reaches the desktop's sound only through its session broker.
    if (std::getenv("VIBEPOLLO_MACHINE_HOST")) {
      // Tried again while the client sends; said once until it works.
      static bool failing = false;
      auto device = std::make_unique<session_device_t>();
      if (const auto why = device->open(); !why.empty()) {
        if (!std::exchange(failing, true)) {
          BOOST_LOG(warning) << "Frametop mic: couldn't add Frametop Microphone to the desktop session: "sv << why;
        }
        return nullptr;
      }
      failing = false;
      BOOST_LOG(info) << "Frametop mic: playing into \"Frametop Microphone\" in the desktop session"sv;
      return device;
    }

    auto device = std::make_unique<pipe_device_t>();
    if (!device->open()) {
      return nullptr;
    }
    BOOST_LOG(info) << "Frametop mic: playing into \"Frametop Microphone\""sv;
    return device;
  }
}  // namespace platf::frametop_mic
