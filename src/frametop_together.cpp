/**
 * @file src/frametop_together.cpp
 * @brief Frametop Together: this PC's windows as panels in its person's room.
 *
 * One call to the headset at a time (ft-pcsend, the helper), and up to four panels on it. A
 * reader thread takes the helper's lines and starts and stops each panel's capture by them
 * (frametop_together_policy.h). A capture is video::capture() of the panel's window output on
 * a thread of its own, with a mail of its own; its pictures come back through
 * take_video_packet(), on that capture's encoding thread, and go to the helper.
 */
#include "frametop_together.h"

#include "frametop_together_policy.h"
#include "frametop_together_wire.h"
#include "src/globals.h"
#include "src/logging.h"
#include "src/platform/linux/frametop_together_helper.h"
#include "src/thread_safe.h"
#include "src/video.h"

#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <set>
#include <thread>

using namespace std::literals;

namespace frametop_together {
  namespace {
    constexpr int PANELS = wire::LAST_PANEL - wire::FIRST_PANEL + 1;
    /// How often the reader looks at its panels when the helper says nothing.
    constexpr int TICK_MS = 100;

    /// A panel's running capture.
    struct capture_t {
      safe::mail_t mail = std::make_shared<safe::mail_raw_t>();
      std::thread thread;
      std::atomic_bool done {false};  ///< video::capture() returned
    };

    /// One of the PC's panels. Its address is its captures' channel_data.
    struct slot_t {
      // Under the state's mutex:
      std::string output;
      policy::extent_t size;  ///< what it is encoded at
      policy::panel_t rules;
      std::unique_ptr<capture_t> capture;
      std::uint32_t sent = 0;  ///< what the helper last said it sent

      // From its capture's encoding thread too:
      std::atomic<std::uint64_t> handed {0};  ///< pictures handed to the helper
      std::atomic_bool needs_key {false};  ///< one wasn't handed over: nothing until a keyframe
    };

    /// The picture as a decoder is to get it: a keyframe's parameter sets are the ones the
    /// encoder session rewrote (the colour description), as for every other consumer.
    std::string picture_of(video::packet_raw_t &packet) {
      std::string picture {reinterpret_cast<const char *>(packet.data()), packet.data_size()};
      if (packet.is_idr() && packet.replacements) {
        for (const auto &replacement : *packet.replacements) {
          if (const auto at = picture.find(replacement.old); at != std::string::npos) {
            picture.replace(at, replacement.old.size(), replacement._new);
          }
        }
      }
      return picture;
    }

    class sender_t {
    public:
      ~sender_t() {
        stop();
      }

      bool call(const std::string &address, int port, const std::string &code, std::string *error) {
        std::lock_guard control {control_};
        end();
        if (!wire::is_address(address) || port < 1 || port > 65535 || !wire::is_code(code)) {
          *error = "an address, a port and a one-time code of eight characters are needed";
          return false;
        }
        if (video::active_hevc_mode < 2) {
          *error = "this PC's encoder has no HEVC, which a room's pictures are";
          return false;
        }
        auto helper = platf::frametop_together::helper_t::start(address, port, code, error);
        if (!helper) {
          return false;
        }
        {
          std::lock_guard feed {feed_};
          helper_ = std::move(helper);
        }
        {
          std::lock_guard state {state_};
          calling_ = true;
          taken_ = false;
          ended_.clear();
        }
        {
          std::lock_guard released {released_lock_};
          released_.clear();  // (of before this call: it has no panels yet)
        }
        BOOST_LOG(info) << "Frametop Together: calling the headset"sv;
        reader_ = std::thread {&sender_t::read, this};
        return true;
      }

      bool show(const std::vector<panel_t> &panels, std::string *error) {
        std::lock_guard control {control_};
        std::lock_guard state {state_};
        if (!calling_) {
          *error = "no call to a headset stands";
          return false;
        }
        drop_released();  // (first: an output named here may be a new window under an old name)
        std::array<const panel_t *, PANELS> named {};
        std::array<policy::extent_t, PANELS> sizes {};
        for (const auto &panel : panels) {
          const auto size = policy::encode_size(panel.width, panel.height);
          const int at = panel.number - wire::FIRST_PANEL;
          if (!wire::is_panel(panel.number) || named[at] || panel.output.empty() || !size) {
            *error = "a panel needs a number from 3 to 6 of its own and a window output of 128 pixels a side or more";
            return false;
          }
          named[at] = &panel;
          sizes[at] = *size;
        }
        for (int at = 0; at < PANELS; ++at) {
          auto &slot = slots_[at];
          const bool same = named[at] && named[at]->output == slot.output && sizes[at] == slot.size;
          if (same) {
            continue;
          }
          apply(slot, slot.rules.window(false));
          slot.output = named[at] ? named[at]->output : std::string {};
          slot.size = sizes[at];
          if (named[at]) {
            apply(slot, slot.rules.window(true));
          }
        }
        return true;
      }

      void stop() {
        std::lock_guard control {control_};
        end();
      }

      void released(const std::string &output) {
        std::lock_guard released {released_lock_};
        released_.insert(output);
      }

      nlohmann::json state() {
        std::lock_guard state {state_};
        auto panels = nlohmann::json::array();
        for (int at = 0; at < PANELS; ++at) {
          const auto &slot = slots_[at];
          if (slot.output.empty()) {
            continue;
          }
          panels.push_back({
            {"number", wire::FIRST_PANEL + at},
            {"output", slot.output},
            {"width", slot.size.width},
            {"height", slot.size.height},
            {"watched", slot.rules.watched()},
            {"encoding", slot.capture != nullptr},
            {"kbps", slot.capture ? slot.rules.run().kbps : 0},
            {"pictures", slot.capture ? slot.rules.run().pictures : 0},
            {"handed", slot.handed.load()},
            {"sent", slot.sent},
          });
        }
        return {{"calling", calling_}, {"called", calling_ && taken_}, {"ended", ended_}, {"panels", panels}};
      }

      bool take(video::packet_raw_t &packet) {
        const auto tag = reinterpret_cast<std::uintptr_t>(packet.channel_data);
        const auto first = reinterpret_cast<std::uintptr_t>(slots_.data());
        if (tag < first || tag >= first + sizeof slots_) {
          return false;
        }
        auto &slot = *static_cast<slot_t *>(packet.channel_data);
        const bool key = packet.is_idr();
        if (slot.needs_key && !key) {
          return true;  // (it leans on a picture that wasn't handed over)
        }
        const auto record = wire::record(wire::FIRST_PANEL + static_cast<int>(&slot - slots_.data()), key, picture_of(packet));
        std::lock_guard feed {feed_};
        const bool fed = !record.empty() && helper_ && helper_->feed(record);
        slot.needs_key = !fed;
        if (fed) {
          ++slot.handed;
        }
        return true;
      }

    private:
      /// Ends the call: the helper first, which ends the reader, which stops the captures.
      void end() {
        {
          std::lock_guard feed {feed_};
          if (helper_) {
            helper_->stop();
          }
        }
        if (reader_.joinable()) {
          reader_.join();
        }
        std::lock_guard feed {feed_};
        helper_.reset();
      }

      /// The reader thread: the helper's lines, and time passing, until the helper ends.
      void read() {
        std::vector<std::string> lines;
        for (bool open = true; open;) {
          lines.clear();
          open = helper_->lines(lines, TICK_MS);
          std::lock_guard state {state_};
          const auto now = std::chrono::steady_clock::now();
          for (const auto &line : lines) {
            if (const auto parsed = wire::parse(line)) {
              std::visit([&](const auto &said) {
                heard(said, now);
              },
                         *parsed);
            }
          }
          drop_released();
          for (auto &slot : slots_) {
            look_after(slot, now);
          }
        }
        std::lock_guard state {state_};
        for (auto &slot : slots_) {
          end_capture(slot);
          slot.output.clear();
          slot.size = {};
          slot.rules = {};
          slot.sent = 0;
          slot.handed = 0;
        }
        calling_ = false;
        if (ended_.empty()) {
          ended_ = "the call ended";
        }
        BOOST_LOG(info) << "Frametop Together: "sv << ended_;
      }

      void heard(const wire::told_t &told, std::chrono::steady_clock::time_point now) {
        if (!taken_) {
          // (not when the link came up: the headset looks at the code after that)
          taken_ = true;
          BOOST_LOG(info) << "Frametop Together: the headset took the call"sv;
        }
        auto &slot = slots_[told.number - wire::FIRST_PANEL];
        apply(slot, slot.rules.heard(told.word, told.value, now));
      }

      void heard(const wire::sent_t &sent, std::chrono::steady_clock::time_point) {
        slots_[sent.number - wire::FIRST_PANEL].sent = sent.count;
      }

      void heard(const wire::called_t &, std::chrono::steady_clock::time_point) {
        BOOST_LOG(info) << "Frametop Together: reached the headset"sv;
      }

      void heard(const wire::ended_t &ended, std::chrono::steady_clock::time_point) {
        ended_ = ended.why;
      }

      /// The panels whose window outputs went since the last look stop being panels.
      void drop_released() {
        std::set<std::string> gone;
        {
          std::lock_guard released {released_lock_};
          gone.swap(released_);
        }
        for (auto &slot : slots_) {
          if (!slot.output.empty() && gone.contains(slot.output)) {
            BOOST_LOG(info) << "Frametop Together: "sv << slot.output << " went: it is no panel any more"sv;
            apply(slot, slot.rules.window(false));
            slot.output.clear();
            slot.size = {};
          }
        }
      }

      /// What a panel needs without being told: a capture that ended taken away, a keyframe
      /// asked again, and what its rules do with time.
      void look_after(slot_t &slot, std::chrono::steady_clock::time_point now) {
        if (slot.capture && slot.capture->done) {
          BOOST_LOG(warning) << "Frametop Together: the capture of "sv << slot.output << " ended by itself"sv;
          end_capture(slot);
          slot.rules.ended(now);
        }
        if (slot.capture && slot.needs_key) {
          slot.capture->mail->event<bool>(mail::idr)->raise(true);
        }
        apply(slot, slot.rules.tick(now));
      }

      void apply(slot_t &slot, const policy::step_t &step) {
        switch (step.act) {
          case policy::act_e::none:
            break;
          case policy::act_e::start:
            start_capture(slot, step.run);
            break;
          case policy::act_e::stop:
            end_capture(slot);
            break;
          case policy::act_e::restart:
            end_capture(slot);
            start_capture(slot, step.run);
            break;
          case policy::act_e::keyframe:
            if (slot.capture) {
              slot.capture->mail->event<bool>(mail::idr)->raise(true);
            }
            break;
          case policy::act_e::bitrate:
            if (slot.capture) {
              slot.capture->mail->event<int>(mail::dynamic_bitrate)->raise(step.run.kbps);
            }
            break;
        }
      }

      void start_capture(slot_t &slot, const policy::run_t &run) {
        video::config_t config {};
        config.width = slot.size.width;
        config.height = slot.size.height;
        config.framerate = run.pictures;
        config.encodingFramerate = run.pictures * 1000;
        config.bitrate = run.kbps;
        config.client_requested_bitrate = run.kbps;
        config.slicesPerFrame = 1;
        config.numRefFrames = 1;
        // Rec. 709, full range: what a Frame's own panels are, and what the room's viewers
        // convert every panel as, whatever its stream says of itself.
        config.encoderCscMode = (1 << 1) | 1;
        config.videoFormat = 1;  // HEVC
        config.capture_source = video::capture_source_e::exact_output;
        config.capture_output = slot.output;
        slot.needs_key = false;
        slot.capture = std::make_unique<capture_t>();
        BOOST_LOG(info) << "Frametop Together: encoding "sv << slot.output << " at "sv << config.width << 'x' << config.height
                        << ", "sv << run.kbps << " kbit/s, "sv << run.pictures << " pictures a second"sv;
        slot.capture->thread = std::thread {[capture = slot.capture.get(), config, tag = &slot]() {
          video::capture(capture->mail, config, tag);
          capture->done = true;
        }};
      }

      void end_capture(slot_t &slot) {
        if (!slot.capture) {
          return;
        }
        slot.capture->mail->event<bool>(mail::shutdown)->raise(true);
        slot.capture->thread.join();
        slot.capture.reset();
      }

      std::mutex control_;  ///< one call, show or stop at a time
      std::mutex state_;  ///< the panels and what the helper said (never held with feed_)
      std::mutex feed_;  ///< the helper's input, and the helper being there
      std::mutex released_lock_;  ///< released_ only: taken last, held for a moment
      std::set<std::string> released_;  ///< window outputs that went since the reader last looked
      std::unique_ptr<platf::frametop_together::helper_t> helper_;
      std::thread reader_;
      std::array<slot_t, PANELS> slots_;
      bool calling_ = false;  ///< the helper runs
      bool taken_ = false;  ///< the headset took its code: it said what to encode
      std::string ended_;
    };

    sender_t sender;
  }  // namespace

  bool call(const std::string &address, int port, const std::string &code, std::string *error) {
    return sender.call(address, port, code, error);
  }

  bool show(const std::vector<panel_t> &panels, std::string *error) {
    return sender.show(panels, error);
  }

  void stop() {
    sender.stop();
  }

  void output_released(const std::string &output) {
    sender.released(output);
  }

  nlohmann::json state() {
    return sender.state();
  }

  bool take_video_packet(video::packet_raw_t &packet) {
    return sender.take(packet);
  }
}  // namespace frametop_together
