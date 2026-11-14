#include "platform_hub_controls.h"

#include "hub_port_doubles.h"

#include <gtest/gtest.h>

using namespace esphome::home_io_control;

// ============================================================================
// PlatformAcceptForeignPairingSwitch test suite
// ============================================================================
// Hub-level switch: on/off arms/disarms the key-extraction responder through the control port, and the
// entity mirrors the hub's own auto-disarm (successful extraction / timeout) rather than only reacting to a
// user-initiated toggle. How the real hub arms and auto-disarms is covered in hub_ports_test.cpp.

/// Exposes protected write_state(bool) for direct invocation in tests.
class TestableAcceptForeignPairingSwitch : public IOHomeAcceptForeignPairingSwitch {
 public:
  using IOHomeAcceptForeignPairingSwitch::write_state;
};

TEST(PlatformAcceptForeignPairingSwitch, WriteStateTrueArmsHubAndPublishesOn) {
  test::ControlPortDouble hub;
  TestableAcceptForeignPairingSwitch sw;
  sw.set_parent(&hub);

  sw.write_state(true);

  EXPECT_TRUE(hub.key_extraction_armed) << "write_state(true) should arm the hub's key-extraction responder";
  EXPECT_FALSE(hub.oneway_key_adoption_armed) << "it must not arm the 1W adoption listener";
  EXPECT_TRUE(sw.get_state()) << "switch should publish on immediately";
}

TEST(PlatformAcceptForeignPairingSwitch, WriteStateFalseDisarmsHubAndPublishesOff) {
  test::ControlPortDouble hub;
  TestableAcceptForeignPairingSwitch sw;
  sw.set_parent(&hub);

  sw.write_state(true);
  sw.write_state(false);

  EXPECT_FALSE(hub.key_extraction_armed) << "write_state(false) should disarm the hub's key-extraction responder";
  EXPECT_FALSE(sw.get_state());
}

TEST(PlatformAcceptForeignPairingSwitch, AutoDisarmUpdatesDisplayedStateWithoutWriteState) {
  test::ControlPortDouble hub;
  TestableAcceptForeignPairingSwitch sw;
  sw.set_parent(&hub);
  sw.setup();  // registers the armed-state callback

  sw.write_state(true);
  ASSERT_TRUE(sw.get_state());
  ASSERT_TRUE(static_cast<bool>(hub.key_extraction_armed_callback)) << "setup() should subscribe to the armed state";

  // The hub disarms itself (extraction window expired) and tells its subscribers; the switch follows
  // through the callback, not a write_state() call.
  hub.key_extraction_armed_callback(false);

  EXPECT_FALSE(sw.get_state()) << "switch should publish off when the hub auto-disarms";
}

TEST(PlatformAcceptForeignPairingSwitch, SetupWithNoParentDoesNotCrash) {
  TestableAcceptForeignPairingSwitch sw;
  sw.setup();  // no parent set — should be a no-op, not a crash
  SUCCEED();
}

// ============================================================================
// PlatformRecoverOneWayKeySwitch test suite
// ============================================================================
// The 1W sibling. It shares HubArmingSwitch's body with the switch above, so what still needs
// its own coverage is the binding: that this class's arm() / subscribe_armed() overrides reach
// the 1W adoption listener and not the 2W key-extraction one.

/// Exposes protected write_state(bool) for direct invocation in tests.
class TestableRecoverOneWayKeySwitch : public IOHomeRecoverOneWayKeySwitch {
 public:
  using IOHomeRecoverOneWayKeySwitch::write_state;
};

TEST(PlatformRecoverOneWayKeySwitch, WriteStateTrueArmsOneWayAdoptionAndPublishesOn) {
  test::ControlPortDouble hub;
  TestableRecoverOneWayKeySwitch sw;
  sw.set_parent(&hub);

  sw.write_state(true);

  EXPECT_TRUE(hub.oneway_key_adoption_armed) << "write_state(true) should arm the 1W adoption listener";
  EXPECT_FALSE(hub.key_extraction_armed) << "it must NOT arm the 2W key-extraction responder";
  EXPECT_TRUE(sw.get_state()) << "switch should publish on immediately";
}

TEST(PlatformRecoverOneWayKeySwitch, WriteStateFalseDisarmsOneWayAdoptionAndPublishesOff) {
  test::ControlPortDouble hub;
  TestableRecoverOneWayKeySwitch sw;
  sw.set_parent(&hub);

  sw.write_state(true);
  sw.write_state(false);

  EXPECT_FALSE(hub.oneway_key_adoption_armed) << "write_state(false) should disarm the 1W adoption listener";
  EXPECT_FALSE(sw.get_state());
}

TEST(PlatformRecoverOneWayKeySwitch, AutoDisarmUpdatesDisplayedStateWithoutWriteState) {
  test::ControlPortDouble hub;
  TestableRecoverOneWayKeySwitch sw;
  sw.set_parent(&hub);
  sw.setup();  // registers the armed-state callback through HubArmingSwitch::setup()

  sw.write_state(true);
  ASSERT_TRUE(sw.get_state());
  ASSERT_TRUE(static_cast<bool>(hub.oneway_key_adoption_armed_callback))
      << "setup() should subscribe to the armed state";

  hub.oneway_key_adoption_armed_callback(false);  // auto-off window expires; the hub disarms itself

  EXPECT_FALSE(sw.get_state()) << "switch should publish off when the hub auto-disarms";
}

TEST(PlatformRecoverOneWayKeySwitch, SetupWithNoParentDoesNotCrash) {
  TestableRecoverOneWayKeySwitch sw;
  sw.setup();  // no parent set — should be a no-op, not a crash
  SUCCEED();
}
