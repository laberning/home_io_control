/// @file hub_dhw_test.cpp
/// @brief The hub's water heater path: the poll (status read plus family-0x00 registers), the status reply's
/// target with no cover side effects, and the target and mode writes. Replies are shaped on the Atlantic
/// Calypso Connecte's (issue #198, corpus atlantic_calypso_*).

#include "hub_core.h"
#include "hub_decisions.h"
#include "hub_internal.h"
#include "proto_commands.h"
#include "proto_dhw.h"
#include "proto_frame.h"
#include "status_poll_policy.h"

#include "test_helpers.h"
#include "stubs/radio_test_common.h"

#include <cmath>
#include <cstring>
#include <vector>

using namespace esphome::home_io_control;

namespace {

class TestableComponent : public IOHomeControlComponent {
 public:
  using IOHomeControlComponent::initialized_;
  using IOHomeControlComponent::node_id_;
  using IOHomeControlComponent::poll_dhw_device_;
  using IOHomeControlComponent::poll_policy_;
  using IOHomeControlComponent::radio_;
  using IOHomeControlComponent::read_dhw_register_;
  using IOHomeControlComponent::system_key_;
};

constexpr uint8_t DEVICE_NODE_ID[3] = {0xAB, 0xC1, 0x23};

/// Initialise the component and register "ABC123" as an entity of `entity_class` declaring `type`.
IoDevice *make_device(TestableComponent &comp, MockRadio &radio, DeviceType type,
                      DeviceCapabilityClass entity_class = DeviceCapabilityClass::WATER_HEATER, uint8_t subtype = 0) {
  comp.node_id_[0] = 0xC0;
  comp.node_id_[1] = 0xFF;
  comp.node_id_[2] = 0xEE;
  static const uint8_t key[] = {0xD1, 0x74, 0x34, 0x93, 0xFA, 0x94, 0x38, 0x45,
                                0xAC, 0x43, 0x50, 0xEE, 0xFF, 0x34, 0x29, 0x34};
  std::memcpy(comp.system_key_, key, AES_KEY_SIZE);
  comp.initialized_ = true;
  comp.radio_ = &radio;
  DeviceConfig config;
  config.type = type;
  config.subtype = subtype;
  config.entity_class = entity_class;
  comp.add_device("ABC123", config);
  return comp.get_device("ABC123");
}

IoDevice *make_water_heater(TestableComponent &comp, MockRadio &radio) {
  return make_device(comp, radio, DeviceType::DOMESTIC_HOT_WATER);
}

/// A frame from the heater with `cmd` and `payload`.
IoFrame from_device(const uint8_t dst[3], uint8_t cmd, const std::vector<uint8_t> &payload) {
  IoFrame f{};
  init_frame(f, true, false, true, false);
  set_dst(f, dst);
  set_src(f, DEVICE_NODE_ID);
  set_cmd(f, cmd, payload.data(), payload.size());
  return f;
}

void queue(TestableComponent &comp, MockRadio &radio, uint8_t cmd, const std::vector<uint8_t> &payload) {
  radio.queue_rx(test::make_rx_packet(from_device(comp.node_id_, cmd, payload)));
}

/// The status reply the Calypso sends for a target `mp` (corpus atlantic_calypso_probe_status_mp_fp).
void queue_status(TestableComponent &comp, MockRadio &radio, uint16_t mp, uint8_t flags = 0x2D) {
  queue(comp, radio, CMD_PRIVATE_RESP,
        {flags, 0x00, static_cast<uint8_t>(mp >> 8U), static_cast<uint8_t>(mp & 0xFFU), 0x00, 0x00});
}

void queue_register(TestableComponent &comp, MockRadio &radio, uint8_t index, const std::vector<uint8_t> &value) {
  std::vector<uint8_t> payload = {0x0C, 0x60, 0x00, index};
  payload.insert(payload.end(), value.begin(), value.end());
  queue(comp, radio, CMD_WRITE_PRIVATE_ACK, payload);
}

/// Queue one whole poll's replies with the values of the reporter's boost run (corpus).
/// @param status_flags Flags byte of the status reply; the Calypso's is 0x2D.
void queue_full_poll(TestableComponent &comp, MockRadio &radio, bool with_brand, uint8_t status_flags = 0x2D) {
  queue_status(comp, radio, 0xA1E8, status_flags);                                               // 54 C
  queue_register(comp, radio, DHW_REG_WATER_TEMPERATURE, {0x02, 0x45, 0x00, 0x5C, 0x02, 0x78});  // 58.1 C
  queue_register(comp, radio, DHW_REG_MODE, {0x01});                                             // manual, ECO on
  queue_register(comp, radio, DHW_REG_HEATING, {0x01, 0xFF, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00});
  queue_register(comp, radio, DHW_REG_BOOST, {0x01});
  queue_register(comp, radio, DHW_REG_ENERGY, {0x00, 0x1B, 0xF3, 0x42});
  if (with_brand)
    queue_register(comp, radio, DHW_REG_BRAND, {0x41});
}

/// The register a sent CMD_WRITE_PRIVATE frame reads (`0C 60 00 <idx>` starts at offset 9).
uint8_t sent_register(const std::vector<uint8_t> &tx) { return tx.at(12); }

}  // namespace

// === The poll ===

TEST(HubDhw, ThePollReadsTheStatusThenTheRegistersThenTheBrandOnce) {
  TestableComponent comp;
  MockRadio radio;
  IoDevice *dev = make_water_heater(comp, radio);
  ASSERT_TRUE(has_feature(dev->features, device_feature::WATER_HEATER));
  queue_full_poll(comp, radio, /*with_brand=*/true);

  EXPECT_TRUE(comp.request_device_status("ABC123"));

  const auto &sent = radio.get_sent_data();
  ASSERT_EQ(sent.size(), 7u);
  EXPECT_EQ(sent[0][8], CMD_PRIVATE) << "the target comes from the status read";
  EXPECT_EQ(sent[0][9], PRIVATE_STATUS_MP_FP) << "the status read the heater has answered";
  const uint8_t expected[] = {
      DHW_REG_WATER_TEMPERATURE, DHW_REG_MODE, DHW_REG_HEATING, DHW_REG_BOOST, DHW_REG_ENERGY, DHW_REG_BRAND};
  for (size_t i = 0; i < std::size(expected); i++) {
    const auto &tx = sent[i + 1];
    EXPECT_EQ(tx[8], CMD_WRITE_PRIVATE) << i;
    EXPECT_EQ(tx[9], HEATING_PAYLOAD_PREFIX) << i;
    EXPECT_EQ(tx[10], HEATING_DIRECTION_GET) << "a poll only reads: " << i;
    EXPECT_EQ(tx[11], DHW_REGISTER_HIGH_BYTE) << "family 0x00: " << i;
    EXPECT_EQ(sent_register(tx), expected[i]) << i;
  }

  EXPECT_FLOAT_EQ(dev->dhw.target_c, 54.0F);
  EXPECT_FLOAT_EQ(dev->dhw.water_c, 58.1F);
  EXPECT_EQ(dev->dhw.mode, static_cast<int16_t>(DhwMode::MANUAL_ECO_ON));
  EXPECT_EQ(dev->dhw.heating, 1);
  EXPECT_EQ(dev->dhw.element, 1);
  EXPECT_EQ(dev->dhw.heat_pump, 1);
  EXPECT_EQ(dev->dhw.boost, 1);
  EXPECT_EQ(dev->dhw.energy_wh, 1831746);
  EXPECT_EQ(dev->dhw.brand, 'A');
  EXPECT_FALSE(comp.poll_policy_.is_tracking_active("ABC123", esphome::millis())) << "a good poll closes its window";
  EXPECT_EQ(comp.last_timeout_id_, decisions::heating_poll_timer_id(dev->node_id)) << "the cadence is armed";
  EXPECT_EQ(comp.last_timeout_ms_, HEATING_POLL_DEFAULT_INTERVAL_MS);

  // The brand is read once: the next poll asks for the status and the five registers only.
  const size_t before = sent.size();
  queue_full_poll(comp, radio, /*with_brand=*/false);
  EXPECT_TRUE(comp.request_device_status("ABC123"));
  EXPECT_EQ(radio.get_sent_data().size() - before, 6u);
}

TEST(HubDhw, AStatusReplyChangesOnlyTheWaterHeaterReadings) {
  TestableComponent comp;
  MockRadio radio;
  IoDevice *dev = make_water_heater(comp, radio);
  // A flags byte without STATUS_STOPPED, which a cover would read as moving, and a main parameter a cover
  // would read as a position: neither may reach the cover state.
  queue_full_poll(comp, radio, /*with_brand=*/true, /*status_flags=*/0x00);

  EXPECT_TRUE(comp.request_device_status("ABC123"));

  EXPECT_EQ(dev->position, UNKNOWN_POSITION) << "no position is decoded from a water heater";
  EXPECT_EQ(dev->target, UNKNOWN_POSITION);
  EXPECT_TRUE(dev->is_stopped) << "and nothing says it moves";
  EXPECT_EQ(dev->last_moving_evidence_ms, 0u);
  EXPECT_FALSE(dev->stop_candidate.armed());
  EXPECT_EQ(dev->battery, BatteryState::NOT_REPORTED);
  EXPECT_TRUE(std::isnan(dev->heating.setpoint_c)) << "the heater readings are a radiator's, never touched";
  EXPECT_EQ(dev->heating.main_parameter, -1);
  EXPECT_EQ(comp.poll_policy_.get_next_update("ABC123"), 0u) << "no follow-up poll for a 'moving' report";
}

TEST(HubDhw, AMainParameterThatIsNotATemperatureLeavesTheTargetUnknown) {
  TestableComponent comp;
  MockRadio radio;
  IoDevice *dev = make_water_heater(comp, radio);
  dev->dhw.target_c = 54.0F;
  queue_status(comp, radio, 0xD812);

  comp.request_device_status("ABC123");  // the registers are not answered; only the status reply matters here

  EXPECT_TRUE(std::isnan(dev->dhw.target_c));
}

TEST(HubDhw, AnUnansweredStatusReadFailsThePollAndMovesItOntoTheLadder) {
  TestableComponent comp;
  MockRadio radio;
  IoDevice *dev = make_water_heater(comp, radio);

  EXPECT_FALSE(comp.request_device_status("ABC123"));

  ASSERT_FALSE(radio.get_sent_data().empty());
  for (const auto &tx : radio.get_sent_data())
    EXPECT_EQ(tx[8], CMD_PRIVATE) << "no register is read after the status read failed";
  EXPECT_LE(radio.get_sent_data().size(), SCHEDULED_POLL_MAX_TRIES) << "the scheduled-poll try band";
  EXPECT_EQ(comp.poll_policy_.get_status_poll_failures("ABC123"), 1u);
  EXPECT_NE(comp.poll_policy_.get_next_update("ABC123"), 0u) << "a retry is scheduled";
  EXPECT_EQ(comp.last_timeout_id_, decisions::heating_poll_timer_id(dev->node_id)) << "the cadence continues";
}

TEST(HubDhw, AMissedRegisterStopsThePollAndKeepsWhatWasRead) {
  TestableComponent comp;
  MockRadio radio;
  IoDevice *dev = make_water_heater(comp, radio);
  queue_status(comp, radio, 0xC800);
  queue_register(comp, radio, DHW_REG_WATER_TEMPERATURE, {0x01, 0xF0});
  // The mode register never answers.

  EXPECT_FALSE(comp.request_device_status("ABC123"));

  EXPECT_FLOAT_EQ(dev->dhw.target_c, 62.0F);
  EXPECT_FLOAT_EQ(dev->dhw.water_c, 49.6F);
  EXPECT_EQ(dev->dhw.mode, -1);
  for (size_t i = 2; i < radio.get_sent_data().size(); i++)
    EXPECT_EQ(sent_register(radio.get_sent_data()[i]), DHW_REG_MODE) << "only the missed register is retried";
  EXPECT_EQ(comp.poll_policy_.get_status_poll_failures("ABC123"), 1u);
}

TEST(HubDhw, AModeValueNeverSeenLeavesTheModeAndThePollGoesOn) {
  TestableComponent comp;
  MockRadio radio;
  IoDevice *dev = make_water_heater(comp, radio);
  dev->dhw.mode = static_cast<int16_t>(DhwMode::AUTO);
  queue_status(comp, radio, 0xBE7A);
  queue_register(comp, radio, DHW_REG_WATER_TEMPERATURE, {0x01, 0xF0});
  queue_register(comp, radio, DHW_REG_MODE, {0x03});
  queue_register(comp, radio, DHW_REG_HEATING, {0x00, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
  queue_register(comp, radio, DHW_REG_BOOST, {0x00});
  queue_register(comp, radio, DHW_REG_ENERGY, {0x00, 0x1B, 0xF3, 0x97});
  queue_register(comp, radio, DHW_REG_BRAND, {});  // an empty answer: still an answer

  EXPECT_TRUE(comp.request_device_status("ABC123"));

  EXPECT_EQ(dev->dhw.mode, static_cast<int16_t>(DhwMode::AUTO));
  EXPECT_EQ(dev->dhw.heating, 0);
  EXPECT_EQ(dev->dhw.heat_pump, 0);
  EXPECT_EQ(dev->dhw.energy_wh, 1831831);
  EXPECT_EQ(dev->dhw.brand, -1) << "an empty brand is read again next time";
}

// === Who gets the water heater path ===

TEST(HubDhw, AWaterHeaterDeclaredOnAnotherPlatformGetsNoneOfIt) {
  TestableComponent comp;
  MockRadio radio;
  IoDevice *dev = make_device(comp, radio, DeviceType::DOMESTIC_HOT_WATER, DeviceCapabilityClass::SWITCH);
  EXPECT_FALSE(has_feature(dev->features, device_feature::WATER_HEATER));

  EXPECT_FALSE(comp.request_device_status("ABC123"));
  EXPECT_FALSE(comp.poll_dhw_device_("ABC123"));
  EXPECT_FALSE(comp.set_water_heater_target("ABC123", 54.0F));
  EXPECT_FALSE(comp.set_water_heater_mode("ABC123", DhwMode::AUTO));
  IoFrame response;
  EXPECT_FALSE(comp.read_dhw_register_("ABC123", DHW_REG_MODE, "dhw_mode", response, 1));
  EXPECT_TRUE(radio.get_sent_data().empty());
}

TEST(HubDhw, ARadiatorAndACoverAreNeverSentAFamilyZeroFrame) {
  for (const DeviceType type : {DeviceType::ELECTRICAL_HEATER, DeviceType::ROLLER_SHUTTER}) {
    TestableComponent comp;
    MockRadio radio;
    make_device(comp, radio, type, DeviceCapabilityClass::UNKNOWN, type == DeviceType::ELECTRICAL_HEATER ? 1 : 0);
    EXPECT_FALSE(comp.poll_dhw_device_("ABC123"));
    EXPECT_FALSE(comp.set_water_heater_target("ABC123", 54.0F));
    EXPECT_FALSE(comp.set_water_heater_mode("ABC123", DhwMode::AUTO));
    IoFrame response;
    EXPECT_FALSE(comp.read_dhw_register_("ABC123", DHW_REG_MODE, "dhw_mode", response, 1));
    EXPECT_TRUE(radio.get_sent_data().empty()) << static_cast<unsigned>(type);
  }
}

// === Writes ===

TEST(HubDhw, TheTargetIsAMainParameterOnlyExecute) {
  TestableComponent comp;
  MockRadio radio;
  IoDevice *dev = make_water_heater(comp, radio);
  // The ack of an EXECUTE echoes the request; its bytes are not a report and are not decoded.
  queue_status(comp, radio, 0x0000);

  EXPECT_TRUE(comp.set_water_heater_target("ABC123", 54.0F));

  ASSERT_GE(radio.get_sent_data().size(), 1u);
  const auto &tx = radio.get_sent_data().front();
  uint8_t expected_raw[64];
  IoFrame expected;
  ASSERT_TRUE(create_execute_heating_main_parameter(expected, comp.node_id_, dev->node_id, false, 0xA1E8));
  const uint8_t expected_len = serialize(expected, expected_raw, sizeof(expected_raw));
  EXPECT_EQ(tx, std::vector<uint8_t>(expected_raw, expected_raw + expected_len));
  EXPECT_EQ(tx[8], CMD_EXECUTE);
  EXPECT_EQ(tx[11], 0xA1);
  EXPECT_EQ(tx[12], 0xE8);
  EXPECT_TRUE(std::isnan(dev->dhw.target_c)) << "the ack's main parameter is not taken as the target";
  EXPECT_EQ(dev->last_moving_evidence_ms, 0u) << "a water heater never moves";
  EXPECT_EQ(comp.last_timeout_id_, decisions::heating_poll_timer_id(dev->node_id));
  EXPECT_EQ(comp.last_timeout_ms_, HEATING_POST_WRITE_POLL_DELAY_MS) << "the heating state is read again soon";
  EXPECT_NE(comp.poll_policy_.get_next_update("ABC123"), 0u) << "and the settle poll reads the target back";
}

TEST(HubDhw, ATargetOutsideTheHeatersRangeIsNeverSent) {
  TestableComponent comp;
  MockRadio radio;
  make_water_heater(comp, radio);
  EXPECT_FALSE(comp.set_water_heater_target("ABC123", 19.5F));
  EXPECT_FALSE(comp.set_water_heater_target("ABC123", 62.5F));
  EXPECT_FALSE(comp.set_water_heater_target("ABC123", NAN));
  EXPECT_TRUE(radio.get_sent_data().empty());
}

TEST(HubDhw, TheModeIsARegisterWriteReadBackAtOnce) {
  TestableComponent comp;
  MockRadio radio;
  IoDevice *dev = make_water_heater(comp, radio);
  queue(comp, radio, CMD_WRITE_PRIVATE_ACK, {0x0C, 0x61, 0x00, 0x06, 0x02});
  queue_register(comp, radio, DHW_REG_MODE, {0x02});

  EXPECT_TRUE(comp.set_water_heater_mode("ABC123", DhwMode::MANUAL_ECO_OFF));

  ASSERT_EQ(radio.get_sent_data().size(), 2u);
  const auto &write = radio.get_sent_data()[0];
  EXPECT_EQ(write[8], CMD_WRITE_PRIVATE);
  EXPECT_EQ(std::vector<uint8_t>(write.begin() + 9, write.begin() + 14),
            (std::vector<uint8_t>{0x0C, 0x61, 0x00, 0x06, 0x02}));
  const auto &read = radio.get_sent_data()[1];
  EXPECT_EQ(std::vector<uint8_t>(read.begin() + 9, read.begin() + 13), (std::vector<uint8_t>{0x0C, 0x60, 0x00, 0x06}));
  EXPECT_EQ(dev->dhw.mode, static_cast<int16_t>(DhwMode::MANUAL_ECO_OFF));
  EXPECT_EQ(comp.last_timeout_ms_, HEATING_POST_WRITE_POLL_DELAY_MS);
}

TEST(HubDhw, AnUnacknowledgedModeWriteIsNotReadBack) {
  TestableComponent comp;
  MockRadio radio;
  IoDevice *dev = make_water_heater(comp, radio);
  EXPECT_FALSE(comp.set_water_heater_mode("ABC123", DhwMode::AUTO));
  for (const auto &tx : radio.get_sent_data())
    EXPECT_EQ(tx[10], HEATING_DIRECTION_SET) << "only the write is tried";
  EXPECT_LE(radio.get_sent_data().size(), static_cast<size_t>(EXCHANGE_RETRY_COUNT));
  EXPECT_EQ(dev->dhw.mode, -1);
}

// === Discovery's YAML block ===

TEST(HubDhw, DiscoveryPrintsAWaterHeaterBlockForTheKind) {
  const std::string yaml = build_device_yaml_snippet(DeviceType::DOMESTIC_HOT_WATER, 0, "F9CF0F", true, false, "");
  EXPECT_NE(yaml.find("water_heater:"), std::string::npos) << yaml;
  EXPECT_NE(yaml.find("io_device_type: \"domestic_hot_water\""), std::string::npos) << yaml;
}
