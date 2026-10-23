/**
 * @file src/frametop_mic.cpp
 * @brief Frametop: plays a client's microphone into a virtual microphone on this computer.
 */
#include "frametop_mic.h"

#include "logging.h"
#include "platform/common.h"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <opus/opus.h>
#include <thread>
#include <utility>
#include <vector>

using namespace std::literals;

namespace frametop_mic {
  namespace {
    /// The largest Opus packet is 120 ms.
    constexpr int MAX_DECODED_SAMPLES = policy::SAMPLE_RATE * 120 / 1000;
    /// Decoded audio waiting for the device. More than this is old: the oldest goes.
    constexpr std::size_t MAX_QUEUED = 10;

    std::mutex g_owner_mutex;
    const receiver_t *g_owner = nullptr;  // the session whose microphone plays
  }  // namespace

  /// Plays decoded audio into the virtual microphone on its own thread.
  class player_t {
  public:
    player_t():
        thread_ {&player_t::run, this} {
    }

    ~player_t() {
      {
        std::lock_guard lock {mutex_};
        stopping_ = true;
      }
      wake_.notify_one();
      thread_.join();
    }

    player_t(const player_t &) = delete;
    player_t &operator=(const player_t &) = delete;

    void push(std::vector<float> samples) {
      {
        std::lock_guard lock {mutex_};
        if (queue_.size() >= MAX_QUEUED) {
          queue_.pop_front();
        }
        queue_.push_back(std::move(samples));
      }
      wake_.notify_one();
    }

  private:
    void run() {
      platf::set_thread_name("frametop::mic");

      std::unique_ptr<platf::frametop_mic::device_t> device;
      auto retry_at = std::chrono::steady_clock::time_point {};
      std::unique_lock lock {mutex_};
      while (true) {
        wake_.wait(lock, [this] {
          return stopping_ || !queue_.empty();
        });
        if (stopping_) {
          break;
        }

        if (!device) {
          if (std::chrono::steady_clock::now() < retry_at) {
            queue_.clear();
            continue;
          }

          lock.unlock();
          device = platf::frametop_mic::open_device();
          lock.lock();
          if (!device) {
            retry_at = std::chrono::steady_clock::now() + 30s;
            queue_.clear();
            continue;
          }

          // Opening can take a while; start with the newest audio.
          while (queue_.size() > 1) {
            queue_.pop_front();
          }
        }

        auto samples = std::move(queue_.front());
        queue_.pop_front();
        lock.unlock();
        if (!device->write(samples.data(), static_cast<int>(samples.size()))) {
          BOOST_LOG(warning) << "Frametop mic: the virtual microphone went away; opening it again shortly"sv;
          device.reset();
          retry_at = std::chrono::steady_clock::now() + 5s;
        }
        lock.lock();
      }
      lock.unlock();

      // Closed on this thread: the platform code may tie the device to it.
      device.reset();
    }

    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<std::vector<float>> queue_;
    bool stopping_ = false;
    std::thread thread_;  // last: it starts using the members above right away
  };

  receiver_t::receiver_t(std::string client_name):
      client_ {std::move(client_name)} {
    int error = OPUS_OK;
    decoder_ = opus_decoder_create(policy::SAMPLE_RATE, 1, &error);
    if (!decoder_) {
      refuse("its Opus decoder failed: "s + opus_strerror(error));
    }
  }

  receiver_t::~receiver_t() {
    if (player_) {
      player_.reset();
      BOOST_LOG(info) << "Frametop mic: ["sv << client_ << "]'s microphone stopped after "sv << packets_
                      << " packets ("sv << concealed_ << " lost)"sv;

      std::lock_guard lock {g_owner_mutex};
      if (g_owner == this) {
        g_owner = nullptr;
      }
    }

    if (decoder_) {
      opus_decoder_destroy(decoder_);
    }
  }

  void receiver_t::refuse(std::string_view reason) {
    if (!refused_) {
      refused_ = true;
      BOOST_LOG(warning) << "Frametop mic: not playing ["sv << client_ << "]'s microphone: "sv << reason;
    }
  }

  void receiver_t::receive(std::string_view payload) {
    if (refused_) {
      return;
    }

    const auto packet = policy::parse(payload);
    if (!packet) {
      if (bad_packets_++ == 0) {
        BOOST_LOG(warning) << "Frametop mic: ["sv << client_ << "] sent a microphone packet this host can't read ("sv
                           << payload.size() << " bytes); ignoring those"sv;
      }
      return;
    }

    if (!player_ && !claim()) {
      return;
    }

    const auto step = sequence_.next(packet->sequence);
    if (!step.play) {
      return;
    }

    if (step.reset) {
      opus_decoder_ctl(decoder_, OPUS_RESET_STATE);
    }
    for (int i = 0; i < step.conceal; ++i) {
      // The last lost packet comes back from this one's forward error correction. The decoder
      // makes up the ones before it.
      const bool fec = i == step.conceal - 1;
      decode(fec ? packet->opus : std::string_view {}, policy::FRAME_SAMPLES, fec);
    }
    decode(packet->opus, MAX_DECODED_SAMPLES, false);

    ++packets_;
    concealed_ += step.conceal;
  }

  bool receiver_t::claim() {
    const auto now = std::chrono::steady_clock::now();
    if (now < retry_at_) {
      return false;
    }

    std::lock_guard lock {g_owner_mutex};
    if (g_owner && g_owner != this) {
      if (!told_waiting_) {
        told_waiting_ = true;
        BOOST_LOG(info) << "Frametop mic: ["sv << client_ << "] waits: another stream's microphone is playing"sv;
      }
      retry_at_ = now + 2s;
      return false;
    }

    g_owner = this;
    player_ = std::make_unique<player_t>();
    BOOST_LOG(info) << "Frametop mic: playing ["sv << client_ << "]'s microphone"sv;
    return true;
  }

  void receiver_t::decode(std::string_view opus, int frame_samples, bool fec) {
    std::vector<float> samples(frame_samples);
    const auto decoded = opus_decode_float(
      decoder_,
      opus.empty() ? nullptr : reinterpret_cast<const unsigned char *>(opus.data()),
      static_cast<opus_int32>(opus.size()),
      samples.data(),
      frame_samples,
      fec ? 1 : 0
    );
    if (decoded <= 0) {
      if (decode_errors_++ == 0) {
        BOOST_LOG(warning) << "Frametop mic: couldn't decode ["sv << client_ << "]'s microphone: "sv << opus_strerror(decoded);
      }
      return;
    }

    samples.resize(decoded);
    player_->push(std::move(samples));
  }
}  // namespace frametop_mic

#if !defined(_WIN32) && !defined(__linux__)
namespace platf::frametop_mic {
  std::unique_ptr<device_t> open_device() {
    BOOST_LOG(warning) << "Frametop mic: this platform has no virtual microphone to play into"sv;
    return nullptr;
  }
}  // namespace platf::frametop_mic
#endif
