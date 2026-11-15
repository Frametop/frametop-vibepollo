/**
 * @file src/platform/windows/frametop_vdesktop.h
 * @brief Frametop windows on Windows' other virtual desktops (Task View's).
 *
 * Desktopless mode lists every app window, as the Hyprland side lists every workspace's. Windows
 * hides the windows of the desktops not shown (DWM cloaks them for the shell) and a capture of
 * one shows nothing, so a window opened from another desktop comes to the one shown first, and
 * goes back to its own when it's released, as a Hyprland window goes back to its workspace.
 *
 * Which desktop a window is on is documented (IVirtualDesktopManager). Moving another program's
 * window isn't: that takes the shell's own IVirtualDesktopManagerInternal, whose interface ids
 * change between Windows builds. The ones here are Windows 11 23H2's and 24H2's (and later's, as
 * of 2025-08); on another build the shell doesn't answer to them and the move fails, safely.
 *
 * Every call needs COM on the calling thread, and runs its shell calls through `as_user`.
 */
#pragma once

#include <windows.h>

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace platf::frametop_vdesktop {
  /**
   * Runs a job where the shell answers it: as the signed-in user when Vibepollo runs as SYSTEM.
   * False when it couldn't run the job.
   */
  using as_user_t = std::function<bool(const std::function<void()> &)>;

  /** Hidden by the shell: on another desktop, or a suspended Store app's frame. Worth asking about. */
  bool shell_cloaked(HWND hwnd);

  /** Of these windows, the ones on a virtual desktop other than the one shown, in the same order. */
  std::vector<HWND> elsewhere(const std::vector<HWND> &windows, const as_user_t &as_user);

  /**
   * Brings a window from another desktop to the one shown. The desktop it was on, to send it back
   * to, or nullopt with *error set.
   */
  std::optional<GUID> bring_here(HWND hwnd, const as_user_t &as_user, std::string *error);

  /** Sends a window back to that desktop. False if it's gone (the window stays where it is). */
  bool send_back(HWND hwnd, const GUID &desktop, const as_user_t &as_user);

  /** A desktop's id as the state file keeps it ({...}), and back. */
  std::string to_string(const GUID &desktop);
  std::optional<GUID> parse(const std::string &text);
}  // namespace platf::frametop_vdesktop
