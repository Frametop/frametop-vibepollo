/**
 * @file src/frametop_mic.h
 * @brief Frametop: plays the microphone a client sends on its encrypted control stream into a
 *        virtual microphone on this computer, so apps here can record it.
 */
#pragma once

#include "frametop_mic_policy.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

struct OpusDecoder;

namespace frametop_mic {
  class player_t;

  /**
   * @brief One session's microphone.
   * @details Called from the control stream thread only. It decodes there and hands the audio to
   *          a player thread, so a slow device (or a driver install) never holds up input. Only
   *          one session plays at a time; another one waits until it ends.
   */
  class receiver_t {
  public:
    explicit receiver_t(std::string client_name);
    ~receiver_t();
    receiver_t(const receiver_t &) = delete;
    receiver_t &operator=(const receiver_t &) = delete;

    /// Handles the payload of one microphone packet.
    void receive(std::string_view payload);
    /// Ignores this session's microphone from now on, and says why once.
    void refuse(std::string_view reason);

  private:
    bool claim();
    void decode(std::string_view opus, int frame_samples, bool fec);

    std::string client_;
    OpusDecoder *decoder_ = nullptr;
    policy::sequence_t sequence_;
    std::unique_ptr<player_t> player_;
    std::chrono::steady_clock::time_point retry_at_ {};
    bool refused_ = false;
    bool told_waiting_ = false;
    std::uint64_t packets_ = 0;
    std::uint64_t concealed_ = 0;
    std::uint64_t bad_packets_ = 0;
    std::uint64_t decode_errors_ = 0;
  };
}  // namespace frametop_mic

namespace platf::frametop_mic {
  /// The virtual microphone apps on this computer record from. Used from one thread.
  class device_t {
  public:
    virtual ~device_t() = default;

    /// Plays mono 48 kHz samples into it. Returns false when the device has gone away.
    virtual bool write(const float *samples, int count) = 0;
  };

  /**
   * @brief Opens the virtual microphone, setting it up first if it's missing. May block for
   *        seconds.
   * @return nullptr when there isn't one (logged).
   */
  std::unique_ptr<device_t> open_device();
}  // namespace platf::frametop_mic
