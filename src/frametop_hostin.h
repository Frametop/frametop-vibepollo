/**
 * @file src/frametop_hostin.h
 * @brief Frametop host input: lends this computer's keyboards and mice to one Frametop client
 *        while it holds a lease (Frametop's docs/host-input.md; wire format in
 *        frametop_hostin_policy.h).
 *
 * On Linux the input comes from the capture service (vibepollo-host-input). Elsewhere every
 * lease is answered "released unsupported".
 */
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace frametop_hostin {
  /**
   * @brief A client's 0x3005 packet, on the control stream thread.
   * @param session The session it came on (only compared, never dereferenced).
   * @param permitted An encrypted control stream, and the client may use the mouse and keyboard.
   */
  void on_lease(const void *session, bool permitted, std::string_view payload);

  /// The session is going: its input goes back, and its queue with it.
  void on_session_end(const void *session);

  /// 0x3004 payloads waiting for this session, oldest first. Control stream thread.
  std::vector<std::string> take(const void *session);

  /// A client holds this computer's input: the control stream loop should wait less.
  bool active();
}  // namespace frametop_hostin
