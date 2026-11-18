/// @file device_feature_set_test.cpp
/// @brief The DeviceFeatures set type: union, containment, emptiness, difference, equality and the
///        guarantee that every capability has a place of its own in the set.
/// @ingroup hioc_protocol

#include "proto_device_features.h"

#include <gtest/gtest.h>

using namespace esphome::home_io_control;
namespace feature = esphome::home_io_control::device_feature;

TEST(DeviceFeatureSet, ADefaultSetIsEmptyAndContainsNothingButTheEmptySet) {
  const DeviceFeatures none;
  EXPECT_TRUE(none.empty());
  EXPECT_FALSE(has_feature(none, feature::LEVELS));
  EXPECT_TRUE(has_feature(none, DeviceFeatures{})) << "every set contains the empty set";
  EXPECT_EQ(none, DeviceFeatures{});
}

TEST(DeviceFeatureSet, ASingleCapabilityConvertsToAOneElementSet) {
  const DeviceFeatures levels = feature::LEVELS;
  EXPECT_FALSE(levels.empty());
  EXPECT_TRUE(has_feature(levels, feature::LEVELS));
  EXPECT_FALSE(has_feature(levels, feature::TILT));
  EXPECT_NE(levels, DeviceFeatures{});
}

TEST(DeviceFeatureSet, UnionHoldsBothSidesAndContainsNeedsEveryRequestedCapability) {
  const DeviceFeatures both = feature::LEVELS | feature::TILT;
  EXPECT_TRUE(has_feature(both, feature::LEVELS));
  EXPECT_TRUE(has_feature(both, feature::TILT));
  EXPECT_FALSE(has_feature(both, feature::FAVORITE));
  EXPECT_TRUE(has_feature(both, feature::LEVELS | feature::TILT));
  EXPECT_FALSE(has_feature(both, feature::LEVELS | feature::FAVORITE)) << "one missing capability fails the set";
  EXPECT_EQ(both | feature::LEVELS, both) << "adding a held capability changes nothing";
  EXPECT_EQ(feature::TILT | feature::LEVELS, both) << "order does not matter";

  DeviceFeatures grown = feature::LEVELS;
  grown |= feature::FAVORITE;
  EXPECT_TRUE(has_feature(grown, feature::LEVELS | feature::FAVORITE));
}

TEST(DeviceFeatureSet, WithoutRemovesOnlyTheNamedCapabilitiesAndIgnoresAbsentOnes) {
  const DeviceFeatures all = feature::LEVELS | feature::TILT | feature::FAVORITE;
  EXPECT_EQ(all.without(feature::TILT), feature::LEVELS | feature::FAVORITE);
  EXPECT_EQ(all.without(feature::TILT | feature::LEVELS), DeviceFeatures{feature::FAVORITE});
  EXPECT_EQ(all.without(feature::VENT), all) << "removing a capability the set lacks changes nothing";
  EXPECT_TRUE(all.without(all).empty());
}

TEST(DeviceFeatureSet, IntersectsIsTrueOnlyWhenTheSetsShareACapability) {
  const DeviceFeatures set = feature::LEVELS | feature::TILT;
  EXPECT_TRUE(set.intersects(feature::TILT | feature::VENT));
  EXPECT_FALSE(set.intersects(feature::VENT | feature::PARTIAL));
  EXPECT_FALSE(set.intersects(DeviceFeatures{}));
}

TEST(DeviceFeatureSet, EveryCapabilityHasItsOwnPlaceInTheSet) {
  const auto count = static_cast<unsigned>(feature::COUNT);
  DeviceFeatures everything;
  for (unsigned i = 0; i < count; i++) {
    const auto one = static_cast<DeviceFeature>(i);
    // A capability that collided with an earlier one, or fell outside the storage, would already be in the set.
    EXPECT_FALSE(has_feature(everything, one)) << "capability " << i << " shares its place with an earlier one";
    everything |= one;
    EXPECT_TRUE(has_feature(everything, one)) << "capability " << i << " does not fit the set";
  }
  for (unsigned i = 0; i < count; i++)
    EXPECT_FALSE(everything.without(static_cast<DeviceFeature>(i)).intersects(static_cast<DeviceFeature>(i)));
}
