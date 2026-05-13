#include "platform_cover.h"
#include "hub_core.h"
#include "proto_frame.h"

#include "test_helpers.h"

#include <cstring>

using namespace esphome::home_io_control;
using namespace esphome::cover;

// ============================================================================
// PlatformCover test suite
// ============================================================================
// IOHomeCover entity: HA ↔ IO position conversion, command queuing, and device update
// callbacks. Covers standard/inverted mapping, movement state, and unknown-position handling.

// Mock IOHomeControlComponent with minimal implementation for testing IOHomeCover
class MockHub : public IOHomeControlComponent {
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
  bool set_switch_state(const std::string &device_id, bool on) override {
    (void) device_id;
    (void) on;
    return false;
  }

  void queue_set_device_position(const std::string &device_id, uint8_t position) override {
    last_set_device_id_ = device_id;
    last_set_position_ = position;
    queued_operations_.push_back({IOHomeControlComponent::PendingOperationType::SET_POSITION, device_id, position});
  }
  void queue_set_device_tilt(const std::string &device_id, uint8_t tilt_percent) override {
    last_set_device_id_ = device_id;
    last_set_tilt_ = tilt_percent;
    queued_operations_.push_back({IOHomeControlComponent::PendingOperationType::SET_TILT, device_id, tilt_percent});
  }
  void queue_request_device_status(const std::string &device_id) override {
    queued_operations_.push_back({IOHomeControlComponent::PendingOperationType::REQUEST_STATUS, device_id, 0});
  }
  void queue_discover_and_pair() override {
    queued_operations_.push_back({IOHomeControlComponent::PendingOperationType::DISCOVER_AND_PAIR, "", 0});
  }
  void queue_set_light_state(const std::string &device_id, bool on) override {
    queued_operations_.push_back(
        {IOHomeControlComponent::PendingOperationType::SET_LIGHT_STATE, device_id, static_cast<uint8_t>(on ? 0 : 100)});
  }
  void queue_set_switch_state(const std::string &device_id, bool on) override {
    queued_operations_.push_back({IOHomeControlComponent::PendingOperationType::SET_SWITCH_STATE, device_id,
                                  static_cast<uint8_t>(on ? 0 : 100)});
  }

  IoDevice *get_device(const std::string &device_id) override {
    auto it = devices_.find(device_id);
    return it != devices_.end() ? &it->second : nullptr;
  }
  void add_device(const std::string &device_id) override {
    if (devices_.count(device_id))
      return;
    devices_[device_id] = IoDevice{};
  }
  void register_device_callback(DeviceUpdateCallback cb) override { callbacks_.push_back(std::move(cb)); }

  // Test accessors
  const std::string &last_set_device_id() const { return last_set_device_id_; }
  uint8_t last_set_position() const { return last_set_position_; }
  uint8_t last_set_tilt() const { return last_set_tilt_; }
  const std::string &last_request_device_id() const { return last_request_device_id_; }
  const std::deque<PendingOperation> &queued_operations() const { return queued_operations_; }

  // Helpers for tests
  void trigger_device_update(const std::string &device_id, const IoDevice &dev) {
    for (auto &cb : callbacks_) {
      cb(device_id, dev);
    }
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
  IOHomeCover cover;
  cover.set_parent(&hub);
  cover.set_device_id("9CA39C");
  cover.set_invert_position(true);

  // Simulate HA calling cover->control()->set_position(0.25)
  CoverCall call(&cover);
  call.set_position(0.25);
  cover.control(call);

  // With invert=true: ha_pos -> io_pos = ha_pos * 100 = 25
  EXPECT_EQ(hub.last_set_device_id(), "9CA39C") << "device ID should match configured ID";
  EXPECT_EQ(hub.last_set_position(), 25u) << "inverted position 0.25 HA should map to 25 IO";
}

TEST(PlatformCover, NonInvertedPosition) {
  MockHub hub;
  IOHomeCover cover;
  cover.set_parent(&hub);
  cover.set_device_id("9CA39C");
  cover.set_invert_position(false);

  CoverCall call(&cover);
  call.set_position(0.75);
  cover.control(call);

  // Without invert: io_pos = (1.0 - ha_pos) * 100 = 25
  EXPECT_EQ(hub.last_set_device_id(), "9CA39C") << "device ID should match configured ID";
  EXPECT_EQ(hub.last_set_position(), 25u) << "non-inverted position 0.75 HA should map to 25 IO";
}

TEST(PlatformCover, DeviceUpdateToHAPosition) {
  MockHub hub;
  IOHomeCover cover;
  cover.set_parent(&hub);
  cover.set_device_id("9CA39C");

  cover.setup();  // register device and callback

  // Simulate device update: position 75% (IO protocol: 75 = mostly closed), stopped
  IoDevice dev{};
  dev.position = 75.0f;
  dev.is_stopped = true;
  hub.trigger_device_update("9CA39C", dev);

  // HA position = 1.0 - (io_pos/100) = 0.25 open
  EXPECT_FLOAT_EQ(cover.position, 0.25f) << "IO position 75% should map to HA 0.25 (open) when not inverted";

  // With invert=true: HA position = io_pos/100 = 0.75
  cover.set_invert_position(true);
  hub.trigger_device_update("9CA39C", dev);
  EXPECT_FLOAT_EQ(cover.position, 0.75f) << "with invert=true, IO position 75% should map to HA 0.75";
}

TEST(PlatformCover, TiltControlQueuesTiltCommand) {
  MockHub hub;
  IOHomeCover cover;
  cover.set_parent(&hub);
  cover.set_device_id("9CA39C");

  hub.add_device("9CA39C");
  auto *dev = hub.get_device("9CA39C");
  ASSERT_NE(dev, nullptr) << "test device should be retrievable after add_device";
  dev->type = DeviceType::VENETIAN_BLIND;

  CoverCall call(&cover);
  call.set_tilt(0.25f);
  cover.control(call);

  EXPECT_EQ(hub.last_set_device_id(), "9CA39C") << "tilt command should target the configured device";
  EXPECT_EQ(hub.last_set_tilt(), 25u) << "tilt 0.25 should map to 25% open";
  ASSERT_FALSE(hub.queued_operations().empty()) << "tilt control should enqueue an operation";
  EXPECT_EQ(hub.queued_operations().back().type, IOHomeControlComponent::PendingOperationType::SET_TILT)
      << "queued operation should be SET_TILT";
}

TEST(PlatformCover, DeviceUpdatePublishesTilt) {
  MockHub hub;
  IOHomeCover cover;
  cover.set_parent(&hub);
  cover.set_device_id("9CA39C");

  hub.add_device("9CA39C");
  auto *registered = hub.get_device("9CA39C");
  ASSERT_NE(registered, nullptr) << "registered device should be available for type configuration";
  registered->type = DeviceType::VENETIAN_BLIND;

  cover.setup();

  IoDevice dev{};
  dev.type = DeviceType::VENETIAN_BLIND;
  dev.position = 75.0f;
  dev.tilt = 40.0f;
  dev.is_stopped = true;
  hub.trigger_device_update("9CA39C", dev);

  EXPECT_FLOAT_EQ(cover.tilt, 0.40f) << "tilt 40% open should map to HA 0.40";
}

TEST(PlatformCover, MovingDevicePublishesPositionAndOperation) {
  MockHub hub;
  IOHomeCover cover;
  cover.set_parent(&hub);
  cover.set_device_id("9CA39C");

  cover.setup();

  // Moving device with position 50% and target 25%: standard mapping means opening.
  IoDevice dev{};
  dev.position = 50.0f;
  dev.target = 25.0f;
  dev.is_stopped = false;
  hub.trigger_device_update("9CA39C", dev);

  EXPECT_FLOAT_EQ(cover.position, 0.50f) << "moving device position should still be published to HA";
  EXPECT_EQ(cover.current_operation, COVER_OPERATION_OPENING) << "target below current should indicate opening";
}

TEST(PlatformCover, StoppedDeviceReturnsToIdleOperation) {
  MockHub hub;
  IOHomeCover cover;
  cover.set_parent(&hub);
  cover.set_device_id("9CA39C");

  cover.setup();

  IoDevice moving{};
  moving.position = 50.0f;
  moving.target = 75.0f;
  moving.is_stopped = false;
  hub.trigger_device_update("9CA39C", moving);
  ASSERT_EQ(cover.current_operation, COVER_OPERATION_CLOSING)
      << "target above current should set closing before the stopped update arrives";

  IoDevice stopped{};
  stopped.position = 75.0f;
  stopped.target = 75.0f;
  stopped.is_stopped = true;
  hub.trigger_device_update("9CA39C", stopped);

  EXPECT_EQ(cover.current_operation, COVER_OPERATION_IDLE) << "stopped updates should return the cover to idle";
}

TEST(PlatformCover, UnknownPositionNotPublished) {
  MockHub hub;
  IOHomeCover cover;
  cover.set_parent(&hub);
  cover.set_device_id("9CA39C");

  cover.setup();

  IoDevice dev{};
  dev.position = UNKNOWN_POSITION;
  dev.is_stopped = true;
  hub.trigger_device_update("9CA39C", dev);

  EXPECT_FLOAT_EQ(cover.position, UNKNOWN_POSITION) << "UNKNOWN_POSITION from device should not update HA position";
}
