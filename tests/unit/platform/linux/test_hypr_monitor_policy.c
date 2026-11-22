// The broker's hypr-monitor verb: which words it takes, and the Lua it writes from them.
#include "../../../../packaging/linux/vibepollo-hypr-monitor-policy.h"

#undef NDEBUG
#include <assert.h>

static bool safe(int argc, const char *const *argv) {
  return hypr_monitor_arguments_are_safe(argc, (char **) argv);
}

static void takes_off_and_on(void) {
  const char *off[] = {"x", "hypr-monitor", "eDP-2", "off"};
  const char *on[] = {"x", "hypr-monitor", "DP-1", "3440x1440@59.973", "-1920x0", "1.25", "0"};
  const char *preferred[] = {"x", "hypr-monitor", "HDMI-A-1", "preferred", "auto", "auto", "7"};
  assert(safe(4, off));
  assert(safe(7, on));
  assert(safe(7, preferred));
}

static void refuses_anything_else(void) {
  const char *quote[] = {"x", "hypr-monitor", "eDP-2\"})hl.exec_cmd(\"id", "off"};
  const char *lua_mode[] = {"x", "hypr-monitor", "DP-1", "1x1@1\"})", "auto", "auto", "0"};
  const char *word[] = {"x", "hypr-monitor", "DP-1", "on"};
  const char *position[] = {"x", "hypr-monitor", "DP-1", "preferred", "1,2", "auto", "0"};
  const char *scale[] = {"x", "hypr-monitor", "DP-1", "preferred", "auto", "0", "0"};
  const char *scale_lua[] = {"x", "hypr-monitor", "DP-1", "preferred", "auto", "1+os.exit()", "0"};
  const char *transform[] = {"x", "hypr-monitor", "DP-1", "preferred", "auto", "auto", "8"};
  const char *too_many[] = {"x", "hypr-monitor", "DP-1", "preferred", "auto", "auto", "0", "0"};
  const char *other[] = {"x", "display-apply", "DP-1", "off"};
  const char *leading_dash[] = {"x", "hypr-monitor", "-DP-1", "off"};
  const char *refresh[] = {"x", "hypr-monitor", "DP-1", "1920x1080@60.0001", "auto", "auto", "0"};
  assert(!safe(4, quote));
  assert(!safe(7, lua_mode));
  assert(!safe(4, word));
  assert(!safe(7, position));
  assert(!safe(7, scale));
  assert(!safe(7, scale_lua));
  assert(!safe(7, transform));
  assert(!safe(8, too_many));
  assert(!safe(4, other));
  assert(!safe(4, leading_dash));
  assert(!safe(7, refresh));
  assert(!safe(3, other));
}

static void writes_the_rule(void) {
  char lua[512];
  const char *off[] = {"x", "hypr-monitor", "eDP-2", "off"};
  assert(hypr_monitor_lua(4, (char **) off, lua, sizeof lua));
  assert(!strcmp(lua, "hl.monitor({output = \"eDP-2\", disabled = true})"));
  const char *on[] = {"x", "hypr-monitor", "eDP-2", "2560x1600@60", "1536x2880", "1.25", "0"};
  assert(hypr_monitor_lua(7, (char **) on, lua, sizeof lua));
  assert(!strcmp(lua, "hl.monitor({output = \"eDP-2\", mode = \"2560x1600@60\", position = \"1536x2880\", scale = 1.25, transform = 0, disabled = false})"));
  const char *preferred[] = {"x", "hypr-monitor", "DP-1", "preferred", "auto", "auto", "0"};
  assert(hypr_monitor_lua(7, (char **) preferred, lua, sizeof lua));
  assert(!strcmp(lua, "hl.monitor({output = \"DP-1\", mode = \"preferred\", position = \"auto\", scale = \"auto\", transform = 0, disabled = false})"));
  char small[16];
  assert(!hypr_monitor_lua(4, (char **) off, small, sizeof small));
}

int main(void) {
  takes_off_and_on();
  refuses_anything_else();
  writes_the_rule();
  return 0;
}
