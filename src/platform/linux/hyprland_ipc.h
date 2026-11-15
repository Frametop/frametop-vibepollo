/**
 * @file src/platform/linux/hyprland_ipc.h
 * @brief The session's Hyprland IPC socket: one command per connection, its whole answer back.
 *
 * Found by HYPRLAND_INSTANCE_SIGNATURE under $XDG_RUNTIME_DIR/hypr, else the one instance there.
 * Every call blocks for at most 5 s each way.
 */
#pragma once

#include <nlohmann/json.hpp>

#include <optional>
#include <string>

namespace platf::hyprland_windows::ipc {
  /** Hyprland's answer to `command`, or nullopt when its socket doesn't answer. */
  std::optional<std::string> ask(const std::string &command);

  /**
   * Hyprland passes window titles through as they are, and one with bytes that aren't UTF-8
   * (an old X11 app's) would make the whole answer unreadable: those bytes become '?'.
   */
  std::string valid_utf8(const std::string &in);

  /** A j/ query's array, or nullopt when Hyprland didn't answer or the answer isn't one. */
  std::optional<nlohmann::json> query(const std::string &what);

  /** query(), with an empty array for no answer. */
  nlohmann::json ask_json(const std::string &what);

  /** A command Hyprland answers "ok" to; false with *error set otherwise. */
  bool run(const std::string &command, std::string *error);

  /** Lua code for Hyprland 0.55+ to eval. */
  bool lua(const std::string &code, std::string *error);

  /** A Lua string literal. Only text from Hyprland or this host's .desktop files gets here. */
  std::string lua_string(const std::string &text);

  /**
   * A connection to Hyprland's event socket (.socket2.sock, beside the one it answers on): a
   * stream of "event>>data" lines. The caller closes it. -1 when there's none.
   */
  int connect_events();
}  // namespace platf::hyprland_windows::ipc
