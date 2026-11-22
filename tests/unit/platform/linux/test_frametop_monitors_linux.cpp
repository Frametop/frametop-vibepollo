/**
 * @file tests/unit/platform/linux/test_frametop_monitors_linux.cpp
 * @brief Frametop's monitor plan on Hyprland and KDE Plasma: what is read from each and what is
 *        sent to it.
 */
#include "../../../tests_common.h"

#include "src/platform/linux/frametop_monitors_hyprland.h"
#include "src/platform/linux/frametop_monitors_kscreen.h"

#include <string>
#include <vector>

namespace {
  using json = nlohmann::json;
  using frametop_monitors::target_t;
  using frametop_monitors::video_mode_t;
  namespace hypr = platf::frametop_monitors_hyprland;
  namespace kscreen = platf::frametop_monitors_kscreen;

  bool never_virtual(const std::string &) {
    return false;
  }

  json hypr_monitors() {
    return json::parse(R"([
      {"name": "eDP-2", "description": "BOE laptop panel", "width": 2560, "height": 1600, "refreshRate": 240.0,
       "x": 1536, "y": 2880, "scale": 1.25, "transform": 0, "disabled": false,
       "availableModes": ["2560x1600@240.00Hz", "2560x1600@60.00Hz", "1920x1200@59.95Hz", "bogus"]},
      {"name": "DP-1", "description": "", "width": 1920, "height": 1080, "refreshRate": 60.0,
       "x": 0, "y": 0, "scale": 1.0, "transform": 1, "disabled": true,
       "availableModes": ["1920x1080@60.00Hz", "1920x1080@144.00Hz"]},
      {"name": "HEADLESS-2", "width": 1280, "height": 720, "refreshRate": 60.0, "disabled": false},
      {"name": "FT-W1", "width": 1280, "height": 720, "refreshRate": 60.0, "disabled": false},
      {"name": "VB-1", "width": 1280, "height": 720, "refreshRate": 60.0, "disabled": false}
    ])");
  }
}  // namespace

TEST(FrametopMonitorsHyprland, ListsTheRealMonitorsAndCountsTheRest) {
  const auto now = hypr::parse_monitors(hypr_monitors(), [](const std::string &name) {
    return name == "VB-1";
  });
  ASSERT_TRUE(now);
  ASSERT_EQ(now->monitors.size(), 2u);
  EXPECT_EQ(now->other_outputs, 3);

  const auto &laptop = now->monitors[0];
  EXPECT_EQ(laptop.id, "eDP-2");
  EXPECT_EQ(laptop.name, "BOE laptop panel");
  EXPECT_TRUE(laptop.on);
  ASSERT_TRUE(laptop.mode);
  EXPECT_EQ(laptop.mode->refresh_mhz, 240000u);
  ASSERT_EQ(laptop.modes.size(), 3u);
  EXPECT_EQ(laptop.modes[2].refresh_mhz, 59950u);
  EXPECT_EQ(laptop.place["x"], 1536);
  EXPECT_EQ(laptop.place["scale"], 1.25);

  const auto &off = now->monitors[1];
  EXPECT_EQ(off.name, "DP-1");  // no description: its name
  EXPECT_FALSE(off.on);
  EXPECT_FALSE(off.mode);  // a disabled output's fields are stale
  EXPECT_TRUE(off.place.empty());
  EXPECT_EQ(off.modes.size(), 2u);  // still listed, to pick one before it comes on
  EXPECT_FALSE(laptop.sideways);
  EXPECT_TRUE(laptop.any_mode) << "Hyprland makes a custom mode for a size it doesn't list";
}

TEST(FrametopMonitorsHyprland, ListsTheScalesItsModeTakes) {
  const auto now = hypr::parse_monitors(hypr_monitors(), never_virtual);
  ASSERT_TRUE(now);
  const auto &laptop = now->monitors[0];
  ASSERT_TRUE(laptop.scale);
  EXPECT_DOUBLE_EQ(*laptop.scale, 1.25);
  EXPECT_EQ(laptop.scales, (std::vector<double> {1.0, 1.25, 1.6, 2.0, 2.5})) << "whole-pixel sizes of 2560x1600";
  EXPECT_TRUE(now->monitors[1].scales.empty()) << "off: its scale comes with it";
}

TEST(FrametopMonitorsHyprland, APickedScaleFitsTheMode) {
  const auto now = hypr::parse_monitors(hypr_monitors(), never_virtual);
  ASSERT_TRUE(now);
  target_t t;
  t.id = "eDP-2";
  t.on = true;
  t.scale = 2.0;
  EXPECT_EQ(hypr::rule_for(t, *now).scale, "2");
  t.scale = 1.5;  // 1706.67 px wide: Hyprland would swap it
  EXPECT_EQ(hypr::rule_for(t, *now).scale, "1.6");
  t.mode = video_mode_t {1920, 1200, 59950};
  EXPECT_EQ(hypr::rule_for(t, *now).scale, "1.5") << "1920x1200 takes 1.5";
}

TEST(FrametopMonitorsHyprland, AQuarterTurnIsSideways) {
  auto listed = hypr_monitors();
  listed[0]["transform"] = 3;
  const auto now = hypr::parse_monitors(listed, never_virtual);
  ASSERT_TRUE(now);
  EXPECT_TRUE(now->monitors[0].sideways);
}

TEST(FrametopMonitorsHyprland, RefusesWhatIsNotItsList) {
  EXPECT_FALSE(hypr::parse_monitors(json::object(), never_virtual));
  EXPECT_FALSE(hypr::parse_monitors(json::parse(R"([{"id": 1}])"), never_virtual));
}

TEST(FrametopMonitorsHyprland, RuleKeepsThePlaceAndSaysDisabledFalse) {
  const auto now = hypr::parse_monitors(hypr_monitors(), never_virtual);
  ASSERT_TRUE(now);
  target_t t;
  t.id = "eDP-2";
  t.on = true;
  t.mode = video_mode_t {2560, 1600, 60000};
  const auto rule = hypr::rule_for(t, *now);
  EXPECT_EQ(rule.mode, "2560x1600@60");
  EXPECT_EQ(rule.position, "1536x2880");
  EXPECT_EQ(rule.scale, "1.25");
  EXPECT_EQ(hypr::lua_rule(rule),
            "hl.monitor({output = \"eDP-2\", mode = \"2560x1600@60\", position = \"1536x2880\", scale = 1.25, "
            "transform = 0, disabled = false})");
}

TEST(FrametopMonitorsHyprland, OnAgainTakesTheSavedPlace) {
  const auto now = hypr::parse_monitors(hypr_monitors(), never_virtual);
  ASSERT_TRUE(now);
  target_t t;
  t.id = "DP-1";
  t.on = true;
  t.mode = video_mode_t {1920, 1080, 59950};
  t.place = {{"x", -1920}, {"y", 0}, {"scale", 1.0}, {"transform", 1}};
  const auto rule = hypr::rule_for(t, *now);
  EXPECT_EQ(rule.mode, "1920x1080@59.95");
  EXPECT_EQ(rule.position, "-1920x0");
  EXPECT_EQ(rule.scale, "1");
  EXPECT_EQ(rule.transform, 1);

  target_t fresh;
  fresh.id = "DP-1";
  fresh.on = true;  // no mode, no place: Hyprland picks
  const auto chosen = hypr::rule_for(fresh, *now);
  EXPECT_EQ(chosen.mode, "preferred");
  EXPECT_EQ(chosen.position, "auto");
  EXPECT_EQ(hypr::lua_rule(chosen),
            "hl.monitor({output = \"DP-1\", mode = \"preferred\", position = \"auto\", scale = \"auto\", "
            "transform = 0, disabled = false})");
}

TEST(FrametopMonitorsHyprland, OffRuleAndUnsafeWords) {
  hypr::rule_t off;
  off.output = "DP-1";
  off.on = false;
  EXPECT_EQ(hypr::lua_rule(off), "hl.monitor({output = \"DP-1\", disabled = true})");

  hypr::rule_t bad = off;
  bad.output = "DP-1\"}) os.execute(\"x";
  EXPECT_EQ(hypr::lua_rule(bad), "");
  bad.output = "-DP";
  EXPECT_FALSE(hypr::rule_is_safe(bad));

  hypr::rule_t on;
  on.output = "DP-1";
  on.mode = "1920x1080@60\"";
  EXPECT_FALSE(hypr::rule_is_safe(on));
  on.mode = "1920x1080@60.0001";
  EXPECT_FALSE(hypr::rule_is_safe(on));
  on.mode = "1920x1080@60";
  on.position = "1x2x3";
  EXPECT_FALSE(hypr::rule_is_safe(on));
  on.position = "auto";
  on.scale = "0";
  EXPECT_FALSE(hypr::rule_is_safe(on));
  on.scale = "1.5";
  on.transform = 8;
  EXPECT_FALSE(hypr::rule_is_safe(on));
  on.transform = 7;
  EXPECT_TRUE(hypr::rule_is_safe(on));
}

namespace {
  // Hyprland that takes each rule and shows it at once.
  struct fake_hypr_t: hypr::link_t {
    json monitors_now = hypr_monitors();
    std::vector<std::string> rules;

    std::optional<json> monitors() override {
      return monitors_now;
    }

    std::string apply(const hypr::rule_t &rule) override {
      rules.push_back(hypr::lua_rule(rule));
      for (auto &m : monitors_now) {
        if (m["name"] == rule.output) m["disabled"] = !rule.on;
      }
      return {};
    }
  };
}  // namespace

TEST(FrametopMonitorsHyprland, TurnsOnBeforeOff) {
  auto link = std::make_unique<fake_hypr_t>();
  auto *fake = link.get();
  hypr::platform_t platform {std::move(link), never_virtual};
  const auto now = platform.inventory();
  ASSERT_TRUE(now);
  target_t off {"eDP-2", false, std::nullopt, json::object()};
  target_t on {"DP-1", true, std::nullopt, json::object()};
  EXPECT_EQ(platform.apply({off, on}, *now), "");
  ASSERT_EQ(fake->rules.size(), 2u);
  EXPECT_NE(fake->rules[0].find("\"DP-1\""), std::string::npos);
  EXPECT_NE(fake->rules[1].find("disabled = true"), std::string::npos);
}

namespace {
  json kscreen_config() {
    return json::parse(R"({"outputs": [
      {"name": "DP-1", "connected": true, "enabled": true, "priority": 1, "currentModeId": "3",
       "pos": {"x": 0, "y": 0}, "scale": 1.5, "rotation": 1,
       "modes": [{"id": "3", "size": {"width": 3840, "height": 2160}, "refreshRate": 59.997},
                 {"id": "4", "size": {"width": 3840, "height": 2160}, "refreshRate": 30.0},
                 {"id": "5", "size": {"width": 1920, "height": 1080}, "refreshRate": 60.0}]},
      {"name": "HDMI-A-1", "connected": true, "enabled": false, "priority": 0,
       "modes": [{"id": "1", "size": {"width": 1920, "height": 1080}, "refreshRate": 60.0}]},
      {"name": "DP-2", "connected": false, "enabled": false},
      {"name": "Virtual-1", "connected": true, "enabled": true, "priority": 2, "modes": []}
    ]})");
  }
}  // namespace

TEST(FrametopMonitorsKScreen, ListsConnectedMonitors) {
  const auto now = kscreen::parse_outputs(kscreen_config(), [](const std::string &name) {
    return name == "Virtual-1";
  });
  ASSERT_TRUE(now);
  ASSERT_EQ(now->monitors.size(), 2u);
  EXPECT_EQ(now->other_outputs, 1);
  const auto &main = now->monitors[0];
  EXPECT_TRUE(main.primary);
  ASSERT_TRUE(main.mode);
  EXPECT_EQ(main.mode->refresh_mhz, 59997u);
  EXPECT_EQ(main.modes.size(), 3u);
  EXPECT_EQ(main.place["priority"], 1);
  EXPECT_FALSE(now->monitors[1].on);
  EXPECT_FALSE(now->monitors[1].primary);
  EXPECT_FALSE(main.sideways) << "rotation 1 is none";
  auto turned = kscreen_config();
  turned["outputs"][0]["rotation"] = 8;
  EXPECT_TRUE(kscreen::parse_outputs(turned, never_virtual)->monitors[0].sideways);
  EXPECT_FALSE(kscreen::parse_outputs(json::array(), never_virtual));
}

TEST(FrametopMonitorsKScreen, ArgumentsPickTheModeIdAndPutOnFirst) {
  target_t off {"DP-1", false, std::nullopt, json::object()};
  target_t on {"HDMI-A-1", true, video_mode_t {1920, 1080, 60000}, json::object()};
  target_t back {"DP-1", true, video_mode_t {3840, 2160, 60000},
                 {{"x", 0}, {"y", 0}, {"scale", 1.5}, {"rotation", 2}, {"priority", 1}}};
  std::string error;
  EXPECT_EQ(kscreen::arguments_for({off, on}, kscreen_config(), &error),
            (std::vector<std::string> {"output.HDMI-A-1.enable", "output.HDMI-A-1.mode.1", "output.DP-1.disable"}));
  EXPECT_EQ(kscreen::arguments_for({back}, kscreen_config(), &error),
            (std::vector<std::string> {"output.DP-1.enable", "output.DP-1.mode.3", "output.DP-1.position.0,0",
                                       "output.DP-1.scale.1.5", "output.DP-1.rotation.left", "output.DP-1.priority.1"}));
}

TEST(FrametopMonitorsKScreen, APickedScaleIsSentOverThePlaces) {
  target_t back {"DP-1", true, std::nullopt, {{"x", 0}, {"y", 0}, {"scale", 1.5}}, 1.25};
  std::string error;
  EXPECT_EQ(kscreen::arguments_for({back}, kscreen_config(), &error),
            (std::vector<std::string> {"output.DP-1.enable", "output.DP-1.position.0,0", "output.DP-1.scale.1.25"}));
  const auto now = kscreen::parse_outputs(kscreen_config(), never_virtual);
  ASSERT_TRUE(now);
  EXPECT_FALSE(now->monitors[0].scales.empty());
}

TEST(FrametopMonitorsKScreen, ArgumentsRefuseWhatIsNotThere) {
  std::string error;
  target_t missing {"DP-9", false, std::nullopt, json::object()};
  EXPECT_TRUE(kscreen::arguments_for({missing}, kscreen_config(), &error).empty());
  EXPECT_NE(error.find("DP-9"), std::string::npos);
  target_t no_mode {"HDMI-A-1", true, video_mode_t {800, 600, 60000}, json::object()};
  EXPECT_TRUE(kscreen::arguments_for({no_mode}, kscreen_config(), &error).empty());
  EXPECT_TRUE(kscreen::arguments_for({missing}, json::object(), &error).empty());
}
