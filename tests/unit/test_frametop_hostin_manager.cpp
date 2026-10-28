#include "../tests_common.h"

#include "src/frametop_hostin_manager.h"
#include "src/frametop_hostin_policy.h"

#include <string>
#include <vector>

namespace {
  using namespace frametop_hostin;
  using namespace std::literals;
  using clock_type = manager_t::clock;

  struct fake_service_t: manager_t::service_t {
    bool reachable = true;
    std::uint64_t id = 1;
    std::vector<std::string> sent;

    bool send(std::string_view request) override {
      if (!reachable) return false;
      sent.emplace_back(request);
      return true;
    }

    const char *unavailable() const override {
      return "service";
    }

    std::uint64_t connection() const override {
      return id;
    }
  };

  std::string engage(std::uint16_t ms = 1500) {
    return {char(policy::VERSION), 1, 0, 0, 0, 0, char(ms & 0xff), char(ms >> 8)};
  }

  const std::string release {char(policy::VERSION), 2};

  std::string state(bool engaged, std::string_view reason = {}) {
    return policy::state_payload(engaged, reason);
  }

  const std::string hello = policy::hello_payload(policy::KEYBOARD | policy::MOUSE, "oma7");

  class FrametopHostInputManager: public ::testing::Test {
  protected:
    fake_service_t service;
    manager_t manager {service, "oma7"};
    const void *const a = reinterpret_cast<const void *>(0x10);
    const void *const b = reinterpret_cast<const void *>(0x20);
    clock_type::time_point t0 = clock_type::now();
  };
}  // namespace

TEST_F(FrametopHostInputManager, EngagesForAPermittedLease) {
  manager.lease(a, true, engage(), t0);
  EXPECT_EQ(service.sent, std::vector<std::string> {"engage"});
  EXPECT_TRUE(manager.active());
  manager.service_message(1, "state engaged");
  manager.service_message(1, "ev k 1 30 1 0 0 0");
  const auto out = manager.take(a, t0);
  // The engaged state with its hello, then the key; the periodic report waits a second.
  ASSERT_EQ(out.size(), 3u);
  EXPECT_EQ(out[0], state(true));
  EXPECT_EQ(out[1], hello);
  EXPECT_EQ(out[2], *policy::events_payload("ev k 1 30 1 0 0 0"));
  EXPECT_TRUE(manager.take(b, t0).empty());
}

TEST_F(FrametopHostInputManager, ReportsTheStateEverySecond) {
  manager.lease(a, true, engage(), t0);
  manager.service_message(1, "state engaged");
  manager.take(a, t0);
  EXPECT_TRUE(manager.take(a, t0 + 500ms).empty());
  manager.lease(a, true, engage(), t0 + 500ms);
  EXPECT_EQ(manager.take(a, t0 + 1s), (std::vector<std::string> {state(true), hello}));
}

TEST_F(FrametopHostInputManager, RefusesWithoutPermission) {
  manager.lease(a, false, engage(), t0);
  EXPECT_TRUE(service.sent.empty());
  EXPECT_FALSE(manager.active());
  EXPECT_EQ(manager.take(a, t0), std::vector<std::string> {state(false, "permission")});
  // Told again at most once a second.
  manager.lease(a, false, engage(), t0 + 100ms);
  EXPECT_TRUE(manager.take(a, t0 + 100ms).empty());
  manager.lease(a, false, engage(), t0 + 1s);
  EXPECT_EQ(manager.take(a, t0 + 1s).size(), 1u);
}

TEST_F(FrametopHostInputManager, GivesTheInputBackWhenPermissionGoes) {
  manager.lease(a, true, engage(), t0);
  manager.lease(a, false, engage(), t0 + 500ms);
  EXPECT_EQ(service.sent, (std::vector<std::string> {"engage", "drop"}));
  EXPECT_FALSE(manager.active());
}

TEST_F(FrametopHostInputManager, IgnoresBadLeasePackets) {
  manager.lease(a, true, "", t0);
  manager.lease(a, true, engage(0), t0);
  EXPECT_TRUE(service.sent.empty());
  EXPECT_TRUE(manager.take(a, t0).empty());
}

TEST_F(FrametopHostInputManager, OneClientAtATime) {
  manager.lease(a, true, engage(), t0);
  manager.lease(b, true, engage(), t0 + 100ms);
  EXPECT_EQ(service.sent, std::vector<std::string> {"engage"});
  EXPECT_EQ(manager.take(b, t0 + 100ms), std::vector<std::string> {state(false, "busy")});
  manager.service_message(1, "ev k 1 30 1 0 0 0");
  EXPECT_TRUE(manager.take(b, t0 + 100ms).empty());  // never another client's keys
}

TEST_F(FrametopHostInputManager, AnotherClientMayTakeALapsedLease) {
  manager.lease(a, true, engage(500), t0);
  manager.take(a, t0);
  manager.lease(b, true, engage(), t0 + 600ms);
  EXPECT_EQ(service.sent, (std::vector<std::string> {"engage", "drop", "engage"}));
  EXPECT_EQ(manager.take(a, t0 + 600ms), std::vector<std::string> {state(false, "lease")});
  manager.service_message(1, "state engaged");
  EXPECT_EQ(manager.take(b, t0 + 600ms), (std::vector<std::string> {state(true), hello}));
}

TEST_F(FrametopHostInputManager, ALapsedLeaseGivesTheInputBack) {
  manager.lease(a, true, engage(500), t0);
  manager.service_message(1, "state engaged");
  manager.take(a, t0);
  EXPECT_TRUE(manager.take(a, t0 + 499ms).empty());
  EXPECT_EQ(manager.take(a, t0 + 500ms), std::vector<std::string> {state(false, "lease")});
  EXPECT_EQ(service.sent, (std::vector<std::string> {"engage", "drop"}));
  EXPECT_FALSE(manager.active());
  manager.service_message(1, "ev k 1 30 1 0 0 0");
  EXPECT_TRUE(manager.take(a, t0 + 500ms).empty());
}

TEST_F(FrametopHostInputManager, TheClientsReleaseAlsoLiftsAnEscape) {
  manager.lease(a, true, engage(), t0);
  manager.lease(a, true, release, t0 + 100ms);
  EXPECT_EQ(service.sent, (std::vector<std::string> {"engage", "release"}));
  EXPECT_EQ(manager.take(a, t0 + 100ms), std::vector<std::string> {state(false, "host")});
  EXPECT_FALSE(manager.active());
  manager.lease(b, true, release, t0 + 200ms);  // not its to release
  EXPECT_EQ(service.sent.size(), 2u);
}

TEST_F(FrametopHostInputManager, AnEscapeIsToldOnce) {
  manager.lease(a, true, engage(), t0);
  manager.service_message(1, "state engaged");
  manager.take(a, t0);
  manager.service_message(1, "ev k 1 42 0 0 0 0");  // the Shifts come up
  manager.service_message(1, "state released escape");
  EXPECT_EQ(manager.take(a, t0 + 100ms), (std::vector<std::string> {*policy::events_payload("ev k 1 42 0 0 0 0"), state(false, "escape")}));
  // The service answers every engage while escaped.
  manager.lease(a, true, engage(), t0 + 500ms);
  manager.service_message(1, "state released escape");
  EXPECT_TRUE(manager.take(a, t0 + 500ms).empty());
  EXPECT_TRUE(manager.active());  // the client still holds the lease, so nobody else gets in
}

TEST_F(FrametopHostInputManager, SessionEndDropsButKeepsAnEscape) {
  manager.lease(a, true, engage(), t0);
  manager.service_message(1, "state engaged");
  manager.session_end(a);
  EXPECT_EQ(service.sent, (std::vector<std::string> {"engage", "drop"}));
  EXPECT_FALSE(manager.active());
  EXPECT_TRUE(manager.take(a, t0).empty());  // its queue went with it
  manager.session_end(b);
  EXPECT_EQ(service.sent.size(), 2u);
}

TEST_F(FrametopHostInputManager, AnUnreachableServiceIsRefused) {
  service.reachable = false;
  manager.lease(a, true, engage(), t0);
  EXPECT_FALSE(manager.active());
  EXPECT_EQ(manager.take(a, t0), std::vector<std::string> {state(false, "service")});
  // A refusal doesn't hold the input: another client may try.
  service.reachable = true;
  manager.lease(b, true, engage(), t0 + 100ms);
  EXPECT_TRUE(manager.active());
}

TEST_F(FrametopHostInputManager, TheServiceGoingIsTold) {
  manager.lease(a, true, engage(), t0);
  manager.service_message(1, "state engaged");
  manager.take(a, t0);
  manager.service_gone(1);
  EXPECT_EQ(manager.take(a, t0 + 100ms), std::vector<std::string> {state(false, "service")});
}

TEST_F(FrametopHostInputManager, IgnoresAnOlderConnection) {
  manager.lease(a, true, engage(), t0);
  manager.take(a, t0);
  service.id = 2;
  manager.service_message(1, "state engaged");
  manager.service_message(1, "ev k 1 30 1 0 0 0");
  manager.service_gone(1);
  EXPECT_TRUE(manager.take(a, t0 + 100ms).empty());
}

TEST_F(FrametopHostInputManager, DropsEventsTheServiceShouldNotSend) {
  manager.lease(a, true, engage(), t0);
  manager.service_message(1, "state engaged");
  manager.take(a, t0);
  manager.service_message(1, "ev k 3 0 100 0 0 0");
  manager.service_message(1, "hello");
  EXPECT_TRUE(manager.take(a, t0 + 100ms).empty());
}

TEST_F(FrametopHostInputManager, BoundsAClientsQueue) {
  manager.lease(a, true, engage(), t0);
  manager.service_message(1, "state engaged");
  for (std::size_t i = 0; i < manager_t::MAX_QUEUED + 10; ++i) manager.service_message(1, "ev m 2 0 1 0 0 0");
  EXPECT_EQ(manager.take(a, t0).size(), manager_t::MAX_QUEUED);
}
