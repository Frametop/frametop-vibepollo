/**
 * @file src/platform/windows/frametop_window_query.h
 * @brief What Windows says about app windows and monitors, for the Frametop window role
 * (frametop_windows.cpp). No state of its own: each answer is asked of Windows when called.
 */
#pragma once

// winsock2.h first: boost.asio (misc.h) refuses a windows.h that came without it.
#include <winsock2.h>
#include <windows.h>

#include <functional>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace platf::frametop_windows::win32 {
  std::string lower(std::string s);
  std::wstring lower(std::wstring s);

  std::wstring class_of(HWND hwnd);
  std::wstring title_of(HWND hwnd);
  std::string title_of_utf8(HWND hwnd);
  DWORD pid_of(HWND hwnd);
  // The process's .exe, lower case; empty when it can't be read.
  std::wstring exe_of(DWORD pid);
  // A window's app id (AUMID), the one Store apps are named by: the window's own (a Store
  // app's frame window says whose it is), else its packaged process's. Empty for others.
  std::string aumid_of(HWND hwnd);
  bool cloaked(HWND hwnd);

  // How a window comes back without taking the focus from what the user is doing now.
  UINT quietly(UINT show);

  // A window Alt+Tab would list on its desktop: not a tool window, not owned by another
  // (unless it asks to be on the taskbar), and titled.
  bool app_like(HWND hwnd);
  // An app window on the desktop shown, as Alt+Tab lists them: shown (or minimized), not cloaked.
  bool app_window(HWND hwnd);
  // An app window on another virtual desktop: opening it brings it here.
  bool away_window(HWND hwnd);

  // The shell's desktop calls as the signed-in user: Vibepollo runs as SYSTEM, the shell as them.
  bool as_user(const std::function<void()> &job);

  struct listed_t {
    std::vector<HWND> windows;  // front to back, every desktop's
    std::set<HWND> away;  // the ones on another virtual desktop
  };

  // The app windows: the desktop shown's, and with every_desktop the other virtual desktops' too,
  // as the Hyprland side lists every workspace's.
  listed_t app_windows(bool every_desktop = false);

  // After a move between desktops, the shell uncloaks the window a moment later.
  bool await_uncloaked(HWND hwnd);

  std::string monitor_name(HMONITOR monitor);
  // A monitor named by its device id (as /api/display-devices lists it) or \\.\DISPLAYn.
  std::optional<MONITORINFOEXW> find_monitor(const std::string &name);

  // Locked (or on the UAC prompt), the input desktop isn't Default: windows show nothing.
  bool session_locked();
}  // namespace platf::frametop_windows::win32
