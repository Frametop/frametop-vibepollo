/**
 * @file src/platform/linux/frametop_monitors_linux.cpp
 * @brief Frametop's monitor plan on Linux: Hyprland through its socket (the user's own session)
 *        or the session broker (the machine service), and KDE Plasma through KScreen.
 */
#include "display_backend.h"
#include "display_helper_process.h"
#include "frametop_monitors_hyprland.h"
#include "frametop_monitors_kscreen.h"
#include "hyprland_ipc.h"
#include "private_display.h"
#include "session_backend.h"
#include "src/config.h"
#include "src/frametop_monitors_service.h"

#include <algorithm>
#include <chrono>
#include <gio/gio.h>
#include <string_view>

namespace platf {
  namespace {
    using json = nlohmann::json;
    constexpr auto SESSION_EXEC = "/usr/libexec/vibeshine/vibepollo-session-exec";

    bool host_virtual_output(const std::string &name) {
      const auto &listed = config::video.dd.virtual_display_outputs;
      return linux_private_display::is_kernel_output(name) || linux_private_display::is_private_output(name) ||
             std::find(listed.begin(), listed.end(), name) != listed.end();
    }

    // The session broker's answer to one request, within 5 s.
    linux_private_display::helper_process::result_t broker(const std::vector<std::string> &arguments) {
      std::vector<const gchar *> argv {SESSION_EXEC};
      for (const auto &a : arguments) argv.push_back(a.c_str());
      argv.push_back(nullptr);
      GError *error = nullptr;
      auto *process = g_subprocess_newv(argv.data(),
                                        static_cast<GSubprocessFlags>(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE),
                                        &error);
      if (!process) {
        linux_private_display::helper_process::result_t result;
        result.stderr_text = error ? error->message : "couldn't start the session broker";
        if (error) g_error_free(error);
        return result;
      }
      auto result = linux_private_display::helper_process::communicate_until(
        process, std::chrono::steady_clock::now() + std::chrono::seconds {5}, true);
      g_object_unref(process);
      return result;
    }

    std::string trimmed(std::string text) {
      while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) text.pop_back();
      return text;
    }

    // The user's own Hyprland session: its socket.
    class hypr_socket_t final: public frametop_monitors_hyprland::link_t {
    public:
      std::optional<json> monitors() override {
        return hyprland_windows::ipc::query("monitors all");
      }

      std::string apply(const frametop_monitors_hyprland::rule_t &rule) override {
        const auto lua = frametop_monitors_hyprland::lua_rule(rule);
        if (lua.empty()) return "not a rule Hyprland can take";
        std::string error;
        return hyprland_windows::ipc::lua(lua, &error) ? std::string {} : (error.empty() ? "Hyprland said no" : error);
      }
    };

    // The machine service: the session broker runs hyprctl as the desktop user and writes the
    // Lua itself from the rule's checked words.
    class hypr_broker_t final: public frametop_monitors_hyprland::link_t {
    public:
      std::optional<json> monitors() override {
        const auto result = broker({"hypr-monitors"});
        if (!result.success) return std::nullopt;
        auto parsed = json::parse(result.stdout_text, nullptr, false);
        if (parsed.is_discarded() || !parsed.is_array()) return std::nullopt;
        return parsed;
      }

      std::string apply(const frametop_monitors_hyprland::rule_t &rule) override {
        if (!frametop_monitors_hyprland::rule_is_safe(rule)) return "not a rule Hyprland can take";
        const auto result = rule.on ?
                              broker({"hypr-monitor", rule.output, rule.mode, rule.position, rule.scale, std::to_string(rule.transform)}) :
                              broker({"hypr-monitor", rule.output, "off"});
        const auto answer = trimmed(result.stdout_text);
        if (result.success && answer == "ok") return {};
        if (!answer.empty()) return answer;
        return trimmed(result.stderr_text).empty() ? std::string {"the session broker said no"} : trimmed(result.stderr_text);
      }
    };

    class kscreen_t final: public frametop_monitors_kscreen::link_t {
    public:
      std::optional<json> query() override {
        return linux_private_display::kscreen_configuration();
      }

      std::string apply(const std::vector<std::string> &arguments) override {
        return linux_private_display::kscreen_apply(arguments);
      }
    };
  }  // namespace

  std::unique_ptr<frametop_monitors::platform_t> make_frametop_monitors() {
    if (linux_display::hyprland_kms_session()) {
      return std::make_unique<frametop_monitors_hyprland::platform_t>(std::make_unique<hypr_broker_t>(), host_virtual_output);
    }
    if (linux_display::hyprland_wlr_session()) {
      return std::make_unique<frametop_monitors_hyprland::platform_t>(std::make_unique<hypr_socket_t>(), host_virtual_output);
    }
    if (std::string_view {linux_display::backend().capabilities().backend_name} == "kscreen-vkms") {
      return std::make_unique<frametop_monitors_kscreen::platform_t>(std::make_unique<kscreen_t>(), host_virtual_output);
    }
    return nullptr;
  }
}  // namespace platf
