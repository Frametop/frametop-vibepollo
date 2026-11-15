/**
 * @file src/platform/linux/hyprland_ipc.cpp
 * @brief The session's Hyprland IPC socket (see hyprland_ipc.h).
 */
#include "hyprland_ipc.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>

#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

namespace platf::hyprland_windows::ipc {
  namespace fs = std::filesystem;

  namespace {
    std::string socket_path() {
      const char *runtime = std::getenv("XDG_RUNTIME_DIR");
      if (!runtime || !*runtime) return {};
      const fs::path hypr = fs::path(runtime) / "hypr";
      if (const char *sig = std::getenv("HYPRLAND_INSTANCE_SIGNATURE"); sig && *sig) {
        return (hypr / sig / ".socket.sock").string();
      }
      // Started outside the session's environment: the one instance there is.
      std::error_code ec;
      for (const auto &entry : fs::directory_iterator(hypr, ec)) {
        const auto sock = entry.path() / ".socket.sock";
        if (fs::exists(sock, ec)) return sock.string();
      }
      return {};
    }
  }  // namespace

  std::optional<std::string> ask(const std::string &command) {
    const auto path = socket_path();
    if (path.empty() || path.size() >= sizeof(sockaddr_un::sun_path)) return std::nullopt;
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return std::nullopt;
    sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    std::copy(path.begin(), path.end(), addr.sun_path);
    timeval timeout {5, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);
    if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof addr) < 0) {
      ::close(fd);
      return std::nullopt;
    }
    size_t sent = 0;
    while (sent < command.size()) {
      const auto n = ::send(fd, command.data() + sent, command.size() - sent, MSG_NOSIGNAL);
      if (n <= 0) {
        ::close(fd);
        return std::nullopt;
      }
      sent += size_t(n);
    }
    std::string out;
    char buf[16384];
    for (;;) {
      const auto n = ::recv(fd, buf, sizeof buf, 0);
      if (n <= 0) break;
      out.append(buf, size_t(n));
    }
    ::close(fd);
    return out;
  }

  std::string valid_utf8(const std::string &in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size();) {
      const auto c = static_cast<unsigned char>(in[i]);
      const size_t n = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
      bool ok = n > 0 && i + n <= in.size();
      for (size_t k = 1; ok && k < n; ++k) ok = (static_cast<unsigned char>(in[i + k]) & 0xC0) == 0x80;
      if (ok) {
        out.append(in, i, n);
        i += n;
      } else {
        out += '?';
        ++i;
      }
    }
    return out;
  }

  std::optional<nlohmann::json> query(const std::string &what) {
    const auto reply = ask("j/" + what);
    if (!reply) return std::nullopt;
    auto parsed = nlohmann::json::parse(valid_utf8(*reply), nullptr, false);
    if (parsed.is_discarded() || !parsed.is_array()) return std::nullopt;
    return parsed;
  }

  nlohmann::json ask_json(const std::string &what) {
    return query(what).value_or(nlohmann::json::array());
  }

  bool run(const std::string &command, std::string *error) {
    const auto reply = ask(command);
    if (reply && *reply == "ok") return true;
    if (error) *error = reply ? "Hyprland: " + *reply : "Hyprland's IPC socket doesn't answer";
    return false;
  }

  bool lua(const std::string &code, std::string *error) {
    return run("eval " + code, error);
  }

  std::string lua_string(const std::string &text) {
    std::string out = "\"";
    for (const char c : text) {
      if (c == '\\' || c == '"') out += '\\';
      if (c == '\n' || c == '\r') out += ' ';
      else out += c;
    }
    return out + "\"";
  }

  int connect_events() {
    auto path = socket_path();
    const std::string answers = ".socket.sock";
    if (path.size() <= answers.size() || !path.ends_with(answers)) return -1;
    path.replace(path.size() - answers.size(), answers.size(), ".socket2.sock");
    sockaddr_un address {};
    address.sun_family = AF_UNIX;
    if (path.size() >= sizeof address.sun_path) return -1;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    if (::connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof address) != 0) {
      ::close(fd);
      return -1;
    }
    return fd;
  }
}  // namespace platf::hyprland_windows::ipc
