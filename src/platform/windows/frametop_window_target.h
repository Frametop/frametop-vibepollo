/**
 * @file src/platform/windows/frametop_window_target.h
 * @brief A Frametop window output's name: "window:0x<HWND>", one app window streamed on its own.
 *
 * The window role (frametop_windows.h) names each window it hands to a Frametop panel this way,
 * and a display-role stream of that name captures the window alone: the WGC helper makes its
 * capture item for the window, not a monitor, and input goes to the window's rectangle. Header
 * only, so the WGC helper (a separate exe) reads the same names.
 */
#pragma once

#include <windows.h>
#include <dwmapi.h>

#include <charconv>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>

namespace platf::frametop_window_target {
  inline constexpr std::string_view prefix = "window:";

  /** The window a "window:0x..." name names, if it's one (it needn't exist any more). */
  inline std::optional<HWND> parse(std::string_view name) {
    if (name.substr(0, prefix.size()) != prefix) return std::nullopt;
    auto hex = name.substr(prefix.size());
    if (hex.size() > 2 && hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X')) hex.remove_prefix(2);
    if (hex.empty() || hex.size() > 16) return std::nullopt;
    std::uint64_t value = 0;
    const auto [end, ec] = std::from_chars(hex.data(), hex.data() + hex.size(), value, 16);
    if (ec != std::errc {} || end != hex.data() + hex.size() || value == 0) return std::nullopt;
    return reinterpret_cast<HWND>(static_cast<std::uintptr_t>(value));
  }

  inline std::optional<HWND> parse(std::wstring_view name) {
    std::string narrow;
    for (const auto c : name) {
      if (c > 0x7f) return std::nullopt;
      narrow.push_back(static_cast<char>(c));
    }
    return parse(std::string_view {narrow});
  }

  inline bool is_window_output(std::string_view name) {
    return name.substr(0, prefix.size()) == prefix;
  }

  /** "0x1a2b": a window's address as the window list gives it. */
  inline std::string address(HWND hwnd) {
    char buffer[24];
    std::snprintf(buffer, sizeof(buffer), "0x%llx", static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(hwnd)));
    return buffer;
  }

  inline std::string output_name(HWND hwnd) {
    return std::string {prefix} + address(hwnd);
  }

  /**
   * The window as it's drawn, without its drop shadow, in screen pixels: what Windows Graphics
   * Capture captures of it. Falls back to the window rectangle.
   */
  inline std::optional<RECT> capture_rect(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return std::nullopt;
    RECT rect {};
    if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &rect, sizeof(rect))) &&
        !GetWindowRect(hwnd, &rect)) {
      return std::nullopt;
    }
    if (rect.right <= rect.left || rect.bottom <= rect.top) return std::nullopt;
    return rect;
  }
}  // namespace platf::frametop_window_target
