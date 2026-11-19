/// @file hub_operations_test.cpp
/// @brief Tests for the position commands of hub_operations.cpp: set_device_position, the frame a command sends
/// (garage opener, extended position frame, speed block) and the capability gating.
///
/// The other subjects are in the hub_operations_*_test.cpp files, the heater commands in
/// hub_heating_operations_test.cpp.

#include "hub_core.h"
#include "hub_internal.h"
#include "proto_frame.h"
#include "proto_commands.h"
#include "proto_private_registers.h"

#include "corpus_test_helpers.h"
#include "test_helpers.h"
#include "reading_store.h"
#include "stubs/radio_test_common.h"

#include <cmath>
#include <cstring>
#include <functional>
#include <utility>
#include <vector>

#include "hub_operations_fixture.h"

using namespace esphome::home_io_control;
using namespace hub_operations_test;

// ========================================================================================
// set_device_position tests
// ========================================================================================

TEST(HubOperations, SetDevicePositionSuccess) {
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);

  // Build a valid status response and queue it
  IoFrame resp = build_status_response(comp.node_id_);
  uint8_t raw[64];
  uint8_t raw_len = serialize(resp, raw, sizeof(raw));
  RadioRxPacket pkt{};
  pkt.len = raw_len;
  memcpy(pkt.data, raw, raw_len);
  pkt.freq_hz = FREQ_CH2;
  radio.queue_rx(pkt);

  bool ok = comp.set_device_position("ABC123", 50);
  EXPECT_TRUE(ok) << "set_device_position should succeed with valid response";
}

TEST(HubOperations, SetDevicePositionRejectsUnknownDevice) {
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);

  bool ok = comp.set_device_position("000000", 50);
  EXPECT_FALSE(ok) << "set_device_position should fail for unregistered device";
}

TEST(HubOperations, SetDevicePositionRejectedForLight) {
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);
  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  test::set_kind(*dev, DeviceType::LIGHT);

  bool ok = comp.set_device_position("ABC123", 50);
  EXPECT_FALSE(ok) << "set_device_position should reject mid position for light device";
}

TEST(HubOperations, LightBinaryPositionExchangeFails) {
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);
  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  test::set_kind(*dev, DeviceType::LIGHT);

  bool ok = comp.set_device_position("ABC123", 0);
  EXPECT_FALSE(ok) << "set_device_position fails because send_and_receive_ gets no response";
}

TEST(HubOperations, SetLockStateSuccess) {
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);
  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  test::set_kind(*dev, DeviceType::LOCK);

  IoFrame resp = build_status_response(comp.node_id_);
  uint8_t raw[64];
  uint8_t raw_len = serialize(resp, raw, sizeof(raw));
  RadioRxPacket pkt{};
  pkt.len = raw_len;
  memcpy(pkt.data, raw, raw_len);
  pkt.freq_hz = FREQ_CH2;
  radio.queue_rx(pkt);

  EXPECT_TRUE(comp.set_lock_state("ABC123", true))
      << "lock devices should support lock commands via shared execute path";
}

TEST(HubOperations, SetLockStateRejectsLightDevice) {
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);
  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  test::set_kind(*dev, DeviceType::LIGHT);

  EXPECT_FALSE(comp.set_lock_state("ABC123", true)) << "non-lock devices should be rejected by lock entity gating";
}

TEST(HubOperations, SetLockStateRejectsTheCarControllerSubtypeOfTheLockType) {
  // The table gives lock subtype 2 (car controller) no entity class, so a `lock:` entry bound to it
  // is refused although the bare lock type would be accepted.
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);
  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  test::set_kind(*dev, DeviceType::LOCK, 2);

  EXPECT_FALSE(comp.set_lock_state("ABC123", true));
  EXPECT_TRUE(radio.get_tx_configs().empty()) << "a refused command puts no frame on air";
}

TEST(HubOperations, SetLockStateAcceptsTheWindowLockSubtypeOfTheLockType) {
  // Lock subtype 1 (window lock) keeps the lock class of its type, so it is driven like a lock.
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);
  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  test::set_kind(*dev, DeviceType::LOCK, 1);

  IoFrame resp = build_status_response(comp.node_id_);
  uint8_t raw[64];
  uint8_t raw_len = serialize(resp, raw, sizeof(raw));
  RadioRxPacket pkt{};
  pkt.len = raw_len;
  memcpy(pkt.data, raw, raw_len);
  pkt.freq_hz = FREQ_CH2;
  radio.queue_rx(pkt);

  EXPECT_TRUE(comp.set_lock_state("ABC123", true));
  EXPECT_FALSE(radio.get_tx_configs().empty());
}

TEST(HubOperations, StatusRequestIsRefusedForTheCarControllerSubtypeOfTheLockType) {
  // A kind with no entity class takes no EXECUTE position and is not polled with a status request.
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);
  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  test::set_kind(*dev, DeviceType::LOCK, 2);

  EXPECT_FALSE(comp.request_device_status("ABC123"));
  EXPECT_TRUE(radio.get_tx_configs().empty()) << "a refused status request puts no frame on air";
}

TEST(HubOperations, SilentDeviceSendsTheSilentExecutePayload) {
  // End-to-end: the YAML-declared per-cover flag has to reach the wire, not just the builder.
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);

  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  dev->travel_speed = TravelSpeed::SILENT;

  comp.set_device_position("ABC123", 100);

  ASSERT_FALSE(radio.get_sent_data().empty());
  const auto &frame = radio.get_sent_data().front();
  ASSERT_GE(frame.size(), 17u) << "an execute frame is 9 header bytes plus an 8-byte payload";
  EXPECT_EQ(frame[14], POS_FAVORITE) << "the secondary-target byte is unaffected";
  EXPECT_EQ(frame[15], 0x05) << "silent devices travel on the slow profile";
}

TEST(HubOperations, ACoverWithoutAChosenSpeedSendsTheShortFrame) {
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);

  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  ASSERT_EQ(dev->travel_speed, TravelSpeed::MOTOR_DEFAULT) << "a speed block is opt-in";

  comp.set_device_position("ABC123", 100);

  ASSERT_FALSE(radio.get_sent_data().empty());
  EXPECT_EQ(radio.get_sent_data().front().size(), 15u) << "9 header bytes plus the 6-byte main-parameter payload";
}

TEST(HubOperations, AFastCoverSendsTheSpeedBlockWithTheFastProfile) {
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);
  comp.get_device("ABC123")->travel_speed = TravelSpeed::FAST;

  comp.set_device_position("ABC123", 100);

  const auto &frame = radio.get_sent_data().front();
  ASSERT_GE(frame.size(), 17u) << "an execute frame is 9 header bytes plus an 8-byte payload";
  EXPECT_EQ(frame[14], POS_FAVORITE);
  EXPECT_EQ(frame[15], 0x06);
}

// ========================================================================================
// Garage opener: main-parameter-only movement (issue #163)
// ========================================================================================

namespace {

// Wire layout: [CTRL0][CTRL1][DST 3B][SRC 3B][CMD][DATA...] — data starts at offset 9.
constexpr size_t WIRE_DATA_OFFSET = 9;

std::vector<uint8_t> first_sent_payload(const MockRadio &radio) {
  if (radio.get_sent_data().empty())
    return {};
  const auto &frame = radio.get_sent_data()[0];
  return {frame.begin() + WIRE_DATA_OFFSET, frame.end()};
}

void make_garage(TestableComponent &comp, MockRadio &radio) {
  setup_cover_component(comp, radio);
  test::set_kind(*comp.get_device("ABC123"), DeviceType::GARAGE_OPENER);
}

}  // namespace

TEST(HubOperations, GarageOpenerPositionSendsMainParameterOnly) {
  TestableComponent comp;
  MockRadio radio;
  make_garage(comp, radio);

  comp.set_device_position("ABC123", 100);  // no reply queued: only the transmitted frame matters

  ASSERT_FALSE(radio.get_sent_data().empty());
  EXPECT_EQ(radio.get_sent_data()[0][WIRE_DATA_OFFSET - 1], CMD_EXECUTE);
  EXPECT_EQ(first_sent_payload(radio), (std::vector<uint8_t>{0x01, 0x63, 0xC8, 0x00, 0x00, 0x00}))
      << "a garage opener gets the 6-byte form, without the functional-parameter block";
}

TEST(HubOperations, RollerShutterPositionAtFastSpeedCarriesTheExtendedBlock) {
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);
  comp.get_device("ABC123")->travel_speed = TravelSpeed::FAST;

  comp.set_device_position("ABC123", 0);

  EXPECT_EQ(first_sent_payload(radio), (std::vector<uint8_t>{0x01, 0x63, 0x00, 0x00, 0x80, 0xD8, 0x06, 0x00}));
}

TEST(HubOperations, GarageOpenerFavoriteIsRefusedWithoutTransmitting) {
  TestableComponent comp;
  MockRadio radio;
  make_garage(comp, radio);

  EXPECT_FALSE(comp.execute_device_command_("ABC123", CoverCommand::FAVORITE));
  EXPECT_TRUE(radio.get_sent_data().empty()) << "the favourite alias is refused by the device, so it is never sent";
}

TEST(HubOperations, GarageOpenerStopIsUnchanged) {
  TestableComponent comp;
  MockRadio radio;
  make_garage(comp, radio);

  comp.execute_device_command_("ABC123", CoverCommand::STOP);

  EXPECT_EQ(first_sent_payload(radio), (std::vector<uint8_t>{0x01, 0x63, 0xD2, 0x00, 0x00, 0x00}));
}

TEST(HubOperations, GarageOpenerForceOpenUsesMainParameterAtForcePriority) {
  TestableComponent comp;
  MockRadio radio;
  make_garage(comp, radio);

  comp.execute_device_command_("ABC123", CoverCommand::FORCE_OPEN);

  const std::vector<uint8_t> payload = first_sent_payload(radio);
  ASSERT_EQ(payload.size(), 6u);
  EXPECT_EQ(payload[1], 0x03) << "force-open priority";
  EXPECT_EQ(payload[2], 0x00) << "fully open";
  EXPECT_EQ(payload[4], 0x00);
  EXPECT_EQ(payload[5], 0x00);
}

// ========================================================================================
// Capability bits decide the frame shape (device_feature::EXTENDED_POSITION_FRAME)
// ========================================================================================

TEST(HubOperations, AnUndeclaredCoverKeepsTheSliderTheSpeedBlockOnRequestAndTheFavourite) {
  TestableComponent comp;
  MockRadio radio;
  setup_declared_component(comp, radio, {.entity_class = DeviceCapabilityClass::COVER});
  const IoDevice *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  EXPECT_EQ(dev->type, DeviceType::UNKNOWN);

  comp.set_device_position("ABC123", 50);
  EXPECT_EQ(first_sent_payload(radio), (std::vector<uint8_t>{0x01, 0x63, 0x64, 0x00, 0x00, 0x00}))
      << "a cover that has not learned its type still gets a slider position, in the short form";

  MockRadio fast_radio;
  TestableComponent fast_comp;
  setup_declared_component(fast_comp, fast_radio,
                           {.entity_class = DeviceCapabilityClass::COVER, .travel_speed = TravelSpeed::FAST});
  fast_comp.set_device_position("ABC123", 50);
  EXPECT_EQ(first_sent_payload(fast_radio), (std::vector<uint8_t>{0x01, 0x63, 0x64, 0x00, 0x80, 0xD8, 0x06, 0x00}))
      << "a chosen speed puts the speed block on the frame";

  MockRadio favourite_radio;
  TestableComponent favourite_comp;
  setup_declared_component(favourite_comp, favourite_radio, {.entity_class = DeviceCapabilityClass::COVER});
  favourite_comp.execute_device_command_("ABC123", CoverCommand::FAVORITE);
  EXPECT_EQ(first_sent_payload(favourite_radio), (std::vector<uint8_t>{0x01, 0x63, 0xD8, 0x00, 0x00, 0x00}));
}

TEST(HubOperations, ALightSwitchOrLockSendsZeroAndHundredInTheShortFrameWhateverTheSpeed) {
  // These kinds have no speed to choose, so even a stray FAST never puts the speed block on the frame.
  for (const DeviceCapabilityClass entity_class :
       {DeviceCapabilityClass::LIGHT, DeviceCapabilityClass::SWITCH, DeviceCapabilityClass::LOCK}) {
    for (const TravelSpeed speed : {TravelSpeed::MOTOR_DEFAULT, TravelSpeed::FAST}) {
      TestableComponent comp;
      MockRadio radio;
      setup_declared_component(comp, radio, {.entity_class = entity_class, .travel_speed = speed});

      comp.set_device_position("ABC123", 100);
      EXPECT_EQ(first_sent_payload(radio), (std::vector<uint8_t>{0x01, 0x63, 0xC8, 0x00, 0x00, 0x00}))
          << static_cast<unsigned>(entity_class) << " " << travel_speed_name(speed);
    }
  }
}

TEST(HubOperations, AFastCoverForceOpensWithTheSpeedBlockAndADefaultCoverWithoutIt) {
  const std::vector<std::pair<TravelSpeed, std::vector<uint8_t>>> cases = {
      {TravelSpeed::MOTOR_DEFAULT, {0x01, 0x03, 0x00, 0x00, 0x00, 0x00}},
      {TravelSpeed::FAST, {0x01, 0x03, 0x00, 0x00, 0x80, 0xD8, 0x06, 0x00}},
  };
  for (const auto &[speed, payload] : cases) {
    TestableComponent comp;
    MockRadio radio;
    setup_declared_component(comp, radio, {.type = DeviceType::ROLLER_SHUTTER, .travel_speed = speed});

    comp.execute_device_command_("ABC123", CoverCommand::FORCE_OPEN);
    EXPECT_EQ(first_sent_payload(radio), payload) << travel_speed_name(speed);
  }
}

TEST(HubOperations, ASubtype58CoverHasNoTiltBecauseItTakesTheMainParameterOnly) {
  // VELUX KLF 200 API specification, Appendix 2: subtype 58 is main parameter only, so no slat angle.
  for (const DeviceType type : {DeviceType::INTERIOR_VENETIAN_BLIND, DeviceType::VERTICAL_INTERIOR_BLIND,
                                DeviceType::EXTERIOR_VENETIAN_BLIND, DeviceType::LOUVRE_BLIND}) {
    TestableComponent comp;
    MockRadio radio;
    setup_declared_component(comp, radio, {.type = type, .subtype = SUBTYPE_MAIN_PARAMETER_ONLY});

    EXPECT_FALSE(comp.set_device_tilt("ABC123", 50)) << static_cast<unsigned>(type);
    EXPECT_TRUE(radio.get_sent_data().empty());
  }
}

TEST(HubOperations, ASilentMoveToAnUnsilentableKindUsesTheShortFrame) {
  TestableComponent comp;
  MockRadio radio;
  setup_declared_component(comp, radio, {.type = DeviceType::GARAGE_OPENER, .travel_speed = TravelSpeed::SILENT});

  comp.set_device_position("ABC123", 0);
  EXPECT_EQ(first_sent_payload(radio), (std::vector<uint8_t>{0x01, 0x63, 0x00, 0x00, 0x00, 0x00}))
      << "the silent profile is a functional parameter, which a main-parameter-only device does not take";
}

TEST(HubOperations, InteriorVenetianAndDualShutterPositionsUseTheShortFrameEvenWhenSilent) {
  // VELUX KLF 200 API specification, Appendix 2: functional parameter 1 is the slat orientation on type 1 and
  // the upper curtain on type 13, not a travel speed, so neither takes the 8-byte form or `silent:`.
  for (const DeviceType type : {DeviceType::INTERIOR_VENETIAN_BLIND, DeviceType::DUAL_ROLLER_SHUTTER}) {
    TestableComponent comp;
    MockRadio radio;
    setup_declared_component(comp, radio, {.type = type, .travel_speed = TravelSpeed::SILENT});

    comp.set_device_position("ABC123", 50);
    EXPECT_EQ(first_sent_payload(radio), (std::vector<uint8_t>{0x01, 0x63, 0x64, 0x00, 0x00, 0x00}))
        << static_cast<unsigned>(type);
  }
}

TEST(HubOperations, InteriorVenetianAndDualShutterForceOpenUsesTheShortFrameAtForcePriority) {
  for (const DeviceType type : {DeviceType::INTERIOR_VENETIAN_BLIND, DeviceType::DUAL_ROLLER_SHUTTER}) {
    TestableComponent comp;
    MockRadio radio;
    setup_declared_component(comp, radio, {.type = type});

    comp.execute_device_command_("ABC123", CoverCommand::FORCE_OPEN);
    EXPECT_EQ(first_sent_payload(radio), (std::vector<uint8_t>{0x01, 0x03, 0x00, 0x00, 0x00, 0x00}))
        << static_cast<unsigned>(type);
  }
}

TEST(HubOperations, ARollerShutterAtFastSpeedStillSendsTheEightByteFrame) {
  TestableComponent comp;
  MockRadio radio;
  setup_declared_component(comp, radio, {.type = DeviceType::ROLLER_SHUTTER, .travel_speed = TravelSpeed::FAST});

  comp.set_device_position("ABC123", 50);
  EXPECT_EQ(first_sent_payload(radio), (std::vector<uint8_t>{0x01, 0x63, 0x64, 0x00, 0x80, 0xD8, 0x06, 0x00}));
}

TEST(HubOperations, EachTravelSpeedSendsItsOwnPositionFrame) {
  // A VELUX KLR 200 pad moves a window with the 6-byte frame; the window then runs at its own speed
  // (44.5 s full travel), against 26.5 s on profile 0x06 and about 65 s on 0x05.
  const std::vector<std::pair<TravelSpeed, std::vector<uint8_t>>> cases = {
      {TravelSpeed::MOTOR_DEFAULT, {0x01, 0x63, 0x64, 0x00, 0x00, 0x00}},
      {TravelSpeed::SILENT, {0x01, 0x63, 0x64, 0x00, 0x80, 0xD8, 0x05, 0x00}},
      {TravelSpeed::FAST, {0x01, 0x63, 0x64, 0x00, 0x80, 0xD8, 0x06, 0x00}},
  };
  for (const auto &[speed, payload] : cases) {
    TestableComponent comp;
    MockRadio radio;
    setup_declared_component(comp, radio, {.type = DeviceType::WINDOW_OPENER, .travel_speed = speed});

    comp.set_device_position("ABC123", 50);
    EXPECT_EQ(first_sent_payload(radio), payload) << travel_speed_name(speed);
  }
}

TEST(HubOperations, ASpeedSetAtRuntimeShapesTheNextMove) {
  TestableComponent comp;
  MockRadio radio;
  setup_declared_component(comp, radio, {.type = DeviceType::WINDOW_OPENER});

  comp.set_device_travel_speed("ABC123", TravelSpeed::MOTOR_DEFAULT);
  comp.set_device_position("ABC123", 100);
  EXPECT_EQ(first_sent_payload(radio), (std::vector<uint8_t>{0x01, 0x63, 0xC8, 0x00, 0x00, 0x00}))
      << "the select changes what the next move sends";
}

TEST(HubOperations, OnlyTheSilentSpeedExtendsTheFavoritePosition) {
  // The favorite position goes out in the 6-byte form unless silent; a Somfy hub sends exactly that pair.
  const std::vector<std::pair<TravelSpeed, std::vector<uint8_t>>> cases = {
      {TravelSpeed::MOTOR_DEFAULT, {0x01, 0x63, 0xD8, 0x00, 0x00, 0x00}},
      {TravelSpeed::SILENT, {0x01, 0x63, 0xD8, 0x00, 0x80, 0xD8, 0x05, 0x00}},
      {TravelSpeed::FAST, {0x01, 0x63, 0xD8, 0x00, 0x00, 0x00}},
  };
  for (const auto &[speed, payload] : cases) {
    TestableComponent comp;
    MockRadio radio;
    setup_declared_component(comp, radio, {.type = DeviceType::WINDOW_OPENER, .travel_speed = speed});

    comp.execute_device_command_("ABC123", CoverCommand::FAVORITE);
    EXPECT_EQ(first_sent_payload(radio), payload) << travel_speed_name(speed);
  }
}

TEST(HubOperations, AnInteriorVenetianBlindKeepsTheTiltFrameOnFunctionalParameterThree) {
  TestableComponent comp;
  MockRadio radio;
  setup_declared_component(comp, radio, {.type = DeviceType::INTERIOR_VENETIAN_BLIND});

  comp.set_device_tilt("ABC123", 25);  // no device answers; the sent frame is what is asserted
  const std::vector<uint8_t> payload = first_sent_payload(radio);
  ASSERT_EQ(payload.size(), 8u);
  EXPECT_EQ(payload[2], POS_UNKNOWN) << "tilt leaves the main position alone";
  EXPECT_EQ(payload[4], STATUS_TILT_SELECTOR);
  EXPECT_EQ(payload[5], 0x96) << "25 % open is 75 % closed, 0x9600";
}

TEST(HubOperations, AVerticalInteriorBlindHasNoTilt) {
  // VELUX KLF 200 API specification, Appendix 2: type 10 lists no slat parameter.
  TestableComponent comp;
  MockRadio radio;
  setup_declared_component(comp, radio, {.type = DeviceType::VERTICAL_INTERIOR_BLIND});

  EXPECT_FALSE(comp.set_device_tilt("ABC123", 50));
  EXPECT_TRUE(radio.get_sent_data().empty());
}
