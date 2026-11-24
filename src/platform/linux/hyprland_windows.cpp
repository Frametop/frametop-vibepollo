/**
 * @file src/platform/linux/hyprland_windows.cpp
 * @brief Frametop windows on a Hyprland desktop (see hyprland_windows.h).
 *
 * Everything goes through Hyprland's IPC socket: j/ queries for state, "output create|remove"
 * for the headless outputs and Lua (Hyprland 0.55+) for the rest.
 */
#include "hyprland_windows.h"
#include "app_window_class.h"
#include "desktop_apps.h"
#include "hyprland_ipc.h"
#include "window_cursor.h"
#include "window_pick.h"

#include "src/config.h"
#include "src/logging.h"
#include "src/rtsp.h"
#include "src/video.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <regex>
#include <set>
#include <sstream>
#include <thread>

#include <poll.h>
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
      // Where it was (Frametop desktopless mode puts it back there): a floating window's place and
      // size; a tiled one's middle, where the cursor goes for dwindle to split it in beside the same
      // neighbour.
      bool floating = false;
      int x = 0, y = 0, w = 0, h = 0;
      clock::time_point seen;  // last captured by a stream, or opened
      bool captured = false;  // a stream captured it at the last watch round
      bool launching = false;  // the output waits for a launched app's window (no address yet)
      std::optional<clock::time_point> refade_at;  // fullscreen again then (see refade)
    };

    std::recursive_timed_mutex lock;  // held across a whole operation: one window move at a time
    std::map<std::string, hosted_t> hosted;  // by output name
    std::map<std::string, int> others_fullscreen;  // other windows' fullscreen state at the last watch
    bool started = false;
    std::atomic<bool> stopping {false};  // process exit: the watcher leaves the windows to release_at_exit

    using namespace ipc;
    using namespace catalog;

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

    monitor_rect_t monitor_rect(const nlohmann::json &m) {
      const auto name = m.value("name", "");
      return {name, m.value("x", 0), m.value("y", 0), m.value("width", 0), m.value("height", 0), m.value("scale", 1.0),
              m.value("transform", 0), m.value("focused", false), window_output(name)};
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
                       {"fullscreen_client", h.fullscreen_client},
                       {"floating", h.floating},
                       {"at", {h.x, h.y}},
                       {"size", {h.w, h.h}}};
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

    // A workspace (as workspace_selector names it) some real monitor shows now.
    bool workspace_shown(const std::string &workspace) {
      for (const auto &m : ask_json("monitors")) {
        if (window_output(m.value("name", "")) || !m.contains("activeWorkspace")) continue;
        if (workspace_selector(m["activeWorkspace"]) == workspace) return true;
      }
      return false;
    }

    // Puts one hosted window back and removes its output. Holds the lock. No logging at exit:
    // the log may be gone by then. False (and the output stays, for the next try) when Hyprland
    // doesn't answer.
    bool put_back(const std::string &output, bool log = true, const drop_t *drop = nullptr) {
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
        auto workspace = h.workspace.empty() ? focused_workspace() : h.workspace;
        auto monitor = h.monitor;
        // Dropped onto a monitor in Frametop: onto the workspace it shows, the cursor at the drop
        // first, so a tiled window tiles there (dwindle splits the window under the cursor).
        std::optional<monitor_rect_t> onto;
        std::pair<int, int> point;
        if (drop && !window_output(drop->monitor)) {
          if (const auto m = find_monitor(drop->monitor); m && !m->value("disabled", false) && m->contains("activeWorkspace")) {
            onto = monitor_rect(*m);
            point = monitor_point(*onto, drop->u, drop->v);
            workspace = workspace_selector((*m)["activeWorkspace"]);
            monitor = drop->monitor;
            lua("hl.dispatch(hl.dsp.cursor.move({x = " + std::to_string(point.first) + ", y = " + std::to_string(point.second) + "}))",
                nullptr);
          }
        }
        // Not dropped somewhere: where it was. A tiled one splits in where its middle was, when that
        // workspace shows there (the cursor goes there for dwindle, then back).
        std::optional<std::pair<int, int>> cursor_was;
        if (!onto && !h.floating && h.w > 0 && h.h > 0 && workspace_shown(workspace)) {
          if (const auto reply = ask("j/cursorpos")) {  // an object, not query()'s array
            const auto c = nlohmann::json::parse(*reply, nullptr, false);
            if (c.is_object() && c.contains("x")) cursor_was = {c.value("x", 0), c.value("y", 0)};
          }
          lua("hl.dispatch(hl.dsp.cursor.move({x = " + std::to_string(h.x + h.w / 2) + ", y = " + std::to_string(h.y + h.h / 2) + "}))",
              nullptr);
        }
        if ((!lua("hl.dispatch(hl.dsp.window.fullscreen_state({window = " + window + ", internal = " +
                    std::to_string(h.fullscreen) + ", client = " + std::to_string(h.fullscreen_client) + "}))",
                  &error) ||
             !lua("hl.dispatch(hl.dsp.window.move({window = " + window + ", workspace = " + lua_string(workspace) +
                    ", follow = false}))",
                  &error)) &&
            log) {
          BOOST_LOG(warning) << "Frametop windows: can't put " << h.address << " back: " << error;
        }
        home_workspace(workspace, monitor, &error);
        if (!onto && h.floating && h.w > 0 && h.h > 0) {
          lua("hl.dispatch(hl.dsp.window.resize({window = " + window + ", x = " + std::to_string(h.w) + ", y = " + std::to_string(h.h) +
                "})) hl.dispatch(hl.dsp.window.move({window = " + window + ", x = " + std::to_string(h.x) + ", y = " +
                std::to_string(h.y) + "}))",
              &error);
        }
        if (cursor_was) {
          lua("hl.dispatch(hl.dsp.cursor.move({x = " + std::to_string(cursor_was->first) + ", y = " +
                std::to_string(cursor_was->second) + "}))",
              nullptr);
        }
        if (onto) {
          // A floating window goes where it was let go, held where the panel was held.
          const auto c = find_client(h.address);
          const auto size = c ? c->value("size", nlohmann::json::array({0, 0})) : nlohmann::json::array({0, 0});
          if (c && c->value("floating", false) && size.size() == 2) {
            const auto [x, y] = drop_origin(*onto, point.first, point.second, drop->fx, drop->fy, size[0].get<int>(), size[1].get<int>());
            lua("hl.dispatch(hl.dsp.window.move({window = " + window + ", x = " + std::to_string(x) + ", y = " + std::to_string(y) + "}))",
                &error);
          }
          if (log) BOOST_LOG(info) << "Frametop windows: " << h.address << " dropped onto " << monitor;
        }
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
      h.floating = client.value("floating", false);
      if (const auto at = client.value("at", nlohmann::json::array()); at.size() == 2 && at[0].is_number() && at[1].is_number())
        h.x = at[0].get<int>(), h.y = at[1].get<int>();
      if (const auto size = client.value("size", nlohmann::json::array()); size.size() == 2 && size[0].is_number() && size[1].is_number())
        h.w = size[0].get<int>(), h.h = size[1].get<int>();
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

    // A window output's stream ended (the Frame went, or its Wi-Fi did) while the cursor was on
    // it: the mouse on the PC would move a cursor nobody sees for the minute's grace.
    void cursor_off_ended(const nlohmann::json &monitors, const std::set<std::string> &ended, const std::string &home) {
      const auto reply = ask("j/cursorpos");
      if (!reply) return;
      const auto at = nlohmann::json::parse(valid_utf8(*reply), nullptr, false);
      if (at.is_discarded() || !at.is_object()) return;
      std::vector<monitor_rect_t> rects;
      for (const auto &m : monitors) {
        if (m.value("disabled", false)) continue;
        rects.push_back(monitor_rect(m));
      }
      const auto to = cursor_off(at.value("x", 0), at.value("y", 0), rects, ended, home);
      if (!to) return;
      std::string error;
      if (lua("hl.dispatch(hl.dsp.cursor.move({x = " + std::to_string(to->first) + ", y = " + std::to_string(to->second) + "}))", &error)) {
        BOOST_LOG(info) << "Frametop windows: the cursor left a window output whose stream ended";
      } else {
        BOOST_LOG(warning) << "Frametop windows: couldn't move the cursor off a window output: " << error;
      }
    }

    // Ends the streams of outputs that went. The client asks for its window again, hears "no
    // window", and its panel shows that instead of a frozen picture.
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
      std::set<std::string> uncaptured;
      std::string home;
      for (auto &[output, h] : hosted) {
        if (h.launching) continue;  // launch() owns it until its window comes
        const bool captured = video::output_is_captured(output);
        if (captured) h.seen = now;
        if (h.captured && !captured) uncaptured.insert(output), home = h.monitor;
        h.captured = captured;
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
      if (!uncaptured.empty()) cursor_off_ended(*monitors, uncaptured, home);
      for (const auto &output : done) put_back(output);
      return ended;
    }

    void end_streams(const std::vector<std::string> &outputs) {
      for (const auto &output : outputs) {
        for (const auto &client : rtsp_stream::client_uuids_capturing(output)) {
          BOOST_LOG(info) << "Frametop windows: ending the stream of " << output;
          rtsp_stream::disconnect_client_sessions_with_result(client, true);
        }
      }
    }

    // A round every kWatch, and one at once when Hyprland says a window closed or a monitor
    // went: a window closed from its panel (Super+W) showed the output's wallpaper until the
    // next round, up to kWatch later.
    void watch() {
      int events = -1;
      std::string pending;
      auto next = clock::now() + kWatch;
      while (!stopping) {
        if (events < 0) events = connect_events(), pending.clear();
        bool now = false;
        const auto wait = std::chrono::duration_cast<std::chrono::milliseconds>(next - clock::now());
        if (events >= 0) {
          pollfd p {events, POLLIN, 0};
          if (::poll(&p, 1, std::max<int>(0, int(wait.count()))) > 0) {
            char buf[4096];
            const auto n = ::read(events, buf, sizeof buf);
            if (n <= 0) {
              ::close(events);
              events = -1;
            } else {
              pending.append(buf, size_t(n));
              for (size_t end; (end = pending.find('\n')) != std::string::npos; pending.erase(0, end + 1)) {
                const auto line = std::string_view(pending).substr(0, end);
                now = now || line.starts_with("closewindow>>") || line.starts_with("monitorremoved>>");
              }
              if (pending.size() > 65536) pending.clear();  // no newline in that much: not Hyprland's
            }
          }
        } else if (wait.count() > 0) {
          std::this_thread::sleep_for(wait);
        }
        if (!now && clock::now() < next) continue;
        next = clock::now() + kWatch;
        try {
          end_streams(watch_once());
        } catch (const std::exception &e) {
          BOOST_LOG(warning) << "Frametop windows: the watcher skipped a round: " << e.what();
        }
      }
      if (events >= 0) ::close(events);
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
    std::set<std::string> shown;  // workspaces a monitor shows now (its own and an open special one)
    for (const auto &m : ask_json("monitors all")) {
      monitor_names[m.value("id", -1)] = m.value("name", "");
      if (!m.value("disabled", false) && !window_output(m.value("name", ""))) {
        for (const auto *key : {"activeWorkspace", "specialWorkspace"}) {
          if (m.contains(key) && m[key].is_object() && !m[key].value("name", "").empty()) shown.insert(m[key].value("name", ""));
        }
      }
    }
    const auto &app_of = app_classes();
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
                        {"output", nullptr},
                        // For Frametop desktopless mode: which to bring out first (shown, then the
                        // most recently focused, 0 = focused now), where it sits and whose app it is.
                        {"focus", c.value("focusHistoryID", -1)},
                        {"floating", c.value("floating", false)},
                        {"shown", false},
                        {"at", c.value("at", nlohmann::json::array({0, 0}))},
                        {"app", nullptr}};
      const auto ws = c.contains("workspace") ? c["workspace"].value("name", "") : "";
      w["shown"] = shown.contains(ws);
      for (const auto *key : {"class", "initialClass"}) {
        if (const auto it = app_of.find(window_class_key(c.value(key, ""))); it != app_of.end()) {
          w["app"] = it->second;
          break;
        }
      }
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
    // Its window: a new one on the output's workspace, or else a new one of the app's class
    // anywhere (a browser that was running opens it where you are; place() brings it over).
    const auto classes = app_window_classes(app->wm_class, app->id, app->exec);
    std::string address, found_by;
    guard.unlock();
    while (address.empty() && clock::now() < deadline) {
      std::this_thread::sleep_for(150ms);
      std::string by_class;
      for (const auto &c : ask_json("clients")) {
        const auto a = c.value("address", "");
        if (before.contains(a) || !c.value("mapped", false)) continue;
        if (std::to_string(c["workspace"].value("id", 0)) == *workspace) {
          address = a;
          break;
        }
        if (by_class.empty() && (classes.contains(window_class_key(c.value("class", ""))) ||
                                 classes.contains(window_class_key(c.value("initialClass", ""))))) {
          by_class = a;
        }
      }
      if (address.empty() && !by_class.empty()) address = by_class, found_by = " (by its class: it opened elsewhere)";
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
    BOOST_LOG(info) << "Frametop windows: launched " << app_id << " as " << address << " on " << output << found_by;
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

  bool release(const std::string &output_or_address, std::string *error, const drop_t *drop) {
    std::lock_guard guard {lock};
    for (const auto &[output, h] : hosted) {
      if (output == output_or_address || h.address == output_or_address) {
        const auto name = output;  // put_back erases the entry
        if (put_back(name, true, drop)) return true;
        *error = "Hyprland didn't answer: " + name + " stays out for now";
        return false;
      }
    }
    *error = "no window output for " + output_or_address;
    return false;
  }

  std::optional<nlohmann::json> at(const std::string &monitor, double u, double v, std::string *error) {
    // As on Windows: a point off the monitor (or none: the route sends -1) is an error, not its edge.
    if (!(u >= 0 && u <= 1 && v >= 0 && v <= 1)) {
      *error = "x and y must be 0 to 1";
      return std::nullopt;
    }
    std::lock_guard guard {lock};
    std::optional<nlohmann::json> m;
    if (!window_output(monitor)) m = find_monitor(monitor);
    if (!m || m->value("disabled", false)) {
      *error = "no monitor " + monitor;
      return std::nullopt;
    }
    const auto [x, y] = monitor_point(monitor_rect(*m), u, v);
    std::vector<int> shown;
    if (const int special = m->contains("specialWorkspace") ? (*m)["specialWorkspace"].value("id", 0) : 0) shown.push_back(special);
    if (m->contains("activeWorkspace")) shown.push_back((*m)["activeWorkspace"].value("id", 0));
    std::vector<client_rect_t> rects;
    std::map<std::string, nlohmann::json> by_address;
    for (const auto &c : ask_json("clients")) {
      if (!c.value("mapped", false) || c.value("hidden", false)) continue;
      const auto pos = c.value("at", nlohmann::json::array({0, 0})), size = c.value("size", nlohmann::json::array({0, 0}));
      if (pos.size() != 2 || size.size() != 2 || !c.contains("workspace")) continue;
      const auto address = c.value("address", "");
      rects.push_back({address, pos[0].get<int>(), pos[1].get<int>(), size[0].get<int>(), size[1].get<int>(),
                       c["workspace"].value("id", 0), c.value("floating", false), c.value("fullscreen", 0) == 2,
                       c.value("focusHistoryID", 0)});
      by_address[address] = c;
    }
    const auto *hit = window_at(rects, shown, x, y);
    if (!hit) {
      *error = "no window there";
      return std::nullopt;
    }
    const auto &c = by_address[hit->address];
    return nlohmann::json {{"address", hit->address},
                           {"class", c.value("class", "")},
                           {"title", c.value("title", "")},
                           {"width", hit->width},
                           {"height", hit->height},
                           {"grab", {double(x - hit->x) / hit->width, double(y - hit->y) / hit->height}}};
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
        back.floating = h.value("floating", false);
        if (const auto at = h.value("at", nlohmann::json::array()); at.size() == 2 && at[0].is_number() && at[1].is_number())
          back.x = at[0].get<int>(), back.y = at[1].get<int>();
        if (const auto size = h.value("size", nlohmann::json::array()); size.size() == 2 && size[0].is_number() && size[1].is_number())
          back.w = size[0].get<int>(), back.h = size[1].get<int>();
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
