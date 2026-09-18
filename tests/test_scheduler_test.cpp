#include <gtest/gtest.h>

#include "esphome/core/application.h"
#include "esphome/core/component.h"
#include "esphome/core/hal.h"

#include <vector>

// ============================================================================
// TestScheduler test suite
// ============================================================================
// tests/include/esphome/core/application.h's test_scheduler::Registry: the pending-timer registry
// backing Component::set_timeout/set_interval/cancel_* and App.scheduler's Component- and
// self-keyed overloads. Exercised through the public entry points a production caller would use
// (Component methods, App.scheduler), not the Registry class directly.
// ============================================================================

using esphome::App;
using esphome::Component;
using esphome::test_clock::ManualClock;
namespace test_scheduler = esphome::test_scheduler;

namespace {

TEST(TestScheduler, SetTimeoutWithTheSameNamePointerReplacesThePendingItem) {
  ManualClock clock(0);
  Component c;
  int calls = 0;
  static const char *const kName = "foo";
  c.set_timeout(kName, 100, [&] { calls++; });
  c.set_timeout(kName, 200, [&] { calls++; });  // same pointer -- must replace, not add a second item

  EXPECT_EQ(test_scheduler::pending_count(), 1u);
  test_scheduler::run_until(199);
  EXPECT_EQ(calls, 0) << "the replaced (100ms) item must not have survived";
  test_scheduler::run_until(200);
  EXPECT_EQ(calls, 1);
}

TEST(TestScheduler, SetTimeoutWithADifferentPointerButEqualNameContentAlsoReplaces) {
  ManualClock clock(0);
  Component c;
  int calls = 0;
  char buf1[] = "foo";
  char buf2[] = "foo";
  ASSERT_NE(static_cast<void *>(buf1), static_cast<void *>(buf2)) << "sanity: these must be distinct pointers";

  c.set_timeout(buf1, 100, [&] { calls++; });
  c.set_timeout(buf2, 200, [&] { calls++; });

  EXPECT_EQ(test_scheduler::pending_count(), 1u)
      << "two different string-literal-like pointers with equal contents are still \"the same timer\"";
  test_scheduler::run_until(200);
  EXPECT_EQ(calls, 1);
}

TEST(TestScheduler, DueItemsFireInDeadlineOrderTiesBrokenByInsertionOrder) {
  ManualClock clock(0);
  Component a, b, c;
  std::vector<int> fired;
  a.set_timeout("a", 100, [&] { fired.push_back(1); });
  b.set_timeout("b", 50, [&] { fired.push_back(2); });
  c.set_timeout("c", 100, [&] { fired.push_back(3); });  // same deadline as `a`, scheduled after it

  test_scheduler::run_until(100);

  EXPECT_EQ(fired, (std::vector<int>{2, 1, 3})) << "b's earlier deadline first, then a before c on the tie";
}

TEST(TestScheduler, ItemAddedFromWithinACallbackIsNotFiredInTheSameRunDueCall) {
  ManualClock clock(0);
  Component c;
  int inner_calls = 0;
  c.set_timeout("outer", 100, [&] {
    c.set_timeout("inner", 0, [&] { inner_calls++; });  // due immediately, but added mid-call
  });

  esphome::test_clock::set_ms(100);
  EXPECT_EQ(test_scheduler::run_due(), 1u) << "only the outer timeout should fire in this call";
  EXPECT_EQ(inner_calls, 0);

  EXPECT_EQ(test_scheduler::run_due(), 1u) << "the inner timeout fires on the next call";
  EXPECT_EQ(inner_calls, 1);
}

TEST(TestScheduler, IntervalFiresAtMostOncePerRunDueCallEvenAfterAnUnattendedJump) {
  ManualClock clock(0);
  Component c;
  int calls = 0;
  c.set_interval("beat", 100, [&] { calls++; });

  esphome::test_clock::set_ms(1000);  // ten periods "elapsed" with no run_due() call in between
  EXPECT_EQ(test_scheduler::run_due(), 1u);
  EXPECT_EQ(calls, 1) << "no catch-up burst: at most one fire per run_due() call";
}

TEST(TestScheduler, IntervalRearmsFromExecutionTimeNotFromTheMissedDeadlineLadder) {
  ManualClock clock(0);
  Component c;
  int calls = 0;
  c.set_interval("beat", 100, [&] { calls++; });

  esphome::test_clock::set_ms(1000);
  test_scheduler::run_due();
  ASSERT_EQ(calls, 1);

  // The next fire is 1000 + 100, from when it actually ran -- not 200, the next slot the original
  // deadline ladder would have used.
  esphome::test_clock::set_ms(1099);
  test_scheduler::run_due();
  EXPECT_EQ(calls, 1) << "not yet due";
  esphome::test_clock::set_ms(1100);
  test_scheduler::run_due();
  EXPECT_EQ(calls, 2);
}

TEST(TestScheduler, RunUntilWalksThroughEveryIntermediateDeadlineUnlikeASingleRunDueCall) {
  // Contrast with IntervalFiresAtMostOncePerRunDueCallEvenAfterAnUnattendedJump above: run_until()
  // is many simulated loop() ticks across the jump, so an interval catching up across it (one fire
  // per period) is correct -- the "no catch-up burst" rule is about a single run_due() call.
  ManualClock clock(0);
  Component c;
  int calls = 0;
  c.set_interval("beat", 100, [&] { calls++; });

  test_scheduler::run_until(350);

  EXPECT_EQ(calls, 3) << "fires at 100, 200, and 300; 350 isn't itself a deadline";
}

TEST(TestScheduler, CancelTimeoutByNameReturnsTrueOnceThenFalse) {
  ManualClock clock(0);
  Component c;
  c.set_timeout("foo", 100, [] {});

  EXPECT_TRUE(c.cancel_timeout("foo"));
  EXPECT_FALSE(c.cancel_timeout("foo")) << "already cancelled";
  EXPECT_FALSE(c.cancel_timeout("bar")) << "never existed";
}

TEST(TestScheduler, CancelTimeoutByNumericIdAndCancelIntervalWork) {
  ManualClock clock(0);
  Component c;
  c.set_timeout(42u, 100, [] {});
  EXPECT_TRUE(c.cancel_timeout(42u));
  EXPECT_FALSE(c.cancel_timeout(42u));

  c.set_interval("beat", 50, [] {});
  EXPECT_TRUE(c.cancel_interval("beat"));
  EXPECT_FALSE(c.cancel_interval("beat"));
}

TEST(TestScheduler, NumericIdsAndStringNamesDoNotCollideEvenWithMatchingContent) {
  ManualClock clock(0);
  Component c;
  int name_calls = 0, id_calls = 0;
  c.set_timeout("1", 100, [&] { name_calls++; });
  c.set_timeout(1u, 100, [&] { id_calls++; });

  EXPECT_EQ(test_scheduler::pending_count(), 2u) << "a numeric id must never match a same-looking name";
  test_scheduler::run_until(100);
  EXPECT_EQ(name_calls, 1);
  EXPECT_EQ(id_calls, 1);
}

// Executable form of the LR1121 flash-confirm rationale: arm_flash_confirmation_() deliberately
// uses App.scheduler's self-keyed overload instead of Component::set_timeout() specifically so the
// auto-disarm still fires on a hub that has marked itself failed (lr1121_firmware_update_controller.cpp).
TEST(TestScheduler, FailedComponentTimeoutIsSkippedWhileASelfKeyedOneStillFires) {
  ManualClock clock(0);
  Component c;
  int comp_calls = 0, self_calls = 0;
  c.set_timeout("foo", 100, [&] { comp_calls++; });
  App.scheduler.set_timeout(static_cast<const void *>(&c), 100, [&] { self_calls++; });
  c.mark_failed();

  test_scheduler::run_until(100);

  EXPECT_EQ(comp_calls, 0) << "a failed component's own timeout must be skipped";
  EXPECT_EQ(self_calls, 1) << "the self-keyed timeout has no Component to check and must still fire";
  EXPECT_EQ(test_scheduler::pending_count(), 1u) << "the skipped item stays pending, not dropped";
}

TEST(TestScheduler, DestroyingAComponentPurgesItsOwnedAndSelfKeyedItems) {
  {
    Component c;
    c.set_timeout("foo", 100, [] {});
    // Self-keyed on the Component's own `this` -- the exact shape arm_flash_confirmation_() uses
    // (it keys on its hub pointer, which is itself a Component). Both must be purged, or the
    // second leaves a callback capturing a dangling `this` behind.
    App.scheduler.set_timeout(static_cast<const void *>(&c), 100, [] {});
    ASSERT_EQ(test_scheduler::pending_count(), 2u);
  }  // c destroyed here

  EXPECT_EQ(test_scheduler::pending_count(), 0u);
}

TEST(TestScheduler, PendingReturnsTheDeadlineAndFireInvokesAndConsumesAOneShotTimeout) {
  ManualClock clock(1000);
  Component c;
  int calls = 0;
  c.set_timeout("foo", 250, [&] { calls++; });

  auto view = test_scheduler::pending(&c, "foo");
  ASSERT_TRUE(view.has_value());
  EXPECT_EQ(view->deadline_ms, 1250u);

  EXPECT_TRUE(view->fire());

  EXPECT_EQ(calls, 1);
  EXPECT_FALSE(test_scheduler::pending(&c, "foo").has_value()) << "fire() must consume a one-shot timeout";
}

TEST(TestScheduler, PendingSelfAndFireWorkForTheSchedulerSelfKeyedOverload) {
  ManualClock clock(0);
  int calls = 0;
  App.scheduler.set_timeout(static_cast<const void *>(&calls), 500, [&] { calls++; });

  auto view = test_scheduler::pending_self(static_cast<const void *>(&calls));
  ASSERT_TRUE(view.has_value());

  EXPECT_TRUE(view->fire());

  EXPECT_EQ(calls, 1);
}

TEST(TestScheduler, PendingViewFireIgnoresTheFailedComponentSkipUnlikeRunDue) {
  // fire_by_seq()'s own doc comment: unlike run_due(), an explicit fire() by key never applies the
  // failed-component skip -- a test asking for this exact item by key has already decided it
  // should fire.
  ManualClock clock(0);
  Component c;
  int calls = 0;
  c.set_timeout("foo", 100, [&] { calls++; });
  c.mark_failed();

  auto view = test_scheduler::pending(&c, "foo");
  ASSERT_TRUE(view.has_value());

  EXPECT_TRUE(view->fire());
  EXPECT_EQ(calls, 1);
}

TEST(TestScheduler, PendingViewFireReturnsFalseWhenTheItemIsNoLongerPending) {
  ManualClock clock(0);
  Component c;
  int calls = 0;
  c.set_timeout("foo", 100, [&] { calls++; });
  auto view = test_scheduler::pending(&c, "foo");
  ASSERT_TRUE(view.has_value());
  ASSERT_TRUE(c.cancel_timeout("foo"));

  EXPECT_FALSE(view->fire()) << "the item was cancelled after the view was obtained";
  EXPECT_EQ(calls, 0);
}

TEST(TestScheduler, IntervalThatCancelsItselfActuallyStops) {
  ManualClock clock(0);
  Component c;
  int calls = 0;
  c.set_interval("beat", 100, [&] {
    calls++;
    c.cancel_interval("beat");
  });

  test_scheduler::run_until(500);

  EXPECT_EQ(calls, 1) << "the self-cancel must not be silently undone by the natural re-arm";
  EXPECT_EQ(test_scheduler::pending_count(), 0u);
}

TEST(TestScheduler, IntervalThatReregistersItselfIsNotDuplicated) {
  ManualClock clock(0);
  Component c;
  int calls = 0;
  c.set_interval("beat", 100, [&] {
    calls++;
    c.set_interval("beat", 50, [&] { calls++; });  // re-arms itself with a different delay
  });

  test_scheduler::run_until(100);

  EXPECT_EQ(calls, 1) << "the callback's own re-register must win, not be duplicated by the natural re-arm";
  EXPECT_EQ(test_scheduler::pending_count(), 1u) << "exactly one pending item for this key, not two";
}

TEST(TestScheduler, CancelTimeoutBySelfWorksForANonMostRecentSelfKeyedItem) {
  ManualClock clock(0);
  int calls_a = 0, calls_b = 0;
  int key_a = 1, key_b = 2;  // distinct addresses to key on; values themselves are irrelevant
  App.scheduler.set_timeout(static_cast<const void *>(&key_a), 100, [&] { calls_a++; });
  App.scheduler.set_timeout(static_cast<const void *>(&key_b), 100, [&] { calls_b++; });  // now "most recent"

  // The older self-keyed item is no longer what last_self_timeout_self_ reflects, but it must
  // still be cancellable -- the registry, not that compat field, is the source of truth for cancel.
  EXPECT_TRUE(App.scheduler.cancel_timeout(static_cast<const void *>(&key_a)));

  test_scheduler::run_until(100);

  EXPECT_EQ(calls_a, 0) << "the older self-keyed item was actually cancelled";
  EXPECT_EQ(calls_b, 1) << "the more recent one is untouched";
}

TEST(TestScheduler, CancelOnlyClearsTheCompatFieldsWhenItMatchesTheMostRecentCall) {
  ManualClock clock(0);
  Component c;
  c.set_timeout("a", 100, [] {});
  c.set_timeout("b", 100, [] {});  // now the most recent -- last_timeout_name_ == "b"

  EXPECT_TRUE(c.cancel_timeout("a")) << "the item existed and must still be cancellable";
  EXPECT_EQ(c.last_timeout_name_, "b")
      << "cancelling an unrelated timer must not clobber the most recent call's own record";
  EXPECT_TRUE(static_cast<bool>(c.last_timeout_callback_));
}

TEST(TestScheduler, LastTimeoutFieldsMirrorTheMostRecentCallForHandFiring) {
  Component c;
  int calls = 0;
  c.set_timeout("foo", 500, [&] { calls++; });

  EXPECT_EQ(c.last_timeout_name_, "foo");
  EXPECT_EQ(c.last_timeout_ms_, 500u);
  ASSERT_TRUE(static_cast<bool>(c.last_timeout_callback_));

  // Firing the captured callback by hand (the idiom several production tests use, to fire a stale
  // timer after a rearm) must keep working independently of the registry.
  c.last_timeout_callback_();
  EXPECT_EQ(calls, 1);
}

}  // namespace
