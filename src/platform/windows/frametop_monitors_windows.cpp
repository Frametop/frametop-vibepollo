/**
 * @file src/platform/windows/frametop_monitors_windows.cpp
 * @brief Frametop's monitor plan on Windows: a monitor off is detached from the desktop (as if
 *        unplugged), and modes change for now only.
 *
 * Nothing here saves to Windows' display settings. A topology change (setTopology) takes the
 * layout Windows saved for the displays that are on, if it has one; without one Windows picks
 * the modes and places itself. So every display's mode, place and the main display are noted
 * first and put back after, each as far as it still applies (layout_to_put_back): other
 * displays keep their modes, a monitor coming back on goes back where it was (main display
 * again if it was), and the places are put back while the main display they were noted with is
 * still on. A reboot undoes it all.
 */
#ifdef _WIN32

  #include "src/frametop_monitors_layout.h"
  #include "src/frametop_monitors_service.h"
  #include "src/logging.h"
  #include "src/platform/windows/frametop_display_access.h"
  #include "src/platform/windows/virtual_display.h"

  #include <algorithm>
  #include <cmath>
  #include <set>
  #include <display_device/windows/win_api_utils.h>
  #include <exception>

namespace platf {
  namespace {
    using frametop_monitors::inventory_t;
    using frametop_monitors::layout_t;
    using frametop_monitors::monitor_t;
    using frametop_monitors::target_t;
    using frametop_monitors::video_mode_t;
    namespace access = frametop_display_access;

    video_mode_t to_mode(const access::listed_mode_t &m) {
      return {m.width, m.height, m.hz * 1000};
    }

    const monitor_t *find(const inventory_t &now, const std::string &id) {
      const auto it = std::find_if(now.monitors.begin(), now.monitors.end(), [&](const monitor_t &m) {
        return m.id == id;
      });
      return it == now.monitors.end() ? nullptr : &*it;
    }

    bool in(const display_device::ActiveTopology &topology, const std::string &id) {
      return std::any_of(topology.begin(), topology.end(), [&](const auto &group) {
        return std::find(group.begin(), group.end(), id) != group.end();
      });
    }

    // The active displays as they are: each one's place, and which is the main display.
    layout_t layout_now(display_device::WinDisplayDeviceInterface &dd) {
      layout_t layout;
      for (const auto &device : dd.enumAvailableDevices(display_device::DeviceEnumerationDetail::Minimal)) {
        if (!device.m_info) continue;
        layout.places[device.m_device_id] = {device.m_info->m_origin_point.m_x, device.m_info->m_origin_point.m_y};
        if (device.m_info->m_primary) layout.primary = device.m_device_id;
      }
      return layout;
    }

    std::map<std::string, display_device::Point> points_of(const layout_t &layout) {
      std::map<std::string, display_device::Point> points;
      for (const auto &[id, place] : layout.places) points[id] = {place.x, place.y};
      return points;
    }

    std::set<std::string> ids_of(const layout_t &layout) {
      std::set<std::string> ids;
      for (const auto &[id, place] : layout.places) ids.insert(id);
      return ids;
    }

    bool targeted(const std::vector<target_t> &targets, const std::string &id) {
      return std::any_of(targets.begin(), targets.end(), [&](const target_t &t) {
        return t.id == id;
      });
    }

    class windows_t final: public frametop_monitors::platform_t {
    public:
      std::string backend() const override {
        return "windows";
      }

      std::optional<inventory_t> inventory() override {
        try {
          auto dd = access::make_display_device();
          inventory_t out;
          for (const auto &device : dd->enumAvailableDevices(display_device::DeviceEnumerationDetail::Minimal)) {
            if (device.m_device_id.empty()) continue;
            const bool on = device.m_info.has_value() && !device.m_display_name.empty();
            if (VDISPLAY::is_virtual_display_output(device.m_device_id)) {
              out.other_outputs += on ? 1 : 0;
              continue;
            }
            monitor_t m;
            m.id = device.m_device_id;
            m.name = device.m_friendly_name.empty() ? device.m_display_name : device.m_friendly_name;
            m.on = on;
            if (on) {
              m.primary = device.m_info->m_primary;
              for (const auto &listed : access::listed_modes(device.m_display_name)) {
                const auto mode = to_mode(listed);
                if (std::none_of(m.modes.begin(), m.modes.end(), [&](const video_mode_t &x) {
                      return x.width == mode.width && x.height == mode.height && x.refresh_mhz == mode.refresh_mhz;
                    })) {
                  m.modes.push_back(mode);
                }
              }
              if (const auto current = access::current_listed_mode(device.m_display_name)) m.mode = to_mode(*current);
              m.place = {{"x", device.m_info->m_origin_point.m_x}, {"y", device.m_info->m_origin_point.m_y},
                         {"primary", device.m_info->m_primary}};
            }
            out.monitors.push_back(std::move(m));
          }
          return out;
        } catch (const std::exception &e) {
          BOOST_LOG(warning) << "Frametop monitors: couldn't list the displays: " << e.what();
          return std::nullopt;
        }
      }

      std::string apply(const std::vector<target_t> &targets, const inventory_t &now) override {
        try {
          auto dd = access::make_display_device();
          const auto noted = layout_now(*dd);
          const auto modes_before = dd->getCurrentDisplayModes(ids_of(noted));
          const auto wanted = frametop_monitors::layout_to_put_back(noted, targets);
          // On and off first: the topology Windows has a saved layout for.
          auto topology = dd->getCurrentTopology();
          bool changed = false;
          for (const auto &t : targets) {
            const auto *m = find(now, t.id);
            if (!m || m->on == t.on) continue;
            if (t.on && !in(topology, t.id)) {
              topology.push_back({t.id});
              changed = true;
            } else if (!t.on) {
              for (auto &group : topology) std::erase(group, t.id);
              std::erase_if(topology, [](const auto &group) {
                return group.empty();
              });
              changed = true;
            }
          }
          if (changed) {
            if (topology.empty()) return "that would leave no display on";
            if (!dd->isTopologyValid(topology)) return "Windows can't have those displays on together";
            if (!dd->setTopology(topology)) return "Windows didn't take the change of displays";
          }
          // Then modes: the targets' own, and every other display back in the mode it had.
          display_device::DeviceDisplayModeMap modes;
          if (changed) {
            const auto after = layout_now(*dd);
            const auto modes_now = dd->getCurrentDisplayModes(ids_of(after));
            for (const auto &[id, mode] : modes_before) {
              const auto now = modes_now.find(id);
              if (!targeted(targets, id) && now != modes_now.end() && !(now->second == mode)) modes[id] = mode;
            }
          }
          for (const auto &t : targets) {
            if (t.on && t.mode) {
              modes[t.id] = display_device::DisplayMode {{t.mode->width, t.mode->height}, display_device::Rational {t.mode->refresh_mhz / 1000, 1}};
            }
          }
          if (!modes.empty() && !dd->setDisplayModesTemporary(modes)) return "Windows didn't take the mode";
          if (!changed && modes.empty()) return {};
          // Then the main display and the places, which only fit together: noted with that main
          // display at 0,0. If it went off, Windows' own arrangement stays.
          const auto after = layout_now(*dd);
          if (wanted.primary.empty() || !after.places.contains(wanted.primary)) {
            BOOST_LOG(info) << "Frametop monitors: the main display is off; Windows arranged the others.";
            return {};
          }
          if (after.primary != wanted.primary && !dd->setAsPrimary(wanted.primary)) {
            BOOST_LOG(warning) << "Frametop monitors: couldn't make " << wanted.primary << " the main display again.";
          }
          if (!wanted.places.empty()) (void) access::put_places_back(points_of(wanted), "Frametop monitors: ");
          return {};
        } catch (const std::exception &e) {
          return e.what();
        }
      }
    };
  }  // namespace

  std::unique_ptr<frametop_monitors::platform_t> make_frametop_monitors() {
    return std::make_unique<windows_t>();
  }
}  // namespace platf

#endif  // _WIN32
