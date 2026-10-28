/**
 * @file src/frametop_hostin_policy.cpp
 * @brief Frametop host input's wire format (see the header).
 */
#include "frametop_hostin_policy.h"

#include <algorithm>
#include <charconv>
#include <vector>

namespace frametop_hostin::policy {
  namespace {
    constexpr std::uint8_t ENGAGE = 1;
    constexpr std::uint8_t RELEASE = 2;
    constexpr std::uint8_t EVENTS = 1;
    constexpr std::uint8_t STATE = 2;
    constexpr std::uint8_t HELLO = 3;

    constexpr std::uint16_t EV_SYN = 0;
    constexpr std::uint16_t EV_KEY = 1;
    constexpr std::uint16_t EV_REL = 2;
    constexpr std::uint16_t KEY_MAX = 0x2ff;
    constexpr std::uint16_t REL_MAX = 0x0f;

    std::uint32_t le32(std::string_view bytes, std::size_t at) {
      return std::uint32_t(std::uint8_t(bytes[at])) | std::uint32_t(std::uint8_t(bytes[at + 1])) << 8 |
             std::uint32_t(std::uint8_t(bytes[at + 2])) << 16 | std::uint32_t(std::uint8_t(bytes[at + 3])) << 24;
    }

    void put16(std::string &out, std::uint16_t value) {
      out.push_back(char(value & 0xff));
      out.push_back(char(value >> 8));
    }

    void put32(std::string &out, std::uint32_t value) {
      for (int shift = 0; shift < 32; shift += 8) out.push_back(char((value >> shift) & 0xff));
    }

    std::string printable(std::string_view text, std::size_t limit) {
      std::string out;
      for (const char c : text.substr(0, std::min(text.size(), limit))) {
        out.push_back(c >= 0x20 && c <= 0x7e ? c : '?');
      }
      return out;
    }

    template<class T>
    bool number(std::string_view word, T &value) {
      const auto [end, error] = std::from_chars(word.data(), word.data() + word.size(), value);
      return error == std::errc {} && end == word.data() + word.size();
    }

    std::vector<std::string_view> words(std::string_view text) {
      std::vector<std::string_view> out;
      while (!text.empty()) {
        const auto space = text.find(' ');
        out.push_back(text.substr(0, space));
        if (space == std::string_view::npos) break;
        text.remove_prefix(space + 1);
      }
      return out;
    }
  }  // namespace

  std::optional<lease_t> parse_lease(std::string_view payload) {
    if (payload.size() < 2 || std::uint8_t(payload[0]) != VERSION) return std::nullopt;
    if (std::uint8_t(payload[1]) == RELEASE && payload.size() == 2) return lease_t {};
    if (std::uint8_t(payload[1]) != ENGAGE || payload.size() != 8) return std::nullopt;
    const auto ms = std::uint16_t(std::uint8_t(payload[6]) | std::uint8_t(payload[7]) << 8);
    if (!ms) return std::nullopt;
    return lease_t {true, le32(payload, 2), std::min(std::chrono::milliseconds {ms}, MAX_LEASE)};
  }

  std::optional<service_state_t> parse_service_state(std::string_view message) {
    if (message == "state engaged") return service_state_t {true, {}};
    constexpr std::string_view released = "state released ";
    if (!message.starts_with(released)) return std::nullopt;
    const auto reason = message.substr(released.size());
    if (reason.empty() || reason.size() > MAX_REASON ||
        !std::ranges::all_of(reason, [](char c) { return (c >= 'a' && c <= 'z') || c == '-'; })) {
      return std::nullopt;
    }
    return service_state_t {false, std::string(reason)};
  }

  std::optional<std::string> events_payload(std::string_view message) {
    const auto parts = words(message);
    if (parts.size() < 5 || parts[0] != "ev" || (parts[1] != "k" && parts[1] != "m") || (parts.size() - 2) % 3) {
      return std::nullopt;
    }
    const std::size_t count = (parts.size() - 2) / 3;
    if (count > MAX_EVENTS) return std::nullopt;
    std::string out;
    out.push_back(char(VERSION));
    out.push_back(char(EVENTS));
    out.push_back(char(parts[1] == "k" ? KEYBOARD : MOUSE));
    put16(out, std::uint16_t(count));
    for (std::size_t i = 0; i < count; ++i) {
      std::uint16_t type = 0, code = 0;
      std::int32_t value = 0;
      if (!number(parts[2 + i * 3], type) || !number(parts[3 + i * 3], code) || !number(parts[4 + i * 3], value)) {
        return std::nullopt;
      }
      const bool ok = (type == EV_SYN && code == 0 && value == 0) || (type == EV_KEY && code > 0 && code <= KEY_MAX && (value == 0 || value == 1)) ||
                      (type == EV_REL && code <= REL_MAX);
      if (!ok) return std::nullopt;
      put16(out, type);
      put16(out, code);
      put32(out, std::uint32_t(value));
    }
    return out;
  }

  std::string state_payload(bool engaged, std::string_view reason) {
    std::string out;
    out.push_back(char(VERSION));
    out.push_back(char(STATE));
    out.push_back(char(engaged ? 1 : 0));
    out += printable(engaged ? std::string_view {} : reason, MAX_REASON);
    return out;
  }

  std::string hello_payload(std::uint8_t kinds, std::string_view name) {
    std::string out;
    out.push_back(char(VERSION));
    out.push_back(char(HELLO));
    out.push_back(char(kinds));
    out += printable(name, MAX_NAME);
    return out;
  }
}  // namespace frametop_hostin::policy
