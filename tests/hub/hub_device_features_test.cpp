/// @file hub_device_features_test.cpp
/// @brief What the hub sends, and refuses before sending, for a device whose features lack
///        intermediate positions or a named position (issue #163).
/// @ingroup hioc_hub

#include "hub_core.h"
#include "hub_internal.h"
#include "proto_commands.h"
#include "proto_device_types.h"

#include "test_helpers.h"
#include "stubs/radio_test_common.h"

#include <cstring>
#include <vector>

using namespace esphome::home_io_control;

namespace {

class TestableComponent : public IOHomeControlComponent {
 public:
  using IOHomeControlComponent::execute_device_command_;
  using IOHomeControlComponent::initialized_;
  using IOHomeControlComponent::node_id_;
  using IOHomeControlComponent::radio_;
  using IOHomeControlComponent::system_key_;
};

/// Wire layout: [CTRL0][CTRL1][DST 3B][SRC 3B][CMD][DATA...], so the data starts at offset 9.
constexpr size_t WIRE_DATA_OFFSET = 9;

/// Initialise the component and register "ABC123" as a device of this kind, as a cover's setup() would.
void setup_kind(TestableComponent &comp, MockRadio &radio, DeviceType type, uint8_t subtype) {
  comp.node_id_[0] = 0xC0;
  comp.node_id_[1] = 0xFF;
  comp.node_id_[2] = 0xEE;
  static const uint8_t key[] = {0xD1, 0x74, 0x34, 0x93, 0xFA, 0x94, 0x38, 0x45,
                                0xAC, 0x43, 0x50, 0xEE, 0xFF, 0x34, 0x29, 0x34};
  std::memcpy(comp.system_key_, key, AES_KEY_SIZE);
  comp.initialized_ = true;
  comp.radio_ = &radio;
  comp.add_device("ABC123", {.type = type, .subtype = subtype, .entity_class = DeviceCapabilityClass::COVER});
}

/// The EXECUTE payload of the first frame sent, or empty when nothing was sent.
std::vector<uint8_t> first_sent_payload(const MockRadio &radio) {
  if (radio.get_sent_data().empty())
    return {};
  const auto &frame = radio.get_sent_data()[0];
  return {frame.begin() + WIRE_DATA_OFFSET, frame.end()};
}

const std::vector<uint8_t> OPEN_SHORT = {0x01, 0x63, 0x00, 0x00, 0x00, 0x00};
const std::vector<uint8_t> CLOSE_SHORT = {0x01, 0x63, 0xC8, 0x00, 0x00, 0x00};
const std::vector<uint8_t> STOP_SHORT = {0x01, 0x63, 0xD2, 0x00, 0x00, 0x00};
const std::vector<uint8_t> FAVORITE_SHORT = {0x01, 0x63, 0xD8, 0x00, 0x00, 0x00};
const std::vector<uint8_t> VENT_SHORT = {0x01, 0x63, 0xD8, 0x03, 0x00, 0x00};

}  // namespace

// ============================================================================
// 5/58 garage opener: open, close and stop only (issue #163, confirmed on a real opener)
// ============================================================================

TEST(HubDeviceFeatures, GarageOpener5_58RefusesFiftyPercentWithoutTransmittingAndWithdrawsTheTarget) {
  TestableComponent comp;
  MockRadio radio;
  setup_kind(comp, radio, DeviceType::GARAGE_OPENER, SUBTYPE_MAIN_PARAMETER_ONLY);
  comp.apply_optimistic_target("ABC123", 50.0f);
  ASSERT_EQ(effective_target(*comp.get_device("ABC123")), 50.0f);

  EXPECT_FALSE(comp.set_device_position("ABC123", 50));

  EXPECT_TRUE(radio.get_sent_data().empty()) << "the opener answers a percent position with FE 28, so none is sent";
  EXPECT_EQ(effective_target(*comp.get_device("ABC123")), UNKNOWN_POSITION)
      << "the optimistic target the entity applied is rolled back";
}

TEST(HubDeviceFeatures, GarageOpener5_58SendsOpenCloseAndStopAsTheShortFrame) {
  for (const auto &[position, expected] :
       {std::pair<uint8_t, std::vector<uint8_t>>{0, OPEN_SHORT}, {100, CLOSE_SHORT}}) {
    TestableComponent comp;
    MockRadio radio;
    setup_kind(comp, radio, DeviceType::GARAGE_OPENER, SUBTYPE_MAIN_PARAMETER_ONLY);

    comp.set_device_position("ABC123", position);  // no reply queued: only the transmitted frame matters

    EXPECT_EQ(first_sent_payload(radio), expected) << +position;
  }

  TestableComponent stopping;
  MockRadio stop_radio;
  setup_kind(stopping, stop_radio, DeviceType::GARAGE_OPENER, SUBTYPE_MAIN_PARAMETER_ONLY);
  stopping.execute_device_command_("ABC123", CoverCommand::STOP);
  EXPECT_EQ(first_sent_payload(stop_radio), STOP_SHORT);
}

TEST(HubDeviceFeatures, GarageOpener5_58RefusesFavouriteAndVentByTheirBits) {
  for (const CoverCommand command : {CoverCommand::FAVORITE, CoverCommand::VENT}) {
    TestableComponent comp;
    MockRadio radio;
    setup_kind(comp, radio, DeviceType::GARAGE_OPENER, SUBTYPE_MAIN_PARAMETER_ONLY);

    EXPECT_FALSE(comp.execute_device_command_("ABC123", command)) << cover_command_name(command);
    EXPECT_TRUE(radio.get_sent_data().empty()) << cover_command_name(command);
  }
}

TEST(HubDeviceFeatures, GarageOpener5_0KeepsPercentPositionsButHasNoFavourite) {
  TestableComponent comp;
  MockRadio radio;
  setup_kind(comp, radio, DeviceType::GARAGE_OPENER, 0);

  comp.set_device_position("ABC123", 50);
  EXPECT_EQ(first_sent_payload(radio), (std::vector<uint8_t>{0x01, 0x63, 0x64, 0x00, 0x00, 0x00}));

  MockRadio favourite_radio;
  TestableComponent favourite_comp;
  setup_kind(favourite_comp, favourite_radio, DeviceType::GARAGE_OPENER, 0);
  EXPECT_FALSE(favourite_comp.execute_device_command_("ABC123", CoverCommand::FAVORITE));
  EXPECT_TRUE(favourite_radio.get_sent_data().empty());
}

// ============================================================================
// 24/58 swinging shutter and 7/58 gate opener (experimental: inferred from 5/58 and the family)
// ============================================================================

TEST(HubDeviceFeatures, SwingingShutter24_58RefusesFiftyPercentButSendsTheFavourite) {
  TestableComponent comp;
  MockRadio radio;
  setup_kind(comp, radio, DeviceType::SWINGING_SHUTTER, SUBTYPE_MAIN_PARAMETER_ONLY);

  EXPECT_FALSE(comp.set_device_position("ABC123", 50));
  EXPECT_TRUE(radio.get_sent_data().empty());

  comp.execute_device_command_("ABC123", CoverCommand::FAVORITE);
  EXPECT_EQ(first_sent_payload(radio), FAVORITE_SHORT);
}

TEST(HubDeviceFeatures, GateOpener7_58SendsTheFavouriteAndRefusesIntermediatePositions) {
  // A motor without a favourite position answers FE 28 before authenticating and nothing moves.
  TestableComponent comp;
  MockRadio radio;
  setup_kind(comp, radio, DeviceType::GATE_OPENER, SUBTYPE_MAIN_PARAMETER_ONLY);

  EXPECT_FALSE(comp.set_device_position("ABC123", 50));
  EXPECT_TRUE(radio.get_sent_data().empty());

  comp.execute_device_command_("ABC123", CoverCommand::FAVORITE);
  EXPECT_EQ(first_sent_payload(radio), FAVORITE_SHORT);
}

TEST(HubDeviceFeatures, AVentPositionIsSentOnlyToAKindWithTheVentBit) {
  TestableComponent window;
  MockRadio window_radio;
  setup_kind(window, window_radio, DeviceType::WINDOW_OPENER, 0);
  window.execute_device_command_("ABC123", CoverCommand::VENT);
  EXPECT_EQ(first_sent_payload(window_radio), VENT_SHORT);

  TestableComponent shutter;
  MockRadio shutter_radio;
  setup_kind(shutter, shutter_radio, DeviceType::ROLLER_SHUTTER, 0);
  EXPECT_FALSE(shutter.execute_device_command_("ABC123", CoverCommand::VENT));
  EXPECT_TRUE(shutter_radio.get_sent_data().empty());
}

TEST(HubDeviceFeatures, ARollerShutterKeepsItsSliderAndItsFavourite) {
  TestableComponent comp;
  MockRadio radio;
  setup_kind(comp, radio, DeviceType::ROLLER_SHUTTER, 0);

  comp.set_device_position("ABC123", 50);
  EXPECT_EQ(first_sent_payload(radio), (std::vector<uint8_t>{0x01, 0x63, 0x64, 0x00, 0x80, 0xD8, 0x06, 0x00}));

  MockRadio favourite_radio;
  TestableComponent favourite_comp;
  setup_kind(favourite_comp, favourite_radio, DeviceType::ROLLER_SHUTTER, 0);
  favourite_comp.execute_device_command_("ABC123", CoverCommand::FAVORITE);
  EXPECT_EQ(first_sent_payload(favourite_radio), FAVORITE_SHORT);
}
