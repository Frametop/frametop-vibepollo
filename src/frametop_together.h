/**
 * @file src/frametop_together.h
 * @brief Frametop Together: this PC's windows as panels in its person's room.
 *
 * A person in a Frametop Together room can show the others windows of their split compute PC.
 * The PC encodes them itself and sends the pictures to its own person's headset, which sends
 * them on to whoever watches (Frametop's docs/together.md, "A PC's panels"). The headset makes a
 * one-time code and hands it over on the paired connection (/api/frametop/together/sender);
 * this host calls the headset with it through ft-pcsend and then encodes each panel as the
 * headset says: at its bitrate and pictures a second, a keyframe when asked, and nothing at all
 * while nobody watches.
 *
 * A panel is a window output (FT-W1, ...) of the Frametop windows role, captured a second time
 * for the room, on its own and at the room's rate: the person's own stream of it is untouched.
 * Linux only so far.
 */
#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace video {
  struct packet_raw_t;
}

namespace frametop_together {
  /// A window output that is to be one of the PC's panels.
  struct panel_t {
    int number = 0;  ///< the panel's number in the room (wire::FIRST_PANEL to wire::LAST_PANEL)
    std::string output;
    int width = 0;  ///< the output's size
    int height = 0;
  };

  /// Calls the headset at address:port with its one-time code, in place of any earlier call.
  /// False with *error set when it can't. Whether the headset took the call shows in state().
  bool call(const std::string &address, int port, const std::string &code, std::string *error);

  /// The PC's panels from now on: every earlier one that isn't named again stops. False with
  /// *error set when one can't be a panel, or no call stands.
  bool show(const std::vector<panel_t> &panels, std::string *error);

  /// Ends the call and every capture for it.
  void stop();

  /// A window output went (its window closed, or it was put back): it is no panel any more,
  /// and a window that gets the output's name later isn't one either until show() says so.
  /// From any thread; it never waits.
  void output_released(const std::string &output);

  /// {"calling", "called", "ended", "panels": [{"number", "output", "width", "height",
  /// "watched", "encoding", "kbps", "pictures", "handed", "sent"}]}: "calling" while a call
  /// stands, "called" once the headset took its code, "ended" why the last call ended; width and
  /// height are what the panel is encoded at.
  nlohmann::json state();

#ifdef __linux__
  /// Takes an encoded picture that is one of this module's captures'. False for any other: the
  /// caller goes on with it.
  bool take_video_packet(video::packet_raw_t &packet);
#else
  inline bool take_video_packet(video::packet_raw_t &) {
    return false;
  }
#endif
}  // namespace frametop_together
