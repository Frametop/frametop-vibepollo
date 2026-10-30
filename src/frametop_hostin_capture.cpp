/**
 * @file src/frametop_hostin_capture.cpp
 * @brief Frametop host input on Windows: keys, lease and escape (see the header).
 */
#include "frametop_hostin_capture.h"

#include <array>

namespace frametop_hostin::capture {
  namespace {
    // Virtual keys (winuser.h) whose scan code says too little, or that stay here.
    constexpr std::uint32_t VK_PAUSE = 0x13;
    constexpr std::uint32_t VK_SNAPSHOT = 0x2c;
    constexpr std::uint32_t VK_NUMLOCK = 0x90;
    constexpr std::uint32_t VK_VOLUME_MUTE = 0xad;
    constexpr std::uint32_t VK_MEDIA_PLAY_PAUSE = 0xb3;

    struct scan_key_t {
      std::uint8_t scan;
      std::uint16_t code;
    };

    // Set 1 scan codes past the ones evdev numbers the same (1 to 0x53).
    constexpr std::array plain_keys {
      scan_key_t {0x54, 99},  // Alt+Print Screen: KEY_SYSRQ
      scan_key_t {0x56, 86},  // KEY_102ND
      scan_key_t {0x57, 87},  // KEY_F11
      scan_key_t {0x58, 88},  // KEY_F12
      scan_key_t {0x59, 117},  // KEY_KPEQUAL
      scan_key_t {0x64, 183},  // KEY_F13
      scan_key_t {0x65, 184},
      scan_key_t {0x66, 185},
      scan_key_t {0x67, 186},
      scan_key_t {0x68, 187},
      scan_key_t {0x69, 188},
      scan_key_t {0x6a, 189},
      scan_key_t {0x6b, 190},
      scan_key_t {0x6c, 191},
      scan_key_t {0x6d, 192},
      scan_key_t {0x6e, 193},  // KEY_F23
      scan_key_t {0x70, 93},  // KEY_KATAKANAHIRAGANA
      scan_key_t {0x71, 123},  // KEY_HANJA
      scan_key_t {0x72, 122},  // KEY_HANGEUL
      scan_key_t {0x73, 89},  // KEY_RO
      scan_key_t {0x76, 194},  // KEY_F24
      scan_key_t {0x79, 92},  // KEY_HENKAN
      scan_key_t {0x7b, 94},  // KEY_MUHENKAN
      scan_key_t {0x7d, 124},  // KEY_YEN
      scan_key_t {0x7e, 121},  // KEY_KPCOMMA
    };

    // With the E0 prefix.
    constexpr std::array extended_keys {
      scan_key_t {0x10, 165},  // KEY_PREVIOUSSONG
      scan_key_t {0x19, 163},  // KEY_NEXTSONG
      scan_key_t {0x1c, 96},  // KEY_KPENTER
      scan_key_t {0x1d, 97},  // KEY_RIGHTCTRL
      scan_key_t {0x20, 113},  // KEY_MUTE
      scan_key_t {0x21, 140},  // KEY_CALC
      scan_key_t {0x22, 164},  // KEY_PLAYPAUSE
      scan_key_t {0x24, 166},  // KEY_STOPCD
      scan_key_t {0x2e, 114},  // KEY_VOLUMEDOWN
      scan_key_t {0x30, 115},  // KEY_VOLUMEUP
      scan_key_t {0x32, 172},  // KEY_HOMEPAGE
      scan_key_t {0x35, 98},  // KEY_KPSLASH
      scan_key_t {0x37, 99},  // KEY_SYSRQ
      scan_key_t {0x38, 100},  // KEY_RIGHTALT
      scan_key_t {0x45, 69},  // KEY_NUMLOCK (a low-level hook marks it extended)
      scan_key_t {0x46, 119},  // Ctrl+Break: KEY_PAUSE
      scan_key_t {0x47, 102},  // KEY_HOME
      scan_key_t {0x48, 103},  // KEY_UP
      scan_key_t {0x49, 104},  // KEY_PAGEUP
      scan_key_t {0x4b, 105},  // KEY_LEFT
      scan_key_t {0x4d, 106},  // KEY_RIGHT
      scan_key_t {0x4f, 107},  // KEY_END
      scan_key_t {0x50, 108},  // KEY_DOWN
      scan_key_t {0x51, 109},  // KEY_PAGEDOWN
      scan_key_t {0x52, 110},  // KEY_INSERT
      scan_key_t {0x53, 111},  // KEY_DELETE
      scan_key_t {0x5b, 125},  // KEY_LEFTMETA
      scan_key_t {0x5c, 126},  // KEY_RIGHTMETA
      scan_key_t {0x5d, 127},  // KEY_COMPOSE
      scan_key_t {0x5e, 116},  // KEY_POWER
      scan_key_t {0x5f, 142},  // KEY_SLEEP
      scan_key_t {0x63, 143},  // KEY_WAKEUP
      scan_key_t {0x65, 217},  // KEY_SEARCH
      scan_key_t {0x66, 156},  // KEY_BOOKMARKS
      scan_key_t {0x67, 173},  // KEY_REFRESH
      scan_key_t {0x68, 128},  // KEY_STOP
      scan_key_t {0x69, 159},  // KEY_FORWARD
      scan_key_t {0x6a, 158},  // KEY_BACK
      scan_key_t {0x6b, 157},  // KEY_COMPUTER
      scan_key_t {0x6c, 155},  // KEY_MAIL
      scan_key_t {0x6d, 226},  // KEY_MEDIA
      scan_key_t {0x71, 123},  // KEY_HANJA
      scan_key_t {0x72, 122},  // KEY_HANGEUL
    };

    template<std::size_t N>
    std::uint16_t find(const std::array<scan_key_t, N> &keys, std::uint32_t scan) {
      for (const auto &key : keys) {
        if (key.scan == scan) return key.code;
      }
      return 0;
    }
  }  // namespace

  std::uint16_t key_code(std::uint32_t scan, bool extended, std::uint32_t vk) {
    // Pause comes as 0x45, Num Lock's scan code, and Print Screen's varies with Alt.
    if (vk == VK_PAUSE) return 119;  // KEY_PAUSE
    if (vk == VK_NUMLOCK) return 69;  // KEY_NUMLOCK
    if (vk == VK_SNAPSHOT) return 99;  // KEY_SYSRQ
    if (scan > 0xff) return 0;
    if (extended) return find(extended_keys, scan);  // E0 2A and E0 36 are no keys: 0
    if (scan >= 1 && scan <= 0x53) return std::uint16_t(scan);
    return find(plain_keys, scan);
  }

  bool kept_key(std::uint16_t code) {
    switch (code) {
      case 113:  // KEY_MUTE
      case 114:  // KEY_VOLUMEDOWN
      case 115:  // KEY_VOLUMEUP
      case 248:  // KEY_MICMUTE
      case 164:  // KEY_PLAYPAUSE
      case 200:  // KEY_PLAYCD
      case 201:  // KEY_PAUSECD
      case 166:  // KEY_STOPCD
      case 163:  // KEY_NEXTSONG
      case 165:  // KEY_PREVIOUSSONG
      case 208:  // KEY_FASTFORWARD
      case 168:  // KEY_REWIND
      case 224:  // KEY_BRIGHTNESSDOWN
      case 225:  // KEY_BRIGHTNESSUP
      case 229:  // KEY_KBDILLUMDOWN
      case 230:  // KEY_KBDILLUMUP
      case 228:  // KEY_KBDILLUMTOGGLE
        return true;
      default:
        return false;
    }
  }

  bool kept_vk(std::uint32_t vk) {
    return vk >= VK_VOLUME_MUTE && vk <= VK_MEDIA_PLAY_PAUSE;
  }

  void keys_t::take(const std::bitset<256> &held_here) {
    taken_ = true;
    here_ = held_here;
    client_.reset();
  }

  route_e keys_t::key(std::uint16_t code, std::uint32_t vk, bool down) {
    const bool known = code > 0 && code <= KEY_MAX;
    if (known && muted_[code]) {
      if (!down) muted_[code] = false;
      return route_e::swallow;
    }
    if (!taken_) return route_e::pass;
    if (vk < here_.size() && here_[vk]) {
      // Down here before the take: it comes up here, and its repeats stay here too.
      if (!down) here_[vk] = false;
      return route_e::pass;
    }
    if (kept_vk(vk) || (known && kept_key(code))) return route_e::pass;
    if (!known) return route_e::swallow;
    if (down) {
      if (client_[code]) return route_e::swallow;  // a repeat: the client types its own
      client_[code] = true;
      return route_e::forward;
    }
    if (!client_[code]) return route_e::swallow;
    client_[code] = false;
    return route_e::forward;
  }

  bool keys_t::escape(std::uint16_t code, bool down) {
    if (code == KEY_LEFTSHIFT) left_shift_ = down;
    else if (code == KEY_RIGHTSHIFT) right_shift_ = down;
    else if (code == KEY_ESC) {
      const bool pressed = down && !escape_down_;
      escape_down_ = down;
      if (pressed && left_shift_ && right_shift_ && taken_) {
        muted_[KEY_ESC] = true;
        return true;
      }
    }
    return false;
  }

  std::vector<std::uint16_t> keys_t::give_back() {
    std::vector<std::uint16_t> lift;
    for (std::uint16_t code = 1; code <= KEY_MAX; ++code) {
      if (client_[code]) lift.push_back(code);
    }
    muted_ |= client_;
    client_.reset();
    here_.reset();
    taken_ = false;
    return lift;
  }

  bool keys_t::draining() const {
    return !taken_ && muted_.any();
  }

  void keys_t::end_drain() {
    muted_.reset();
  }

  bool keys_t::taken() const {
    return taken_;
  }

  int wheel_t::notches(int delta) {
    if ((delta > 0 && partial_ < 0) || (delta < 0 && partial_ > 0)) partial_ = 0;
    partial_ += delta;
    const int whole = partial_ / 120;
    partial_ -= whole * 120;
    return whole;
  }

  void wheel_t::reset() {
    partial_ = 0;
  }

  lease_t::request_e lease_t::parse(std::string_view request) {
    if (request == "engage") return request_e::engage;
    if (request == "release") return request_e::release;
    if (request == "drop") return request_e::drop;
    return request_e::invalid;
  }

  lease_t::step_e lease_t::request(request_e request, clock::time_point now) {
    switch (request) {
      case request_e::engage:
        if (escaped_) return step_e::still_escaped;
        until_ = now + LEASE;
        if (engaged_) return step_e::stay;
        engaged_ = true;
        return step_e::take;
      case request_e::release:
        escaped_ = false;
        [[fallthrough]];
      case request_e::drop:
        if (!engaged_) return step_e::stay;
        engaged_ = false;
        return step_e::give_back;
      case request_e::invalid:
        break;
    }
    return step_e::stay;
  }

  bool lease_t::lapsed(clock::time_point now) {
    if (!engaged_ || now < until_) return false;
    engaged_ = false;
    return true;
  }

  void lease_t::escape() {
    engaged_ = false;
    escaped_ = true;
  }

  void lease_t::refuse() {
    engaged_ = false;
  }

  bool lease_t::engaged() const {
    return engaged_;
  }

  lease_t::clock::time_point lease_t::until() const {
    return until_;
  }

  std::string events_message(char kind, const std::vector<event_t> &events) {
    std::string out = "ev ";
    out.push_back(kind);
    for (const auto &event : events) {
      out += ' ' + std::to_string(event.type) + ' ' + std::to_string(event.code) + ' ' + std::to_string(event.value);
    }
    out += " 0 0 0";
    return out;
  }
}  // namespace frametop_hostin::capture
