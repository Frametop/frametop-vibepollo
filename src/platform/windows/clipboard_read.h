/**
 * @file src/platform/windows/clipboard_read.h
 * @brief Frametop shared clipboard: reading the Windows clipboard's text or image.
 *
 * Used by tools/frametop_clipboard.cpp, which the host runs as the signed-in user: the host,
 * started by its service, can't see the formats other apps register (a password manager's
 * mark, an app's PNG), and a process of the user's can.
 */
#pragma once

#include <cstddef>
#include <string>

namespace platf::clipboard_read {
  /// The most either kind may be: a bigger one reads as nothing.
  constexpr std::size_t max_bytes = std::size_t {64} << 20;

  enum class result {
    ok,  ///< out holds the text (UTF-8, line ends as held) or the PNG file, empty for none
    secret,  ///< a password manager marked the copy: nothing is read
    busy,  ///< another app has the clipboard open
    failed,  ///< a bitmap that couldn't be made a PNG
  };

  /// The clipboard's text, or (image) its image as a PNG file: the app's own PNG if it put one
  /// there, else its bitmap (CF_DIB) made one.
  result read(bool image, std::string &out);

  /// Exit codes of tools/frametop_clipboard.exe for each result (and 1 for a bad command line).
  constexpr int exit_code(result r) {
    return r == result::ok ? 0 : r == result::secret ? 3 : r == result::busy ? 4 : 5;
  }
}  // namespace platf::clipboard_read
