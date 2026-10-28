#include "../tests_common.h"

#include "src/frametop_hostin_policy.h"

#include <string>

namespace {
  using namespace frametop_hostin::policy;

  std::string engage(std::uint32_t sequence, std::uint16_t ms) {
    return {char(VERSION), 1, char(sequence & 0xff), char(sequence >> 8 & 0xff), char(sequence >> 16 & 0xff), char(sequence >> 24), char(ms & 0xff), char(ms >> 8)};
  }

  std::string bytes(std::initializer_list<int> values) {
    std::string out;
    for (const int value : values) out.push_back(char(value));
    return out;
  }
}  // namespace

TEST(FrametopHostInputPolicy, ParsesALease) {
  const auto lease = parse_lease(engage(0x01020304, 1000));
  ASSERT_TRUE(lease);
  EXPECT_EQ(lease->action, lease_t::engage);
  EXPECT_EQ(lease->sequence, 0x01020304u);
  EXPECT_EQ(lease->duration, std::chrono::milliseconds {1000});
}

TEST(FrametopHostInputPolicy, CapsALongLease) {
  EXPECT_EQ(parse_lease(engage(1, 60000))->duration, MAX_LEASE);
}

TEST(FrametopHostInputPolicy, ParsesARelease) {
  EXPECT_EQ(parse_lease(bytes({VERSION, 2})), lease_t {lease_t::release});
}

TEST(FrametopHostInputPolicy, ParsesADrop) {
  EXPECT_EQ(parse_lease(bytes({VERSION, 3})), lease_t {lease_t::drop});
}

TEST(FrametopHostInputPolicy, RejectsOtherLeasePackets) {
  EXPECT_FALSE(parse_lease(""));
  EXPECT_FALSE(parse_lease(bytes({VERSION})));
  EXPECT_FALSE(parse_lease(bytes({2, 2})));  // another version
  EXPECT_FALSE(parse_lease(bytes({VERSION, 2, 0})));  // a release with more
  EXPECT_FALSE(parse_lease(bytes({VERSION, 3, 0})));
  EXPECT_FALSE(parse_lease(bytes({VERSION, 4})));
  EXPECT_FALSE(parse_lease(bytes({VERSION, 0})));
  EXPECT_FALSE(parse_lease(engage(1, 0)));  // a lease of nothing
  EXPECT_FALSE(parse_lease(engage(1, 500).substr(0, 7)));
  EXPECT_FALSE(parse_lease(engage(1, 500) + '\0'));
}

TEST(FrametopHostInputPolicy, ParsesServiceStates) {
  EXPECT_EQ(parse_service_state("state engaged"), (service_state_t {true, ""}));
  EXPECT_EQ(parse_service_state("state released escape"), (service_state_t {false, "escape"}));
  EXPECT_FALSE(parse_service_state("state released "));
  EXPECT_FALSE(parse_service_state("state released Escape"));
  EXPECT_FALSE(parse_service_state("state released es cape"));
  EXPECT_FALSE(parse_service_state("state released " + std::string(MAX_REASON + 1, 'a')));
  EXPECT_FALSE(parse_service_state("state engaged "));
  EXPECT_FALSE(parse_service_state("ev k 0 0 0"));
}

TEST(FrametopHostInputPolicy, EncodesEvents) {
  const auto payload = events_payload("ev m 2 0 -5 1 272 1 0 0 0");
  ASSERT_TRUE(payload);
  EXPECT_EQ(*payload, bytes({VERSION, 1, MOUSE, 3, 0,  // three events
                             2, 0, 0, 0, 0xfb, 0xff, 0xff, 0xff,  // REL_X -5
                             1, 0, 0x10, 0x01, 1, 0, 0, 0,  // BTN_LEFT down
                             0, 0, 0, 0, 0, 0, 0, 0}));
  EXPECT_EQ(events_payload("ev k 1 30 1 0 0 0")->at(2), char(KEYBOARD));
}

TEST(FrametopHostInputPolicy, RefusesEventsTheFrameShouldNotGet) {
  EXPECT_FALSE(events_payload("ev"));
  EXPECT_FALSE(events_payload("ev x 1 30 1"));
  EXPECT_FALSE(events_payload("ev k 1 30"));  // not a whole event
  EXPECT_FALSE(events_payload("ev k 1 30 2"));  // a repeat
  EXPECT_FALSE(events_payload("ev k 1 0 1"));
  EXPECT_FALSE(events_payload("ev k 1 768 1"));  // past KEY_MAX
  EXPECT_FALSE(events_payload("ev k 3 0 100"));  // absolute
  EXPECT_FALSE(events_payload("ev k 4 4 30"));  // a scan code
  EXPECT_FALSE(events_payload("ev m 2 16 1"));  // past REL_MAX
  EXPECT_FALSE(events_payload("ev k 0 1 0"));  // SYN_CONFIG
  EXPECT_FALSE(events_payload("ev k 1 30 1x"));
  EXPECT_FALSE(events_payload("ev k 1 30  1"));
  EXPECT_FALSE(events_payload("ev k -1 30 1"));
  // Each kind carries only its own: buttons and motion are the mouse's, other keys the keyboard's.
  EXPECT_FALSE(events_payload("ev k 1 272 1"));  // BTN_LEFT
  EXPECT_FALSE(events_payload("ev k 1 279 1"));  // BTN_TASK
  EXPECT_FALSE(events_payload("ev k 2 0 1"));
  EXPECT_FALSE(events_payload("ev m 1 30 1"));
  EXPECT_FALSE(events_payload("ev m 1 271 1"));  // BTN_MOUSE - 1
  EXPECT_FALSE(events_payload("ev m 1 280 1"));  // BTN_TASK + 1
  EXPECT_TRUE(events_payload("ev m 1 279 1 2 8 -1 0 0 0"));
  EXPECT_TRUE(events_payload("ev k 1 271 1 1 280 0"));
  std::string many = "ev k";
  for (std::size_t i = 0; i < MAX_EVENTS; ++i) many += " 1 30 1";
  EXPECT_TRUE(events_payload(many));
  EXPECT_EQ(events_payload(many)->size(), MAX_PAYLOAD);
  EXPECT_FALSE(events_payload(many + " 0 0 0"));
}

TEST(FrametopHostInputPolicy, EncodesStateAndHello) {
  EXPECT_EQ(state_payload(true, "ignored"), bytes({VERSION, 2, 1}));
  EXPECT_EQ(state_payload(false, "busy"), bytes({VERSION, 2, 0}) + "busy");
  EXPECT_EQ(state_payload(false, std::string(40, 'a')).size(), 3 + MAX_REASON);
  EXPECT_EQ(hello_payload(KEYBOARD | MOUSE, "oma7\n"), bytes({VERSION, 3, 3}) + "oma7?");
  EXPECT_EQ(hello_payload(KEYBOARD, std::string(80, 'n')).size(), 3 + MAX_NAME);
}
