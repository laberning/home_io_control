/// @file device_features_resolver_test.cpp
/// @brief resolve_device_features(): the one place a device's capability bits and inversion are decided.
/// @ingroup hioc_protocol

#include "proto_device_features.h"
#include "proto_device_model.h"
#include "proto_device_types.h"

#include <gtest/gtest.h>

using namespace esphome::home_io_control;
namespace feature = esphome::home_io_control::device_feature;

namespace {

IoDevice make_device(DeviceType type, uint8_t subtype, DeviceCapabilityClass entity_class) {
  IoDevice dev{};
  dev.type = type;
  dev.subtype = subtype;
  dev.entity_class = entity_class;
  return dev;
}

}  // namespace

TEST(ResolveDeviceFeatures, ADeclaredKindTakesItsRowWhateverEntityDeclaredIt) {
  IoDevice dev = make_device(DeviceType::INTERIOR_VENETIAN_BLIND, 0, DeviceCapabilityClass::COVER);
  resolve_device_features(dev);
  EXPECT_EQ(dev.features, device_features(DeviceType::INTERIOR_VENETIAN_BLIND, 0));

  IoDevice without_entity = make_device(DeviceType::INTERIOR_VENETIAN_BLIND, 0, DeviceCapabilityClass::UNKNOWN);
  resolve_device_features(without_entity);
  EXPECT_EQ(without_entity.features, dev.features) << "a record no entity declared resolves from its type";
}

TEST(ResolveDeviceFeatures, AnUndeclaredCoverGetsTheCoverDefaults) {
  IoDevice dev = make_device(DeviceType::UNKNOWN, 0, DeviceCapabilityClass::COVER);
  resolve_device_features(dev);
  EXPECT_EQ(dev.features, undeclared_kind_features(DeviceCapabilityClass::COVER));
}

TEST(ResolveDeviceFeatures, AnUndeclaredKindAtSubtype58IsMainParameterOnlyToo) {
  IoDevice dev = make_device(DeviceType::UNKNOWN, SUBTYPE_MAIN_PARAMETER_ONLY, DeviceCapabilityClass::COVER);
  resolve_device_features(dev);
  EXPECT_EQ(dev.features, main_parameter_only_variant(undeclared_kind_features(DeviceCapabilityClass::COVER)))
      << "a raw type at subtype 58 keeps the short frame";
  EXPECT_FALSE(has_feature(dev.features, feature::EXTENDED_POSITION_FRAME));
}

TEST(ResolveDeviceFeatures, ARawTypeWithNoRowTakesTheEntityDefaultsToo) {
  IoDevice dev = make_device(static_cast<DeviceType>(26), 0, DeviceCapabilityClass::COVER);
  resolve_device_features(dev);
  EXPECT_EQ(dev.features, undeclared_kind_features(DeviceCapabilityClass::COVER));
}

TEST(ResolveDeviceFeatures, ALightHasLevelsOnlyWhenDimmableIsDeclared) {
  IoDevice plain = make_device(DeviceType::LIGHT, 0, DeviceCapabilityClass::LIGHT);
  resolve_device_features(plain);
  EXPECT_FALSE(has_feature(plain.features, feature::LEVELS));

  IoDevice dimmable = plain;
  dimmable.dimmable = true;
  resolve_device_features(dimmable);
  EXPECT_TRUE(has_feature(dimmable.features, feature::LEVELS));
}

TEST(ResolveDeviceFeatures, AnRgbLightRowDoesNotMakeALightDimmableOnItsOwn) {
  // `dimmable:` has a schema default of false, so the row's LEVELS only drives the snippet.
  IoDevice dev = make_device(DeviceType::LIGHT, 1, DeviceCapabilityClass::LIGHT);
  resolve_device_features(dev);
  EXPECT_FALSE(has_feature(dev.features, feature::LEVELS));
}

TEST(ResolveDeviceFeatures, ALightOfSubtype58StaysOnOffEvenWhenDeclaredDimmable) {
  // VELUX KLF 200 API specification, Appendix 2: light subtype 58 is on/off only. The profile name
  // "dimmable_light" is chosen from LEVELS, so a learned 6/58 no longer logs as dimmable.
  IoDevice dev = make_device(DeviceType::LIGHT, SUBTYPE_MAIN_PARAMETER_ONLY, DeviceCapabilityClass::LIGHT);
  dev.dimmable = true;
  resolve_device_features(dev);
  EXPECT_FALSE(has_feature(dev.features, feature::LEVELS));
  EXPECT_TRUE(has_feature(dev.features, feature::MAIN_PARAMETER_ONLY));
}

TEST(ResolveDeviceFeatures, DimmableOnACoverDoesNotChangeItsLevels) {
  IoDevice dev = make_device(DeviceType::ROLLER_SHUTTER, 0, DeviceCapabilityClass::COVER);
  dev.dimmable = true;
  resolve_device_features(dev);
  EXPECT_EQ(dev.features, device_features(DeviceType::ROLLER_SHUTTER, 0));
}

TEST(ResolveDeviceFeatures, InversionIsTheOverrideElseTheKindsDefault) {
  IoDevice awning = make_device(DeviceType::HORIZONTAL_AWNING, 0, DeviceCapabilityClass::COVER);
  resolve_device_features(awning);
  EXPECT_TRUE(awning.inverted);

  awning.invert_override = false;
  resolve_device_features(awning);
  EXPECT_FALSE(awning.inverted);

  IoDevice shutter = make_device(DeviceType::ROLLER_SHUTTER, 0, DeviceCapabilityClass::COVER);
  shutter.invert_override = true;
  resolve_device_features(shutter);
  EXPECT_TRUE(shutter.inverted);
}

TEST(ResolveDeviceFeatures, ResolvingAgainAfterTheKindChangesFollowsTheNewKind) {
  IoDevice dev = make_device(DeviceType::UNKNOWN, 0, DeviceCapabilityClass::COVER);
  resolve_device_features(dev);
  EXPECT_FALSE(has_feature(dev.features, feature::TILT));

  dev.type = DeviceType::LOUVRE_BLIND;
  resolve_device_features(dev);
  EXPECT_TRUE(has_feature(dev.features, feature::TILT));
}

TEST(DeviceOperationProfileName, ACoverIsTiltingOnlyWhereItsKindHasASlatAngle) {
  EXPECT_STREQ(device_operation_profile_name(DeviceType::EXTERIOR_VENETIAN_BLIND, 0), "cover_position_tilt");
  EXPECT_STREQ(device_operation_profile_name(DeviceType::EXTERIOR_VENETIAN_BLIND, SUBTYPE_MAIN_PARAMETER_ONLY),
               "cover_position");
}
