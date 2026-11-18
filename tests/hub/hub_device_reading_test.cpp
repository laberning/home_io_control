/// @file hub_device_reading_test.cpp
/// @brief The hub's scheduled data-object readings: the schedule on the periodic-poll machinery, how the end of a
/// transfer read reaches the device's value store, and the capability of a device kind that has no status read.
///
/// The production reading and recipe tables have no data-object row until a device needs one, so each test gives the
/// hub a table of its own (reading_table_ and TransferReader::set_recipes()).

#include "hub_core.h"
#include "proto_commands.h"
#include "proto_constants.h"
#include "proto_timing.h"

#include "esphome/core/application.h"

#include "stubs/radio_test_common.h"
#include "test_helpers.h"

#include <gtest/gtest.h>

#include <vector>

using namespace esphome::home_io_control;
using test::TestableHubComponent;

namespace {

constexpr const char *DEVICE_ID = "ABC123";
constexpr uint32_t HOUR_MS = 3600000;

constexpr TransferOpenStep OBJECT_STEP[] = {{.object = {0x00, 0x8003}}};
constexpr uint16_t OFFSETS[] = {0x0010};
/// Recipe 0: one object, readable at offset 0x10.
constexpr TransferRecipe TEST_RECIPES[] = {{.opens = OBJECT_STEP, .offsets = OFFSETS, .max_bytes = 40}};

/// The counter sits at bytes 2 to 4 of the data read, as a marked word.
constexpr DeviceReadingRow TEST_READINGS[] = {
    {.name = "energy_total",
     .source = DataObjectSource{.recipe = 0,
                                .offset = 0x0010,
                                .first = 2,
                                .length = 3,
                                .codec = ValueCodec::MARKED_U16_BE,
                                .max_value = 0},
     .reading = DeviceReading::ENERGY_TOTAL,
     .feature = DeviceFeature::LEVELS},
};

/// The data of an object holding the counter: two bytes ahead of it, then `80 <u16 BE>`.
const std::vector<uint8_t> COUNTER_3262 = {0x11, 0x22, 0x80, 0x0C, 0xBE};
/// The same object with the counter's no-value pattern.
const std::vector<uint8_t> COUNTER_NONE = {0x11, 0x22, 0x81, 0xFF, 0xFF};

/// A hub with a roller shutter "ABC123" (it has the LEVELS capability the test reading names), on the injected
/// tables, with the clock under the test's control.
struct HubRig {
  esphome::test_clock::ManualClock clock{200000};
  TestableHubComponent comp;
  MockRadio radio;
  int notifications{0};

  HubRig() {
    comp.node_id_[0] = 0xC0;
    comp.node_id_[1] = 0xFF;
    comp.node_id_[2] = 0xEE;
    static const uint8_t key[] = {0xD1, 0x74, 0x34, 0x93, 0xFA, 0x94, 0x38, 0x45,
                                  0xAC, 0x43, 0x50, 0xEE, 0xFF, 0x34, 0x29, 0x34};
    std::memcpy(comp.system_key_, key, AES_KEY_SIZE);
    comp.initialized_ = true;
    comp.radio_ = &radio;
    comp.reading_table_ = TEST_READINGS;
    comp.transfer_reader_.set_recipes(TEST_RECIPES);
    esphome::test_random::set(0);
    DeviceConfig cfg;
    cfg.type = DeviceType::ROLLER_SHUTTER;
    comp.add_device(DEVICE_ID, cfg);
    comp.register_device_callback([this](const std::string &, const IoDevice &) { notifications++; });
    esphome::test_scheduler::clear();
  }
  ~HubRig() { esphome::test_scheduler::clear(); }

  IoDevice &device() { return *comp.get_device(DEVICE_ID); }
  std::string key() const { return TestableHubComponent::device_reading_key(DEVICE_ID, DeviceReading::ENERGY_TOTAL); }
  std::optional<float> counter() { return reading_value(device(), DeviceReading::ENERGY_TOTAL, TEST_READINGS); }

  void schedule(uint32_t interval_ms = HOUR_MS) {
    comp.set_device_reading_poll_interval(DEVICE_ID, DeviceReading::ENERGY_TOTAL, interval_ms);
  }
  void answer(TransferOutcome outcome, const std::vector<uint8_t> &data = {}) {
    comp.apply_device_reading_answer_(DEVICE_ID, DeviceReading::ENERGY_TOTAL, outcome, data);
  }
};

}  // namespace

// === The schedule ===

TEST(HubDeviceReading, TheFirstReadIsQueuedASixtySecondsAfterTheReadingIsScheduled) {
  HubRig rig;
  rig.schedule();

  esphome::test_clock::advance_ms(DEVICE_READING_FIRST_READ_DELAY_MS - 1);
  rig.comp.loop();
  EXPECT_TRUE(rig.comp.op_queue_.empty()) << "not before the minute is up";

  esphome::test_clock::advance_ms(1);
  rig.comp.loop();
  ASSERT_EQ(rig.comp.op_queue_.size(), 1u);
  EXPECT_EQ(rig.comp.op_queue_.front().type, PendingOperationType::REQUEST_DEVICE_READING);
  EXPECT_EQ(rig.comp.op_queue_.front().device_id, DEVICE_ID);
  EXPECT_EQ(rig.comp.op_queue_.front().reading, static_cast<uint8_t>(DeviceReading::ENERGY_TOTAL));
}

TEST(HubDeviceReading, TheNextReadFollowsTheIntervalWithinTheJitter) {
  HubRig rig;
  rig.schedule();
  esphome::test_clock::advance_ms(DEVICE_READING_FIRST_READ_DELAY_MS);
  const uint32_t popped_at = esphome::millis();
  rig.comp.loop();
  ASSERT_EQ(rig.comp.op_queue_.size(), 1u);

  const uint32_t next = rig.comp.device_reading_poll_policy_.get_next_poll(rig.key());
  EXPECT_GE(next - popped_at, periodic_poll_jittered_interval_ms(HOUR_MS, 0));
  EXPECT_LE(next - popped_at, periodic_poll_jittered_interval_ms(HOUR_MS, UINT32_MAX));
}

TEST(HubDeviceReading, AnIntervalBelowTheMinimumIsRaisedToIt) {
  HubRig rig;
  rig.schedule(60000);
  EXPECT_EQ(rig.comp.device_reading_poll_policy_.get_interval(rig.key()), DEVICE_READING_MIN_INTERVAL_MS);
}

TEST(HubDeviceReading, ZeroTakesTheReadingOutOfTheSchedule) {
  HubRig rig;
  rig.schedule();
  rig.schedule(0);
  EXPECT_EQ(rig.comp.device_reading_poll_policy_.get_interval(rig.key()), 0u);
  esphome::test_clock::advance_ms(2 * HOUR_MS);
  rig.comp.loop();
  EXPECT_TRUE(rig.comp.op_queue_.empty());
}

TEST(HubDeviceReading, ADeviceWithoutTheCapabilityOfTheReadingIsNotScheduled) {
  HubRig rig;
  rig.device().features = DeviceFeatures{};
  rig.schedule();
  EXPECT_EQ(rig.comp.device_reading_poll_policy_.get_interval(rig.key()), 0u);
}

TEST(HubDeviceReading, AnUnknownDeviceOrAReadingWithoutADataObjectSourceIsNotScheduled) {
  HubRig rig;
  rig.comp.set_device_reading_poll_interval("ABC999", DeviceReading::ENERGY_TOTAL, HOUR_MS);
  rig.comp.set_device_reading_poll_interval(DEVICE_ID, DeviceReading::OCCUPANCY, HOUR_MS);
  EXPECT_EQ(rig.comp.device_reading_poll_policy_.get_interval(rig.key()), 0u);
  EXPECT_EQ(rig.comp.device_reading_poll_policy_.get_interval(
                TestableHubComponent::device_reading_key("ABC999", DeviceReading::ENERGY_TOTAL)),
            0u);
  EXPECT_EQ(rig.comp.device_reading_poll_policy_.get_interval(
                TestableHubComponent::device_reading_key(DEVICE_ID, DeviceReading::OCCUPANCY)),
            0u);
}

TEST(HubDeviceReading, TheQueuedReadStartsATransferReadOfItsRecipeAndSilenceArmsTheRetry) {
  HubRig rig;
  rig.schedule();
  esphome::test_clock::advance_ms(DEVICE_READING_FIRST_READ_DELAY_MS);
  rig.comp.loop();
  ASSERT_EQ(rig.comp.op_queue_.size(), 1u);

  rig.comp.process_pending_operation_();
  // The reader starts on its own timer, never inside the call; a device that stays silent ends the read.
  for (size_t i = 0; i < EXCHANGE_MAX_ATTEMPTS; ++i)
    rig.radio.queue_tx_result(false);
  esphome::test_scheduler::run_until(esphome::millis() + 5000);

  EXPECT_GT(rig.radio.get_send_count(), 0) << "an open of the recipe went out";
  EXPECT_TRUE(rig.comp.device_reading_poll_policy_.retry_armed(rig.key()))
      << "silence arms the one quick retry, like the other periodic polls";
  EXPECT_FALSE(rig.counter().has_value());
}

TEST(HubDeviceReading, AMovingDeviceIsNotReadAndAsksAgainSoon) {
  HubRig rig;
  rig.schedule();
  rig.device().is_stopped = false;
  rig.device().execution_state = ExecutionState::EXECUTING;

  EXPECT_FALSE(rig.comp.request_device_reading(DEVICE_ID, DeviceReading::ENERGY_TOTAL));

  EXPECT_FALSE(rig.comp.transfer_reader_.transfer_open_to(DEVICE_ID));
  EXPECT_LE(rig.comp.device_reading_poll_policy_.get_next_poll(rig.key()) - esphome::millis(),
            PERIODIC_POLL_MOVING_RETRY_MS);
}

// === The value store ===

TEST(HubDeviceReading, AReadStoresTheDecodedSliceAndNotifiesTheDevicesSubscribers) {
  HubRig rig;
  rig.schedule();

  rig.answer(TransferOutcome::READ, COUNTER_3262);

  ASSERT_TRUE(rig.counter().has_value());
  EXPECT_FLOAT_EQ(*rig.counter(), 3262.0F);
  EXPECT_GE(rig.notifications, 1);
}

TEST(HubDeviceReading, ANoValueAnswerKeepsTheLastValue) {
  HubRig rig;
  rig.schedule();
  rig.answer(TransferOutcome::READ, COUNTER_3262);

  rig.answer(TransferOutcome::READ, COUNTER_NONE);

  EXPECT_FLOAT_EQ(*rig.counter(), 3262.0F);
}

TEST(HubDeviceReading, EveryFailedReadKeepsTheLastValueEvenPastTheMissLimit) {
  HubRig rig;
  rig.schedule();
  rig.answer(TransferOutcome::READ, COUNTER_3262);

  for (const TransferOutcome failure : {TransferOutcome::NO_DATA, TransferOutcome::MALFORMED, TransferOutcome::TOO_LONG,
                                        TransferOutcome::INCOMPLETE, TransferOutcome::NO_DATA}) {
    rig.answer(failure);
    ASSERT_TRUE(rig.counter().has_value()) << transfer_outcome_name(failure);
  }
  EXPECT_FLOAT_EQ(*rig.counter(), 3262.0F);
  EXPECT_GE(rig.comp.device_reading_poll_policy_.get_failures(rig.key()), PERIODIC_POLL_FAILURES_BEFORE_UNKNOWN);
}

TEST(HubDeviceReading, ASilentDeviceIsAskedOnceMoreBeforeItCountsAMiss) {
  HubRig rig;
  rig.schedule();
  rig.answer(TransferOutcome::NO_ANSWER);
  EXPECT_TRUE(rig.comp.device_reading_poll_policy_.retry_armed(rig.key()));
  EXPECT_EQ(rig.comp.device_reading_poll_policy_.get_failures(rig.key()), 0u) << "the first silence counts nothing";
}

TEST(HubDeviceReading, AnAnsweredReadClearsTheMissCount) {
  HubRig rig;
  rig.schedule();
  rig.answer(TransferOutcome::MALFORMED);
  ASSERT_EQ(rig.comp.device_reading_poll_policy_.get_failures(rig.key()), 1u);

  rig.answer(TransferOutcome::READ, COUNTER_3262);

  EXPECT_EQ(rig.comp.device_reading_poll_policy_.get_failures(rig.key()), 0u);
}

TEST(HubDeviceReading, AnEmptyOrDeclinedOpenIsAMissThatKeepsTheValueAndIsReadAtTheNextInterval) {
  for (const TransferOutcome outcome : {TransferOutcome::EMPTY, TransferOutcome::DECLINED}) {
    SCOPED_TRACE(transfer_outcome_name(outcome));
    HubRig rig;
    rig.schedule();
    rig.answer(TransferOutcome::READ, COUNTER_3262);
    const auto kept = rig.counter();
    ASSERT_TRUE(kept.has_value());

    rig.answer(outcome);

    EXPECT_FALSE(rig.device().reading_values.absent(reading_slot(DeviceReading::ENERGY_TOTAL)));
    EXPECT_EQ(rig.counter(), kept) << "the last value stays";
    EXPECT_GT(rig.comp.device_reading_poll_policy_.get_interval(rig.key()), 0u) << "still on the schedule";
    EXPECT_GE(rig.comp.device_reading_poll_policy_.get_failures(rig.key()), 1u);
    EXPECT_TRUE(rig.comp.request_device_reading(DEVICE_ID, DeviceReading::ENERGY_TOTAL)) << "read again";
  }
}

TEST(HubDeviceReading, ANoSuchObjectOrAHubSideRefusalMarksTheReadingAbsentAndItIsNotReadAgain) {
  for (const TransferOutcome outcome : {TransferOutcome::NO_SUCH_OBJECT, TransferOutcome::REFUSED}) {
    SCOPED_TRACE(transfer_outcome_name(outcome));
    HubRig rig;
    rig.schedule();
    rig.answer(TransferOutcome::READ, COUNTER_3262);

    rig.answer(outcome);

    EXPECT_TRUE(rig.device().reading_values.absent(reading_slot(DeviceReading::ENERGY_TOTAL)));
    EXPECT_FALSE(rig.counter().has_value());
    EXPECT_EQ(rig.comp.device_reading_poll_policy_.get_interval(rig.key()), 0u) << "off the schedule";
    EXPECT_FALSE(rig.comp.request_device_reading(DEVICE_ID, DeviceReading::ENERGY_TOTAL)) << "and never started";
    esphome::test_clock::advance_ms(2 * HOUR_MS);
    rig.comp.loop();
    EXPECT_TRUE(rig.comp.op_queue_.empty());
  }
}

TEST(HubDeviceReading, ARoutineReadCutOffByAnotherControllerCountsNothingAndIsRetried) {
  HubRig rig;
  rig.schedule();

  rig.answer(TransferOutcome::ABANDONED);

  EXPECT_EQ(rig.comp.device_reading_poll_policy_.get_failures(rig.key()), 0u);
  EXPECT_LE(rig.comp.device_reading_poll_policy_.get_next_poll(rig.key()) - esphome::millis(),
            PERIODIC_POLL_MISS_RETRY_MS);
}

// === A device kind with no status read ===

namespace {

/// The hub's tracking of a status poll for the device, as other controllers' traffic arms it.
bool status_poll_armed(HubRig &rig) { return rig.comp.poll_policy_.is_tracking_active(DEVICE_ID, esphome::millis()); }

}  // namespace

TEST(HubNoStatusRead, AnyOtherDeviceHasItsStatusPollArmedByAnotherControllersCommand) {
  HubRig rig;  // the control case: the same traffic does arm a poll for a device that is read
  rig.comp.act_on_foreign_frame_(DEVICE_ID, ForeignFrameKind::REQUEST_TO_DEVICE, "123456", CMD_EXECUTE);
  EXPECT_TRUE(status_poll_armed(rig));
}

TEST(HubNoStatusRead, OverheardTrafficNeverArmsAStatusPollForSuchADevice) {
  HubRig rig;
  rig.device().features |= device_feature::NO_STATUS_READ;

  rig.comp.act_on_foreign_frame_(DEVICE_ID, ForeignFrameKind::REQUEST_TO_DEVICE, "123456", CMD_EXECUTE);
  rig.comp.act_on_foreign_frame_(DEVICE_ID, ForeignFrameKind::DEVICE_CHALLENGE_TO_OTHER, "123456", CMD_CHALLENGE_REQ);

  EXPECT_FALSE(status_poll_armed(rig));
  esphome::test_scheduler::run_until(esphome::millis() + 60000);
  EXPECT_TRUE(rig.comp.op_queue_.empty());
  EXPECT_EQ(rig.radio.get_send_count(), 0);
}

TEST(HubNoStatusRead, AnyOtherDevicesStatusReadIsQueued) {
  HubRig rig;
  rig.comp.queue_request_device_status(DEVICE_ID);
  EXPECT_EQ(rig.comp.op_queue_.size(), 1u);
}

TEST(HubNoStatusRead, NoStatusReadIsQueuedOrSentForSuchADevice) {
  HubRig rig;
  rig.device().features |= device_feature::NO_STATUS_READ;
  rig.comp.queue_request_device_status(DEVICE_ID);
  EXPECT_TRUE(rig.comp.op_queue_.empty());
  EXPECT_FALSE(rig.comp.request_device_status(DEVICE_ID));
  EXPECT_EQ(rig.radio.get_send_count(), 0);
}

TEST(HubNoStatusRead, StatusTrackingIsNotBegunForSuchADevice) {
  HubRig rig;
  rig.device().features |= device_feature::NO_STATUS_READ;
  rig.comp.begin_status_poll_tracking_(DEVICE_ID, 0);
  EXPECT_FALSE(status_poll_armed(rig));
}
