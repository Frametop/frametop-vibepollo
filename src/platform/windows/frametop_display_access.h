/**
 * @file src/platform/windows/frametop_display_access.h
 * @brief What Frametop's display changes share on Windows: the display device API as the
 *        logged-in user, the modes a display lists, and putting displays back in their places.
 *        None of it saves to Windows' display settings, so a reboot undoes it all.
 */
#pragma once

#ifdef _WIN32

  #include "src/platform/windows/misc.h"

  #include <display_device/types.h>
  #include <display_device/windows/win_display_device_interface.h>
  #include <map>
  #include <memory>
  #include <optional>
  #include <string>
  #include <vector>

namespace platf::frametop_display_access {
  /// The display device API, impersonating the logged-in user (display settings are theirs).
  std::shared_ptr<display_device::WinDisplayDeviceInterface> make_display_device();

  /// Runs a display change as the logged-in user, like ImpersonatingDisplayDevice does.
  template<class Fn>
  bool as_user(Fn &&fn) {
    if (!platf::is_running_as_system()) {
      return fn();
    }
    HANDLE token = platf::retrieve_users_token(/*elevated*/ true);
    if (!token) {
      return !platf::has_active_console_session() && fn();
    }
    bool result = false;
    (void) platf::impersonate_current_user(token, [&]() {
      result = fn();
    });
    CloseHandle(token);
    return result;
  }

  struct listed_mode_t {
    unsigned width, height, hz;  // Windows lists whole hertz
  };

  /// The modes the active display named `display_name` (\\.\DISPLAYn) lists.
  std::vector<listed_mode_t> listed_modes(const std::string &display_name);

  /// Its mode now, as the list has it (whole hertz); nullopt when it isn't active.
  std::optional<listed_mode_t> current_listed_mode(const std::string &display_name);

  /// Puts every listed display back in its place in one change that Windows doesn't save, so it
  /// is all or nothing. Displays added since keep theirs. `log_prefix` starts each log line.
  bool put_places_back(const std::map<std::string, display_device::Point> &places, const char *log_prefix);
}  // namespace platf::frametop_display_access

#endif  // _WIN32
