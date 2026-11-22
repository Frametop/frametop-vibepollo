/**
 * @file src/confighttp_frametop_monitors.cpp
 * @brief /api/frametop/monitors: the host's physical monitors and Frametop's plan for them
 *        (which are on, at what mode). The plan holds while Frametop streams from this host.
 */
#include "confighttp.h"
#include "frametop_monitors_service.h"

#include <nlohmann/json.hpp>
#include <Simple-Web-Server/server_https.hpp>
#include <sstream>

namespace confighttp {
  using resp_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTPS>::Response>;
  using req_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTPS>::Request>;
  bool authenticate(resp_https_t response, req_https_t request);
  bool check_content_type(resp_https_t response, req_https_t request, const std::string_view &contentType);
  void send_response(resp_https_t response, const nlohmann::json &output_tree);
  void bad_request(resp_https_t response, req_https_t request, const std::string &error_message = "Bad Request");

  void getFrametopMonitors(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) return;
    send_response(response, frametop_monitors::service::state());
  }

  // Body: {"monitors": {"<id>": {"on": bool, "mode": {"width", "height", "refresh_mhz"}}}}, the
  // whole plan (a monitor left out goes back as it was). Answers the state after it.
  void postFrametopMonitors(resp_https_t response, req_https_t request) {
    if (!check_content_type(response, request, "application/json") || !authenticate(response, request)) return;
    std::stringstream ss;
    ss << request->content.rdbuf();
    const auto body = nlohmann::json::parse(ss.str(), nullptr, false);
    if (!body.is_object()) return bad_request(response, request, "the body must be a JSON object");
    if (const auto error = frametop_monitors::service::set_plan(body); !error.empty()) {
      return bad_request(response, request, error);
    }
    send_response(response, frametop_monitors::service::state());
  }
}  // namespace confighttp
