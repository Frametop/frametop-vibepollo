/**
 * @file src/platform/linux/frametop_mic.cpp
 * @brief Frametop: a client's microphone plays into a "Frametop Microphone" source, made with
 *        module-pipe-source (PulseAudio or PipeWire) while a client sends it.
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
#include <pulse/mainloop.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using namespace std::literals;

namespace platf::frametop_mic {
  namespace {
    constexpr auto SOURCE_NAME = "frametop_mic";
    /// The pipe holds at most this much, so audio no app reads yet can't pile up as delay.
    constexpr int PIPE_BYTES = 8192;  // 85 ms of 16-bit mono at 48 kHz
    /// Pipe writes up to PIPE_BUF bytes go in whole or not at all.
    constexpr int CHUNK_SAMPLES = PIPE_BUF / sizeof(std::int16_t);

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

    class pipe_device_t: public device_t {
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
        // A write after the sound server goes away raises SIGPIPE, which would end Sunshine. This
        // thread only plays the microphone, so it blocks the signal and handles EPIPE instead.
        sigemptyset(&sigpipe_);
        sigaddset(&sigpipe_, SIGPIPE);
        pthread_sigmask(SIG_BLOCK, &sigpipe_, nullptr);

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
        buffer_.resize(count);
        for (int i = 0; i < count; ++i) {
          buffer_[i] = static_cast<std::int16_t>(std::lrint(std::clamp(samples[i], -1.0f, 1.0f) * 32767.0f));
        }

        for (int done = 0; done < count; done += CHUNK_SAMPLES) {
          const auto samples_now = std::min(CHUNK_SAMPLES, count - done);
          if (::write(fd_, buffer_.data() + done, samples_now * sizeof(std::int16_t)) >= 0) {
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
      pulse_t pulse_;
      sigset_t sigpipe_ {};
      std::string dir_;
      std::string fifo_;
      std::uint32_t module_ = PA_INVALID_INDEX;
      int fd_ = -1;
      std::vector<std::int16_t> buffer_;
    };
  }  // namespace

  std::unique_ptr<device_t> open_device() {
    auto device = std::make_unique<pipe_device_t>();
    if (!device->open()) {
      return nullptr;
    }
    BOOST_LOG(info) << "Frametop mic: playing into \"Frametop Microphone\""sv;
    return device;
  }
}  // namespace platf::frametop_mic
