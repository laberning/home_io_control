#include "platform_lock.h"
#include "hub_core.h"

#include "test_helpers.h"

using namespace esphome::home_io_control;
using namespace esphome::lock;

namespace {

class MockHubLock : public test::MockPlatformHubBase {
 public:
  MockHubLock() {}
  ~MockHubLock() override = default;

  bool set_lock_state(const std::string &device_id, bool locked) override {
    this->last_lock_device_id_ = device_id;
    this->last_locked_ = locked;
    return true;
  }

  void queue_set_lock_state(const std::string &device_id, bool locked) override {
    this->last_lock_device_id_ = device_id;
    this->last_locked_ = locked;
    this->queued_lock_ops_.push_back({device_id, locked});
  }

  void trigger_device_update(const std::string &device_id, const IoDevice &dev) {
    test::MockPlatformHubBase::trigger_device_update(device_id, dev);
  }

  const std::string &last_lock_device_id() const { return this->last_lock_device_id_; }
  bool last_locked() const { return this->last_locked_; }

 private:
  std::string last_lock_device_id_;
  bool last_locked_{false};
  std::deque<std::pair<std::string, bool>> queued_lock_ops_;
};

}  // namespace

TEST(PlatformLock, LockAndUnlockQueueSemanticCommands) {
  MockHubLock hub;
  IOHomeLock lock;
  lock.set_parent(&hub);
  lock.set_device_id("ABC123");

  auto lock_call = lock.make_call();
  lock_call.set_state(LOCK_STATE_LOCKED);
  lock_call.perform();
  EXPECT_EQ(hub.last_lock_device_id(), "ABC123") << "lock command should target configured device";
  EXPECT_TRUE(hub.last_locked()) << "LOCKED call should queue a lock command";

  auto unlock_call = lock.make_call();
  unlock_call.set_state(LOCK_STATE_UNLOCKED);
  unlock_call.perform();
  EXPECT_FALSE(hub.last_locked()) << "UNLOCKED call should queue an unlock command";
}

TEST(PlatformLock, DeviceUpdatePublishesLockedAndUnlockedStates) {
  MockHubLock hub;
  IOHomeLock lock;
  lock.set_parent(&hub);
  lock.set_device_id("ABC123");
  lock.setup();

  EXPECT_TRUE(lock.traits.supports_state(LOCK_STATE_LOCKING)) << "lock traits should advertise LOCKING";
  EXPECT_TRUE(lock.traits.supports_state(LOCK_STATE_UNLOCKING)) << "lock traits should advertise UNLOCKING";

  IoDevice dev{};
  dev.position = 80.0f;
  dev.is_stopped = true;
  hub.trigger_device_update("ABC123", dev);
  EXPECT_EQ(lock.state, LOCK_STATE_LOCKED) << "position >= 50 should map to LOCKED";

  dev.position = 10.0f;
  hub.trigger_device_update("ABC123", dev);
  EXPECT_EQ(lock.state, LOCK_STATE_UNLOCKED) << "position < 50 should map to UNLOCKED";
}

TEST(PlatformLock, MovingDevicePublishesTransitionStates) {
  MockHubLock hub;
  IOHomeLock lock;
  lock.set_parent(&hub);
  lock.set_device_id("ABC123");
  lock.setup();

  IoDevice dev{};
  dev.position = 80.0f;
  dev.target = 100.0f;
  dev.is_stopped = false;
  hub.trigger_device_update("ABC123", dev);
  EXPECT_EQ(lock.state, LOCK_STATE_LOCKING) << "moving toward the locked side should publish LOCKING";

  dev.position = 20.0f;
  dev.target = 0.0f;
  hub.trigger_device_update("ABC123", dev);
  EXPECT_EQ(lock.state, LOCK_STATE_UNLOCKING) << "moving toward the unlocked side should publish UNLOCKING";
}

TEST(PlatformLock, MovingDeviceWithoutTargetKeepsPreviousState) {
  MockHubLock hub;
  IOHomeLock lock;
  lock.set_parent(&hub);
  lock.set_device_id("ABC123");
  lock.setup();

  IoDevice settled{};
  settled.position = 80.0f;
  settled.is_stopped = true;
  hub.trigger_device_update("ABC123", settled);
  ASSERT_EQ(lock.state, LOCK_STATE_LOCKED);

  IoDevice moving{};
  moving.position = 20.0f;
  moving.target = UNKNOWN_POSITION;
  moving.is_stopped = false;
  hub.trigger_device_update("ABC123", moving);

  EXPECT_EQ(lock.state, LOCK_STATE_LOCKED)
      << "without a target hint, moving updates should not guess a transition direction";
}