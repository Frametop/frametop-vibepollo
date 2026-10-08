#include "frametop_display_hdr.h"

#ifdef _WIN32

  #include "src/logging.h"
  #include "src/platform/windows/display.h"
  #include "src/platform/windows/impersonating_display_device.h"
  #include "src/state_storage.h"

  #include <boost/algorithm/string/predicate.hpp>
  #include <boost/property_tree/ptree.hpp>
  #include <chrono>
  #include <display_device/windows/win_api_layer.h>
  #include <display_device/windows/win_display_device.h>
  #include <exception>
  #include <filesystem>
  #include <map>
  #include <mutex>
  #include <nlohmann/json.hpp>
  #include <set>
  #include <thread>
  #include <utility>

namespace platf::frametop_display_hdr {
  using namespace std::literals;

  namespace {
    std::mutex g_mutex;
    std::map<std::string, int> g_holds;  // device id -> streams holding its HDR off

    std::shared_ptr<display_device::WinDisplayDeviceInterface> make_display_device() {
      auto api = std::make_shared<display_device::WinApiLayer>();
      auto dd = std::make_shared<display_device::WinDisplayDevice>(api);
      return std::make_shared<display_device::ImpersonatingDisplayDevice>(dd);
    }

    std::string pending_path() {
      return (std::filesystem::path {statefile::vibeshine_state_path()}.parent_path() / "frametop_display_hdr.json").string();
    }

    // Device ids whose HDR this turned off and hasn't turned back on yet. Caller holds g_mutex.
    std::set<std::string> load_pending() {
      std::set<std::string> ids;
      nlohmann::json tree;
      if (statefile::load_json(pending_path(), tree) == statefile::json_load_result_e::loaded && tree.is_array()) {
        for (const auto &id : tree) {
          if (id.is_string()) {
            ids.insert(id.get<std::string>());
          }
        }
      }
      return ids;
    }

    void save_pending(const std::set<std::string> &ids) {
      try {
        if (ids.empty()) {
          std::error_code ec;
          std::filesystem::remove(pending_path(), ec);
        } else {
          statefile::write_json_atomic(pending_path(), nlohmann::json(ids));
        }
      } catch (const std::exception &e) {
        BOOST_LOG(warning) << "Frametop display: couldn't save " << pending_path() << ": " << e.what();
      }
    }

    bool set_hdr(display_device::WinDisplayDeviceInterface &dd, const std::string &device_id, const bool on) {
      return dd.setHdrStates({{device_id, on ? display_device::HdrState::Enabled : display_device::HdrState::Disabled}});
    }

    // Caller holds g_mutex. False leaves the display listed for restore_after_restart().
    bool turn_back_on(const std::string &device_id, const std::string &label, const bool log_failure = true) {
      try {
        auto dd = make_display_device();
        if (!set_hdr(*dd, device_id, true)) {
          if (log_failure) {
            BOOST_LOG(warning) << "Frametop display: couldn't turn HDR back on for " << label << "; trying again when Vibepollo next starts.";
          }
          return false;
        }
        BOOST_LOG(info) << "Frametop display: HDR back on for " << label << '.';
      } catch (const std::exception &e) {
        if (log_failure) {
          BOOST_LOG(warning) << "Frametop display: turning HDR back on for " << label << " failed: " << e.what();
        }
        return false;
      }
      auto pending = load_pending();
      pending.erase(device_id);
      save_pending(pending);
      return true;
    }

    // Caller holds g_mutex. True when nothing is left to turn back on.
    bool restore_pending(const bool log_failure) {
      bool done = true;
      for (const auto &device_id : load_pending()) {
        if (!g_holds.contains(device_id) && !turn_back_on(device_id, device_id, log_failure)) {
          done = false;
        }
      }
      return done;
    }

    class hold_t {
    public:
      hold_t(std::string device_id, std::string output_name):
          device_id(std::move(device_id)),
          output_name(std::move(output_name)) {}

      hold_t(const hold_t &) = delete;
      hold_t &operator=(const hold_t &) = delete;

      ~hold_t() {
        std::lock_guard lock {g_mutex};
        if (--g_holds[device_id] > 0) {
          return;
        }
        g_holds.erase(device_id);
        turn_back_on(device_id, output_name);
      }

    private:
      std::string device_id;
      std::string output_name;
    };
  }  // namespace

  std::shared_ptr<void> hold_sdr(const std::string &output_name) {
    std::string device_id;
    {
      std::lock_guard lock {g_mutex};
      try {
        auto dd = make_display_device();
        for (const auto &device : dd->enumAvailableDevices(display_device::DeviceEnumerationDetail::Minimal)) {
          if (device.m_info && boost::iequals(device.m_display_name, output_name)) {
            device_id = device.m_device_id;
            break;
          }
        }
        if (device_id.empty()) {
          BOOST_LOG(warning) << "Frametop display: no active display " << output_name << " to turn HDR off on.";
          return nullptr;
        }

        if (const auto held = g_holds.find(device_id); held != g_holds.end()) {
          ++held->second;  // already off for another stream
          return std::make_shared<hold_t>(device_id, output_name);
        }

        const auto states = dd->getCurrentHdrStates({device_id});
        const auto state = states.find(device_id);
        if (state == states.end() || state->second != display_device::HdrState::Enabled) {
          return nullptr;
        }

        // Listed first, so a crash while it's off still gets it turned back on.
        auto pending = load_pending();
        pending.insert(device_id);
        save_pending(pending);
        if (!set_hdr(*dd, device_id, false)) {
          BOOST_LOG(warning) << "Frametop display: couldn't turn HDR off for " << output_name << "; streaming it as it is.";
          pending.erase(device_id);
          save_pending(pending);
          return nullptr;
        }
        g_holds[device_id] = 1;
      } catch (const std::exception &e) {
        BOOST_LOG(warning) << "Frametop display: turning HDR off for " << output_name << " failed: " << e.what();
        return nullptr;
      }
    }

    // Capture starts in SDR rather than switching formats mid-stream.
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (platf::dxgi::is_hdr_active_for_output(output_name) && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(100ms);
    }
    BOOST_LOG(info) << "Frametop display: HDR off for " << output_name << " while it streams in SDR.";
    return std::make_shared<hold_t>(device_id, output_name);
  }

  void restore_after_restart() {
    {
      std::lock_guard lock {g_mutex};
      if (restore_pending(true)) {
        return;
      }
    }
    // Started before anyone logged in (after a reboot): HDR settings belong to the user, so keep
    // trying for a while.
    std::thread([] {
      const auto deadline = std::chrono::steady_clock::now() + 30min;
      while (std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(15s);
        std::lock_guard lock {g_mutex};
        if (restore_pending(false)) {
          return;
        }
      }
    }).detach();
  }
}  // namespace platf::frametop_display_hdr

#endif  // _WIN32
