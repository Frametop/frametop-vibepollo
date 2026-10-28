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
  const char *why = "service";
    std::uint64_t id = 1;
    std::vector<std::string> sent;

    bool send(std::string_view request) override {
      if (!reachable) return false;
      sent.emplace_back(request);
      return true;
    }

    const char *unavailable() const override {
      return why;
    }

    std::uint64_t connection() const override {
      return id;
    }
  };

  std::string engage(std::uint16_t ms = 1500) {
    return {char(policy::VERSION), 1, 0, 0, 0, 0, char(ms & 0xff), char(ms >> 8)};
  }

  const std::string release {char(policy::VERSION), 2};
  const std::string drop {char(policy::VERSION), 3};

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
  const auto out = manager.take(a, true, t0);
  // The engaged state with its hello, then the key; the periodic report waits a second.
  ASSERT_EQ(out.size(), 3u);
  EXPECT_EQ(out[0], state(true));
  EXPECT_EQ(out[1], hello);
  EXPECT_EQ(out[2], *policy::events_payload("ev k 1 30 1 0 0 0"));
  EXPECT_TRUE(manager.take(b, true, t0).empty());
}

TEST_F(FrametopHostInputManager, ReportsTheStateEverySecond) {
  manager.lease(a, true, engage(), t0);
  manager.service_message(1, "state engaged");
  manager.take(a, true, t0);
  EXPECT_TRUE(manager.take(a, true, t0 + 500ms).empty());
  manager.lease(a, true, engage(), t0 + 500ms);
  EXPECT_EQ(manager.take(a, true, t0 + 1s), (std::vector<std::string> {state(true), hello}));
}

TEST_F(FrametopHostInputManager, RefusesWithoutPermission) {
  manager.lease(a, false, engage(), t0);
  EXPECT_TRUE(service.sent.empty());
  EXPECT_FALSE(manager.active());
  EXPECT_EQ(manager.take(a, true, t0), std::vector<std::string> {state(false, "permission")});
  // Told again at most once a second.
  manager.lease(a, false, engage(), t0 + 100ms);
  EXPECT_TRUE(manager.take(a, true, t0 + 100ms).empty());
  manager.lease(a, false, engage(), t0 + 1s);
  EXPECT_EQ(manager.take(a, true, t0 + 1s).size(), 1u);
}

TEST_F(FrametopHostInputManager, ANewReasonIsToldAtOnce) {
  manager.lease(a, true, engage(), t0);
  manager.lease(b, true, engage(), t0 + 100ms);
  EXPECT_EQ(manager.take(b, true, t0 + 100ms), std::vector<std::string> {state(false, "busy")});
  manager.lease(b, false, engage(), t0 + 200ms);
  EXPECT_EQ(manager.take(b, true, t0 + 200ms), std::vector<std::string> {state(false, "permission")});
}

TEST_F(FrametopHostInputManager, BusyIsToldOnceASecond) {
  manager.lease(a, true, engage(), t0);
  manager.lease(b, true, engage(), t0 + 100ms);
  manager.take(b, true, t0 + 100ms);
  manager.lease(b, true, engage(), t0 + 600ms);
  manager.lease(a, true, engage(), t0 + 600ms);
  EXPECT_TRUE(manager.take(b, true, t0 + 600ms).empty());
  manager.lease(b, true, engage(), t0 + 1100ms);
  EXPECT_EQ(manager.take(b, true, t0 + 1100ms), std::vector<std::string> {state(false, "busy")});
}

TEST_F(FrametopHostInputManager, GivesTheInputBackWhenPermissionGoes) {
  manager.lease(a, true, engage(), t0);
  manager.lease(a, false, engage(), t0 + 500ms);
  EXPECT_EQ(service.sent, (std::vector<std::string> {"engage", "drop"}));
  EXPECT_FALSE(manager.active());
  EXPECT_EQ(manager.take(a, true, t0 + 500ms), std::vector<std::string> {state(false, "permission")});
}

TEST_F(FrametopHostInputManager, ChecksPermissionBeforeEveryDrain) {
  manager.lease(a, true, engage(), t0);
  manager.service_message(1, "state engaged");
  manager.service_message(1, "ev k 1 30 1 0 0 0");
  // Revoked between leases: nothing more goes to it.
  EXPECT_EQ(manager.take(a, false, t0 + 100ms), std::vector<std::string> {state(false, "permission")});
  EXPECT_EQ(service.sent, (std::vector<std::string> {"engage", "drop"}));
  EXPECT_FALSE(manager.active());
  EXPECT_TRUE(manager.take(b, false, t0 + 100ms).empty());  // not the owner: nothing to give back
}

TEST_F(FrametopHostInputManager, IgnoresBadLeasePackets) {
  manager.lease(a, true, "", t0);
  manager.lease(a, true, engage(0), t0);
  EXPECT_TRUE(service.sent.empty());
  EXPECT_TRUE(manager.take(a, true, t0).empty());
}

TEST_F(FrametopHostInputManager, OneClientAtATime) {
  manager.lease(a, true, engage(), t0);
  manager.lease(b, true, engage(), t0 + 100ms);
  EXPECT_EQ(service.sent, std::vector<std::string> {"engage"});
  EXPECT_EQ(manager.take(b, true, t0 + 100ms), std::vector<std::string> {state(false, "busy")});
  manager.service_message(1, "ev k 1 30 1 0 0 0");
  EXPECT_TRUE(manager.take(b, true, t0 + 100ms).empty());  // never another client's keys
}

TEST_F(FrametopHostInputManager, AnotherClientMayTakeALapsedLease) {
  manager.lease(a, true, engage(500), t0);
  manager.take(a, true, t0);
  manager.lease(b, true, engage(), t0 + 600ms);
  EXPECT_EQ(service.sent, (std::vector<std::string> {"engage", "drop", "engage"}));
  EXPECT_EQ(manager.take(a, true, t0 + 600ms), std::vector<std::string> {state(false, "lease")});
  manager.service_message(1, "done");  // the drop's
  manager.service_message(1, "state engaged");
  EXPECT_EQ(manager.take(b, true, t0 + 600ms), (std::vector<std::string> {state(true), hello}));
}

TEST_F(FrametopHostInputManager, TheLastLeasesKeysNeverReachTheNextOwner) {
  manager.lease(a, true, engage(), t0);
  manager.service_message(1, "state engaged");
  manager.take(a, true, t0);
  manager.lease(a, true, release, t0 + 100ms);
  manager.lease(b, true, engage(), t0 + 200ms);
  // Still in flight from a's lease when the service read the release.
  manager.service_message(1, "ev k 1 30 1 0 0 0");
  manager.service_message(1, "state engaged");
  EXPECT_TRUE(manager.take(b, true, t0 + 200ms).empty());
  manager.service_message(1, "state released host");
  manager.service_message(1, "done");
  manager.service_message(1, "state engaged");  // b's engage
  manager.service_message(1, "ev k 1 31 1 0 0 0");
  EXPECT_EQ(manager.take(b, true, t0 + 200ms), (std::vector<std::string> {state(true), hello, *policy::events_payload("ev k 1 31 1 0 0 0")}));
}

TEST_F(FrametopHostInputManager, ANewConnectionOwesNoAnswers) {
  manager.lease(a, true, engage(), t0);
  manager.lease(a, true, drop, t0 + 100ms);  // unanswered when the service goes
  service.id = 2;
  manager.lease(a, true, engage(), t0 + 200ms);
  manager.service_message(2, "state engaged");
  EXPECT_EQ(manager.take(a, true, t0 + 200ms), (std::vector<std::string> {state(false, "host"), state(true), hello}));
}

TEST_F(FrametopHostInputManager, ALapsedLeaseGivesTheInputBack) {
  manager.lease(a, true, engage(500), t0);
  manager.service_message(1, "state engaged");
  manager.take(a, true, t0);
  EXPECT_TRUE(manager.take(a, true, t0 + 499ms).empty());
  EXPECT_TRUE(manager.active());
  EXPECT_EQ(manager.take(a, true, t0 + 500ms), std::vector<std::string> {state(false, "lease")});
  EXPECT_EQ(service.sent, (std::vector<std::string> {"engage", "drop"}));
  EXPECT_FALSE(manager.active());
  manager.service_message(1, "ev k 1 30 1 0 0 0");
  EXPECT_TRUE(manager.take(a, true, t0 + 500ms).empty());
}

TEST_F(FrametopHostInputManager, TheClientsReleaseAlsoLiftsAnEscape) {
  manager.lease(a, true, engage(), t0);
  manager.lease(a, true, release, t0 + 100ms);
  EXPECT_EQ(service.sent, (std::vector<std::string> {"engage", "release"}));
  EXPECT_EQ(manager.take(a, true, t0 + 100ms), std::vector<std::string> {state(false, "host")});
  EXPECT_FALSE(manager.active());
}

TEST_F(FrametopHostInputManager, OnlyTheOwnerGivesTheInputBack) {
  manager.lease(a, true, engage(), t0);
  manager.lease(b, true, release, t0 + 100ms);
  manager.lease(b, true, drop, t0 + 100ms);
  EXPECT_EQ(service.sent, std::vector<std::string> {"engage"});
  EXPECT_TRUE(manager.active());
}

TEST_F(FrametopHostInputManager, AReleaseWithNoOwnerStillLiftsAnEscape) {
  // The session that escaped ended (a drop), so its escape stays until a client releases.
  manager.lease(a, true, engage(), t0);
  manager.service_message(1, "state released escape");
  manager.session_end(a);
  manager.lease(b, true, release, t0 + 100ms);
  EXPECT_EQ(service.sent, (std::vector<std::string> {"engage", "drop", "release"}));
  EXPECT_EQ(manager.take(b, true, t0 + 100ms), std::vector<std::string> {state(false, "host")});
  // A drop has nothing to give back.
  manager.lease(b, true, drop, t0 + 200ms);
  EXPECT_EQ(service.sent.size(), 3u);
}

TEST_F(FrametopHostInputManager, TheClientsDropKeepsAnEscape) {
  manager.lease(a, true, engage(), t0);
  manager.service_message(1, "state engaged");
  manager.take(a, true, t0);
  manager.lease(a, true, drop, t0 + 100ms);
  EXPECT_EQ(service.sent, (std::vector<std::string> {"engage", "drop"}));
  EXPECT_FALSE(manager.active());
  EXPECT_EQ(manager.take(a, true, t0 + 100ms), std::vector<std::string> {state(false, "host")});
}

TEST_F(FrametopHostInputManager, AnEscapeIsToldOnce) {
  manager.lease(a, true, engage(), t0);
  manager.service_message(1, "state engaged");
  manager.take(a, true, t0);
  manager.service_message(1, "ev k 1 42 0 0 0 0");  // the Shifts come up
  manager.service_message(1, "state released escape");
  EXPECT_EQ(manager.take(a, true, t0 + 100ms), (std::vector<std::string> {*policy::events_payload("ev k 1 42 0 0 0 0"), state(false, "escape")}));
  // The service answers every engage while escaped.
  manager.lease(a, true, engage(), t0 + 500ms);
  manager.service_message(1, "state released escape");
  EXPECT_TRUE(manager.take(a, true, t0 + 500ms).empty());
  EXPECT_TRUE(manager.active());  // the client still holds the lease, so nobody else gets in
}

TEST_F(FrametopHostInputManager, AnEscapeOutlivesTheService) {
  manager.lease(a, true, engage(), t0);
  manager.service_message(1, "state engaged");
  manager.service_message(1, "state released escape");
  manager.take(a, true, t0);
  manager.service_gone(1);
  service.id = 2;
  // A fresh service knows nothing of it: no engage goes until a release.
  manager.lease(a, true, engage(), t0 + 500ms);
  EXPECT_EQ(service.sent, std::vector<std::string> {"engage"});
  manager.session_end(a);
  manager.lease(b, true, engage(), t0 + 600ms);
  EXPECT_EQ(manager.take(b, true, t0 + 600ms), std::vector<std::string> {state(false, "escape")});
  manager.lease(b, true, release, t0 + 700ms);
  manager.lease(b, true, engage(), t0 + 800ms);
  EXPECT_EQ(service.sent, (std::vector<std::string> {"engage", "drop", "release", "engage"}));
}

TEST_F(FrametopHostInputManager, SessionEndDropsButKeepsAnEscape) {
  manager.lease(a, true, engage(), t0);
  manager.service_message(1, "state engaged");
  manager.service_message(1, "ev k 1 30 1 0 0 0");
  manager.session_end(a);
  EXPECT_EQ(service.sent, (std::vector<std::string> {"engage", "drop"}));
  EXPECT_FALSE(manager.active());
  EXPECT_TRUE(manager.take(a, true, t0).empty());  // its queue went with it
  manager.session_end(b);
  EXPECT_EQ(service.sent.size(), 2u);
}

TEST_F(FrametopHostInputManager, AnUnreachableServiceIsRefused) {
  service.reachable = false;
  manager.lease(a, true, engage(), t0);
  EXPECT_FALSE(manager.active());
  EXPECT_EQ(manager.take(a, true, t0), std::vector<std::string> {state(false, "service")});
  // Told once a second, not on every lease.
  manager.lease(a, true, engage(), t0 + 500ms);
  EXPECT_TRUE(manager.take(a, true, t0 + 500ms).empty());
  manager.lease(a, true, engage(), t0 + 1s);
  EXPECT_EQ(manager.take(a, true, t0 + 1s), std::vector<std::string> {state(false, "service")});
  // A refusal doesn't hold the input: another client may try.
  service.reachable = true;
  manager.lease(b, true, engage(), t0 + 1100ms);
  EXPECT_TRUE(manager.active());
}

TEST_F(FrametopHostInputManager, SaysWhenTheHostHasNotTurnedItOn) {
  service.reachable = false;
  service.why = "off";
  manager.lease(a, true, engage(), t0);
  EXPECT_EQ(manager.take(a, true, t0), std::vector<std::string> {state(false, "off")});
}

TEST_F(FrametopHostInputManager, TheServiceGoingIsTold) {
  manager.lease(a, true, engage(), t0);
  manager.service_message(1, "state engaged");
  manager.take(a, true, t0);
  manager.service_gone(1);
  EXPECT_EQ(manager.take(a, true, t0 + 100ms), std::vector<std::string> {state(false, "service")});
  // The next lease connects again.
  service.id = 2;
  manager.lease(a, true, engage(), t0 + 500ms);
  manager.service_message(2, "state engaged");
  EXPECT_EQ(manager.take(a, true, t0 + 500ms), (std::vector<std::string> {state(true), hello}));
}

TEST_F(FrametopHostInputManager, IgnoresAnOlderConnection) {
  manager.lease(a, true, engage(), t0);
  manager.take(a, true, t0);
  service.id = 2;
  manager.service_message(1, "state engaged");
  manager.service_message(1, "ev k 1 30 1 0 0 0");
  manager.service_gone(1);
  EXPECT_TRUE(manager.take(a, true, t0 + 100ms).empty());
}

TEST_F(FrametopHostInputManager, DropsEventsTheServiceShouldNotSend) {
  manager.lease(a, true, engage(), t0);
  manager.service_message(1, "state engaged");
  manager.take(a, true, t0);
  manager.service_message(1, "ev k 3 0 100 0 0 0");
  manager.service_message(1, "hello");
  EXPECT_TRUE(manager.take(a, true, t0 + 100ms).empty());
}

TEST_F(FrametopHostInputManager, AnotherSessionsFullQueueOnlyStopsGrowing) {
  manager.lease(a, true, engage(), t0);
  // A new reason is told at once: alternating them fills b's queue, never drained.
  for (int i = 0; i < 600; ++i) manager.lease(b, i % 2 == 0, engage(), t0);
  EXPECT_EQ(service.sent, std::vector<std::string> {"engage"});
  EXPECT_TRUE(manager.active());
  EXPECT_EQ(manager.take(b, true, t0).size(), manager_t::MAX_QUEUED);
}

TEST_F(FrametopHostInputManager, AReleaseAfterALapseStillReachesTheService) {
  manager.lease(a, true, engage(500), t0);
  manager.lease(a, true, release, t0 + 600ms);
  EXPECT_EQ(service.sent, (std::vector<std::string> {"engage", "drop", "release"}));
  EXPECT_EQ(manager.take(a, true, t0 + 600ms), (std::vector<std::string> {state(false, "lease"), state(false, "host")}));
}

TEST_F(FrametopHostInputManager, AFullQueueGivesTheInputBack) {
  manager.lease(a, true, engage(), t0);
  manager.service_message(1, "state engaged");
  for (std::size_t i = 0; i < manager_t::MAX_QUEUED + 10; ++i) manager.service_message(1, "ev m 2 0 1 0 0 0");
  // Not a queue missing a key-up: the input goes back and the client is told why.
  EXPECT_EQ(manager.take(a, true, t0), std::vector<std::string> {state(false, "overflow")});
  EXPECT_EQ(service.sent, (std::vector<std::string> {"engage", "drop"}));
  EXPECT_FALSE(manager.active());
  // It may lease again.
  manager.lease(a, true, engage(), t0 + 500ms);
  EXPECT_EQ(service.sent.back(), "engage");
}
