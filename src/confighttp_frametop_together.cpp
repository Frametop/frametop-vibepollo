/**
 * @file src/confighttp_frametop_together.cpp
 * @brief Frametop Together's HTTP endpoints: this PC's windows as panels in its person's room
 *        (src/frametop_together.h). Linux only so far.
 *
 * For the headset this PC is paired with, like the Frametop windows endpoints, and only where
 * those work: the panels are window outputs they opened.
 *
 *   POST /api/frametop/together/sender {"address", "port", "code"}
 *        Calls the headset's room program with the one-time code it made, in place of any
 *        earlier call.
 *   POST /api/frametop/together/panels {"panels": [{"number": 3, "output": "FT-W1"}, ...]}
 *        The window outputs that are the PC's panels from now on (numbers 3 to 6).
 *   POST /api/frametop/together/stop
 *   GET  /api/frametop/together
 *        {"calling", "called", "ended", "panels": [...]}; the POSTs answer with it too.
 */

#ifdef __linux__

  // standard includes
  #include <memory>
  #include <optional>
  #include <string>
  #include <vector>

  // third-party includes
  #include <nlohmann/json.hpp>
  #include <Simple-Web-Server/server_https.hpp>

  // local includes
  #include "confighttp.h"
  #include "src/frametop_together.h"
  #include "src/platform/linux/hyprland_windows.h"

namespace confighttp {
  using resp_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTPS>::Response>;
  using req_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTPS>::Request>;

  // Helpers defined in confighttp.cpp
  bool authenticate(resp_https_t response, req_https_t request);
  bool validateContentType(resp_https_t response, req_https_t request, const std::string_view &contentType);
  void bad_request(resp_https_t response, req_https_t request, const std::string &error_message);
  void send_response(resp_https_t response, const nlohmann::json &output_tree);
  bool frametop_windows_ready(resp_https_t response);
  std::optional<nlohmann::json> frametop_windows_body(resp_https_t response, req_https_t request);

  namespace {
    /// A JSON POST from the paired headset, on a host that has window outputs.
    std::optional<nlohmann::json> together_body(resp_https_t response, req_https_t request) {
      if (!validateContentType(response, request, "application/json") || !authenticate(response, request) ||
          !frametop_windows_ready(response)) {
        return std::nullopt;
      }
      return frametop_windows_body(response, request);
    }

    void send_state(resp_https_t response) {
      auto state = frametop_together::state();
      state["status"] = true;
      send_response(response, state);
    }
  }  // namespace

  void postFrametopTogetherSender(resp_https_t response, req_https_t request) {
    const auto body = together_body(response, request);
    if (!body) {
      return;
    }
    const auto address = body->find("address");
    const auto port = body->find("port");
    const auto code = body->find("code");
    if (address == body->end() || !address->is_string() || port == body->end() || !port->is_number_integer() ||
        code == body->end() || !code->is_string()) {
      return bad_request(response, request, "address, port and code are needed");
    }
    std::string error;
    if (!frametop_together::call(address->get<std::string>(), port->get<int>(), code->get<std::string>(), &error)) {
      return bad_request(response, request, error);
    }
    send_state(response);
  }

  void postFrametopTogetherPanels(resp_https_t response, req_https_t request) {
    const auto body = together_body(response, request);
    if (!body) {
      return;
    }
    const auto listed = body->find("panels");
    if (listed == body->end() || !listed->is_array()) {
      return bad_request(response, request, "panels must be a list");
    }
    const auto monitors = platf::hyprland_windows::monitor_list();
    std::vector<frametop_together::panel_t> panels;
    for (const auto &entry : *listed) {
      if (!entry.is_object() || !entry.contains("number") || !entry["number"].is_number_integer() ||
          !entry.contains("output") || !entry["output"].is_string()) {
        return bad_request(response, request, "a panel is {\"number\", \"output\"}");
      }
      frametop_together::panel_t panel;
      panel.number = entry["number"].get<int>();
      panel.output = entry["output"].get<std::string>();
      // Only a window's own output: never a real monitor, which the windows role didn't open.
      for (const auto &monitor : monitors) {
        if (monitor.window && monitor.name == panel.output) {
          panel.width = monitor.width;
          panel.height = monitor.height;
        }
      }
      if (!panel.width) {
        return bad_request(response, request, panel.output + " isn't a window output that is open");
      }
      panels.push_back(std::move(panel));
    }
    std::string error;
    if (!frametop_together::show(panels, &error)) {
      return bad_request(response, request, error);
    }
    send_state(response);
  }

  void postFrametopTogetherStop(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }
    frametop_together::stop();
    send_state(response);
  }

  void getFrametopTogether(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }
    send_state(response);
  }
}  // namespace confighttp

#endif
