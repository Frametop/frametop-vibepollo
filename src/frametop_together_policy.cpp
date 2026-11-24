/**
 * @file src/frametop_together_policy.cpp
 * @brief Frametop Together: when one of this PC's panels is captured and encoded, and at what.
 */
#include "frametop_together_policy.h"

#include <algorithm>

namespace frametop_together::policy {
  run_t panel_t::wanted() const {
    return {
      std::clamp(static_cast<int>(bits_ / 1000), MIN_KBPS, MAX_KBPS),
      std::clamp(std::min(pictures_, full_), 1, MAX_PICTURES),
    };
  }

  step_t panel_t::start() {
    running_ = true;
    run_ = wanted();
    lower_since_.reset();
    return {act_e::start, run_};
  }

  step_t panel_t::settle() {
    const bool wanted_running = watched_ && has_window_;
    if (wanted_running && !running_ && !retry_at_) {
      return start();
    }
    if (!wanted_running) {
      retry_at_.reset();
      if (running_) {
        running_ = false;
        return {act_e::stop};
      }
    }
    return {};
  }

  step_t panel_t::pictures_changed(instant_t now) {
    if (!running_) {
      return {};
    }
    const auto pictures = wanted().pictures;
    if (pictures > run_.pictures) {
      const auto step = start();
      return {act_e::restart, step.run};
    }
    if (pictures < run_.pictures) {
      lower_since_ = lower_since_.value_or(now);
    } else {
      lower_since_.reset();
    }
    return {};
  }

  step_t panel_t::heard(char word, std::uint32_t value, instant_t now) {
    switch (word) {
      case 'W':
        {
          watched_ = value != 0;
          // (said of a panel that runs, so is watched already: a new viewer needs a keyframe)
          return watched_ && running_ ? step_t {act_e::keyframe} : settle();
        }
      case 'K':
        return running_ ? step_t {act_e::keyframe} : step_t {};
      case 'R':
        {
          bits_ = value;
          if (!running_) {
            return {};
          }
          const auto kbps = wanted().kbps;
          if (kbps == run_.kbps) {
            return {};
          }
          run_.kbps = kbps;
          return {act_e::bitrate, run_};
        }
      case 'F':
        full_ = static_cast<int>(std::min<std::uint32_t>(value, MAX_PICTURES));
        return pictures_changed(now);
      case 'P':
        pictures_ = static_cast<int>(std::min<std::uint32_t>(value, MAX_PICTURES));
        return pictures_changed(now);
      default:
        return {};
    }
  }

  step_t panel_t::window(bool has_one) {
    has_window_ = has_one;
    return settle();
  }

  void panel_t::ended(instant_t now) {
    if (!running_) {
      return;
    }
    running_ = false;
    retry_at_ = now + RETRY_AFTER;
  }

  step_t panel_t::tick(instant_t now) {
    if (retry_at_ && now >= *retry_at_) {
      retry_at_.reset();
      return settle();
    }
    if (running_ && lower_since_ && now - *lower_since_ >= LOWER_AFTER) {
      const auto step = start();
      return {act_e::restart, step.run};
    }
    return {};
  }

  std::optional<extent_t> encode_size(int output_width, int output_height) {
    if (output_width < MIN_SIDE || output_height < MIN_SIDE) {
      return std::nullopt;
    }
    // Fits the room's bounds first, then the widths a Frame takes. Whole numbers throughout: a
    // width that is a step already stays itself.
    const std::int64_t wide = output_width, high = output_height;
    const auto fitted = std::min({wide, std::int64_t {MAX_SIDE}, wide * MAX_SIDE / high});
    const auto width = std::max<std::int64_t>(MIN_SIDE, fitted / WIDTH_STEP * WIDTH_STEP);
    // (the height in the same shape, to the nearest step)
    const auto height = (2 * high * width + HEIGHT_STEP * wide) / (2 * HEIGHT_STEP * wide) * HEIGHT_STEP;
    return extent_t {static_cast<int>(width), static_cast<int>(std::clamp<std::int64_t>(height, MIN_SIDE, MAX_SIDE))};
  }
}  // namespace frametop_together::policy
