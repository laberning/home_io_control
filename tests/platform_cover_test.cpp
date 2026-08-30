#include "platform_cover.h"
#include "hub_core.h"

#include "test_helpers.h"

#include <cstring>

using namespace esphome::home_io_control;
using namespace esphome::cover;

// ============================================================================
// PlatformCover test suite
// ============================================================================
/// Exposes protected control() for direct call in tests.
class TestableCover : public IOHomeCover {
 public:
  using IOHomeCover::control;
};

// IOHomeCover entity: HA ↔ IO position conversion, command queuing, and device update
// callbacks. Covers standard/inverted mapping, movement state, and unknown-position handling.

// Mock IOHomeControlComponent with minimal implementation for testing IOHomeCover
class MockHub : public test::MockPlatformHubBase {
 public:
  MockHub() {}
  ~MockHub() override = default;

  bool set_device_position(const std::string &device_id, uint8_t position) override {
    last_set_device_id_ = device_id;
    last_set_position_ = position;
    return true;
  }
  bool set_device_tilt(const std::string &device_id, uint8_t tilt_percent) override {
    last_set_device_id_ = device_id;
    last_set_tilt_ = tilt_percent;
    return true;
  }
  bool request_device_status(const std::string &device_id) override {
    last_request_device_id_ = device_id;
    return true;
  }
  bool discover_and_pair() override { return false; }
  bool set_light_state(const std::string &device_id, bool on) override {
    (void) device_id;
    (void) on;
    return false;
  }
  bool set_lock_state(const std::string &device_id, bool locked) override {
    (void) device_id;
    (void) locked;
    return false;
  }
  bool set_switch_state(const std::string &device_id, bool on) override {
    (void) device_id;
    (void) on;
    return false;
  }

  void queue_set_device_position(const std::string &device_id, uint8_t position) override {
    last_set_device_id_ = device_id;
    last_set_position_ = position;
    queued_operations_.push_back({PendingOperationType::SET_POSITION, device_id, position});
  }
  bool queue_device_command(const std::string &device_id, CoverCommand cmd) override {
    last_set_device_id_ = device_id;
    PendingOperation op{};
    op.type = PendingOperationType::DEVICE_COMMAND;
    op.device_id = device_id;
    op.command = cmd;
    queued_operations_.push_back(op);
    return true;
  }
  void queue_set_device_tilt(const std::string &device_id, uint8_t tilt_percent) override {
    last_set_device_id_ = device_id;
    last_set_tilt_ = tilt_percent;
    queued_operations_.push_back({PendingOperationType::SET_TILT, device_id, tilt_percent});
  }
  void queue_request_device_status(const std::string &device_id) override {
    queued_operations_.push_back({PendingOperationType::REQUEST_STATUS, device_id, 0});
  }
  void queue_discover_and_pair() override {
    queued_operations_.push_back({PendingOperationType::DISCOVER_AND_PAIR, "", 0});
  }
  void queue_set_light_state(const std::string &device_id, bool on) override {
    queued_operations_.push_back(
        {PendingOperationType::SET_LIGHT_STATE, device_id, static_cast<uint8_t>(on ? 0 : 100)});
  }
  void queue_set_lock_state(const std::string &device_id, bool locked) override {
    queued_operations_.push_back(
        {PendingOperationType::SET_LOCK_STATE, device_id, static_cast<uint8_t>(locked ? 100 : 0)});
  }
  void queue_set_switch_state(const std::string &device_id, bool on) override {
    queued_operations_.push_back(
        {PendingOperationType::SET_SWITCH_STATE, device_id, static_cast<uint8_t>(on ? 0 : 100)});
  }

  // Test accessors
  const std::string &last_set_device_id() const { return last_set_device_id_; }
  uint8_t last_set_position() const { return last_set_position_; }
  uint8_t last_set_tilt() const { return last_set_tilt_; }
  const std::string &last_request_device_id() const { return last_request_device_id_; }
  const std::deque<PendingOperation> &queued_operations() const { return queued_operations_; }

  // Helpers for tests
  void trigger_device_update(const std::string &device_id, const IoDevice &dev) {
    test::MockPlatformHubBase::trigger_device_update(device_id, dev, true);
  }

 private:
  std::string last_set_device_id_;
  uint8_t last_set_position_{0};
  uint8_t last_set_tilt_{0};
  std::string last_request_device_id_;
  std::deque<PendingOperation> queued_operations_;
};

// ========================================================================================
// IOHomeCover: position conversion and command sending
// ========================================================================================

TEST(PlatformCover, InvertsPositionWhenConfigured) {
  MockHub hub;
  TestableCover cover;
  cover.set_parent(&hub);
  cover.set_device_id("ABC123");
  cover.set_invert_position(true);

  // Simulate HA calling cover->control()->set_position(0.25)
  CoverCall call(&cover);
  call.set_position(0.25);
  cover.control(call);

  // With invert=true: ha_pos -> io_pos = ha_pos * 100 = 25
  EXPECT_EQ(hub.last_set_device_id(), "ABC123") << "device ID should match configured ID";
  EXPECT_EQ(hub.last_set_position(), 25u) << "inverted position 0.25 HA should map to 25 IO";
}

TEST(PlatformCover, NonInvertedPosition) {
  MockHub hub;
  TestableCover cover;
  cover.set_parent(&hub);
  cover.set_device_id("ABC123");
  cover.set_invert_position(false);

  CoverCall call(&cover);
  call.set_position(0.75);
  cover.control(call);

  // Without invert: io_pos = (1.0 - ha_pos) * 100 = 25
  EXPECT_EQ(hub.last_set_device_id(), "ABC123") << "device ID should match configured ID";
  EXPECT_EQ(hub.last_set_position(), 25u) << "non-inverted position 0.75 HA should map to 25 IO";
}

// Real-hardware regression (same root cause as PlatformLight.DimmableWriteStateRoundsInstead-
// OfTruncating): HA quantizes call position to 0-255 before it reaches us, so its "50%" is
// 128/255=0.50196, not exactly 0.5. A truncating cast turned (1-0.50196)*100=49.8 into IO
// position 49 instead of 50.
TEST(PlatformCover, ControlRoundsQuantizedPositionInsteadOfTruncating) {
  MockHub hub;
  TestableCover cover;
  cover.set_parent(&hub);
  cover.set_device_id("ABC123");
  cover.set_invert_position(false);

  CoverCall call(&cover);
  call.set_position(128.0f / 255.0f);  // HA's actual "50%" value
  cover.control(call);

  EXPECT_EQ(hub.last_set_position(), 50u) << "128/255 (HA's '50%') should round to IO position 50, not truncate to 49";
}

TEST(PlatformCover, DeviceUpdateToHAPosition) {
  MockHub hub;
  TestableCover cover;
  cover.set_parent(&hub);
  cover.set_device_id("ABC123");

  cover.setup();  // register device and callback

  // Simulate device update: position 75% (IO protocol: 75 = mostly closed), stopped
  IoDevice dev{};
  dev.position = 75.0f;
  dev.is_stopped = true;
  hub.trigger_device_update("ABC123", dev);

  // HA position = 1.0 - (io_pos/100) = 0.25 open
  EXPECT_FLOAT_EQ(cover.position, 0.25f) << "IO position 75% should map to HA 0.25 (open) when not inverted";

  // With invert=true: HA position = io_pos/100 = 0.75
  cover.set_invert_position(true);
  hub.trigger_device_update("ABC123", dev);
  EXPECT_FLOAT_EQ(cover.position, 0.75f) << "with invert=true, IO position 75% should map to HA 0.75";
}

TEST(PlatformCover, LearnedInversionAppliesWhenNoOverrideIsConfigured) {
  MockHub hub;
  TestableCover cover;
  cover.set_parent(&hub);
  cover.set_device_id("ABC123");

  cover.setup();

  IoDevice dev{};
  dev.position = 75.0f;
  dev.is_stopped = true;
  dev.inverted = true;
  hub.trigger_device_update("ABC123", dev);

  EXPECT_FLOAT_EQ(cover.position, 0.75f)
      << "when invert_position is not configured, learned device inversion should drive HA mapping";
}

TEST(PlatformCover, SetupStoresConfiguredStatusPollInterval) {
  MockHub hub;
  TestableCover cover;
  cover.set_parent(&hub);
  cover.set_device_id("ABC123");
  cover.set_status_poll_interval(3000);

  cover.setup();

  auto *dev = hub.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  EXPECT_EQ(hub.poll_policy_.get_interval("ABC123"), 3000u)
      << "entity setup should store the configured status poll interval in the shared device registry";
}

TEST(PlatformCover, SetupStoresLowPowerFlag) {
  MockHub hub;
  TestableCover cover;
  cover.set_parent(&hub);
  cover.set_device_id("ABC123");
  cover.set_low_power(true);

  cover.setup();

  auto *dev = hub.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  EXPECT_TRUE(dev->low_power) << "set_low_power(true) must reach the device registry through the binding";
}

TEST(PlatformCover, SetupDefaultsLowPowerToFalse) {
  MockHub hub;
  TestableCover cover;
  cover.set_parent(&hub);
  cover.set_device_id("ABC123");

  cover.setup();

  auto *dev = hub.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  EXPECT_FALSE(dev->low_power) << "a cover with no low_power: declaration is not low-power";
}

TEST(PlatformCover, ExplicitInvertFalseOverridesLearnedInversion) {
  MockHub hub;
  TestableCover cover;
  cover.set_parent(&hub);
  cover.set_device_id("ABC123");
  cover.set_invert_position(false);

  cover.setup();

  IoDevice dev{};
  dev.position = 75.0f;
  dev.is_stopped = true;
  dev.inverted = true;
  hub.trigger_device_update("ABC123", dev);

  EXPECT_FLOAT_EQ(cover.position, 0.25f)
      << "an explicit invert_position: false should override any inversion learned from radio metadata";
}

TEST(PlatformCover, TiltControlQueuesTiltCommand) {
  MockHub hub;
  TestableCover cover;
  cover.set_parent(&hub);
  cover.set_device_id("ABC123");

  hub.add_device("ABC123");
  auto *dev = hub.get_device("ABC123");
  ASSERT_NE(dev, nullptr) << "test device should be retrievable after add_device";
  dev->type = DeviceType::VENETIAN_BLIND;

  CoverCall call(&cover);
  call.set_tilt(0.25f);
  cover.control(call);

  EXPECT_EQ(hub.last_set_device_id(), "ABC123") << "tilt command should target the configured device";
  EXPECT_EQ(hub.last_set_tilt(), 25u) << "tilt 0.25 should map to 25% open";
  ASSERT_FALSE(hub.queued_operations().empty()) << "tilt control should enqueue an operation";
  EXPECT_EQ(hub.queued_operations().back().type, PendingOperationType::SET_TILT)
      << "queued operation should be SET_TILT";
}

// ========================================================================================
// IOHomeCover: optimistic state on HA-issued 2W commands (control())
// ========================================================================================

TEST(PlatformCover, ControlSetPositionAppliesOptimisticTargetImmediately) {
  MockHub hub;
  TestableCover cover;
  cover.set_parent(&hub);
  cover.set_device_id("ABC123");
  hub.add_device("ABC123", {DeviceType::ROLLER_SHUTTER, 0, false});

  CoverCall call(&cover);
  call.set_position(0.25);  // HA 0.25 open -> IO 75 (non-inverted)
  cover.control(call);

  const auto *dev = hub.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  EXPECT_FLOAT_EQ(dev->target, 75.0f) << "control() should apply the optimistic target before the queued command runs";
  EXPECT_FALSE(dev->is_stopped) << "optimistic apply should mark the device as moving";
}

TEST(PlatformCover, ControlStopClearsOptimisticTargetImmediately) {
  MockHub hub;
  TestableCover cover;
  cover.set_parent(&hub);
  cover.set_device_id("ABC123");
  hub.add_device("ABC123", {DeviceType::ROLLER_SHUTTER, 0, false});
  hub.get_device("ABC123")->target = 50.0f;
  hub.get_device("ABC123")->is_stopped = false;

  CoverCall call(&cover);
  call.set_stop(true);
  cover.control(call);

  const auto *dev = hub.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  EXPECT_EQ(dev->target, UNKNOWN_POSITION) << "control() STOP should clear any optimistic target immediately";
  EXPECT_TRUE(dev->is_stopped) << "control() STOP must also mark the device stopped, or the HA cover UI keeps "
                                  "animating movement until the confirming response arrives";
}

TEST(PlatformCover, ControlCombinedPositionAndTiltAppliesOptimisticTargetImmediately) {
  MockHub hub;
  TestableCover cover;
  cover.set_parent(&hub);
  cover.set_device_id("ABC123");
  cover.set_device_type(DeviceType::VENETIAN_BLIND);  // tilt-capable, required for the combined branch
  hub.add_device("ABC123", {DeviceType::VENETIAN_BLIND, 0, false});

  CoverCall call(&cover);
  call.set_position(0.25);  // HA 0.25 open -> IO 75 (non-inverted)
  call.set_tilt(0.5f);
  cover.control(call);

  // Note: MockHub doesn't override queue_set_device_position_and_tilt() (unlike the other
  // queue_* methods), so this only verifies the optimistic-target side effect, not queuing.
  const auto *dev = hub.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  EXPECT_FLOAT_EQ(dev->target, 75.0f)
      << "the combined position+tilt branch should apply the optimistic target immediately, same as position-only";
  EXPECT_FALSE(dev->is_stopped);
}

TEST(PlatformCover, ControlSetTiltAppliesOptimisticTiltImmediately) {
  // Issue 60: a tilt command's own reply carries no usable slat angle, so without an optimistic
  // apply the HA slider snaps back to the pre-command angle until the next status poll.
  MockHub hub;
  TestableCover cover;
  cover.set_parent(&hub);
  cover.set_device_id("ABC123");
  cover.set_device_type(DeviceType::VENETIAN_BLIND);
  hub.add_device("ABC123", {DeviceType::VENETIAN_BLIND, 0, false});

  CoverCall call(&cover);
  call.set_tilt(0.83f);
  cover.control(call);

  const auto *dev = hub.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  EXPECT_FLOAT_EQ(dev->tilt, 83.0f) << "control() should apply the optimistic tilt before the queued command runs";
  EXPECT_TRUE(dev->is_stopped) << "a tilt-only command must not start the HA open/close animation";
  EXPECT_EQ(dev->target, UNKNOWN_POSITION) << "a tilt-only command must not invent a position target";
  EXPECT_EQ(hub.last_set_tilt(), 83u) << "the command itself should still be queued as normal";
}

TEST(PlatformCover, ControlCombinedPositionAndTiltAppliesOptimisticTiltToo) {
  MockHub hub;
  TestableCover cover;
  cover.set_parent(&hub);
  cover.set_device_id("ABC123");
  cover.set_device_type(DeviceType::VENETIAN_BLIND);
  hub.add_device("ABC123", {DeviceType::VENETIAN_BLIND, 0, false});

  CoverCall call(&cover);
  call.set_position(0.25);
  call.set_tilt(0.5f);
  cover.control(call);

  const auto *dev = hub.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  EXPECT_FLOAT_EQ(dev->tilt, 50.0f) << "the combined branch should apply optimistic tilt alongside optimistic target";
  EXPECT_FLOAT_EQ(dev->target, 75.0f);
}

TEST(PlatformCover, ControlSetTiltRespectsOptimisticStateFalse) {
  MockHub hub;
  TestableCover cover;
  cover.set_parent(&hub);
  cover.set_device_id("ABC123");
  cover.set_device_type(DeviceType::VENETIAN_BLIND);
  hub.add_device("ABC123", {DeviceType::VENETIAN_BLIND, 0, false, /*optimistic_state=*/false});

  CoverCall call(&cover);
  call.set_tilt(0.83f);
  cover.control(call);

  const auto *dev = hub.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  EXPECT_EQ(dev->tilt, UNKNOWN_POSITION) << "optimistic_state=false must leave tilt untouched (poll-only)";
  EXPECT_EQ(hub.last_set_tilt(), 83u) << "the command itself should still be queued as normal";
}

TEST(PlatformCover, ControlRespectsOptimisticStateFalse) {
  MockHub hub;
  TestableCover cover;
  cover.set_parent(&hub);
  cover.set_device_id("ABC123");
  hub.add_device("ABC123", {DeviceType::ROLLER_SHUTTER, 0, false, /*optimistic_state=*/false});

  CoverCall call(&cover);
  call.set_position(0.25);
  cover.control(call);

  const auto *dev = hub.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  EXPECT_EQ(dev->target, UNKNOWN_POSITION) << "optimistic_state=false must leave target untouched (poll-only)";
  EXPECT_EQ(hub.last_set_position(), 75u) << "the command itself should still be queued as normal";
}

TEST(PlatformCover, SupportsTiltBasedOnDeviceType) {
  MockHub hub;
  TestableCover cover;
  cover.set_parent(&hub);
  cover.set_device_id("ABC123");

  // Before set_device_type: no tilt support
  EXPECT_FALSE(cover.supports_tilt()) << "UNKNOWN device type should not support tilt";

  // After set_device_type with a tilt-capable type: tilt support even without registry
  cover.set_device_type(DeviceType::EXTERNAL_VENETIAN_BLIND);
  EXPECT_TRUE(cover.supports_tilt()) << "EXTERNAL_VENETIAN_BLIND should support tilt without registry lookup";

  // Non-tilt type
  TestableCover cover2;
  cover2.set_parent(&hub);
  cover2.set_device_id("DEF456");
  cover2.set_device_type(DeviceType::ROLLER_SHUTTER);
  EXPECT_FALSE(cover2.supports_tilt()) << "ROLLER_SHUTTER should not support tilt";
}

TEST(PlatformCover, DeviceUpdatePublishesTilt) {
  MockHub hub;
  TestableCover cover;
  cover.set_parent(&hub);
  cover.set_device_id("ABC123");
  cover.set_device_type(DeviceType::VENETIAN_BLIND);

  hub.add_device("ABC123");
  auto *registered = hub.get_device("ABC123");
  ASSERT_NE(registered, nullptr) << "registered device should be available for type configuration";
  registered->type = DeviceType::VENETIAN_BLIND;

  cover.setup();

  IoDevice dev{};
  dev.type = DeviceType::VENETIAN_BLIND;
  dev.position = 75.0f;
  dev.tilt = 40.0f;
  dev.is_stopped = true;
  hub.trigger_device_update("ABC123", dev);

  EXPECT_FLOAT_EQ(cover.tilt, 0.40f) << "tilt 40% open should map to HA 0.40";
}

TEST(PlatformCover, MovingDevicePublishesPositionAndOperation) {
  MockHub hub;
  TestableCover cover;
  cover.set_parent(&hub);
  cover.set_device_id("ABC123");

  cover.setup();

  // Moving device with position 50% and target 25%: standard mapping means opening.
  IoDevice dev{};
  dev.position = 50.0f;
  dev.target = 25.0f;
  dev.is_stopped = false;
  hub.trigger_device_update("ABC123", dev);

  EXPECT_FLOAT_EQ(cover.position, 0.50f) << "moving device position should still be published to HA";
  EXPECT_EQ(cover.current_operation, COVER_OPERATION_OPENING) << "target below current should indicate opening";
}

TEST(PlatformCover, MovingDeviceWithUnknownTargetInfersDirectionFromPositionDelta) {
  MockHub hub;
  TestableCover cover;
  cover.set_parent(&hub);
  cover.set_device_id("ABC123");

  cover.setup();

  IoDevice first{};
  first.position = 50.0f;
  first.is_stopped = true;
  hub.trigger_device_update("ABC123", first);
  ASSERT_EQ(cover.current_operation, COVER_OPERATION_IDLE) << "stopped baseline update should leave the cover idle";

  IoDevice moving{};
  moving.position = 60.0f;
  moving.target = UNKNOWN_POSITION;
  moving.is_stopped = false;
  hub.trigger_device_update("ABC123", moving);

  EXPECT_EQ(cover.current_operation, COVER_OPERATION_CLOSING)
      << "with standard mapping, increasing IO position and unknown target should infer closing";
}

TEST(PlatformCover, MovingInvertedDeviceWithUnknownTargetInfersDirectionFromPositionDelta) {
  MockHub hub;
  TestableCover cover;
  cover.set_parent(&hub);
  cover.set_device_id("ABC123");
  cover.set_invert_position(true);

  cover.setup();

  IoDevice first{};
  first.position = 50.0f;
  first.is_stopped = true;
  hub.trigger_device_update("ABC123", first);

  IoDevice moving{};
  moving.position = 60.0f;
  moving.target = UNKNOWN_POSITION;
  moving.is_stopped = false;
  hub.trigger_device_update("ABC123", moving);

  EXPECT_EQ(cover.current_operation, COVER_OPERATION_OPENING)
      << "with inverted mapping, increasing IO position and unknown target should infer opening";
}

TEST(PlatformCover, StoppedDeviceReturnsToIdleOperation) {
  MockHub hub;
  TestableCover cover;
  cover.set_parent(&hub);
  cover.set_device_id("ABC123");

  cover.setup();

  IoDevice moving{};
  moving.position = 50.0f;
  moving.target = 75.0f;
  moving.is_stopped = false;
  hub.trigger_device_update("ABC123", moving);
  ASSERT_EQ(cover.current_operation, COVER_OPERATION_CLOSING)
      << "target above current should set closing before the stopped update arrives";

  IoDevice stopped{};
  stopped.position = 75.0f;
  stopped.target = 75.0f;
  stopped.is_stopped = true;
  hub.trigger_device_update("ABC123", stopped);

  EXPECT_EQ(cover.current_operation, COVER_OPERATION_IDLE) << "stopped updates should return the cover to idle";
}

TEST(PlatformCover, UnknownPositionNotPublished) {
  MockHub hub;
  TestableCover cover;
  cover.set_parent(&hub);
  cover.set_device_id("ABC123");

  cover.setup();

  IoDevice dev{};
  dev.position = UNKNOWN_POSITION;
  dev.is_stopped = true;
  hub.trigger_device_update("ABC123", dev);

  EXPECT_FLOAT_EQ(cover.position, 0.0f) << "UNKNOWN_POSITION from device should not update HA position";
}

// ============================================================================
// A device that reports its pre-command target while already flagging itself as moving must not be
// rendered as travelling in the wrong direction. Observed on a Somfy RS100: after "open", a closed
// shutter reports `position=100 target=100 moving` for about a second before its target catches
// up, and Home Assistant showed the cover briefly *closing* before it began to open. 16 such
// frames appear across the field logs.
// ============================================================================

TEST(PlatformCover, MovingWithTargetEqualToPositionIsNotReportedAsClosing) {
  MockHub hub;
  TestableCover cover;
  cover.set_parent(&hub);
  cover.set_device_id("ABC123");
  cover.setup();

  IoDevice dev{};
  dev.position = 100.0f;  // fully closed on the IO scale
  dev.target = 100.0f;    // pre-command target, not yet caught up
  dev.is_stopped = false;
  hub.trigger_device_update("ABC123", dev);

  EXPECT_NE(cover.current_operation, COVER_OPERATION_CLOSING)
      << "a closed cover told to open must never be shown as closing";
  EXPECT_EQ(cover.current_operation, COVER_OPERATION_IDLE)
      << "with no position change yet, the direction is simply not known";
}

TEST(PlatformCover, DirectionIsReportedOnceTheTargetCatchesUp) {
  MockHub hub;
  TestableCover cover;
  cover.set_parent(&hub);
  cover.set_device_id("ABC123");
  cover.setup();

  IoDevice stale{};
  stale.position = 100.0f;
  stale.target = 100.0f;
  stale.is_stopped = false;
  hub.trigger_device_update("ABC123", stale);

  IoDevice moving{};
  moving.position = 38.0f;
  moving.target = 0.0f;
  moving.is_stopped = false;
  hub.trigger_device_update("ABC123", moving);

  EXPECT_EQ(cover.current_operation, COVER_OPERATION_OPENING);
}

TEST(PlatformCover, MovingWithTargetEqualToPositionStillTracksAnObservedDelta) {
  // Equal target and position does not mean "stationary" — if the position has actually changed
  // since the last frame that delta is real direction information, and the fallback must use it.
  MockHub hub;
  TestableCover cover;
  cover.set_parent(&hub);
  cover.set_device_id("ABC123");
  cover.setup();

  IoDevice first{};
  first.position = 100.0f;
  first.target = 100.0f;
  first.is_stopped = false;
  hub.trigger_device_update("ABC123", first);

  IoDevice second{};
  second.position = 80.0f;  // actually moved toward open
  second.target = 80.0f;    // target still merely echoing the current position
  second.is_stopped = false;
  hub.trigger_device_update("ABC123", second);

  EXPECT_EQ(cover.current_operation, COVER_OPERATION_OPENING)
      << "a real position change reveals the direction even when the target is uninformative";
}

// ============================================================================
// Some actuators withhold their live position while travelling. A Somfy RS100 answers every poll
// mid-travel with current = POS_UNKNOWN (0xD4) and only reports a real value once it settles. The
// target is still reported, so the direction is knowable from the last position actually seen.
// ============================================================================

TEST(PlatformCover, MovingWithWithheldPositionStillReportsDirectionFromLastKnown) {
  MockHub hub;
  TestableCover cover;
  cover.set_parent(&hub);
  cover.set_device_id("ABC123");
  cover.setup();

  IoDevice resting{};
  resting.position = 100.0f;  // fully closed, last value the device published
  resting.target = 100.0f;
  resting.is_stopped = true;
  hub.trigger_device_update("ABC123", resting);
  ASSERT_EQ(cover.current_operation, COVER_OPERATION_IDLE);

  IoDevice moving{};
  moving.position = UNKNOWN_POSITION;  // device declines to say while travelling
  moving.target = 0.0f;                // but it does say where it is going
  moving.is_stopped = false;
  hub.trigger_device_update("ABC123", moving);

  EXPECT_EQ(cover.current_operation, COVER_OPERATION_OPENING)
      << "target 0 from a last-known 100 is opening, whether or not the live position is published";
}

TEST(PlatformCover, WithheldPositionKeepsTheLastPublishedPositionRatherThanBlanking) {
  MockHub hub;
  TestableCover cover;
  cover.set_parent(&hub);
  cover.set_device_id("ABC123");
  cover.setup();

  IoDevice resting{};
  resting.position = 100.0f;
  resting.target = 100.0f;
  resting.is_stopped = true;
  hub.trigger_device_update("ABC123", resting);
  const float shown_when_resting = cover.position;

  IoDevice moving{};
  moving.position = UNKNOWN_POSITION;
  moving.target = 0.0f;
  moving.is_stopped = false;
  hub.trigger_device_update("ABC123", moving);

  EXPECT_FLOAT_EQ(cover.position, shown_when_resting)
      << "an unknown reading must not blank a position Home Assistant was already showing";
}

TEST(PlatformCover, WithheldPositionDirectionRespectsInversion) {
  MockHub hub;
  TestableCover cover;
  cover.set_parent(&hub);
  cover.set_device_id("ABC123");
  cover.set_invert_position(true);
  cover.setup();

  IoDevice resting{};
  resting.position = 100.0f;
  resting.target = 100.0f;
  resting.is_stopped = true;
  hub.trigger_device_update("ABC123", resting);

  IoDevice moving{};
  moving.position = UNKNOWN_POSITION;
  moving.target = 0.0f;
  moving.is_stopped = false;
  hub.trigger_device_update("ABC123", moving);

  EXPECT_EQ(cover.current_operation, COVER_OPERATION_CLOSING)
      << "on an inverted device the same travel is the opposite direction";
}

TEST(PlatformCover, WithheldPositionWithNoLastKnownStaysIdle) {
  // Nothing to compare against yet — inventing a direction would be worse than admitting none.
  MockHub hub;
  TestableCover cover;
  cover.set_parent(&hub);
  cover.set_device_id("ABC123");
  cover.setup();

  IoDevice moving{};
  moving.position = UNKNOWN_POSITION;
  moving.target = 0.0f;
  moving.is_stopped = false;
  hub.trigger_device_update("ABC123", moving);

  EXPECT_EQ(cover.current_operation, COVER_OPERATION_IDLE);
}
