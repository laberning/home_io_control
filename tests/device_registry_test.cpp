/// @file device_registry_test.cpp
/// @brief Unit tests for DeviceRegistry.
///
/// Covers: add/get round-trip, duplicate-add ignored, invalid-hex rejected,
/// callback fan-out on notify, put upsert, and linked-remote lookup.

#include "device_registry.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace esphome::home_io_control;

// --- add / get ---

TEST(DeviceRegistry, AddAndGetReturnsDevice) {
  DeviceRegistry reg;
  reg.add("ABC123");
  IoDevice *dev = reg.get("ABC123");
  ASSERT_NE(dev, nullptr);
  EXPECT_EQ(dev->type, DeviceType::UNKNOWN);
  EXPECT_EQ(dev->subtype, 0);
  EXPECT_FALSE(dev->inverted);
}

TEST(DeviceRegistry, AddWithMetadataStoresFields) {
  DeviceRegistry reg;
  reg.add("AABBCC", {DeviceType::AWNING, 2, true});
  IoDevice *dev = reg.get("AABBCC");
  ASSERT_NE(dev, nullptr);
  EXPECT_EQ(dev->type, DeviceType::AWNING);
  EXPECT_EQ(dev->subtype, 2);
  EXPECT_TRUE(dev->inverted);
}

TEST(DeviceRegistry, AddStoresNodeIdBytes) {
  DeviceRegistry reg;
  reg.add("ABC123");
  IoDevice *dev = reg.get("ABC123");
  ASSERT_NE(dev, nullptr);
  EXPECT_EQ(dev->node_id[0], 0xAB);
  EXPECT_EQ(dev->node_id[1], 0xC1);
  EXPECT_EQ(dev->node_id[2], 0x23);
}

TEST(DeviceRegistry, DuplicateAddIsIgnored) {
  DeviceRegistry reg;
  reg.add("ABC123");
  reg.add("ABC123", {DeviceType::AWNING, 5, true});
  IoDevice *dev = reg.get("ABC123");
  ASSERT_NE(dev, nullptr);
  EXPECT_EQ(dev->type, DeviceType::UNKNOWN);  // first registration wins
}

TEST(DeviceRegistry, InvalidHexIsRejected) {
  DeviceRegistry reg;
  reg.add("GGGGGG");
  EXPECT_EQ(reg.get("GGGGGG"), nullptr);
  EXPECT_EQ(reg.size(), 0u);
}

TEST(DeviceRegistry, GetReturnsNullptrForUnknownId) {
  DeviceRegistry reg;
  EXPECT_EQ(reg.get("FFFFFF"), nullptr);
}

TEST(DeviceRegistry, SetDimmableUpdatesRegisteredDevice) {
  DeviceRegistry reg;
  reg.add("AABBCC", {DeviceType::LIGHT, 0, false});
  EXPECT_FALSE(reg.get("AABBCC")->dimmable) << "dimmable defaults to false";

  reg.set_dimmable("AABBCC", true);
  EXPECT_TRUE(reg.get("AABBCC")->dimmable);

  reg.set_dimmable("AABBCC", false);
  EXPECT_FALSE(reg.get("AABBCC")->dimmable);
}

TEST(DeviceRegistry, SetDimmableOnUnknownDeviceIsNoop) {
  DeviceRegistry reg;
  reg.set_dimmable("FFFFFF", true);  // must not crash
  EXPECT_EQ(reg.get("FFFFFF"), nullptr);
}

TEST(DeviceRegistry, SizeReflectsRegistrationCount) {
  DeviceRegistry reg;
  EXPECT_EQ(reg.size(), 0u);
  reg.add("AA1122");
  EXPECT_EQ(reg.size(), 1u);
  reg.add("BB3344");
  EXPECT_EQ(reg.size(), 2u);
}

// --- put (upsert) ---

TEST(DeviceRegistry, PutInsertsDevice) {
  DeviceRegistry reg;
  IoDevice dev{};
  dev.type = DeviceType::WINDOW_OPENER;
  reg.put("CC5566", dev);
  IoDevice *stored = reg.get("CC5566");
  ASSERT_NE(stored, nullptr);
  EXPECT_EQ(stored->type, DeviceType::WINDOW_OPENER);
}

TEST(DeviceRegistry, PutOverwritesExistingEntry) {
  DeviceRegistry reg;
  reg.add("DD7788");
  IoDevice replacement{};
  replacement.type = DeviceType::AWNING;
  reg.put("DD7788", replacement);
  IoDevice *stored = reg.get("DD7788");
  ASSERT_NE(stored, nullptr);
  EXPECT_EQ(stored->type, DeviceType::AWNING);
}

// --- callback fan-out (notify) ---

TEST(DeviceRegistry, NotifyFiresRegisteredCallback) {
  DeviceRegistry reg;
  reg.add("EE9900");

  std::string notified_id;
  int call_count = 0;
  reg.subscribe([&](const std::string &id, const IoDevice &) {
    notified_id = id;
    ++call_count;
  });

  reg.notify("EE9900");
  EXPECT_EQ(call_count, 1);
  EXPECT_EQ(notified_id, "EE9900");
}

TEST(DeviceRegistry, NotifyFiresMultipleCallbacks) {
  DeviceRegistry reg;
  reg.add("FF1122");

  int count_a = 0;
  int count_b = 0;
  reg.subscribe([&](const std::string &, const IoDevice &) { ++count_a; });
  reg.subscribe([&](const std::string &, const IoDevice &) { ++count_b; });

  reg.notify("FF1122");
  EXPECT_EQ(count_a, 1);
  EXPECT_EQ(count_b, 1);
}

TEST(DeviceRegistry, NotifyPassesCurrentDeviceState) {
  DeviceRegistry reg;
  reg.add("AA0011");
  IoDevice *dev = reg.get("AA0011");
  ASSERT_NE(dev, nullptr);
  dev->position = 75;

  uint8_t seen_pos = 0;
  reg.subscribe([&](const std::string &, const IoDevice &d) { seen_pos = d.position; });

  reg.notify("AA0011");
  EXPECT_EQ(seen_pos, 75);
}

TEST(DeviceRegistry, NotifyOnUnknownIdIsNoop) {
  DeviceRegistry reg;
  int call_count = 0;
  reg.subscribe([&](const std::string &, const IoDevice &) { ++call_count; });
  reg.notify("ZZZZZZ");  // not registered — must not crash or fire callback
  EXPECT_EQ(call_count, 0);
}

// --- linked remote lookup ---

TEST(DeviceRegistry, LinkedDevicesReturnsList) {
  DeviceRegistry reg;
  reg.add_linked_remote("REMOTE1", "DEVICE1");
  reg.add_linked_remote("REMOTE1", "DEVICE2");

  const std::vector<std::string> *linked = reg.linked_devices("REMOTE1");
  ASSERT_NE(linked, nullptr);
  ASSERT_EQ(linked->size(), 2u);
  EXPECT_EQ((*linked)[0], "DEVICE1");
  EXPECT_EQ((*linked)[1], "DEVICE2");
}

TEST(DeviceRegistry, LinkedDevicesReturnsNullptrForUnknownRemote) {
  DeviceRegistry reg;
  EXPECT_EQ(reg.linked_devices("UNKNOWN_REMOTE"), nullptr);
}

TEST(DeviceRegistry, LinkedRemoteCountReflectsEntries) {
  DeviceRegistry reg;
  EXPECT_EQ(reg.linked_remote_count(), 0u);
  reg.add_linked_remote("R1", "D1");
  EXPECT_EQ(reg.linked_remote_count(), 1u);
  reg.add_linked_remote("R2", "D2");
  EXPECT_EQ(reg.linked_remote_count(), 2u);
}

TEST(DeviceRegistry, ForEachLinkedRemoteVisitsAllEntries) {
  DeviceRegistry reg;
  reg.add_linked_remote("RA", "DA");
  reg.add_linked_remote("RB", "DB1");
  reg.add_linked_remote("RB", "DB2");

  std::map<std::string, std::vector<std::string>> collected;
  reg.for_each_linked_remote(
      [&](const std::string &remote, const std::vector<std::string> &devices) { collected[remote] = devices; });

  ASSERT_EQ(collected.size(), 2u);
  ASSERT_EQ(collected["RA"].size(), 1u);
  EXPECT_EQ(collected["RA"][0], "DA");
  ASSERT_EQ(collected["RB"].size(), 2u);
  EXPECT_EQ(collected["RB"][0], "DB1");
  EXPECT_EQ(collected["RB"][1], "DB2");
}

// --- optimistic target ---

TEST(DeviceRegistry, ApplyOptimisticTargetSetsTargetAndClearsStopped) {
  DeviceRegistry reg;
  reg.add("ABC123", {DeviceType::ROLLER_SHUTTER, 0, false});

  EXPECT_TRUE(reg.apply_optimistic_target("ABC123", 75.0f));

  const IoDevice *dev = reg.get("ABC123");
  ASSERT_NE(dev, nullptr);
  EXPECT_FLOAT_EQ(dev->target, 75.0f);
  EXPECT_FALSE(dev->is_stopped);
}

TEST(DeviceRegistry, ClearOptimisticTargetResetsTargetAndSetsStopped) {
  DeviceRegistry reg;
  reg.add("ABC123", {DeviceType::ROLLER_SHUTTER, 0, false});
  reg.apply_optimistic_target("ABC123", 75.0f);

  EXPECT_TRUE(reg.clear_optimistic_target("ABC123"));

  const IoDevice *dev = reg.get("ABC123");
  ASSERT_NE(dev, nullptr);
  EXPECT_EQ(dev->target, UNKNOWN_POSITION);
  EXPECT_TRUE(dev->is_stopped) << "clearing the optimistic target on STOP must also mark the "
                                  "device stopped, or the HA cover UI keeps animating movement";
}

TEST(DeviceRegistry, ApplyOptimisticTargetOnUnknownDeviceReturnsFalseWithoutCrashing) {
  DeviceRegistry reg;
  EXPECT_FALSE(reg.apply_optimistic_target("UNKNOWN1", 50.0f));
}

TEST(DeviceRegistry, ClearOptimisticTargetOnUnknownDeviceReturnsFalseWithoutCrashing) {
  DeviceRegistry reg;
  EXPECT_FALSE(reg.clear_optimistic_target("UNKNOWN1"));
}

TEST(DeviceRegistry, OptimisticTargetNoOpWhenOptimisticStateDisabled) {
  DeviceRegistry reg;
  reg.add("ABC123", {DeviceType::ROLLER_SHUTTER, 0, false, /*optimistic_state=*/false});

  EXPECT_FALSE(reg.apply_optimistic_target("ABC123", 75.0f));
  EXPECT_FALSE(reg.clear_optimistic_target("ABC123"));

  const IoDevice *dev = reg.get("ABC123");
  ASSERT_NE(dev, nullptr);
  EXPECT_EQ(dev->target, UNKNOWN_POSITION) << "optimistic_state=false must leave target untouched";
}

// --- linked remote class lookup ---

TEST(DeviceRegistry, LinkedDevicesForClassReturnsAllSameTypeDevices) {
  DeviceRegistry reg;
  reg.add_linked_remote_class(DeviceType::AWNING, "DEVICE1");
  reg.add_linked_remote_class(DeviceType::AWNING, "DEVICE2");

  const std::vector<std::string> *linked = reg.linked_devices_for_class(DeviceType::AWNING);
  ASSERT_NE(linked, nullptr);
  ASSERT_EQ(linked->size(), 2u);
  EXPECT_EQ((*linked)[0], "DEVICE1");
  EXPECT_EQ((*linked)[1], "DEVICE2");
}

TEST(DeviceRegistry, LinkedDevicesForClassReturnsNullptrForUnknownClass) {
  DeviceRegistry reg;
  reg.add_linked_remote_class(DeviceType::AWNING, "DEVICE1");
  EXPECT_EQ(reg.linked_devices_for_class(DeviceType::ROLLER_SHUTTER), nullptr)
      << "a class with no linked devices must not be confused with a different registered class";
}

TEST(DeviceRegistry, IdLinkingAndClassLinkingAreIndependentMaps) {
  DeviceRegistry reg;
  reg.add_linked_remote("REMOTE1", "DEVICE1");
  reg.add_linked_remote_class(DeviceType::AWNING, "DEVICE1");

  const std::vector<std::string> *by_id = reg.linked_devices("REMOTE1");
  const std::vector<std::string> *by_class = reg.linked_devices_for_class(DeviceType::AWNING);
  ASSERT_NE(by_id, nullptr);
  ASSERT_NE(by_class, nullptr);
  EXPECT_EQ((*by_id)[0], "DEVICE1");
  EXPECT_EQ((*by_class)[0], "DEVICE1")
      << "the same device may be linked both by remote id and by class; the two maps are independent";
}

// --- mutable iteration (begin/end) ---

TEST(DeviceRegistry, RangeForIterationAllowsMutation) {
  DeviceRegistry reg;
  reg.add("111AAA");
  reg.add("222BBB");

  for (auto &pair : reg)
    pair.second.position = 42;

  EXPECT_EQ(reg.get("111AAA")->position, 42);
  EXPECT_EQ(reg.get("222BBB")->position, 42);
}
