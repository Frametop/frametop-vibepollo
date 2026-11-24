/**
 * @file src/frametop_together_policy.h
 * @brief Frametop Together: when one of this PC's panels is captured and encoded, and at what.
 *
 * The headset says of each panel what to encode it at and whether anyone watches it
 * (frametop_together_wire.h). A panel nobody watches costs this PC nothing: its capture runs
 * only while it is watched and has a window. No rules about the room live here: the headset
 * worked those out, and this is told the result.
 */
#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

namespace frametop_together::policy {
  using instant_t = std::chrono::steady_clock::time_point;

  /// What a capture runs at.
  constexpr int MIN_KBPS = 100;
  constexpr int MAX_KBPS = 100'000;
  constexpr int MAX_PICTURES = 60;
  /// Until the headset says: 2 Mbit/s, 30 pictures a second at the most, 10 now.
  constexpr std::uint32_t START_BITS = 2'000'000;
  constexpr int START_FULL = 30;
  constexpr int START_PICTURES = 10;
  /// Pictures a second are set when a capture starts. More are given at once (someone turned to
  /// the panel), fewer once that has stood this long: a glance away starts no encoder over.
  constexpr auto LOWER_AFTER = std::chrono::seconds(5);
  /// A capture that ended by itself (its window's output went, the encoder failed) is tried
  /// again this much later, while the panel is still watched.
  constexpr auto RETRY_AFTER = std::chrono::seconds(2);
  struct run_t {
    int kbps = 0;
    int pictures = 0;
    friend bool operator==(const run_t &, const run_t &) = default;
  };

  enum class act_e {
    none,
    start,  ///< start a capture at `run`
    stop,  ///< stop the capture
    restart,  ///< stop it and start another at `run`
    keyframe,  ///< the capture's next picture is to be a keyframe
    bitrate,  ///< tell the running capture `run.kbps` (it retunes its encoder, or builds another)
  };

  struct step_t {
    act_e act = act_e::none;
    run_t run {};
    friend bool operator==(const step_t &, const step_t &) = default;
  };

  /// One panel: what the headset said of it, and whether its capture runs.
  class panel_t {
  public:
    /// A word from the headset (wire::told_t): R, F, P, K or W with its value.
    step_t heard(char word, std::uint32_t value, instant_t now);
    /// The panel got a window to show, or lost it.
    step_t window(bool has_one);
    /// Its capture ended without being stopped.
    void ended(instant_t now);
    /// Time passed.
    step_t tick(instant_t now);

    bool watched() const {
      return watched_;
    }

    bool running() const {
      return running_;
    }

    /// What its capture runs at (while it runs).
    run_t run() const {
      return run_;
    }

  private:
    run_t wanted() const;
    step_t start();
    /// Starts or stops the capture when being watched or having a window changed.
    step_t settle();
    /// The pictures a second that are wanted changed.
    step_t pictures_changed(instant_t now);

    bool watched_ = false;
    bool has_window_ = false;
    bool running_ = false;
    std::uint32_t bits_ = START_BITS;
    int full_ = START_FULL;
    int pictures_ = START_PICTURES;
    run_t run_ {};
    std::optional<instant_t> lower_since_;
    std::optional<instant_t> retry_at_;
  };

  struct extent_t {
    int width = 0;
    int height = 0;
    friend bool operator==(const extent_t &, const extent_t &) = default;
  };

  /// A room's stream is 128 to 4096 pixels a side.
  constexpr int MIN_SIDE = 128;
  constexpr int MAX_SIDE = 4096;
  /// A Frame's video hardware takes pictures whose width is a multiple of this, and resets on
  /// some that aren't (Frametop's docs/together.md, M0), so no other width is sent to one.
  constexpr int WIDTH_STEP = 128;
  constexpr int HEIGHT_STEP = 8;

  /// The size a window's output is encoded at for the room: its own, or the nearest below that
  /// a Frame takes, in the same shape. nullopt for an output too small to be a panel.
  std::optional<extent_t> encode_size(int output_width, int output_height);
}  // namespace frametop_together::policy
