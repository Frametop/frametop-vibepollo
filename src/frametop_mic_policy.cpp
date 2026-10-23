/**
 * @file src/frametop_mic_policy.cpp
 * @brief Frametop: the wire format of a client's microphone and what to do about lost packets.
 */
#include "frametop_mic_policy.h"

namespace frametop_mic::policy {
  std::optional<packet_t> parse(std::string_view payload) {
    if (payload.size() <= HEADER_SIZE || payload.size() > HEADER_SIZE + MAX_OPUS_BYTES) {
      return std::nullopt;
    }

    const auto *bytes = reinterpret_cast<const unsigned char *>(payload.data());
    if (bytes[2] != CHANNEL || bytes[3] != FLAGS) {
      return std::nullopt;
    }

    return packet_t {
      static_cast<std::uint16_t>(bytes[0] << 8 | bytes[1]),
      payload.substr(HEADER_SIZE),
    };
  }

  step_t sequence_t::next(std::uint16_t sequence) {
    const auto ahead = static_cast<std::int16_t>(static_cast<std::uint16_t>(sequence - last_));
    if (started_ && ahead <= 0 && ahead > -RESTART_BEHIND) {
      return {};
    }

    const bool restart = !started_ || ahead <= 0 || ahead - 1 > MAX_CONCEALED;
    started_ = true;
    last_ = sequence;
    if (restart) {
      return {.play = true, .reset = true};
    }
    return {.play = true, .conceal = ahead - 1};
  }
}  // namespace frametop_mic::policy
