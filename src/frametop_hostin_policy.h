/**
 * @file src/frametop_hostin_policy.h
 * @brief Frametop host input: the wire format between the capture service, this host and a
 *        Frametop client (Frametop's docs/host-input.md).
 *
 * The capture service (packaging/linux/vibepollo-host-input.c) takes this computer's keyboards
 * and mice while a client holds a lease. Its text messages become control stream packets:
 *
 * - Host -> client, 0x3004: version (1), then a record.
 *   - events (1): kind (1 keyboard, 2 mouse), a little-endian u16 count, then that many
 *     evdev events, each type (u16), code (u16) and value (s32), little-endian. A report ends
 *     with 0 0 0.
 *     A keyboard record carries keys outside the mouse buttons; a mouse record those buttons
 *     (BTN_MOUSE to BTN_TASK) and relative motion.
 *   - state (2): engaged (0 or 1), then the reason it isn't, ASCII (lease, host, escape, busy,
 *     permission, overflow, dropped, stalled, closed, off, service, unsupported).
 *   - hello (3): the kinds it lends (bit 1 keyboard, bit 2 mouse), then this host's name.
 * - Client -> host, 0x3005: version (1), then engage (1) with a u32 sequence and a u16 lease
 *   in milliseconds; release (2), which also lifts the host's escape; or drop (3), which gives the
 *   input back but leaves an escape in place.
 *
 * Both only on the encrypted control stream.
 */
#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace frametop_hostin::policy {
  constexpr std::uint16_t HOST_INPUT_PACKET_TYPE = 0x3004;
  constexpr std::uint16_t LEASE_PACKET_TYPE = 0x3005;
  constexpr std::uint8_t VERSION = 1;
  /// The longest lease a client gets, whatever it asks for.
  constexpr std::chrono::milliseconds MAX_LEASE {1500};
  /// Events in one record (the capture service sends at most this many).
  constexpr std::size_t MAX_EVENTS = 64;
  constexpr std::size_t MAX_REASON = 32;
  constexpr std::size_t MAX_NAME = 60;
  /// The longest 0x3004 payload: an events record with MAX_EVENTS events.
  constexpr std::size_t MAX_PAYLOAD = 5 + MAX_EVENTS * 8;

  constexpr std::uint8_t KEYBOARD = 1;
  constexpr std::uint8_t MOUSE = 2;

  struct lease_t {
    enum action_e : std::uint8_t {
      engage = 1,
      release = 2,
      drop = 3,
    } action = release;
    std::uint32_t sequence = 0;
    std::chrono::milliseconds duration {0};  ///< capped at MAX_LEASE
    friend bool operator==(const lease_t &, const lease_t &) = default;
  };

  /// A client's 0x3005 payload, or nullopt for anything else.
  std::optional<lease_t> parse_lease(std::string_view payload);

  /// The capture service's "state engaged" or "state released REASON".
  struct service_state_t {
    bool engaged = false;
    std::string reason;
    friend bool operator==(const service_state_t &, const service_state_t &) = default;
  };

  std::optional<service_state_t> parse_service_state(std::string_view message);

  /// The capture service's "ev k|m T C V..." as a 0x3004 events payload, or nullopt when it
  /// isn't one: only key, relative and sync events of its kind, at most MAX_EVENTS of them.
  std::optional<std::string> events_payload(std::string_view message);

  /// 0x3004 state and hello payloads. Reason and name are cut to their limits, printable ASCII.
  std::string state_payload(bool engaged, std::string_view reason);
  std::string hello_payload(std::uint8_t kinds, std::string_view name);
}  // namespace frametop_hostin::policy
