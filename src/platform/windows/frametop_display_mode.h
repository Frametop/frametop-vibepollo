/**
 * @file src/platform/windows/frametop_display_mode.h
 * @brief Frametop: an existing display switches to a Frametop display stream's resolution while
 *        the stream captures it, and back afterwards.
 */
#pragma once

#ifdef _WIN32

  #include <memory>
  #include <string>

namespace platf::frametop_display_mode {
  /**
   * @brief Switches the display shown by @p output_name (\\.\DISPLAYn) to @p width x @p height
   *        at its current refresh rate (or the display's closest mode with the same shape), so
   *        the stream shows the desktop pixel for pixel instead of scaling it, and waits until
   *        capture sees the new size.
   * @return A hold that puts the display's own mode, and every display's place, back when the
   *         last stream holding it lets go, or nullptr when the display already has that
   *         resolution or couldn't be changed (logged).
   * @details The change is temporary: Windows' saved display settings never change, so a reboot
   *          also undoes it. Displays it changed are listed in frametop_display_mode.json next to
   *          the state files until they are back, so restore_after_restart() can finish the job
   *          after a crash or a forced stop.
   */
  std::shared_ptr<void> hold_resolution(const std::string &output_name, int width, int height);

  /// Puts back the displays a previous run left at a stream's resolution.
  void restore_after_restart();
}  // namespace platf::frametop_display_mode

#endif  // _WIN32
