/**
 * @file src/frametop_monitors_service.cpp
 * @brief The host's one Frametop monitor plan (see the header).
 */
#include "frametop_monitors_service.h"

#include "src/config.h"
#include "src/logging.h"
#include "src/state_storage.h"

#include <boost/property_tree/ptree.hpp>  // state_storage overloads on it

#include <chrono>
#include <filesystem>
#include <thread>

namespace frametop_monitors::service {
  namespace {
    using namespace std::literals;

    class file_store_t final: public store_t {
    public:
      std::optional<nlohmann::json> load() override {
        nlohmann::json tree;
        if (statefile::load_json(path(), tree) != statefile::json_load_result_e::loaded || !tree.is_object()) return std::nullopt;
        return tree;
      }

      void save(const nlohmann::json &state) override {
        try {
          statefile::write_json_atomic(path(), state);
        } catch (const std::exception &e) {
          BOOST_LOG(warning) << "Frametop monitors: couldn't save " << path() << ": " << e.what();
        }
      }

    private:
      static std::string path() {
        // Beside this instance's own state (as frametop_windows.json): two instances of one user
        // share the folder of vibeshine_state but not their plans.
        const std::filesystem::path state = config::nvhttp.file_state;
        return ((state.has_parent_path() ? state.parent_path() : std::filesystem::path {"."}) / "frametop_monitors.json").string();
      }
    };

    // Each wait is its own sleeping thread: a few a day, and the manager ignores stale ones.
    void schedule(std::chrono::milliseconds after, std::function<void()> run) {
      std::thread([after, run = std::move(run)] {
        std::this_thread::sleep_for(after);
        run();
      }).detach();
    }

    void log(const std::string &line) {
      BOOST_LOG(info) << "Frametop monitors: " << line;
    }

    struct service_t {
      std::unique_ptr<platform_t> platform;
      file_store_t store;
      std::unique_ptr<manager_t> manager;
    };

    // Never destroyed: holds and timers may outlive static destruction at exit.
    service_t *instance() {
      static service_t *const service = [] {
        auto *s = new service_t;
        s->platform = platf::make_frametop_monitors();
        if (s->platform) s->manager = std::make_unique<manager_t>(*s->platform, s->store, schedule, manager_t::default_grace, log);
        return s;
      }();
      return service;
    }
  }  // namespace

  void start() {
    auto *manager = instance()->manager.get();
    if (!manager || manager->start()) return;
    // Started before anyone logged in (after a crash, then a sign-out): keep trying a while.
    std::thread([manager] {
      const auto deadline = std::chrono::steady_clock::now() + 30min;
      while (std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(15s);
        if (manager->start()) return;
      }
      BOOST_LOG(warning) << "Frametop monitors: couldn't put the monitors back as they were; giving up.";
    }).detach();
  }

  std::shared_ptr<void> hold() {
    auto *manager = instance()->manager.get();
    return manager ? manager->hold() : nullptr;
  }

  nlohmann::json state() {
    auto *manager = instance()->manager.get();
    if (!manager) return {{"status", false}, {"error", "This host can't turn its monitors on or off."}};
    auto out = manager->state();
    out["status"] = true;
    return out;
  }

  std::string set_plan(const nlohmann::json &body) {
    auto *manager = instance()->manager.get();
    if (!manager) return "This host can't turn its monitors on or off.";
    return manager->set_plan(body);
  }
}  // namespace frametop_monitors::service

#if !defined(_WIN32) && !defined(__linux__)
namespace platf {
  std::unique_ptr<frametop_monitors::platform_t> make_frametop_monitors() {
    return nullptr;
  }
}  // namespace platf
#endif
