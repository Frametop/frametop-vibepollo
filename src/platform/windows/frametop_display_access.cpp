/**
 * @file src/platform/windows/frametop_display_access.cpp
 * @brief What Frametop's display changes share on Windows (see the header).
 */
#include "frametop_display_access.h"

#ifdef _WIN32

  #include "src/logging.h"
  #include "src/platform/windows/impersonating_display_device.h"
  #include "src/platform/windows/utf_utils.h"

  #include <display_device/windows/win_api_layer.h>
  #include <display_device/windows/win_api_utils.h>
  #include <display_device/windows/win_display_device.h>

namespace platf::frametop_display_access {
  std::shared_ptr<display_device::WinDisplayDeviceInterface> make_display_device() {
    auto api = std::make_shared<display_device::WinApiLayer>();
    auto dd = std::make_shared<display_device::WinDisplayDevice>(api);
    return std::make_shared<display_device::ImpersonatingDisplayDevice>(dd);
  }

  std::vector<listed_mode_t> listed_modes(const std::string &display_name) {
    std::vector<listed_mode_t> modes;
    if (display_name.empty()) return modes;
    const std::wstring name = utf_utils::from_utf8(display_name);
    (void) as_user([&]() {
      DEVMODEW dm {};
      dm.dmSize = sizeof dm;
      for (DWORD i = 0; EnumDisplaySettingsW(name.c_str(), i, &dm); ++i) {
        if ((dm.dmFields & (DM_PELSWIDTH | DM_PELSHEIGHT)) == (DM_PELSWIDTH | DM_PELSHEIGHT) && dm.dmPelsWidth && dm.dmPelsHeight) {
          modes.push_back({dm.dmPelsWidth, dm.dmPelsHeight, dm.dmDisplayFrequency});
        }
      }
      return true;
    });
    return modes;
  }

  std::optional<listed_mode_t> current_listed_mode(const std::string &display_name) {
    if (display_name.empty()) return std::nullopt;
    const std::wstring name = utf_utils::from_utf8(display_name);
    std::optional<listed_mode_t> mode;
    (void) as_user([&]() {
      DEVMODEW dm {};
      dm.dmSize = sizeof dm;
      if (EnumDisplaySettingsW(name.c_str(), ENUM_CURRENT_SETTINGS, &dm) && dm.dmPelsWidth && dm.dmPelsHeight) {
        mode = listed_mode_t {dm.dmPelsWidth, dm.dmPelsHeight, dm.dmDisplayFrequency};
      }
      return true;
    });
    return mode;
  }

  bool put_places_back(const std::map<std::string, display_device::Point> &places, const char *log_prefix) {
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
        BOOST_LOG(warning) << log_prefix << "couldn't put the displays back in their places: " << api.getErrorString(result);
        return false;
      }
      BOOST_LOG(info) << log_prefix << "displays back in their places.";
      return true;
    });
  }
}  // namespace platf::frametop_display_access

#endif  // _WIN32
