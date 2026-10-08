/**
 * @file src/platform/windows/frametop_display_hdr.h
 * @brief Frametop: an existing display's HDR goes off while an SDR Frametop display stream
 *        captures it, and back on afterwards.
 */
#pragma once

#ifdef _WIN32

  #include <memory>
  #include <string>

namespace platf::frametop_display_hdr {
  /**
   * @brief Turns HDR off on the display shown by @p output_name (\\.\DISPLAYn) if it is on, and
   *        waits until capture sees it in SDR.
   * @return A hold that turns HDR back on when the last stream holding it lets go, or nullptr
   *         when HDR was off already or couldn't be changed (logged).
   * @details Displays it turns off are listed in frametop_display_hdr.json next to the state
   *          files until they are back on, so restore_after_restart() can finish the job after
   *          a crash or a forced stop.
   */
  std::shared_ptr<void> hold_sdr(const std::string &output_name);

  /// Turns HDR back on for the displays a previous run left off.
  void restore_after_restart();
}  // namespace platf::frametop_display_hdr

#endif  // _WIN32
