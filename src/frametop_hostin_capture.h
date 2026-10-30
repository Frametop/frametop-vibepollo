/**
 * @file src/frametop_hostin_capture.h
 * @brief Frametop host input on Windows: which of this computer's keys and buttons go to the
 *        client, the lease and the escape hotkey. Pure, so the component tests check it on every
 *        platform; src/platform/windows/frametop_hostin.cpp feeds it from its hooks.
 *
 * The rules are the Linux capture service's (packaging/linux/vibepollo-host-input-policy.h),
 * with Windows' events: a low-level hook sees every keyboard and mouse at once, and a key held
 * down repeats as more downs.
 */
#pragma once

#include <bitset>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace frametop_hostin::capture {
  // The evdev codes this uses (linux/input-event-codes.h).
  constexpr std::uint16_t EV_SYN = 0;
  constexpr std::uint16_t EV_KEY = 1;
  constexpr std::uint16_t EV_REL = 2;
  constexpr std::uint16_t REL_X = 0x00;
  constexpr std::uint16_t REL_Y = 0x01;
  constexpr std::uint16_t REL_HWHEEL = 0x06;
  constexpr std::uint16_t REL_WHEEL = 0x08;
  constexpr std::uint16_t REL_WHEEL_HI_RES = 0x0b;
  constexpr std::uint16_t REL_HWHEEL_HI_RES = 0x0c;
  constexpr std::uint16_t KEY_ESC = 1;
  constexpr std::uint16_t KEY_LEFTSHIFT = 42;
  constexpr std::uint16_t KEY_RIGHTSHIFT = 54;
  constexpr std::uint16_t KEY_MAX = 0x2ff;
  constexpr std::uint16_t BTN_LEFT = 0x110;
  constexpr std::uint16_t BTN_RIGHT = 0x111;
  constexpr std::uint16_t BTN_MIDDLE = 0x112;
  constexpr std::uint16_t BTN_SIDE = 0x113;
  constexpr std::uint16_t BTN_EXTRA = 0x114;

  /// A lease keeps the input for this long; the Frame renews it every 500 ms.
  constexpr std::chrono::milliseconds LEASE {1500};
  /// Keys that went to the client stay taken after it lets go, until they come up here (so a
  /// held key doesn't start repeating on this computer), but no longer than this.
  constexpr std::chrono::milliseconds DRAIN_LIMIT {2000};

  /**
   * @brief A low-level keyboard hook's key as an evdev key code: positional, so the client's
   *        keyboard layout decides what it types.
   * @param scan Its set 1 scan code (KBDLLHOOKSTRUCT::scanCode).
   * @param extended LLKHF_EXTENDED: the E0 prefix.
   * @param vk Its virtual key, for the few keys whose scan code says too little.
   * @return 0 for a key there's no code for.
   */
  std::uint16_t key_code(std::uint32_t scan, bool extended, std::uint32_t vk);

  /// Volume, media and brightness keys never go to the client (the Linux service's list).
  bool kept_key(std::uint16_t code);
  /// The same by virtual key: some keyboards send these with no scan code.
  bool kept_vk(std::uint32_t vk);

  /// What happens to one key or button event.
  enum class route_e {
    pass,  ///< this computer gets it
    swallow,  ///< nobody does
    forward,  ///< the client gets it, this computer doesn't
  };

  /**
   * @brief Where each key and mouse button goes. Keys are evdev codes (buttons BTN_LEFT to
   *        BTN_EXTRA); a virtual key (0 to 255, the button VKs for buttons) tells keys held
   *        here at the take apart.
   */
  class keys_t {
  public:
    /// The input is taken; @p held_here are the virtual keys down on this computer now.
    void take(const std::bitset<256> &held_here);

    /// One key or button event (a repeat is another down), while taken or draining.
    route_e key(std::uint16_t code, std::uint32_t vk, bool down);

    /// Follows every key event here; true when, taken, it completes the escape hotkey: both
    /// Shifts held, and Escape pressed. That Escape is taken too, until it comes up.
    bool escape(std::uint16_t code, bool down);

    /// The input goes back: the keys and buttons the client holds, to let go of there. Those
    /// still down here stay taken until they come up (draining).
    std::vector<std::uint16_t> give_back();

    /// After give_back: keys still waiting to come up here.
    bool draining() const;
    void end_drain();
    bool taken() const;

  private:
    bool taken_ = false;
    std::bitset<KEY_MAX + 1> client_;  ///< down on the client, from here
    std::bitset<KEY_MAX + 1> muted_;  ///< taken until they come up, nowhere
    std::bitset<256> here_;  ///< virtual keys down here at the take: they stay here
    bool left_shift_ = false;
    bool right_shift_ = false;
    bool escape_down_ = false;
  };

  /// A wheel's 120ths, and the whole notches they add up to (the client's REL_WHEEL).
  class wheel_t {
  public:
    /// The notches this delta completes. Turning the other way starts counting again.
    int notches(int delta);
    void reset();

  private:
    int partial_ = 0;
  };

  /// The lease and the escape latch: while escaped no engage takes the input; release lifts
  /// that, drop doesn't.
  class lease_t {
  public:
    using clock = std::chrono::steady_clock;

    enum class request_e {
      invalid,
      engage,
      release,
      drop,
    };

    enum class step_e {
      stay,  ///< nothing changes
      take,  ///< take the input, and say "state engaged"
      give_back,  ///< give it back, and say "state released host"
      still_escaped,  ///< say "state released escape"
    };

    /// Exactly "engage", "release" or "drop".
    static request_e parse(std::string_view request);

    step_e request(request_e request, clock::time_point now);
    /// True when the lease ran out now: give the input back ("state released lease").
    bool lapsed(clock::time_point now);
    /// The hotkey: give it back ("state released escape") and refuse engage until a release.
    void escape();
    /// Taking it failed, or this computer's secure desktop has it: not engaged, not escaped.
    void refuse();
    bool engaged() const;
    clock::time_point until() const;

  private:
    bool engaged_ = false;
    bool escaped_ = false;
    clock::time_point until_ {};
  };

  struct event_t {
    std::uint16_t type;
    std::uint16_t code;
    std::int32_t value;
  };

  /// The capture service's "ev k|m T C V..." for a keyboard ('k') or mouse ('m') report,
  /// ending with 0 0 0 (frametop_hostin_policy.h reads it).
  std::string events_message(char kind, const std::vector<event_t> &events);
}  // namespace frametop_hostin::capture
