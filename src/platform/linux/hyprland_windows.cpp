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
      int fullscreen = 0;  // its fullscreen state before, internal and client
      int fullscreen_client = 0;
      clock::time_point seen;  // last captured by a stream, or opened
    };

    std::recursive_mutex lock;  // held across a whole operation: one window move at a time
    std::map<std::string, hosted_t> hosted;  // by output name
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

    nlohmann::json ask_json(const std::string &what) {
      const auto reply = ask("j/" + what);
      if (!reply) return nlohmann::json::array();
      auto parsed = nlohmann::json::parse(*reply, nullptr, false);
      return parsed.is_discarded() ? nlohmann::json::array() : parsed;
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
        out[output] = {{"address", h.address}, {"workspace", h.workspace}, {"fullscreen", h.fullscreen},
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

    std::string focused_workspace() {
      const auto reply = ask("j/activeworkspace");
      const auto w = reply ? nlohmann::json::parse(*reply, nullptr, false) : nlohmann::json {};
      return w.is_object() ? std::to_string(w.value("id", 1)) : "1";
    }

    // Puts one hosted window back and removes its output. Holds the lock. No logging at exit:
    // the log may be gone by then.
    void put_back(const std::string &output, bool log = true) {
      const auto it = hosted.find(output);
      if (it == hosted.end()) return;
      const auto h = it->second;
      hosted.erase(it);
      if (find_client(h.address)) {
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
      }
      if (find_monitor(output)) run("output remove " + output, nullptr);
      if (log) BOOST_LOG(info) << "Frametop windows: " << output << " released (" << h.address << ")";
      save();
    }

    std::optional<nlohmann::json> host(const std::string &address, const nlohmann::json &client, int width, int height,
                                       std::string workspace_back, std::string *error) {
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
      h.fullscreen = client.value("fullscreen", 0);
      h.fullscreen_client = client.value("fullscreenClient", 0);
      h.seen = clock::now();
      hosted[output] = h;
      save();
      if (!place(address, *workspace, error)) {
        put_back(output);
        return std::nullopt;
      }
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
            const auto key = line.substr(0, eq), value = line.substr(eq + 1);
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

    void watch() {
      for (;;) {
        std::this_thread::sleep_for(kWatch);
        if (stopping) return;
        std::lock_guard guard {lock};
        if (hosted.empty()) continue;
        std::set<std::string> windows, outputs;
        for (const auto &c : ask_json("clients")) windows.insert(c.value("address", ""));
        for (const auto &m : ask_json("monitors all")) outputs.insert(m.value("name", ""));
        if (windows.empty() && outputs.empty()) continue;  // Hyprland didn't answer: change nothing
        const auto now = clock::now();
        std::vector<std::string> done;
        for (auto &[output, h] : hosted) {
          if (video::output_is_captured(output)) h.seen = now;
          if (!windows.contains(h.address)) {
            BOOST_LOG(info) << "Frametop windows: the window on " << output << " closed";
            done.push_back(output);
          } else if (!outputs.contains(output)) {
            BOOST_LOG(info) << "Frametop windows: " << output << " went away";
            done.push_back(output);
          } else if (now - h.seen > kGrace) {
            BOOST_LOG(info) << "Frametop windows: no stream of " << output << " for a minute";
            done.push_back(output);
          }
        }
        for (const auto &output : done) put_back(output);
      }
    }

    void release_at_exit() {
      stopping = true;
      std::unique_lock guard {lock, std::try_to_lock};
      if (!guard.owns_lock()) return;
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
    return {{"windows", windows}, {"outputs", outputs}};
  }

  nlohmann::json apps() {
    nlohmann::json out = nlohmann::json::array();
    for (const auto &app : desktop_apps()) out.push_back({{"id", app.id}, {"name", app.name}, {"icon", app.icon}});
    return out;
  }

  nlohmann::json monitors() {
    nlohmann::json out = nlohmann::json::array();
    for (const auto &m : ask_json("monitors")) {
      out.push_back({{"name", m.value("name", "")},
                     {"description", m.value("description", "")},
                     {"width", m.value("width", 0)},
                     {"height", m.value("height", 0)},
                     {"refresh", m.value("refreshRate", 0.0)},
                     {"window", window_output(m.value("name", ""))}});
    }
    return out;
  }

  std::optional<nlohmann::json> open(const std::string &address, int width, int height, std::string *error) {
    std::lock_guard guard {lock};
    if (!valid_address(address)) {
      *error = "address must be a Hyprland window address (0x...)";
      return std::nullopt;
    }
    for (const auto &[output, h] : hosted) {
      if (h.address != address) continue;
      // Already out: the Frame asks again after a restart of its own. Same output, new size.
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
    if ((*client)["workspace"].value("id", 0) < 0) {
      *error = "windows on a special workspace stay there";
      return std::nullopt;
    }
    const auto size = client->value("size", nlohmann::json::array({1280, 800}));
    width = even_clamp(width > 0 ? width : size[0].get<int>(), 320, 7680);
    height = even_clamp(height > 0 ? height : size[1].get<int>(), 240, 4320);
    return host(address, *client, width, height, std::to_string((*client)["workspace"].value("id", 1)), error);
  }

  std::optional<nlohmann::json> launch(const std::string &app_id, int width, int height, std::string *error) {
    std::lock_guard guard {lock};
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
    std::set<std::string> before;
    for (const auto &c : ask_json("clients")) before.insert(c.value("address", ""));
    if (!lua("hl.exec_cmd(" + lua_string(app->exec) + ", {workspace = " + lua_string(*workspace + " silent") + "})", error)) {
      run("output remove " + output, nullptr);
      return std::nullopt;
    }
    const auto deadline = clock::now() + kLaunchWait;
    while (clock::now() < deadline) {
      std::this_thread::sleep_for(150ms);
      for (const auto &c : ask_json("clients")) {
        const auto address = c.value("address", "");
        if (before.contains(address) || !c.value("mapped", false)) continue;
        if (std::to_string(c["workspace"].value("id", 0)) != *workspace) continue;
        hosted_t h;
        h.address = address;
        h.seen = clock::now();
        hosted[output] = h;  // a launched app goes back to the focused workspace
        save();
        if (!place(address, *workspace, error)) {
          put_back(output);
          return std::nullopt;
        }
        BOOST_LOG(info) << "Frametop windows: launched " << app_id << " as " << address << " on " << output;
        return nlohmann::json {{"output", output}, {"width", width}, {"height", height}, {"address", address}};
      }
    }
    run("output remove " + output, nullptr);
    *error = app->name + " didn't open a window in time";
    return std::nullopt;
  }

  bool resize(const std::string &output, int width, int height, std::string *error) {
    std::lock_guard guard {lock};
    if (!hosted.contains(output)) {
      *error = "no window output " + output;
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
        put_back(name);
        return true;
      }
    }
    *error = "no window output for " + output_or_address;
    return false;
  }

  int release_all() {
    std::lock_guard guard {lock};
    std::vector<std::string> outputs;
    for (const auto &[output, h] : hosted) outputs.push_back(output);
    for (const auto &output : outputs) put_back(output);
    return int(outputs.size());
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
        hosted[output] = {h.value("address", ""), h.value("workspace", ""), h.value("fullscreen", 0),
                          h.value("fullscreen_client", 0), clock::now()};
      }
      if (!hosted.empty()) BOOST_LOG(info) << "Frametop windows: putting back " << hosted.size() << " left from before";
      release_all();
    }
    std::atexit(release_at_exit);
    std::thread {watch}.detach();
  }
}  // namespace platf::hyprland_windows
