#include "frametop_display_mode.h"

#ifdef _WIN32

  #include "src/logging.h"
  #include "src/platform/windows/display.h"
  #include "src/platform/windows/impersonating_display_device.h"
  #include "src/platform/windows/misc.h"
  #include "src/platform/windows/utf_utils.h"
  #include "src/state_storage.h"

  #include <boost/algorithm/string/predicate.hpp>
  #include <boost/property_tree/ptree.hpp>
  #include <chrono>
  #include <display_device/windows/win_api_layer.h>
  #include <display_device/windows/win_api_utils.h>
  #include <display_device/windows/win_display_device.h>
  #include <exception>
  #include <filesystem>
  #include <map>
  #include <mutex>
  #include <nlohmann/json.hpp>
  #include <optional>
  #include <thread>
  #include <utility>

namespace platf::frametop_display_mode {
  using namespace std::literals;
  using display_device::DisplayMode;
  using display_device::Point;

  namespace {
    struct held_t {
      int streams;  // streams holding it at this mode
      DisplayMode original;
    };

    std::mutex g_mutex;
    std::map<std::string, held_t> g_holds;  // device id -> its hold

    std::shared_ptr<display_device::WinDisplayDeviceInterface> make_display_device() {
      auto api = std::make_shared<display_device::WinApiLayer>();
      auto dd = std::make_shared<display_device::WinDisplayDevice>(api);
      return std::make_shared<display_device::ImpersonatingDisplayDevice>(dd);
    }

    // Runs a display change as the logged-in user, like ImpersonatingDisplayDevice does.
    template<class Fn>
    bool as_user(Fn &&fn) {
      if (!platf::is_running_as_system()) {
        return fn();
      }
      HANDLE token = platf::retrieve_users_token(/*elevated*/ true);
      if (!token) {
        return !platf::has_active_console_session() && fn();
      }
      bool result = false;
      (void) platf::impersonate_current_user(token, [&]() {
        result = fn();
      });
      CloseHandle(token);
      return result;
    }

    std::optional<DisplayMode> current_mode(display_device::WinDisplayDeviceInterface &dd, const std::string &device_id) {
      const auto modes = dd.getCurrentDisplayModes({device_id});
      const auto mode = modes.find(device_id);
      if (mode == modes.end()) {
        return std::nullopt;
      }
      return mode->second;
    }

    bool same_size(const DisplayMode &a, const DisplayMode &b) {
      return a.m_resolution.m_width == b.m_resolution.m_width && a.m_resolution.m_height == b.m_resolution.m_height;
    }

    std::string describe(const DisplayMode &mode) {
      return std::to_string(mode.m_resolution.m_width) + "x" + std::to_string(mode.m_resolution.m_height);
    }

    nlohmann::json to_json(const DisplayMode &mode) {
      return {mode.m_resolution.m_width, mode.m_resolution.m_height, mode.m_refresh_rate.m_numerator, mode.m_refresh_rate.m_denominator};
    }

    std::optional<DisplayMode> mode_from_json(const nlohmann::json &value) {
      if (!value.is_array() || value.size() != 4) {
        return std::nullopt;
      }
      for (const auto &v : value) {
        if (!v.is_number_unsigned()) {
          return std::nullopt;
        }
      }
      return DisplayMode {{value[0].get<unsigned>(), value[1].get<unsigned>()}, {value[2].get<unsigned>(), value[3].get<unsigned>()}};
    }

    std::string pending_path() {
      return (std::filesystem::path {statefile::vibeshine_state_path()}.parent_path() / "frametop_display_mode.json").string();
    }

    // What this changed and hasn't put back yet: {"displays": {id: {"original": mode, "set": mode}},
    // "places": {id: [x, y]}}, where places are every active display's place before the first
    // change, put back after the last one (Windows may move the others to keep them touching a
    // display that changed size). Caller holds g_mutex.
    nlohmann::json load_pending() {
      nlohmann::json tree;
      if (statefile::load_json(pending_path(), tree) != statefile::json_load_result_e::loaded || !tree.is_object()) {
        tree = nlohmann::json::object();
      }
      if (!tree.contains("displays") || !tree["displays"].is_object()) {
        tree["displays"] = nlohmann::json::object();
      }
      if (!tree.contains("places") || !tree["places"].is_object()) {
        tree["places"] = nlohmann::json::object();
      }
      return tree;
    }

    void save_pending(const nlohmann::json &tree) {
      try {
        if (tree["displays"].empty()) {
          std::error_code ec;
          std::filesystem::remove(pending_path(), ec);
        } else {
          statefile::write_json_atomic(pending_path(), tree);
        }
      } catch (const std::exception &e) {
        BOOST_LOG(warning) << "Frametop display: couldn't save " << pending_path() << ": " << e.what();
      }
    }

    // Puts every listed display back in its place in one change that Windows doesn't save, so it
    // is all or nothing. Displays added since keep theirs.
    bool put_places_back(const std::map<std::string, Point> &places) {
      if (places.empty()) {
        return true;
      }
      return as_user([&] {
        display_device::WinApiLayer api;
        auto data = api.queryDisplayConfig(display_device::QueryType::Active);
        if (!data) {
          return false;
        }
        bool moved = false;
        for (const auto &[device_id, place] : places) {
          const auto *path = display_device::win_utils::getActivePath(api, device_id, data->m_paths);
          if (!path) {
            continue;
          }
          auto *source = display_device::win_utils::getSourceMode(display_device::win_utils::getSourceIndex(*path, data->m_modes), data->m_modes);
          if (!source || (source->position.x == place.m_x && source->position.y == place.m_y)) {
            continue;
          }
          source->position.x = place.m_x;
          source->position.y = place.m_y;
          moved = true;
        }
        if (!moved) {
          return true;
        }
        const LONG result = api.setDisplayConfig(data->m_paths, data->m_modes, SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_VIRTUAL_MODE_AWARE);
        if (result != ERROR_SUCCESS) {
          BOOST_LOG(warning) << "Frametop display: couldn't put the displays back in their places: " << api.getErrorString(result);
          return false;
        }
        BOOST_LOG(info) << "Frametop display: displays back in their places.";
        return true;
      });
    }

    std::map<std::string, Point> places_from_json(const nlohmann::json &tree) {
      std::map<std::string, Point> places;
      for (const auto &[device_id, place] : tree.items()) {
        if (place.is_array() && place.size() == 2 && place[0].is_number_integer() && place[1].is_number_integer()) {
          places[device_id] = {place[0].get<int>(), place[1].get<int>()};
        }
      }
      return places;
    }

    // Caller holds g_mutex. False leaves the display listed for restore_after_restart().
    bool put_back(const std::string &device_id, const DisplayMode &original, const std::string &label, const bool log_failure = true) {
      try {
        if (!make_display_device()->setDisplayModesTemporary({{device_id, original}})) {
          if (log_failure) {
            BOOST_LOG(warning) << "Frametop display: couldn't put " << label << " back to " << describe(original)
                               << "; trying again when Vibepollo next starts.";
          }
          return false;
        }
        BOOST_LOG(info) << "Frametop display: " << label << " back to " << describe(original) << '.';
      } catch (const std::exception &e) {
        if (log_failure) {
          BOOST_LOG(warning) << "Frametop display: putting " << label << " back to " << describe(original) << " failed: " << e.what();
        }
        return false;
      }
      auto pending = load_pending();
      pending["displays"].erase(device_id);
      if (pending["displays"].empty()) {
        put_places_back(places_from_json(pending["places"]));
        pending["places"] = nlohmann::json::object();
      }
      save_pending(pending);
      return true;
    }

    // Caller holds g_mutex. True when nothing is left to put back.
    bool restore_pending(const bool log_failure) {
      auto pending = load_pending();
      bool done = true;
      for (const auto &[device_id, entry] : pending["displays"].items()) {
        if (g_holds.contains(device_id)) {
          continue;
        }
        const auto original = mode_from_json(entry.value("original", nlohmann::json {}));
        const auto set = mode_from_json(entry.value("set", nlohmann::json {}));
        std::optional<DisplayMode> now;
        try {
          now = current_mode(*make_display_device(), device_id);
        } catch (const std::exception &) {
        }
        if (!now) {
          done = false;  // not active (yet): leave it listed
          continue;
        }
        if (!original || !set || !same_size(*now, *set)) {
          // A reboot already put it back, or someone has changed it since: theirs stays.
          auto tree = load_pending();
          tree["displays"].erase(device_id);
          if (tree["displays"].empty()) {
            tree["places"] = nlohmann::json::object();
          }
          save_pending(tree);
          continue;
        }
        if (!put_back(device_id, *original, device_id, log_failure)) {
          done = false;
        }
      }
      return done;
    }

    // Waits until capture sees the display at its new size, so the stream starts at it.
    void wait_for_capture_size(const std::string &output_name, const DisplayMode &mode) {
      const auto deadline = std::chrono::steady_clock::now() + 3s;
      while (std::chrono::steady_clock::now() < deadline) {
        dxgi::factory1_t factory;
        if (FAILED(CreateDXGIFactory1(IID_IDXGIFactory1, (void **) &factory))) {
          return;
        }
        dxgi::adapter_t::pointer adapter_p {};
        for (int x = 0; factory->EnumAdapters1(x, &adapter_p) != DXGI_ERROR_NOT_FOUND; ++x) {
          dxgi::adapter_t adapter {adapter_p};
          dxgi::output_t::pointer output_p {};
          for (int y = 0; adapter->EnumOutputs(y, &output_p) != DXGI_ERROR_NOT_FOUND; ++y) {
            dxgi::output_t output {output_p};
            DXGI_OUTPUT_DESC desc;
            if (FAILED(output->GetDesc(&desc)) || !desc.AttachedToDesktop || utf_utils::to_utf8(desc.DeviceName) != output_name) {
              continue;
            }
            const auto &r = desc.DesktopCoordinates;
            if (static_cast<unsigned>(r.right - r.left) == mode.m_resolution.m_width &&
                static_cast<unsigned>(r.bottom - r.top) == mode.m_resolution.m_height) {
              return;
            }
          }
        }
        std::this_thread::sleep_for(100ms);
      }
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
        const auto held = g_holds.find(device_id);
        if (held == g_holds.end() || --held->second.streams > 0) {
          return;
        }
        const auto original = held->second.original;
        g_holds.erase(held);
        put_back(device_id, original, output_name);
      }

    private:
      std::string device_id;
      std::string output_name;
    };
  }  // namespace

  std::shared_ptr<void> hold_resolution(const std::string &output_name, const int width, const int height) {
    if (width <= 0 || height <= 0) {
      return nullptr;
    }
    std::string device_id;
    DisplayMode set;
    {
      std::lock_guard lock {g_mutex};
      try {
        auto dd = make_display_device();
        const auto devices = dd->enumAvailableDevices(display_device::DeviceEnumerationDetail::Minimal);
        for (const auto &device : devices) {
          if (device.m_info && boost::iequals(device.m_display_name, output_name)) {
            device_id = device.m_device_id;
            break;
          }
        }
        if (device_id.empty()) {
          BOOST_LOG(warning) << "Frametop display: no active display " << output_name << " to change the resolution of.";
          return nullptr;
        }

        if (const auto held = g_holds.find(device_id); held != g_holds.end()) {
          ++held->second.streams;  // already changed for another stream, which keeps its size
          return std::make_shared<hold_t>(device_id, output_name);
        }

        const auto original = current_mode(*dd, device_id);
        if (!original) {
          BOOST_LOG(warning) << "Frametop display: couldn't read " << output_name << "'s resolution; streaming it as it is.";
          return nullptr;
        }
        const DisplayMode wanted {{static_cast<unsigned>(width), static_cast<unsigned>(height)}, original->m_refresh_rate};
        if (same_size(*original, wanted)) {
          return nullptr;
        }

        // Listed first, so a crash while it's changed still gets it put back.
        auto pending = load_pending();
        if (pending["displays"].empty()) {
          pending["places"] = nlohmann::json::object();
          for (const auto &device : devices) {
            if (device.m_info) {
              pending["places"][device.m_device_id] = {device.m_info->m_origin_point.m_x, device.m_info->m_origin_point.m_y};
            }
          }
        }
        pending["displays"][device_id] = {{"original", to_json(*original)}, {"set", to_json(wanted)}};
        save_pending(pending);

        const bool applied = dd->setDisplayModesTemporary({{device_id, wanted}});
        const auto now = current_mode(*dd, device_id);
        if (!applied || !now || same_size(*now, *original)) {
          BOOST_LOG(warning) << "Frametop display: couldn't switch " << output_name << " from " << describe(*original) << " to "
                             << describe(wanted) << "; streaming it scaled.";
          if (now && !display_device::win_utils::fuzzyCompareModes(*now, *original)) {
            (void) dd->setDisplayModesTemporary({{device_id, *original}});
          }
          pending["displays"].erase(device_id);
          save_pending(pending);
          return nullptr;
        }
        set = *now;
        pending["displays"][device_id]["set"] = to_json(set);
        save_pending(pending);
        g_holds[device_id] = {1, *original};
        if (same_size(set, wanted)) {
          BOOST_LOG(info) << "Frametop display: " << output_name << " at " << describe(set) << " (was " << describe(*original)
                          << ") while it streams.";
        } else {
          BOOST_LOG(info) << "Frametop display: " << output_name << " has no " << describe(wanted) << " mode; at "
                          << describe(set) << " (was " << describe(*original) << ") while it streams.";
        }
      } catch (const std::exception &e) {
        BOOST_LOG(warning) << "Frametop display: changing " << output_name << "'s resolution failed: " << e.what();
        return nullptr;
      }
    }

    auto hold = std::make_shared<hold_t>(device_id, output_name);
    wait_for_capture_size(output_name, set);
    return hold;
  }

  void restore_after_restart() {
    {
      std::lock_guard lock {g_mutex};
      if (restore_pending(true)) {
        return;
      }
    }
    // Started before anyone logged in (after a crash, then a sign-out): display settings belong
    // to the user, so keep trying for a while.
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
}  // namespace platf::frametop_display_mode

#endif  // _WIN32
