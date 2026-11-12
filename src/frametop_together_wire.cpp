/**
 * @file src/frametop_together_wire.cpp
 * @brief Frametop Together: what this host and ft-pcsend say to each other.
 */
#include "frametop_together_wire.h"

#include <algorithm>
#include <charconv>

namespace frametop_together::wire {
  namespace {
    using namespace std::literals;

    constexpr auto WORDS = "RFPKW"sv;

    /// The next word of a line, taken off its front.
    std::string_view word(std::string_view &line) {
      const auto end = std::min(line.find(' '), line.size());
      const auto first = line.substr(0, end);
      line.remove_prefix(std::min(end + 1, line.size()));
      return first;
    }

    /// A whole number and nothing else.
    std::optional<std::uint32_t> number(std::string_view text) {
      std::uint32_t value = 0;
      const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
      if (text.empty() || error != std::errc {} || end != text.data() + text.size()) {
        return std::nullopt;
      }
      return value;
    }
  }  // namespace

  std::string record(int number, bool key, std::string_view picture) {
    if (!is_panel(number) || picture.empty() || picture.size() > MAX_PICTURE) {
      return {};
    }
    const auto length = static_cast<std::uint32_t>(3 + picture.size());
    std::string bytes;
    bytes.reserve(4 + length);
    for (int shift = 0; shift < 32; shift += 8) {
      bytes.push_back(static_cast<char>(length >> shift & 0xff));
    }
    bytes.push_back('N');
    bytes.push_back(static_cast<char>(number));
    bytes.push_back(key ? 1 : 0);
    return bytes.append(picture);
  }

  std::optional<line_t> parse(std::string_view line) {
    if (line.starts_with("refused: "sv)) {
      return ended_t {std::string {line.substr(9)}};
    }
    if (line == "the link to the headset ended"sv || line == "the headset said what isn't a message"sv) {
      return ended_t {std::string {line}};
    }
    const auto first = word(line);
    if (first == "called"sv) {
      return is_address(line) ? std::optional<line_t> {called_t {std::string {line}}} : std::nullopt;
    }
    if (first != "panel"sv) {
      return std::nullopt;
    }
    const auto panel = number(word(line));
    const auto what = word(line);
    const auto value = number(line);
    if (!panel || !value || !is_panel(static_cast<int>(*panel))) {
      return std::nullopt;
    }
    if (what == "sent"sv) {
      return sent_t {static_cast<int>(*panel), *value};
    }
    if (what.size() != 1 || WORDS.find(what[0]) == std::string_view::npos) {
      return std::nullopt;
    }
    return told_t {static_cast<int>(*panel), what[0], *value};
  }

  bool is_address(std::string_view text) {
    const auto of_an_address = [](char c) {
      return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F') || c == '.' || c == ':';
    };
    return text.size() >= 2 && text.size() <= 45 && std::all_of(text.begin(), text.end(), of_an_address);
  }

  bool is_code(std::string_view text) {
    const auto of_a_code = [](char c) {
      return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    };
    return text.size() == CODE_LENGTH && std::all_of(text.begin(), text.end(), of_a_code);
  }
}  // namespace frametop_together::wire
