#include <gtest/gtest.h>

#include "esphome/core/application.h"
#include "esphome/core/hal.h"

/// Per-test reset for the deterministic-clock/scheduler-stub pair (WP1): a gtest listener,
/// registered once during static initialization, so no individual test has to remember to clean
/// up shared state that outlives it.
namespace {

class TestIsolationListener : public ::testing::EmptyTestEventListener {
 public:
  void OnTestStart(const ::testing::TestInfo & /*test_info*/) override {
    esphome::test_scheduler::clear();
    // App.scheduler's last_self_timeout_* fields live on the single global Scheduler instance
    // (unlike Component's own last_* fields, which start fresh every test simply because each
    // test constructs its own Component) — clear them too, or a self pointer from a destroyed
    // Component in a previous test lingers as a stale (but never dereferenced) comparison target.
    esphome::App.scheduler.last_self_timeout_self_ = nullptr;
    esphome::App.scheduler.last_self_timeout_callback_ = nullptr;
  }

  void OnTestEnd(const ::testing::TestInfo & /*test_info*/) override {
    // Only triggers if a test bypasses the ManualClock RAII guard some other way (an early
    // return past it, a raw state() write) — normal use always restores legacy mode via the
    // guard's destructor before the test function returns.
    if (esphome::test_clock::is_manual()) {
      ADD_FAILURE() << "ManualClock leaked out of the test -- forcing legacy mode so later tests aren't affected.";
      esphome::test_clock::state().mode = esphome::test_clock::Mode::kLegacy;
    }
  }
};

// Registered during static initialization so the listener is active before any TEST() runs: this
// binary links -lgtest_main rather than defining its own main()/RUN_ALL_TESTS(), so there is no
// later hook to install it from. ::testing::UnitTest::GetInstance() is itself a function-local
// singleton, safe to call here regardless of this translation unit's construction order relative
// to any other.
//
// Appending (not prepending) also matters for OnTestEnd()'s ADD_FAILURE() above: gtest dispatches
// OnTestEnd to listeners in the *reverse* of their registration order, so an appended listener
// runs before the default result printer and its failure is reported normally instead of after
// the test's result has already been printed.
struct TestIsolationInstaller {
  TestIsolationInstaller() { ::testing::UnitTest::GetInstance()->listeners().Append(new TestIsolationListener()); }
};

const TestIsolationInstaller installer;

}  // namespace
