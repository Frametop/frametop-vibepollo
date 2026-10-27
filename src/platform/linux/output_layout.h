/**
 * @file src/platform/linux/output_layout.h
 * @brief The compositor's output layout, as reported by vibepollo-display-observer --layout.
 */
#pragma once

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace platf::linux_display::layout {
  // One output's rectangle in compositor (logical) coordinates.
  struct output_rect_t {
    std::string name;
    int x;
    int y;
    int width;
    int height;
    bool operator==(const output_rect_t &) const = default;
  };

  struct layout_t {
    std::vector<output_rect_t> outputs;
    // The bounding box of every output. Compositors map an absolute pointer onto this box.
    int width;
    int height;
  };

  inline constexpr std::size_t max_reply_bytes = 16 * 1024;
  inline constexpr std::size_t max_outputs = 64;
  inline constexpr std::size_t max_name_length = 127;
  inline constexpr int max_coordinate = 1 << 20;
  inline constexpr int max_extent = 1 << 16;

  namespace detail {
    inline std::optional<int> parse_int(std::string_view text, int minimum, int maximum) {
      int value = 0;
      const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
      if (error != std::errc {} || end != text.data() + text.size() || value < minimum || value > maximum) {
        return std::nullopt;
      }
      return value;
    }

    inline bool valid_name(std::string_view name) {
      return !name.empty() && name.size() <= max_name_length && std::ranges::all_of(name, [](const unsigned char character) {
        return (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') ||
               (character >= '0' && character <= '9') || character == '_' || character == '.' || character == '-';
      });
    }
  }  // namespace detail

  /**
   * Parse the observer's reply: one "L\tname\tx\ty\twidth\theight" line per output.
   * The reply crosses the session boundary, so any malformed or duplicate line rejects all of it.
   * Positions come back moved so the bounding box starts at (0, 0).
   */
  inline std::optional<layout_t> parse(std::string_view reply) {
    if (reply.size() > max_reply_bytes) return std::nullopt;
    std::vector<output_rect_t> outputs;
    while (!reply.empty()) {
      const auto end = reply.find('\n');
      if (end == std::string_view::npos) return std::nullopt;
      auto line = reply.substr(0, end);
      reply.remove_prefix(end + 1);

      std::vector<std::string_view> fields;
      while (true) {
        const auto tab = line.find('\t');
        fields.push_back(line.substr(0, tab));
        if (tab == std::string_view::npos) break;
        line.remove_prefix(tab + 1);
      }
      if (fields.size() != 6 || fields[0] != "L" || !detail::valid_name(fields[1])) return std::nullopt;
      const auto x = detail::parse_int(fields[2], -max_coordinate, max_coordinate);
      const auto y = detail::parse_int(fields[3], -max_coordinate, max_coordinate);
      const auto width = detail::parse_int(fields[4], 1, max_extent);
      const auto height = detail::parse_int(fields[5], 1, max_extent);
      if (!x || !y || !width || !height || outputs.size() >= max_outputs ||
          std::ranges::any_of(outputs, [&](const auto &output) { return output.name == fields[1]; })) {
        return std::nullopt;
      }
      outputs.push_back({std::string(fields[1]), *x, *y, *width, *height});
    }
    if (outputs.empty()) return std::nullopt;

    const auto left = std::ranges::min(outputs, {}, &output_rect_t::x).x;
    const auto top = std::ranges::min(outputs, {}, &output_rect_t::y).y;
    layout_t result {std::move(outputs), 0, 0};
    for (auto &output : result.outputs) {
      output.x -= left;
      output.y -= top;
      result.width = std::max(result.width, output.x + output.width);
      result.height = std::max(result.height, output.y + output.height);
    }
    return result;
  }

  // A capture output the layout can place. Viewport is platf::touch_port_t.
  template<class Viewport>
  struct capture_output_t {
    std::string_view connector_name;
    Viewport *viewport;
  };

  /**
   * Give each capture output its layout position and logical size, matched by exact connector
   * name. A name shared by two capture outputs (one per GPU) can't be placed, and neither can an
   * output whose logical size is turned against its scanout, since that needs a rotation the
   * touch port can't express. Returns the layout outputs left unplaced.
   */
  template<class Viewport>
  std::vector<std::string> place(const layout_t &layout, const std::vector<capture_output_t<Viewport>> &capture_outputs) {
    std::vector<std::string> unplaced;
    for (const auto &output : layout.outputs) {
      Viewport *viewport = nullptr;
      std::size_t matches = 0;
      for (const auto &capture_output : capture_outputs) {
        if (capture_output.connector_name == output.name) {
          viewport = capture_output.viewport;
          ++matches;
        }
      }
      if (matches != 1 || (viewport->width > viewport->height) != (output.width > output.height)) {
        unplaced.push_back(output.name);
        continue;
      }
      viewport->output_name = output.name;
      viewport->offset_x = output.x;
      viewport->offset_y = output.y;
      viewport->logical_width = output.width;
      viewport->logical_height = output.height;
    }
    return unplaced;
  }
}  // namespace platf::linux_display::layout
