/**
 * @file src/platform/linux/hyprland_windows.cpp
 * @brief Frametop windows on a Hyprland desktop (see hyprland_windows.h).
 *
 * Everything goes through Hyprland's IPC socket: j/ queries for state, "output create|remove"
 * for the headless outputs and Lua (Hyprland 0.55+) for the rest.
 */
#include "hyprland_windows.h"

#include "src/config.h"
#include "src/logging.h"
#include "src/rtsp.h"
#include "src/video.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <regex>
#include <set>
#include <sstream>
#include <thread>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

using namespace std::literals;

namespace platf::hyprland_windows {
  namespace {
    using clock = std::chrono::steady_clock;
    namespace fs = std::filesystem;

    constexpr auto kGrace = 60s;  // no stream of a window output this long: the window goes back
    constexpr auto kWatch = 2s;
    constexpr auto kLaunchWait = 15s;
    constexpr int kFirstX = 30000;  // window outputs sit far right of any real monitor layout
    constexpr int kSlotWidth = 8200;
    constexpr int kMaxOutputs = 16;
    constexpr const char *kPrefix = "FT-W";

    struct hosted_t {
      std::string address;
      std::string workspace;  // where it goes back to ("" for a launched app: the focused one)
      std::string monitor;  // the monitor that workspace was on ("": the focused one)
      int fullscreen = 0;  // its fullscreen state before, internal and client
      int fullscreen_client = 0;
      clock::time_point seen;  // last captured by a stream, or opened
      bool launching = false;  // the output waits for a launched app's window (no address yet)
      std::optional<clock::time_point> refade_at;  // fullscreen again then (see refade)
    };

    std::recursive_timed_mutex lock;  // held across a whole operation: one window move at a time
    std::map<std::string, hosted_t> hosted;  // by output name
    std::map<std::string, int> others_fullscreen;  // other windows' fullscreen state at the last watch
    bool started = false;
    std::atomic<bool> stopping {false};  // process exit: the watcher leaves the windows to release_at_exit

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

    // Hyprland passes window titles through as they are, and one with bytes that aren't UTF-8
    // (an old X11 app's) would make the whole answer unreadable: those bytes become '?'.
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

    // A j/ query's array, or nullopt when Hyprland didn't answer or the answer isn't one.
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

    // A Lua string literal. Only text from Hyprland or this host's .desktop files gets here.
    std::string lua_string(const std::string &text) {
      std::string out = "\"";
      for (const char c : text) {
        if (c == '\\' || c == '"') out += '\\';
        if (c == '\n' || c == '\r') out += ' ';
        else out += c;
      }
      return out + "\"";
    }

    bool valid_address(const std::string &address) {
      static const std::regex pattern {"^0x[0-9a-fA-F]{1,16}$"};
      return std::regex_match(address, pattern);
    }

    bool window_output(const std::string &name) {
      return name.rfind(kPrefix, 0) == 0;
    }

    std::optional<nlohmann::json> find_client(const std::string &address) {
      for (const auto &c : ask_json("clients")) {
        if (c.value("address", "") == address) return c;
      }
      return std::nullopt;
    }

    std::optional<nlohmann::json> find_monitor(const std::string &name) {
      for (const auto &m : ask_json("monitors")) {
        if (m.value("name", "") == name) return m;
      }
      return std::nullopt;
    }

    int even_clamp(int value, int low, int high) {
      return std::clamp(value, low, high) & ~1;
    }

    fs::path state_path() {
      const fs::path state = config::nvhttp.file_state;
      return (state.has_parent_path() ? state.parent_path() : fs::path {"."}) / "frametop_windows.json";
    }

    void save() {
      nlohmann::json out = nlohmann::json::object();
      for (const auto &[output, h] : hosted) {
        out[output] = {{"address", h.address}, {"workspace", h.workspace}, {"monitor", h.monitor},
                       {"fullscreen", h.fullscreen},
                       {"fullscreen_client", h.fullscreen_client}};
      }
      const auto path = state_path();
      const auto tmp = path.string() + ".tmp";
      {
        std::ofstream file {tmp, std::ios::trunc};
        file << out.dump(2);
        if (!file) return;
      }
      std::error_code ec;
      fs::rename(tmp, path, ec);
    }

    std::string free_output_name() {
      std::set<std::string> taken;
      for (const auto &m : ask_json("monitors all")) taken.insert(m.value("name", ""));
      for (const auto &[output, h] : hosted) taken.insert(output);
      for (int n = 1; n <= kMaxOutputs; ++n) {
        const auto name = kPrefix + std::to_string(n);
        if (!taken.contains(name)) return name;
      }
      return {};
    }

    std::string mode_rule(const std::string &output, int width, int height) {
      const int slot = std::max(1, std::atoi(output.c_str() + std::char_traits<char>::length(kPrefix)));
      return "hl.monitor({output = " + lua_string(output) + ", mode = \"" + std::to_string(width) + "x" +
        std::to_string(height) + "@60\", position = \"" + std::to_string(kFirstX + (slot - 1) * kSlotWidth) +
        "x0\", scale = 1})";
    }

    // A new headless output of this size; its workspace id, or nullopt.
    std::optional<std::string> create_output(const std::string &output, int width, int height, std::string *error) {
      if (!run("output create headless " + output, error)) return std::nullopt;
      if (!lua(mode_rule(output, width, height), error)) {
        run("output remove " + output, nullptr);
        return std::nullopt;
      }
      for (int i = 0; i < 40; ++i) {
        if (const auto m = find_monitor(output)) {
          if (m->value("width", 0) == width && m->value("height", 0) == height && (*m)["activeWorkspace"].is_object()) {
            return std::to_string((*m)["activeWorkspace"].value("id", 0));
          }
        }
        std::this_thread::sleep_for(50ms);
      }
      run("output remove " + output, nullptr);
      if (error) *error = "Hyprland didn't make the output " + output;
      return std::nullopt;
    }

    bool place(const std::string &address, const std::string &workspace, std::string *error) {
      const auto window = lua_string("address:" + address);
      return lua("hl.dispatch(hl.dsp.window.move({window = " + window + ", workspace = " + lua_string(workspace) +
                   ", follow = false}))",
                 error) &&
        lua("hl.dispatch(hl.dsp.window.fullscreen_state({window = " + window + ", internal = 2, client = 0}))", error);
    }

    // Hyprland 0.56 fades a monitor's top layers (the desktop's bar and dock) under a fullscreen
    // window, but lifts them back above it when they map after the window went fullscreen (a new
    // output's bar comes a moment later) and whenever a window elsewhere leaves fullscreen. They'd
    // cover the top and bottom of the panel. Going fullscreen again fades them: off and on, as
    // setting the same state again changes nothing.
    void refade(const std::string &address) {
      const auto window = lua_string("address:" + address);
      lua("hl.dispatch(hl.dsp.window.fullscreen_state({window = " + window + ", internal = 0, client = 0})) "
          "hl.dispatch(hl.dsp.window.fullscreen_state({window = " + window + ", internal = 2, client = 0}))",
          nullptr);
    }

    // How a window move names a workspace: its number, or "name:NAME" for a named one (Hyprland
    // numbers those below zero).
    std::string workspace_selector(const nlohmann::json &w) {
      const int id = w.value("id", 0);
      return id > 0 ? std::to_string(id) : "name:" + w.value("name", "");
    }

    // A real monitor's name by Hyprland's id ("" for a window output or none).
    std::string monitor_name(int id) {
      for (const auto &m : ask_json("monitors")) {
        if (m.value("id", -1) == id) {
          const auto name = m.value("name", "");
          return window_output(name) ? "" : name;
        }
      }
      return {};
    }

    std::string focused_monitor() {
      for (const auto &m : ask_json("monitors")) {
        if (m.value("focused", false) && !window_output(m.value("name", ""))) return m.value("name", "");
      }
      return {};
    }

    // A workspace the window went back to that didn't exist any more is made on the window
    // output, and would leave with it to whichever monitor Hyprland (or the desktop's own
    // workspace rules) picks: take it to the monitor it came from.
    void home_workspace(const std::string &workspace, const std::string &monitor, std::string *error) {
      for (const auto &w : ask_json("workspaces")) {
        if (workspace_selector(w) != workspace || !window_output(w.value("monitor", ""))) continue;
        auto target = monitor.empty() || !find_monitor(monitor) ? focused_monitor() : monitor;
        if (!target.empty()) {
          lua("hl.dispatch(hl.dsp.workspace.move({workspace = " + lua_string(workspace) + ", monitor = " +
                lua_string(target) + "}))",
              error);
        }
        return;
      }
    }

    std::string focused_workspace() {
      const auto reply = ask("j/activeworkspace");
      const auto w = reply ? nlohmann::json::parse(*reply, nullptr, false) : nlohmann::json {};
      return w.is_object() ? std::to_string(w.value("id", 1)) : "1";
    }

    // Puts one hosted window back and removes its output. Holds the lock. No logging at exit:
    // the log may be gone by then. False (and the output stays, for the next try) when Hyprland
    // doesn't answer.
    bool put_back(const std::string &output, bool log = true) {
      const auto it = hosted.find(output);
      if (it == hosted.end()) return true;
      const auto clients = query("clients");
      if (!clients) {
        if (log) BOOST_LOG(warning) << "Frametop windows: Hyprland didn't answer: " << output << " stays for now";
        return false;
      }
      const auto h = it->second;
      hosted.erase(it);
      const bool open = std::any_of(clients->begin(), clients->end(), [&](const auto &c) {
        return !h.address.empty() && c.value("address", "") == h.address;
      });
      if (open) {
        const auto window = lua_string("address:" + h.address);
        std::string error;
        const auto workspace = h.workspace.empty() ? focused_workspace() : h.workspace;
        if ((!lua("hl.dispatch(hl.dsp.window.fullscreen_state({window = " + window + ", internal = " +
                    std::to_string(h.fullscreen) + ", client = " + std::to_string(h.fullscreen_client) + "}))",
                  &error) ||
             !lua("hl.dispatch(hl.dsp.window.move({window = " + window + ", workspace = " + lua_string(workspace) +
                    ", follow = false}))",
                  &error)) &&
            log) {
          BOOST_LOG(warning) << "Frametop windows: can't put " << h.address << " back: " << error;
        }
        home_workspace(workspace, h.monitor, &error);
      }
      if (find_monitor(output)) run("output remove " + output, nullptr);
      if (log) BOOST_LOG(info) << "Frametop windows: " << output << " released (" << h.address << ")";
      save();
      return true;
    }

    std::optional<nlohmann::json> host(const std::string &address, const nlohmann::json &client, int width, int height,
                                       std::string workspace_back, std::string monitor_back, std::string *error) {
      const auto output = free_output_name();
      if (output.empty()) {
        *error = "every window output is in use";
        return std::nullopt;
      }
      const auto workspace = create_output(output, width, height, error);
      if (!workspace) return std::nullopt;
      hosted_t h;
      h.address = address;
      h.workspace = std::move(workspace_back);
      h.monitor = std::move(monitor_back);
      h.fullscreen = client.value("fullscreen", 0);
      h.fullscreen_client = client.value("fullscreenClient", 0);
      h.seen = clock::now();
      hosted[output] = h;
      save();
      if (!place(address, *workspace, error)) {
        put_back(output);
        return std::nullopt;
      }
      hosted[output].refade_at = clock::now() + 1500ms;
      BOOST_LOG(info) << "Frametop windows: " << address << " (" << client.value("class", "") << ") on " << output << ' '
                      << width << 'x' << height;
      return nlohmann::json {{"output", output}, {"width", width}, {"height", height}, {"address", address}};
    }

    struct desktop_app_t {
      std::string id, name, icon, exec;
    };

    std::vector<fs::path> application_dirs() {
      std::vector<fs::path> dirs;
      const char *home = std::getenv("HOME");
      const char *data_home = std::getenv("XDG_DATA_HOME");
      if (data_home && *data_home) dirs.emplace_back(fs::path(data_home) / "applications");
      else if (home && *home) dirs.emplace_back(fs::path(home) / ".local/share/applications");
      const char *data_dirs = std::getenv("XDG_DATA_DIRS");
      std::stringstream list {data_dirs && *data_dirs ? data_dirs : "/usr/local/share:/usr/share"};
      for (std::string dir; std::getline(list, dir, ':');) {
        if (!dir.empty()) dirs.emplace_back(fs::path(dir) / "applications");
      }
      return dirs;
    }

    // The Exec line without its field codes (the desktop entry spec's %f, %u, ...).
    std::string strip_field_codes(const std::string &exec) {
      std::string out;
      for (size_t i = 0; i < exec.size(); ++i) {
        if (exec[i] != '%' || i + 1 == exec.size()) {
          out += exec[i];
          continue;
        }
        if (exec[++i] == '%') out += '%';
      }
      return out;
    }

    std::vector<desktop_app_t> desktop_apps() {
      std::vector<desktop_app_t> apps;
      std::set<std::string> seen;  // the first directory with an id wins, as the spec says
      for (const auto &dir : application_dirs()) {
        std::error_code ec;
        for (const auto &entry : fs::directory_iterator(dir, ec)) {
          const auto id = entry.path().filename().string();
          if (entry.path().extension() != ".desktop" || !seen.insert(id).second) continue;
          std::ifstream file {entry.path()};
          desktop_app_t app {id, {}, {}, {}};
          bool in_entry = false, hide = false, application = false;
          for (std::string line; std::getline(file, line);) {
            if (!line.empty() && line[0] == '[') {
              in_entry = line == "[Desktop Entry]";
              continue;
            }
            if (!in_entry) continue;
            const auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            const auto trim = [](std::string t) {  // "Key = value" is allowed too
              const auto first = t.find_first_not_of(" \t"), last = t.find_last_not_of(" \t\r");
              return first == std::string::npos ? std::string {} : t.substr(first, last - first + 1);
            };
            const auto key = trim(line.substr(0, eq)), value = trim(line.substr(eq + 1));
            if (key == "Name") app.name = value;
            else if (key == "Icon") app.icon = value;
            else if (key == "Exec") app.exec = strip_field_codes(value);
            else if (key == "Type") application = value == "Application";
            else if ((key == "NoDisplay" || key == "Hidden" || key == "Terminal") && value == "true") hide = true;
          }
          if (application && !hide && !app.name.empty() && !app.exec.empty()) apps.push_back(std::move(app));
        }
      }
      std::sort(apps.begin(), apps.end(), [](const auto &a, const auto &b) { return a.name < b.name; });
      return apps;
    }

    // The outputs it released whose window closed or that went away: their streams end
    // (watch(), outside the lock), or the panel would keep the window's last picture.
    std::vector<std::string> watch_once() {
      std::lock_guard guard {lock};
      if (stopping || hosted.empty()) return {};
      const auto clients = query("clients"), monitors = query("monitors all");
      if (!clients || !monitors) return {};  // Hyprland didn't answer: change nothing
      std::set<std::string> windows, outputs, ours;
      for (const auto &[output, h] : hosted) ours.insert(h.address);
      for (const auto &c : *clients) windows.insert(c.value("address", ""));
      for (const auto &m : *monitors) outputs.insert(m.value("name", ""));
      // Another window that left fullscreen (or closed while in it) lifted our bars: refade.
      std::map<std::string, int> fullscreen;
      for (const auto &c : *clients) {
        if (!ours.contains(c.value("address", ""))) fullscreen[c.value("address", "")] = c.value("fullscreen", 0);
      }
      bool left = false;
      for (const auto &[address, state] : others_fullscreen) {
        const auto now_state = fullscreen.find(address);
        if (state != 0 && (now_state == fullscreen.end() || now_state->second == 0)) left = true;
      }
      others_fullscreen = std::move(fullscreen);
      const auto now = clock::now();
      std::vector<std::string> done, ended;
      for (auto &[output, h] : hosted) {
        if (h.launching) continue;  // launch() owns it until its window comes
        if (video::output_is_captured(output)) h.seen = now;
        if (windows.contains(h.address) && (left || (h.refade_at && now >= *h.refade_at))) {
          refade(h.address);
          h.refade_at.reset();
        }
        if (!windows.contains(h.address)) {
          BOOST_LOG(info) << "Frametop windows: the window on " << output << " closed";
          done.push_back(output);
          ended.push_back(output);
        } else if (!outputs.contains(output)) {
          BOOST_LOG(info) << "Frametop windows: " << output << " went away";
          done.push_back(output);
          ended.push_back(output);
        } else if (now - h.seen > kGrace) {
          BOOST_LOG(info) << "Frametop windows: no stream of " << output << " for a minute";
          done.push_back(output);
        }
      }
      for (const auto &output : done) put_back(output);
      return ended;
    }

    // Ends the streams of outputs that went. The client asks for its window again, hears "no
    // window", and its panel shows that instead of a frozen picture.
    void end_streams(const std::vector<std::string> &outputs) {
      for (const auto &output : outputs) {
        for (const auto &client : rtsp_stream::client_uuids_capturing(output)) {
          BOOST_LOG(info) << "Frametop windows: ending the stream of " << output;
          rtsp_stream::disconnect_client_sessions_with_result(client, true);
        }
      }
    }

    void watch() {
      while (!stopping) {
        std::this_thread::sleep_for(kWatch);
        try {
          end_streams(watch_once());
        } catch (const std::exception &e) {
          BOOST_LOG(warning) << "Frametop windows: the watcher skipped a round: " << e.what();
        }
      }
    }

    void release_at_exit() {
      stopping = true;
      // The watcher or a launch may hold the lock for a moment: windows left out stay on
      // outputs nobody sees until the next start.
      std::unique_lock guard {lock, std::defer_lock};
      if (!guard.try_lock_for(3s)) return;
      std::vector<std::string> outputs;
      for (const auto &[output, h] : hosted) outputs.push_back(output);
      for (const auto &output : outputs) put_back(output, false);
    }
  }  // namespace

  bool available() {
    return ask("j/version").has_value();
  }

  nlohmann::json list() {
    std::lock_guard guard {lock};
    std::map<int, std::string> monitor_names;
    for (const auto &m : ask_json("monitors all")) monitor_names[m.value("id", -1)] = m.value("name", "");
    std::map<std::string, std::string> output_of;
    for (const auto &[output, h] : hosted) output_of[h.address] = output;
    nlohmann::json windows = nlohmann::json::array();
    for (const auto &c : ask_json("clients")) {
      if (!c.value("mapped", false) || c.value("hidden", false)) continue;
      const auto address = c.value("address", "");
      const auto size = c.value("size", nlohmann::json::array({0, 0}));
      nlohmann::json w {{"address", address},
                        {"class", c.value("class", "")},
                        {"title", c.value("title", "")},
                        {"pid", c.value("pid", 0)},
                        {"xwayland", c.value("xwayland", false)},
                        {"width", size.size() == 2 ? size[0] : nlohmann::json(0)},
                        {"height", size.size() == 2 ? size[1] : nlohmann::json(0)},
                        {"workspace", c.contains("workspace") ? c["workspace"].value("name", "") : ""},
                        {"monitor", monitor_names[c.value("monitor", -1)]},
                        {"output", nullptr}};
      if (const auto it = output_of.find(address); it != output_of.end()) w["output"] = it->second;
      windows.push_back(std::move(w));
    }
    nlohmann::json outputs = nlohmann::json::array();
    for (const auto &[output, h] : hosted) outputs.push_back({{"output", output}, {"address", h.address}});
    // Locked, the desktop draws only its lock screen: window outputs stream black until it's unlocked.
    const auto locked = ask("locked");
    return {{"windows", windows}, {"outputs", outputs}, {"locked", locked && locked->rfind("true", 0) == 0}};
  }

  nlohmann::json apps() {
    nlohmann::json out = nlohmann::json::array();
    for (const auto &app : desktop_apps()) out.push_back({{"id", app.id}, {"name", app.name}, {"icon", app.icon}});
    return out;
  }

  std::vector<monitor_t> monitor_list() {
    std::vector<monitor_t> out;
    for (const auto &m : ask_json("monitors")) {
      const auto name = m.value("name", "");
      out.push_back({name, m.value("description", ""), m.value("width", 0), m.value("height", 0),
                     m.value("refreshRate", 0.0), window_output(name)});
    }
    return out;
  }

  std::optional<nlohmann::json> open(const std::string &address, int width, int height, std::string *error) {
    std::lock_guard guard {lock};
    if (!valid_address(address)) {
      *error = "address must be a Hyprland window address (0x...)";
      return std::nullopt;
    }
    for (auto &[output, h] : hosted) {
      if (h.address != address) continue;
      // Already out: the Frame asks again after a restart of its own. Same output, new size,
      // and its grace starts over (the new stream may take a moment to capture it).
      h.seen = clock::now();
      if (width > 0 && height > 0 && !resize(output, width, height, error)) return std::nullopt;
      const auto m = find_monitor(output);
      return nlohmann::json {{"output", output},
                             {"width", m ? m->value("width", width) : width},
                             {"height", m ? m->value("height", height) : height},
                             {"address", address}};
    }
    const auto client = find_client(address);
    if (!client || !client->value("mapped", false)) {
      *error = "no window " + address;
      return std::nullopt;
    }
    const auto &workspace = (*client)["workspace"];
    if (workspace.value("name", "").rfind("special", 0) == 0) {
      *error = "windows on a special workspace stay there";
      return std::nullopt;
    }
    const auto size = client->value("size", nlohmann::json::array({1280, 800}));
    width = even_clamp(width > 0 ? width : size[0].get<int>(), 320, 7680);
    height = even_clamp(height > 0 ? height : size[1].get<int>(), 240, 4320);
    return host(address, *client, width, height, workspace_selector(workspace), monitor_name(client->value("monitor", -1)),
                error);
  }

  std::optional<nlohmann::json> launch(const std::string &app_id, int width, int height, std::string *error) {
    std::unique_lock guard {lock};
    const auto apps = desktop_apps();
    const auto app = std::find_if(apps.begin(), apps.end(), [&](const auto &a) { return a.id == app_id; });
    if (app == apps.end()) {
      *error = "no app " + app_id;
      return std::nullopt;
    }
    width = even_clamp(width > 0 ? width : 1280, 320, 7680);
    height = even_clamp(height > 0 ? height : 800, 240, 4320);
    const auto output = free_output_name();
    if (output.empty()) {
      *error = "every window output is in use";
      return std::nullopt;
    }
    const auto workspace = create_output(output, width, height, error);
    if (!workspace) return std::nullopt;
    const auto clients = query("clients");
    if (!clients) {
      run("output remove " + output, nullptr);
      *error = "Hyprland didn't answer";
      return std::nullopt;
    }
    std::set<std::string> before;
    for (const auto &c : *clients) before.insert(c.value("address", ""));
    if (!lua("hl.exec_cmd(" + lua_string(app->exec) + ", {workspace = " + lua_string(*workspace + " silent") + "})", error)) {
      run("output remove " + output, nullptr);
      return std::nullopt;
    }
    // The output is spoken for while the app starts, without holding up the other calls.
    hosted_t reserved;
    reserved.launching = true;
    reserved.seen = clock::now();
    hosted[output] = reserved;
    const auto deadline = clock::now() + kLaunchWait;
    std::string address;
    guard.unlock();
    while (address.empty() && clock::now() < deadline) {
      std::this_thread::sleep_for(150ms);
      for (const auto &c : ask_json("clients")) {
        const auto a = c.value("address", "");
        if (before.contains(a) || !c.value("mapped", false)) continue;
        if (std::to_string(c["workspace"].value("id", 0)) != *workspace) continue;
        address = a;
        break;
      }
    }
    guard.lock();
    const auto it = hosted.find(output);
    if (it == hosted.end() || !it->second.launching) {  // released while the app started
      *error = app->name + "'s output was released before its window came";
      return std::nullopt;
    }
    if (address.empty()) {
      hosted.erase(it);
      run("output remove " + output, nullptr);
      *error = app->name + " didn't open a window in time";
      return std::nullopt;
    }
    it->second.launching = false;
    it->second.address = address;  // no workspace: a launched app goes back to the focused one
    it->second.seen = clock::now();
    save();
    if (!place(address, *workspace, error)) {
      put_back(output);
      return std::nullopt;
    }
    hosted[output].refade_at = clock::now() + 1500ms;
    BOOST_LOG(info) << "Frametop windows: launched " << app_id << " as " << address << " on " << output;
    return nlohmann::json {{"output", output}, {"width", width}, {"height", height}, {"address", address}};
  }

  bool resize(const std::string &output, int width, int height, std::string *error) {
    std::lock_guard guard {lock};
    if (!hosted.contains(output)) {
      *error = "no window output " + output;
      return false;
    }
    if (width <= 0 || height <= 0) {
      *error = "a resize needs a width and a height";
      return false;
    }
    width = even_clamp(width, 320, 7680);
    height = even_clamp(height, 240, 4320);
    return lua(mode_rule(output, width, height), error);
  }

  bool release(const std::string &output_or_address, std::string *error) {
    std::lock_guard guard {lock};
    for (const auto &[output, h] : hosted) {
      if (output == output_or_address || h.address == output_or_address) {
        const auto name = output;  // put_back erases the entry
        if (put_back(name)) return true;
        *error = "Hyprland didn't answer: " + name + " stays out for now";
        return false;
      }
    }
    *error = "no window output for " + output_or_address;
    return false;
  }

  int release_all() {
    std::lock_guard guard {lock};
    std::vector<std::string> outputs;
    for (const auto &[output, h] : hosted) outputs.push_back(output);
    return int(std::count_if(outputs.begin(), outputs.end(), [](const auto &output) { return put_back(output); }));
  }

  void start() {
    std::lock_guard guard {lock};
    if (started) return;
    started = true;
    // Windows a crash left out: back to where they came from, before anything new opens.
    std::ifstream file {state_path()};
    const auto saved = file ? nlohmann::json::parse(file, nullptr, false) : nlohmann::json {};
    if (saved.is_object()) {
      for (const auto &[output, h] : saved.items()) {
        if (!window_output(output) || !h.is_object()) continue;
        hosted_t back;
        back.address = h.value("address", "");
        back.workspace = h.value("workspace", "");
        back.monitor = h.value("monitor", "");
        back.fullscreen = h.value("fullscreen", 0);
        back.fullscreen_client = h.value("fullscreen_client", 0);
        back.seen = clock::now();
        hosted[output] = back;
      }
      if (!hosted.empty()) BOOST_LOG(info) << "Frametop windows: putting back " << hosted.size() << " left from before";
      release_all();
    }
    std::atexit(release_at_exit);
    std::thread {watch}.detach();
  }
}  // namespace platf::hyprland_windows
