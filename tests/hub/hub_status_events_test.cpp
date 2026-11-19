/// @file hub_status_events_test.cpp
/// @brief Tests for the device events of hub_status.cpp: record_device_event_() logs, counts and publishes an event
/// report or any other frame the hub does not handle.

#include "hub_core.h"
#include "hub_internal.h"
#include "radio_interface.h"
#include "proto_frame.h"
#include "esphome/core/application.h"
#include "esphome/core/component.h"

#include "corpus_test_helpers.h"
#include "test_helpers.h"
#include "stubs/radio_test_common.h"

using namespace esphome::home_io_control;
using test::make_rx_packet;
using test::RxTestableComponent;
using test::setup_rx_test_component;

// ============================================================================
// Device events (event reports: 0x70 or a 0x71 with a subscribed event; and other unhandled frames)
// ============================================================================
// record_device_event_() logs, counts and publishes a frame; it never transmits and never touches
// device state (ADR 0022, ADR 0060).

namespace {

class EventTestableComponent : public RxTestableComponent {
 public:
  using IOHomeControlComponent::execute_request_and_update_;
  using IOHomeControlComponent::registry_;
};

IoFrame device_event_frame(const uint8_t src[3], const uint8_t dst[3], uint8_t cmd = CMD_SET_CONFIG1_RESP) {
  IoFrame f{};
  init_frame(f, true, false, true, false);
  set_src(f, src);
  set_dst(f, dst);
  const uint8_t payload[4] = {0x20, 0x05, 0x06, 0x2A};
  set_cmd(f, cmd, payload, sizeof(payload));
  return f;
}

const uint8_t EVENT_DEVICE_ID[3] = {0x05, 0x4E, 0x17};  // the device setup_rx_test_component() registers
const uint8_t EVENT_OTHER_ID[3] = {0x43, 0x44, 0xE3};

}  // namespace

TEST(HubStatus, DeviceEventFromARegisteredDeviceIsRecordedWithoutTransmitting) {
  EventTestableComponent comp;
  MockRadio radio;
  setup_rx_test_component(comp, radio);
  comp.set_diagnostic_probes_enabled(true);
  int published = 0;
  comp.set_device_event_callback([&]() { published++; });
  IoDevice *dev = comp.get_device("054E17");
  ASSERT_NE(dev, nullptr);
  dev->position = 42.0F;
  dev->target = 42.0F;
  dev->is_stopped = true;

  comp.process_received_packet_(make_rx_packet(device_event_frame(EVENT_DEVICE_ID, comp.node_id_)));

  EXPECT_EQ(comp.device_event_count(), 1u);
  EXPECT_EQ(published, 1);
  const std::string &text = comp.last_device_event();
  EXPECT_EQ(text.rfind("#1 t=", 0), 0u) << text;
  EXPECT_NE(text.find(" 054E17 0x70 20 05 06 2A"), std::string::npos) << text;
  EXPECT_LT(text.size(), 255u);
  EXPECT_EQ(radio.get_send_count(), 0) << "a recorded frame is never answered";
  EXPECT_FLOAT_EQ(dev->position, 42.0F);
  EXPECT_FLOAT_EQ(dev->target, 42.0F);
  EXPECT_TRUE(dev->is_stopped);
}

TEST(HubStatus, DeviceEventCounterMakesARepeatedReportANewValue) {
  EventTestableComponent comp;
  MockRadio radio;
  setup_rx_test_component(comp, radio);
  comp.set_diagnostic_probes_enabled(true);

  comp.process_received_packet_(make_rx_packet(device_event_frame(EVENT_DEVICE_ID, comp.node_id_)));
  const std::string first = comp.last_device_event();
  comp.process_received_packet_(make_rx_packet(device_event_frame(EVENT_DEVICE_ID, comp.node_id_)));

  EXPECT_EQ(comp.device_event_count(), 2u);
  EXPECT_NE(comp.last_device_event(), first) << "identical reports must still change the published state";
  EXPECT_EQ(comp.last_device_event().rfind("#2 ", 0), 0u);
}

TEST(HubStatus, DeviceEventToAnotherDestinationIsStillRecorded) {
  EventTestableComponent comp;
  MockRadio radio;
  setup_rx_test_component(comp, radio);
  comp.set_diagnostic_probes_enabled(true);

  comp.process_received_packet_(make_rx_packet(device_event_frame(EVENT_DEVICE_ID, EVENT_OTHER_ID)));

  EXPECT_EQ(comp.device_event_count(), 1u);
  EXPECT_NE(comp.last_device_event().find("054E17 0x70"), std::string::npos);
  EXPECT_EQ(radio.get_send_count(), 0);
}

TEST(HubStatus, DeviceEventFromAnUnregisteredNodeIsNotRecorded) {
  EventTestableComponent comp;
  MockRadio radio;
  setup_rx_test_component(comp, radio);
  comp.set_diagnostic_probes_enabled(true);

  comp.process_received_packet_(make_rx_packet(device_event_frame(EVENT_OTHER_ID, comp.node_id_)));

  EXPECT_EQ(comp.device_event_count(), 0u);
  EXPECT_TRUE(comp.last_device_event().empty());
}

TEST(HubStatus, DeviceEventIsRecordedWithoutDiagnosticProbesToo) {
  // The subscription outlives the probe build: the frame is still counted and published (the sensor
  // only exists with probes on); only the log level differs.
  EventTestableComponent comp;
  MockRadio radio;
  setup_rx_test_component(comp, radio);

  comp.process_received_packet_(make_rx_packet(device_event_frame(EVENT_DEVICE_ID, comp.node_id_)));

  EXPECT_EQ(comp.device_event_count(), 1u);
}

TEST(HubStatus, UnhandledFrameFromARegisteredDeviceIsRecordedOnlyWithProbesOn) {
  const uint8_t unknown_cmd = 0x5C;  // a command nothing here handles
  {
    EventTestableComponent comp;
    MockRadio radio;
    setup_rx_test_component(comp, radio);
    comp.set_diagnostic_probes_enabled(true);
    comp.process_received_packet_(make_rx_packet(device_event_frame(EVENT_DEVICE_ID, comp.node_id_, unknown_cmd)));
    EXPECT_EQ(comp.device_event_count(), 1u);
    EXPECT_NE(comp.last_device_event().find("054E17 0x5C"), std::string::npos) << comp.last_device_event();
    EXPECT_EQ(radio.get_send_count(), 0);
  }
  {
    EventTestableComponent comp;
    MockRadio radio;
    setup_rx_test_component(comp, radio);
    comp.process_received_packet_(make_rx_packet(device_event_frame(EVENT_DEVICE_ID, comp.node_id_, unknown_cmd)));
    EXPECT_EQ(comp.device_event_count(), 0u) << "probes off: only the 0x70 a subscription produces is recorded";
  }
}

TEST(HubStatus, DeviceEventInsideAPollExchangeIsRecordedAndThePollAskedAgain) {
  EventTestableComponent comp;
  MockRadio radio;
  setup_rx_test_component(comp, radio);
  IoDevice *dev = comp.get_device("054E17");
  ASSERT_NE(dev, nullptr);
  dev->position = 42.0F;
  dev->target = 42.0F;
  dev->is_stopped = true;
  IoFrame request{};
  ASSERT_TRUE(create_get_status(request, comp.node_id_, EVENT_DEVICE_ID, false));
  radio.queue_rx(make_rx_packet(device_event_frame(EVENT_DEVICE_ID, comp.node_id_)));
  // The real status reply answers the second ask: stopped at 50 %.
  IoFrame reply{};
  init_frame(reply, true, false, false, false);
  set_src(reply, EVENT_DEVICE_ID);
  set_dst(reply, comp.node_id_);
  const uint8_t payload[8] = {test::STATUS_BYTE_AT_REST, 0x00, 0x64, 0x00, 0x64, 0x00, 0x00, 0x00};
  ASSERT_TRUE(set_cmd(reply, CMD_PRIVATE_RESP, payload, sizeof(payload)));
  radio.queue_rx_hold_until_sent(2);
  radio.queue_rx(make_rx_packet(reply));

  EXPECT_TRUE(comp.execute_request_and_update_("054E17", request, false, 0, 1));

  EXPECT_EQ(radio.get_send_count(), 2) << "the poll is sent once more after the report";
  EXPECT_EQ(comp.device_event_count(), 1u);
  EXPECT_FLOAT_EQ(dev->position, 50.0F) << "the real reply is applied";
}

TEST(HubStatus, SensorEventStatusUpdateIsRecordedNotDecodedAsAPosition) {
  // The authenticated path: update_device_status_() is what a 0x71 addressed to the hub reaches
  // after its challenge. A 10-byte sensor report used to be dropped as too short for a position.
  EventTestableComponent comp;
  MockRadio radio;
  setup_rx_test_component(comp, radio);
  comp.set_diagnostic_probes_enabled(true);
  IoDevice *dev = comp.get_device("054E17");
  ASSERT_NE(dev, nullptr);
  dev->position = 42.0F;
  dev->target = 42.0F;
  dev->is_stopped = true;

  comp.update_device_status_(corpus_test::sensor_event_report(EVENT_DEVICE_ID, comp.node_id_));

  EXPECT_EQ(comp.device_event_count(), 1u);
  EXPECT_NE(comp.last_device_event().find(" 054E17 0x71 05 60 20 05 05 06 00 00 00 00"), std::string::npos)
      << comp.last_device_event();
  EXPECT_FLOAT_EQ(dev->position, 42.0F);
  EXPECT_FLOAT_EQ(dev->target, 42.0F);
  EXPECT_TRUE(dev->is_stopped);
}

TEST(HubStatus, LongSensorEventStatusUpdateIsNeverReadAsAPosition) {
  // A report long enough for the position offsets must still not be decoded as one.
  EventTestableComponent comp;
  MockRadio radio;
  setup_rx_test_component(comp, radio);
  IoDevice *dev = comp.get_device("054E17");
  ASSERT_NE(dev, nullptr);
  dev->position = 42.0F;
  dev->target = 42.0F;
  IoFrame f{};
  init_frame(f, true, false, true, false);
  set_src(f, EVENT_DEVICE_ID);
  set_dst(f, comp.node_id_);
  const uint8_t payload[16] = {0x05, 0x60, 0x20, 0x05, 0x0B, 0x06, 0x00, 0x00,
                               0x64, 0x00, 0x00, 0x64, 0x00, 0x00, 0x01, 0x00};
  set_cmd(f, CMD_STATUS_UPDATE, payload, sizeof(payload));

  comp.update_device_status_(f);

  EXPECT_EQ(comp.device_event_count(), 1u);
  EXPECT_FLOAT_EQ(dev->position, 42.0F);
  EXPECT_FLOAT_EQ(dev->target, 42.0F);
}

TEST(HubStatus, PositionStatusUpdateIsStillAppliedAndNotRecorded) {
  // Event EVENT_STATUS_CHANGE with 11 data bytes: the corpus shape of every position update.
  EventTestableComponent comp;
  MockRadio radio;
  setup_rx_test_component(comp, radio);
  comp.set_diagnostic_probes_enabled(true);
  IoDevice *dev = comp.get_device("054E17");
  ASSERT_NE(dev, nullptr);
  IoFrame f{};
  init_frame(f, true, false, true, false);
  set_src(f, EVENT_DEVICE_ID);
  set_dst(f, comp.node_id_);
  const uint8_t payload[16] = {test::STATUS_BYTE_AT_REST,
                               0x60,
                               0x10,
                               0x0A,
                               0x0B,
                               0x64,
                               0x00,
                               0x64,
                               0x00,
                               0x00,
                               0x00,
                               0xBE,
                               0xFE,
                               0xDB,
                               0x01,
                               0x00};
  set_cmd(f, CMD_STATUS_UPDATE, payload, sizeof(payload));

  comp.update_device_status_(f);

  EXPECT_EQ(comp.device_event_count(), 0u);
  EXPECT_FLOAT_EQ(dev->position, 50.0F);
}

TEST(HubStatus, OverheardSensorEventStatusUpdateIsRecordedWithoutTransmitting) {
  EventTestableComponent comp;
  MockRadio radio;
  setup_rx_test_component(comp, radio);
  comp.set_diagnostic_probes_enabled(true);

  comp.process_received_packet_(make_rx_packet(corpus_test::sensor_event_report(EVENT_DEVICE_ID, EVENT_OTHER_ID)));

  EXPECT_EQ(comp.device_event_count(), 1u);
  EXPECT_NE(comp.last_device_event().find("054E17 0x71"), std::string::npos) << comp.last_device_event();
  EXPECT_EQ(radio.get_send_count(), 0);
}

TEST(HubStatus, SensorEventStatusUpdateInsideAPollExchangeIsRecordedAndThePollAskedAgain) {
  EventTestableComponent comp;
  MockRadio radio;
  setup_rx_test_component(comp, radio);
  IoDevice *dev = comp.get_device("054E17");
  ASSERT_NE(dev, nullptr);
  dev->position = 42.0F;
  dev->target = 42.0F;
  dev->is_stopped = true;
  IoFrame request{};
  ASSERT_TRUE(create_get_status(request, comp.node_id_, EVENT_DEVICE_ID, false));
  radio.queue_rx(make_rx_packet(corpus_test::sensor_event_report(EVENT_DEVICE_ID, comp.node_id_)));
  IoFrame reply{};
  init_frame(reply, true, false, false, false);
  set_src(reply, EVENT_DEVICE_ID);
  set_dst(reply, comp.node_id_);
  const uint8_t payload[8] = {test::STATUS_BYTE_AT_REST, 0x00, 0x64, 0x00, 0x64, 0x00, 0x00, 0x00};
  ASSERT_TRUE(set_cmd(reply, CMD_PRIVATE_RESP, payload, sizeof(payload)));
  radio.queue_rx_hold_until_sent(2);
  radio.queue_rx(make_rx_packet(reply));

  EXPECT_TRUE(comp.execute_request_and_update_("054E17", request, false, 0, 1));

  EXPECT_EQ(radio.get_send_count(), 2) << "the poll is sent once more after the report";
  EXPECT_EQ(comp.device_event_count(), 1u);
  EXPECT_FLOAT_EQ(dev->position, 50.0F) << "the real reply is applied";
}

TEST(HubStatus, DeviceEventInsideAPollExchangeTwiceCountsAsUnanswered) {
  EventTestableComponent comp;
  MockRadio radio;
  setup_rx_test_component(comp, radio);
  comp.set_diagnostic_probes_enabled(true);
  IoDevice *dev = comp.get_device("054E17");
  ASSERT_NE(dev, nullptr);
  dev->position = 42.0F;
  dev->target = 42.0F;
  dev->is_stopped = true;
  IoFrame request{};
  ASSERT_TRUE(create_get_status(request, comp.node_id_, EVENT_DEVICE_ID, false));
  // The device's report answers both listen windows: it matches the exchange endpoints.
  radio.queue_rx(make_rx_packet(device_event_frame(EVENT_DEVICE_ID, comp.node_id_)));
  radio.queue_rx_hold_until_sent(2);
  radio.queue_rx(make_rx_packet(device_event_frame(EVENT_DEVICE_ID, comp.node_id_)));

  EXPECT_FALSE(comp.execute_request_and_update_("054E17", request, false, 0, 1))
      << "a 0x70 is not the reply to a status poll";

  EXPECT_EQ(radio.get_send_count(), 2) << "asked once more, never a third time";
  EXPECT_EQ(comp.device_event_count(), 2u);
  EXPECT_FLOAT_EQ(dev->position, 42.0F) << "the report must not be applied as a status";
  EXPECT_FLOAT_EQ(dev->target, 42.0F);
  EXPECT_TRUE(dev->is_stopped);
}
