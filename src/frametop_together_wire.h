/**
 * @file src/frametop_together_wire.h
 * @brief Frametop Together: what this host and ft-pcsend say to each other.
 *
 * A PC's windows can be panels in a Frametop Together room. Frametop's ft-pcsend is the PC's
 * end of that room: this host runs it, hands it each encoded picture on its standard input and
 * encodes by the lines it prints (Frametop's together/pcsend/feed.h is the other side of this
 * file, and the two meet only in these bytes and lines).
 *
 * A picture is one record:
 *   u32 its length after these four bytes (little-endian), 'N', u8 the panel's number,
 *   u8 flags (1: a keyframe), the encoded picture (an HEVC access unit)
 *
 * The lines:
 *   panel N R BITS     the bits a second to encode panel N at
 *   panel N F COUNT    the pictures a second its owner lets it have
 *   panel N P COUNT    the pictures a second to encode, at most
 *   panel N K 0        the next picture is to be a keyframe
 *   panel N W 1|0      whether anyone watches it
 *   panel N sent COUNT how many pictures it sent of it so far
 *   called ADDRESS     the headset took the call
 *   refused: WHY       it didn't
 *   the link to the headset ended
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace frametop_together::wire {
  /// The numbers a PC's panels have in its person's room (the Frame's own are 0 to 2).
  constexpr int FIRST_PANEL = 3;
  constexpr int LAST_PANEL = 6;
  /// The longest picture a room takes.
  constexpr std::size_t MAX_PICTURE = 1u << 20;
  /// A one-time code's length.
  constexpr std::size_t CODE_LENGTH = 8;

  constexpr bool is_panel(int number) {
    return number >= FIRST_PANEL && number <= LAST_PANEL;
  }

  /// One picture as ft-pcsend reads it. Empty when it can't be one: a number no PC panel has, no
  /// picture, or one longer than a room takes.
  std::string record(int number, bool key, std::string_view picture);

  /// "panel N R|F|P|K|W VALUE"
  struct told_t {
    int number;
    char word;
    std::uint32_t value;
    friend bool operator==(const told_t &, const told_t &) = default;
  };

  /// "panel N sent COUNT"
  struct sent_t {
    int number;
    std::uint32_t count;
    friend bool operator==(const sent_t &, const sent_t &) = default;
  };

  /// "called ADDRESS"
  struct called_t {
    std::string address;
    friend bool operator==(const called_t &, const called_t &) = default;
  };

  /// "refused: WHY", or a line that says the link ended.
  struct ended_t {
    std::string why;
    friend bool operator==(const ended_t &, const ended_t &) = default;
  };

  using line_t = std::variant<told_t, sent_t, called_t, ended_t>;

  /// One line of ft-pcsend's, without its newline. nullopt for anything else: it is ignored.
  std::optional<line_t> parse(std::string_view line);

  /// An IP address as written in numbers (ft-pcsend itself refuses one that isn't the tailnet's).
  bool is_address(std::string_view text);
  /// A one-time code: eight letters or digits.
  bool is_code(std::string_view text);
}  // namespace frametop_together::wire
