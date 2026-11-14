/// @file device_event_log_test.cpp
/// @brief Tests for DeviceEventLog (device_event_log.h): counting, rendering and the subscriber.

#include "device_event_log.h"

#include "test_helpers.h"

using namespace esphome::home_io_control;

namespace {

const uint8_t DEVICE_ID[NODE_ID_SIZE] = {0x05, 0x4E, 0x17};
const uint8_t HUB_ID[NODE_ID_SIZE] = {0x01, 0x02, 0x03};

IoFrame event_frame(const uint8_t *payload, uint8_t len) {
  IoFrame f{};
  init_frame(f, /*is_2w=*/true, /*start=*/true, /*end=*/true, /*low_power=*/false);
  set_dst(f, HUB_ID);
  set_src(f, DEVICE_ID);
  set_cmd(f, CMD_SET_CONFIG1_RESP, payload, len);
  return f;
}

}  // namespace

TEST(DeviceEventLog, StartsEmpty) {
  const DeviceEventLog log;
  EXPECT_EQ(log.count(), 0u);
  EXPECT_TRUE(log.last().empty());
}

TEST(DeviceEventLog, RendersCounterUptimeSourceCommandAndPayload) {
  esphome::test_clock::ManualClock clock(42000);
  DeviceEventLog log;
  const uint8_t payload[3] = {0x05, 0x60, 0x0A};

  log.record(event_frame(payload, sizeof(payload)), /*verbose=*/false);

  EXPECT_EQ(log.count(), 1u);
  EXPECT_EQ(log.last(), "#1 t=42 054E17 0x70 05 60 0A");
}

TEST(DeviceEventLog, AnEventWithoutPayloadHasNoTrailingSpace) {
  esphome::test_clock::ManualClock clock(1000);
  DeviceEventLog log;

  log.record(event_frame(nullptr, 0), /*verbose=*/false);

  EXPECT_NE(log.last().back(), ' ');
}

TEST(DeviceEventLog, ARepeatedIdenticalReportIsANewValue) {
  esphome::test_clock::ManualClock clock(1000);
  DeviceEventLog log;
  const uint8_t payload[1] = {0x01};

  log.record(event_frame(payload, 1), false);
  const std::string first = log.last();
  log.record(event_frame(payload, 1), false);

  EXPECT_EQ(log.count(), 2u);
  EXPECT_NE(log.last(), first);
  EXPECT_EQ(log.last().rfind("#2 ", 0), 0u);
}

TEST(DeviceEventLog, TheSubscriberRunsAfterEveryRecordingAndSeesTheNewText) {
  esphome::test_clock::ManualClock clock(1000);
  DeviceEventLog log;
  int calls = 0;
  std::string seen;
  log.set_callback([&]() {
    calls++;
    seen = log.last();
  });

  log.record(event_frame(nullptr, 0), false);
  log.record(event_frame(nullptr, 0), true);

  EXPECT_EQ(calls, 2);
  EXPECT_EQ(seen.rfind("#2 ", 0), 0u);
}

TEST(DeviceEventLog, RecordingWorksWithoutASubscriber) {
  DeviceEventLog log;
  log.record(event_frame(nullptr, 0), false);
  EXPECT_EQ(log.count(), 1u);
}
