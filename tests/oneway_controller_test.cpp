#include "oneway_controller.h"
#include "proto_codecs.h"

#include "test_helpers.h"

#include <cstring>

using namespace esphome::home_io_control;

// ============================================================================
// OneWayController test suite
// ============================================================================
// The 1W controller-identity model (ADR 0027): class-addressed destinations, per-identity keys,
// and the registry the transmit engine will consume.

namespace {

OneWayControllerIdentity make_identity(const std::string &id, DeviceType type) {
  OneWayControllerIdentity identity{};
  identity.id = id;
  identity.io_device_type = type;
  return identity;
}

}  // namespace

TEST(OneWayController, BroadcastAddressEncodesDeviceClass) {
  // 1W addresses a device *class*, never a node -- which is why a 1W entity has no io_device_id.
  // The awning class (0x03) is small enough to fit entirely in the low byte; see
  // BroadcastAddressMatchesKnownWireValues for one that is not.
  const OneWayControllerIdentity awning = make_identity("awning", DeviceType::AWNING);

  uint8_t addr[NODE_ID_SIZE] = {0xFF, 0xFF, 0xFF};
  awning.broadcast_address(addr);

  EXPECT_EQ(addr[0], 0x00);
  EXPECT_EQ(addr[1], 0x00);
  EXPECT_EQ(addr[2], 0xFF) << "awning (0x03) encodes to 00 00 FF";
}

TEST(OneWayController, BroadcastAddressRoundTripsThroughTheDecoder) {
  // The class occupies bits [9:2] and therefore spans bytes 1-2. A single-byte encoding looks
  // right for classes 0-3 and silently produces the wrong class from 4 upward, so round-trip
  // every class this project names rather than spot-checking a low one.
  for (uint16_t raw = 0; raw <= static_cast<uint16_t>(DeviceType::SWINGING_SHUTTER); raw++) {
    const auto type = static_cast<DeviceType>(raw);
    const OneWayControllerIdentity identity = make_identity("id", type);

    uint8_t addr[NODE_ID_SIZE] = {0};
    identity.broadcast_address(addr);

    EXPECT_EQ(broadcast_target_type(addr), type)
        << "class 0x" << std::hex << raw << " must decode back to itself from " << std::hex << static_cast<int>(addr[0])
        << static_cast<int>(addr[1]) << static_cast<int>(addr[2]);
  }
}

TEST(OneWayController, BroadcastAddressMatchesKnownWireValues) {
  // Anchors against addresses observed on the wire rather than only against our own decoder --
  // a matched encoder/decoder pair can agree with each other and still both be wrong.
  const OneWayControllerIdentity light = make_identity("light", DeviceType::LIGHT);
  uint8_t addr[NODE_ID_SIZE] = {0};
  light.broadcast_address(addr);

  EXPECT_EQ(addr[0], 0x00);
  EXPECT_EQ(addr[1], 0x01) << "the light class (0x06) spills into byte 1: 00 01 BF";
  EXPECT_EQ(addr[2], 0xBF);
}

TEST(OneWayController, DifferentClassesProduceDifferentBroadcastAddresses) {
  const OneWayControllerIdentity awning = make_identity("awning", DeviceType::AWNING);
  const OneWayControllerIdentity light = make_identity("light", DeviceType::LIGHT);

  uint8_t awning_addr[NODE_ID_SIZE] = {0};
  uint8_t light_addr[NODE_ID_SIZE] = {0};
  awning.broadcast_address(awning_addr);
  light.broadcast_address(light_addr);

  EXPECT_NE(0, memcmp(awning_addr, light_addr, NODE_ID_SIZE))
      << "two classes must not share a destination, or a command would reach both";
}

TEST(OneWayController, RegistryLooksUpByHandleAndPreservesOrder) {
  OneWayControllerRegistry registry;
  EXPECT_TRUE(registry.empty());

  registry.add(make_identity("first", DeviceType::AWNING));
  registry.add(make_identity("second", DeviceType::ROLLER_SHUTTER));

  ASSERT_EQ(registry.all().size(), 2u);
  EXPECT_EQ(registry.all()[0].id, "first") << "declaration order is preserved so boot logs read like the YAML";
  EXPECT_EQ(registry.all()[1].id, "second");

  const OneWayControllerIdentity *found = registry.get("second");
  ASSERT_NE(found, nullptr);
  EXPECT_EQ(found->io_device_type, DeviceType::ROLLER_SHUTTER);
  EXPECT_EQ(registry.get("missing"), nullptr) << "an unknown handle must resolve to nullptr, not a default identity";
}

TEST(OneWayController, IdentitiesCarryIndependentKeys) {
  // The point of the per-identity key: an adopted foreign network's key must coexist with the
  // hub's own rather than replace it.
  OneWayControllerRegistry registry;

  OneWayControllerIdentity own = make_identity("own_net", DeviceType::AWNING);
  memset(own.system_key, 0x11, AES_KEY_SIZE);
  OneWayControllerIdentity adopted = make_identity("adopted_net", DeviceType::AWNING);
  memset(adopted.system_key, 0x22, AES_KEY_SIZE);

  registry.add(own);
  registry.add(adopted);

  EXPECT_NE(0, memcmp(registry.get("own_net")->system_key, registry.get("adopted_net")->system_key, AES_KEY_SIZE))
      << "identities must keep separate keys; sharing one would defeat adoption";
}
