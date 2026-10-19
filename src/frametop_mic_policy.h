/**
 * @file src/frametop_mic_policy.h
 * @brief Frametop: the wire format of the microphone a client sends on its encrypted control
 *        stream, and what to do about lost packets.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace frametop_mic::policy {
  /// Control stream packet type of a microphone packet (Apollo protocol extension range).
  constexpr std::uint16_t PACKET_TYPE = 0x3003;
  /// Bytes before the Opus packet: sequence number (2, big endian), channel (1), flags (1).
  constexpr std::size_t HEADER_SIZE = 4;
  constexpr std::uint8_t CHANNEL = 1;
  constexpr std::uint8_t FLAGS = 0;
  /// The largest Opus packet there is.
  constexpr std::size_t MAX_OPUS_BYTES = 1275;

  /// The audio: mono, 48 kHz, 20 ms per packet.
  constexpr int SAMPLE_RATE = 48000;
  constexpr int FRAME_SAMPLES = 960;

  /// Packets lost in a row that are filled in. A longer gap starts the decoder over.
  constexpr int MAX_CONCEALED = 5;
  /// A packet this far behind means the client started counting again.
  constexpr int RESTART_BEHIND = 50;

  struct packet_t {
    std::uint16_t sequence;
    std::string_view opus;
  };

  /// Splits a microphone payload, or nullopt when it isn't one this host can play.
  std::optional<packet_t> parse(std::string_view payload);

  struct step_t {
    bool play = false;  ///< false: a repeated or late packet, dropped
    bool reset = false;  ///< start the decoder over before this packet
    int conceal = 0;  ///< packets lost just before this one; the last comes back from its FEC
    friend bool operator==(const step_t &, const step_t &) = default;
  };

  /// Follows the sequence numbers of one session's microphone.
  class sequence_t {
  public:
    step_t next(std::uint16_t sequence);

  private:
    bool started_ = false;
    std::uint16_t last_ = 0;
  };
}  // namespace frametop_mic::policy
