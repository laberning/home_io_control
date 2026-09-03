#include "platform_accept_foreign_pairing_switch.h"
#include "hub_core.h"
#include "pairing_responder.h"

#include "test_helpers.h"

using namespace esphome::home_io_control;
using test::TestableHubComponent;

// ============================================================================
// PlatformAcceptForeignPairingSwitch test suite
// ============================================================================
// Hub-level switch: on/off arms/disarms the key-extraction responder, and the entity mirrors
// the hub's own auto-disarm (successful extraction / timeout) rather than only reacting to a
// user-initiated toggle.

/// Exposes protected write_state(bool) for direct invocation in tests.
class TestableAcceptForeignPairingSwitch : public IOHomeAcceptForeignPairingSwitch {
 public:
  using IOHomeAcceptForeignPairingSwitch::write_state;
};

TEST(PlatformAcceptForeignPairingSwitch, WriteStateTrueArmsHubAndPublishesOn) {
  TestableHubComponent hub;
  TestableAcceptForeignPairingSwitch sw;
  sw.set_parent(&hub);

  sw.write_state(true);

  EXPECT_EQ(hub.key_extraction_.key_extraction_ctx_.state, pairing_responder::ResponderState::ARMED_IDLE)
      << "write_state(true) should arm the hub's key-extraction responder";
  EXPECT_TRUE(sw.get_state()) << "switch should publish on immediately";
}

TEST(PlatformAcceptForeignPairingSwitch, WriteStateFalseDisarmsHubAndPublishesOff) {
  TestableHubComponent hub;
  TestableAcceptForeignPairingSwitch sw;
  sw.set_parent(&hub);

  sw.write_state(true);
  sw.write_state(false);

  EXPECT_EQ(hub.key_extraction_.key_extraction_ctx_.state, pairing_responder::ResponderState::DISARMED)
      << "write_state(false) should disarm the hub's key-extraction responder";
  EXPECT_FALSE(sw.get_state());
}

TEST(PlatformAcceptForeignPairingSwitch, AutoDisarmUpdatesDisplayedStateWithoutWriteState) {
  TestableHubComponent hub;
  TestableAcceptForeignPairingSwitch sw;
  sw.set_parent(&hub);
  sw.setup();  // registers the armed-state callback

  sw.write_state(true);
  ASSERT_TRUE(sw.get_state());
  ASSERT_TRUE(static_cast<bool>(hub.last_timeout_callback_)) << "arming should schedule the auto-off timeout";

  // Simulate the auto-off timeout firing (extraction window expired) — the hub disarms itself,
  // and the switch should follow via the armed-state callback, not a write_state() call.
  hub.last_timeout_callback_();

  EXPECT_EQ(hub.key_extraction_.key_extraction_ctx_.state, pairing_responder::ResponderState::DISARMED);
  EXPECT_FALSE(sw.get_state()) << "switch should publish off when the hub auto-disarms";
}

TEST(PlatformAcceptForeignPairingSwitch, SetupWithNoParentDoesNotCrash) {
  TestableAcceptForeignPairingSwitch sw;
  sw.setup();  // no parent set — should be a no-op, not a crash
  SUCCEED();
}
