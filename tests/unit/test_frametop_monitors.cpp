#include "../tests_common.h"

#include "src/frametop_monitors.h"
#include "src/frametop_monitors_layout.h"

#include <algorithm>
#include <string>
#include <vector>

namespace {
  using namespace frametop_monitors;
  using json = nlohmann::json;

  const video_mode_t hi {2560, 1600, 240000};
  const video_mode_t lo {2560, 1600, 60000};

  monitor_t monitor(const std::string &id, bool on, bool primary = false) {
    monitor_t m;
    m.id = id;
    m.name = id + " name";
    m.on = on;
    m.primary = primary;
    m.modes = {hi, lo};
    if (on) m.mode = hi;
    m.place = {{"x", primary ? 0 : 2560}, {"y", 0}};
    return m;
  }

  // A desktop that does what it's told, and can be told no.
  struct fake_platform_t: platform_t {
    inventory_t now;
    bool readable = true;
    std::string refuse;
    std::vector<std::vector<target_t>> applied;

    std::string backend() const override {
      return "fake";
    }

    std::optional<inventory_t> inventory() override {
      if (!readable) return std::nullopt;
      return now;
    }

    monitor_t &get(const std::string &id) {
      return *std::find_if(now.monitors.begin(), now.monitors.end(), [&](const monitor_t &m) {
        return m.id == id;
      });
    }

    std::string apply(const std::vector<target_t> &targets, const inventory_t &) override {
      applied.push_back(targets);
      if (!refuse.empty()) return refuse;
      for (const auto &t : targets) {
        auto &m = get(t.id);
        m.on = t.on;
        m.mode = t.on ? (t.mode ? t.mode : m.mode ? m.mode : std::optional<video_mode_t> {hi}) : std::nullopt;
        if (t.on && !t.place.empty()) m.place = t.place;
        if (!t.on) m.place = json::object();
      }
      return {};
    }
  };

  struct fake_store_t: store_t {
    std::optional<json> state;

    std::optional<json> load() override {
      return state;
    }

    void save(const json &value) override {
      state = value;
    }
  };

  struct timers_t {
    std::vector<std::pair<std::chrono::milliseconds, std::function<void()>>> pending;

    manager_t::schedule_fn fn() {
      return [this](std::chrono::milliseconds after, std::function<void()> run) {
        pending.emplace_back(after, std::move(run));
      };
    }

    void run_all() {
      auto now = std::move(pending);
      pending.clear();
      for (auto &[after, run] : now) run();
    }
  };

  json off(const std::string &id) {
    return {{"monitors", {{id, {{"on", false}}}}}};
  }

  struct FrametopMonitors: ::testing::Test {
    fake_platform_t platform;
    fake_store_t store;
    timers_t timers;

    void SetUp() override {
      platform.now.monitors = {monitor("A", true, true), monitor("B", true)};
    }

    manager_t make() {
      return manager_t(platform, store, timers.fn());
    }
  };
}  // namespace

TEST(FrametopMonitorsModes, SameModeWithinTenMillihertz) {
  EXPECT_TRUE(same_mode({2560, 1600, 239761}, {2560, 1600, 239760})) << "Hyprland's 239.761 listed as 239.76";
  EXPECT_FALSE(same_mode({1920, 1080, 59940}, {1920, 1080, 60000})) << "59.94 and 60 Hz are two modes";
  EXPECT_FALSE(same_mode({1920, 1200, 60000}, {1920, 1080, 60000}));
}

TEST(FrametopMonitorsPlan, ParsesAndRejects) {
  std::string error;
  auto plan = plan_from_json(json::parse(R"({"A":{"on":true,"mode":{"width":2560,"height":1600,"refresh_mhz":60000}},"B":{"on":false,"mode":{"width":1,"height":1,"refresh_mhz":1}}})"), &error);
  ASSERT_TRUE(plan);
  EXPECT_TRUE(plan->at("A").mode);
  EXPECT_FALSE(plan->at("B").mode) << "an off monitor has no mode";
  EXPECT_FALSE(plan_from_json(json::parse(R"({"A":{"on":"yes"}})"), &error));
  EXPECT_FALSE(plan_from_json(json::parse(R"({"A":{"on":true,"mode":{"width":-1,"height":2,"refresh_mhz":3}}})"), &error));
  EXPECT_FALSE(plan_from_json(json::array(), &error));
  EXPECT_EQ(to_json(*plan_from_json(to_json(*plan), &error)), to_json(*plan));
}

TEST(FrametopMonitorsPlan, ValidateNeedsKnownMonitorsModesAndOneOn) {
  inventory_t now {{monitor("A", true, true), monitor("B", true)}, 0};
  EXPECT_EQ(validate({{"A", {false, {}}}}, now), "");
  EXPECT_NE(validate({{"C", {false, {}}}}, now), "");
  EXPECT_NE(validate({{"A", {true, video_mode_t {1920, 1080, 60000}}}}, now), "");
  EXPECT_EQ(validate({{"A", {true, video_mode_t {2560, 1600, 60005}}}}, now), "");
  EXPECT_NE(validate({{"A", {true, video_mode_t {2560, 1600, 59940}}}}, now), "");
  EXPECT_NE(validate({{"A", {false, {}}}, {"B", {false, {}}}}, now), "");
  now.other_outputs = 1;  // a virtual display goes with its stream: it doesn't count
  EXPECT_NE(validate({{"A", {false, {}}}, {"B", {false, {}}}}, now), "");
  // B goes back off as it was saved, so A off would leave none.
  EXPECT_NE(validate({{"A", {false, {}}}}, now, {monitor("B", false)}), "");
  EXPECT_EQ(validate({{"A", {false, {}}}}, now, {monitor("B", true)}), "");
}

TEST(FrametopMonitorsPlan, KeepsThePrimaryOnWhenEverythingWouldGo) {
  inventory_t now {{monitor("A", true, true), monitor("B", true)}, 0};
  std::string kept;
  const auto targets = plan_targets({{"A", {false, {}}}, {"B", {false, {}}}}, now, {}, &kept);
  ASSERT_EQ(targets.size(), 1u);
  EXPECT_EQ(targets[0].id, "B");
  EXPECT_EQ(kept, "A name");
}

TEST_F(FrametopMonitors, NothingChangesWithoutAStream) {
  auto manager = make();
  EXPECT_EQ(manager.set_plan(off("B")), "");
  EXPECT_TRUE(platform.applied.empty());
  EXPECT_TRUE(platform.get("B").on);
  EXPECT_FALSE(manager.state()["applies"].get<bool>());
}

TEST_F(FrametopMonitors, AppliesWhileHeldAndPutsBackAfterTheGrace) {
  auto manager = make();
  ASSERT_EQ(manager.set_plan({{"monitors", {{"A", {{"on", true}, {"mode", to_json(lo)}}}, {"B", {{"on", false}}}}}}), "");
  auto lease = manager.hold();
  EXPECT_FALSE(platform.get("B").on);
  EXPECT_TRUE(same_mode(*platform.get("A").mode, lo));
  ASSERT_TRUE(store.state);
  EXPECT_EQ((*store.state)["saved"].size(), 2u);

  lease.reset();
  EXPECT_FALSE(platform.get("B").on) << "nothing goes back before the grace";
  ASSERT_EQ(timers.pending.size(), 1u);
  EXPECT_EQ(timers.pending[0].first, manager_t::default_grace);
  timers.run_all();
  EXPECT_TRUE(platform.get("B").on);
  EXPECT_EQ(platform.get("B").place, (json {{"x", 2560}, {"y", 0}}));
  EXPECT_TRUE(same_mode(*platform.get("A").mode, hi));
  EXPECT_TRUE((*store.state)["set"].empty());
}

TEST_F(FrametopMonitors, AStreamWithinTheGraceKeepsThePlan) {
  auto manager = make();
  manager.set_plan(off("B"));
  auto lease = manager.hold();
  lease.reset();
  auto again = manager.hold();
  timers.run_all();  // the first grace, now stale
  EXPECT_FALSE(platform.get("B").on);
  again.reset();
  timers.run_all();
  EXPECT_TRUE(platform.get("B").on);
}

TEST_F(FrametopMonitors, APlanChangeWhileHeldAppliesAndUndoesDroppedMonitors) {
  auto manager = make();
  manager.set_plan(off("B"));
  auto lease = manager.hold();
  EXPECT_FALSE(platform.get("B").on);
  ASSERT_EQ(manager.set_plan({{"monitors", json::object()}}), "");
  EXPECT_TRUE(platform.get("B").on) << "a monitor the plan no longer names goes back";
  EXPECT_TRUE((*store.state)["set"].empty());
}

TEST_F(FrametopMonitors, SomeoneElsesChangeIsLeftAlone) {
  auto manager = make();
  manager.set_plan({{"monitors", {{"A", {{"on", true}, {"mode", to_json(lo)}}}}}});
  auto lease = manager.hold();
  platform.get("A").mode = video_mode_t {2560, 1600, 120000};  // changed by hand since
  lease.reset();
  timers.run_all();
  EXPECT_EQ(platform.get("A").mode->refresh_mhz, 120000u);
}

TEST_F(FrametopMonitors, ARestartPutsBackWhatACrashLeft) {
  {
    auto manager = make();
    manager.set_plan(off("B"));
    auto lease = manager.hold();
    EXPECT_FALSE(platform.get("B").on);
    // the process dies here: the lease is never released
    new std::shared_ptr<void>(lease);
  }
  auto manager = make();
  EXPECT_TRUE(manager.start());
  EXPECT_TRUE(platform.get("B").on);
  EXPECT_TRUE((*store.state)["set"].empty());
  EXPECT_EQ((*store.state)["plan"].size(), 1u) << "the plan itself stays for the next stream";
}

TEST_F(FrametopMonitors, ARefusedRestoreIsTriedAgain) {
  auto manager = make();
  manager.set_plan(off("B"));
  auto lease = manager.hold();
  lease.reset();
  platform.refuse = "busy";
  timers.run_all();
  EXPECT_FALSE(platform.get("B").on);
  ASSERT_EQ(timers.pending.size(), 1u);
  platform.refuse.clear();
  timers.run_all();
  EXPECT_TRUE(platform.get("B").on);
}

TEST_F(FrametopMonitors, AFailedChangeLeavesNothingToPutBack) {
  auto manager = make();
  manager.set_plan(off("B"));
  platform.refuse = "no";
  auto lease = manager.hold();
  EXPECT_TRUE((*store.state)["set"].empty());
  EXPECT_EQ(manager.state()["error"], "no");
}

TEST_F(FrametopMonitors, StartWhenItCantReadTriesAgainLater) {
  {
    auto manager = make();
    manager.set_plan(off("B"));
    new std::shared_ptr<void>(manager.hold());
  }
  platform.readable = false;
  auto manager = make();
  EXPECT_FALSE(manager.start());
  platform.readable = true;
  EXPECT_TRUE(manager.start());
  EXPECT_TRUE(platform.get("B").on);
}

TEST_F(FrametopMonitors, AStreamDuringStartRetriesKeepsThePlan) {
  {
    auto manager = make();
    manager.set_plan(off("B"));
    new std::shared_ptr<void>(manager.hold());
  }
  platform.readable = false;
  auto manager = make();
  EXPECT_FALSE(manager.start());
  platform.readable = true;
  auto lease = manager.hold();  // a stream came before the retry
  EXPECT_FALSE(platform.get("B").on);
  EXPECT_TRUE(manager.start()) << "the retry is done: the stream owns the change now";
  EXPECT_FALSE(platform.get("B").on);
}

TEST_F(FrametopMonitors, StateShowsTheWish) {
  auto manager = make();
  manager.set_plan(off("B"));
  const auto state = manager.state();
  EXPECT_EQ(state["backend"], "fake");
  ASSERT_EQ(state["monitors"].size(), 2u);
  EXPECT_TRUE(state["monitors"][0]["wish"].is_null());
  EXPECT_FALSE(state["monitors"][1]["wish"]["on"].get<bool>());
  EXPECT_FALSE(state["monitors"][1].contains("place"));
}

TEST_F(FrametopMonitors, AMonitorGoingBackOffCountsAgainstTheLastOneOn) {
  platform.now.monitors = {monitor("A", true, true), monitor("B", false)};
  auto manager = make();
  ASSERT_EQ(manager.set_plan({{"monitors", {{"B", {{"on", true}}}}}}), "");
  auto lease = manager.hold();
  ASSERT_TRUE(platform.get("B").on);
  EXPECT_NE(manager.set_plan(off("A")), "") << "B would go back off with A off";
  EXPECT_TRUE(platform.get("A").on);
}

TEST_F(FrametopMonitors, AStreamBetweenStartRetriesStillGetsItsGrace) {
  {
    auto manager = make();
    manager.set_plan(off("B"));
    new std::shared_ptr<void>(manager.hold());
  }
  platform.readable = false;
  auto manager = make();
  EXPECT_FALSE(manager.start());
  platform.readable = true;
  manager.hold().reset();  // a stream came and went before the retry
  EXPECT_TRUE(manager.start());
  EXPECT_FALSE(platform.get("B").on) << "the retry doesn't cut the grace short";
  timers.run_all();
  EXPECT_TRUE(platform.get("B").on);
}

TEST_F(FrametopMonitors, ADisplayStreamHoldsOnlyForAMonitorItCanHave) {
  platform.now.monitors = {monitor("A", true, true), monitor("B", true), monitor("C", false)};
  auto manager = make();
  using use = manager_t::display_use_e;
  EXPECT_EQ(manager.display_use("B"), use::hold_after) << "no plan for it";
  ASSERT_EQ(manager.set_plan({{"monitors", {{"B", {{"on", false}}}, {"C", {{"on", true}}}}}}), "");
  EXPECT_EQ(manager.display_use("b"), use::refuse) << "the plan turns it off (ids match in any case)";
  EXPECT_EQ(manager.display_use("C"), use::hold_first) << "the plan turns it on: hold before the lookup";
  EXPECT_EQ(manager.display_use("A"), use::hold_after);
  EXPECT_EQ(manager.display_use("HDMI-9"), use::hold_after) << "not one of its monitors";
  platform.readable = false;
  EXPECT_EQ(manager.display_use("B"), use::hold_after);
}

TEST_F(FrametopMonitors, ADisplayStreamOfTheMonitorKeptOnIsTaken) {
  auto manager = make();
  ASSERT_EQ(manager.set_plan(off("B")), "");
  platform.now.monitors.erase(platform.now.monitors.begin());  // A unplugged: B is the last one
  using use = manager_t::display_use_e;
  EXPECT_EQ(manager.display_use("B"), use::hold_after) << "the desktop needs it, so it stays on";
  auto lease = manager.hold();
  EXPECT_TRUE(platform.get("B").on);
}

TEST_F(FrametopMonitors, ACrashDuringAChangePutsBackWhatItGotTo) {
  const auto journal = [&](const json &pending) {
    store.state = json {{"version", 1}, {"plan", {{"A", {{"on", true}, {"mode", to_json(lo)}}}}},
                        {"saved", {to_json(monitor("A", true, true)), to_json(monitor("B", true))}},
                        {"set", {{"A", {{"on", true}, {"mode", to_json(lo)}}}}}, {"pending", pending}};
  };
  const video_mode_t mid {2560, 1600, 120000};
  // A was at 60 Hz and the change to 120 never happened: A still goes back.
  journal({{"A", {{"on", true}, {"mode", to_json(mid)}}}});
  platform.get("A").mode = lo;
  EXPECT_TRUE(make().start());
  EXPECT_TRUE(same_mode(*platform.get("A").mode, hi));
  // The change did happen: A goes back from 120 Hz too.
  journal({{"A", {{"on", true}, {"mode", to_json(mid)}}}});
  platform.get("A").mode = mid;
  EXPECT_TRUE(make().start());
  EXPECT_TRUE(same_mode(*platform.get("A").mode, hi));
  // B's first change got made before the crash: it comes back on.
  journal({{"B", {{"on", false}, {"mode", nullptr}}}});
  (*store.state)["set"] = json::object();
  platform.get("B").on = false;
  EXPECT_TRUE(make().start());
  EXPECT_TRUE(platform.get("B").on);
  EXPECT_TRUE((*store.state)["pending"].empty());
}

TEST_F(FrametopMonitors, MonitorsGoingBackAndThePlanStillLeaveOneOn) {
  // A run before turned B on (it was off), then the plan became "A off" with C on; C is gone now.
  auto b_off = monitor("B", false);
  store.state = json {{"version", 1}, {"plan", {{"A", {{"on", false}, {"mode", nullptr}}}}},
                      {"saved", {to_json(b_off)}}, {"set", {{"B", {{"on", true}, {"mode", nullptr}}}}}};
  platform.readable = false;
  auto manager = make();
  EXPECT_FALSE(manager.start());
  platform.readable = true;
  auto lease = manager.hold();
  EXPECT_TRUE(platform.get("A").on) << "B goes back off, so A stays on";
  EXPECT_FALSE(platform.get("B").on);
  EXPECT_EQ(manager.state()["error"], "A name stays on: the desktop needs one display");
}

TEST_F(FrametopMonitors, StateSaysWhichMonitorIsSideways) {
  platform.get("B").sideways = true;
  auto manager = make();
  const auto state = manager.state();
  EXPECT_FALSE(state["monitors"][0]["sideways"].get<bool>());
  EXPECT_TRUE(state["monitors"][1]["sideways"].get<bool>());
  ASSERT_EQ(manager.set_plan(off("B")), "");
  auto lease = manager.hold();
  platform.get("B").sideways = false;  // off, its rotation isn't listed
  EXPECT_TRUE(manager.state()["monitors"][1]["sideways"].get<bool>()) << "as it was saved";
}

TEST(FrametopMonitorsLayout, AMonitorBackOnGoesWhereItWas) {
  const layout_t before {{{"A", {0, 0}}, {"C", {-1920, 0}}}, "A"};
  const auto wanted = layout_to_put_back(before, {{"B", true, std::nullopt, {{"x", 2560}, {"y", 0}, {"primary", false}}},
                                                  {"C", false, std::nullopt, json::object()}});
  EXPECT_EQ(wanted.primary, "A");
  ASSERT_EQ(wanted.places.size(), 3u);
  EXPECT_EQ(wanted.places.at("B").x, 2560);
  EXPECT_EQ(wanted.places.at("C").x, -1920) << "an off target's place is left for Windows to drop";
}

TEST(FrametopMonitorsLayout, TheMainDisplayBackOnPutsNoPlacesBack) {
  // A was the main display; with it off, Windows made B the main one at 0,0.
  const layout_t before {{{"B", {0, 0}}}, "B"};
  const auto wanted = layout_to_put_back(before, {{"A", true, std::nullopt, {{"x", 0}, {"y", 0}, {"primary", true}}}});
  EXPECT_EQ(wanted.primary, "A");
  EXPECT_TRUE(wanted.places.empty()) << "B's 0,0 was noted around B: putting it back would cover A";
}
