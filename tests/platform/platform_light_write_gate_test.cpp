/// @file platform_light_write_gate_test.cpp
/// @brief Tests for LightWriteGate (platform_light.h), the one rule every light output of this component applies to
/// the writes ESPHome makes on its LightState: a published reading is never sent back, a user value always is, the boot
/// write and the steps of a transition are not.

#include "platform_light.h"

#include <gtest/gtest.h>

using namespace esphome::home_io_control;

namespace {

/// A user's call, as Home Assistant's API makes it: values on the LightState and one pending write.
void user_sets(esphome::light::LightState &state, bool on, float brightness) {
  auto call = state.make_call();
  call.set_state(on);
  call.set_brightness(brightness);
  call.perform();
}

/// The light's loop: the pending write, if any, reaches the gate. Returns whether the gate lets it go to the device.
bool writes_to_device(LightWriteGate &gate, esphome::light::LightState &state) {
  if (!state.next_write_)
    return false;
  state.next_write_ = false;
  return gate.should_send(&state);
}

}  // namespace

// === The pure comparison ===

TEST(LightEcho, AWriteThatSaysWhatTheReadingSaidIsTheEcho) {
  EXPECT_TRUE(is_light_echo({true, 0.5F}, true, 0.5F));
  EXPECT_TRUE(is_light_echo({false, 0.5F}, false, 0.5F));
  EXPECT_TRUE(is_light_echo({true, std::nullopt}, true, 0.7F)) << "an on/off light publishes no brightness";
}

TEST(LightEcho, AWriteThatDiffersInOnOffOrBrightnessIsNotTheEcho) {
  EXPECT_FALSE(is_light_echo({true, 0.5F}, false, 0.5F));
  EXPECT_FALSE(is_light_echo({false, std::nullopt}, true, 1.0F));
  EXPECT_FALSE(is_light_echo({true, 0.5F}, true, 0.6F));
}

TEST(LightEcho, AnOffLightHasNoLevelToDiffer) { EXPECT_TRUE(is_light_echo({false, 0.0F}, false, 0.8F)); }

// === The gate ===

TEST(LightWriteGate, ThePublishedReadingIsShownAndItsWriteIsNotSent) {
  LightWriteGate gate;
  esphome::light::LightState state;

  gate.publish(&state, true, 0.4F);

  EXPECT_TRUE(state.current_values.is_on());
  EXPECT_FLOAT_EQ(state.current_values.get_brightness(), 0.4F);
  EXPECT_FALSE(writes_to_device(gate, state)) << "a reading never causes a command";
}

TEST(LightWriteGate, AReadingIsShownAtOnceWhateverTheLightsDefaultTransition) {
  LightWriteGate gate;
  esphome::light::LightState state;
  state.set_default_transition_length(1000);  // a user's default_transition_length

  gate.publish(&state, true, 0.4F);

  EXPECT_EQ(state.test_last_transition_ms_, 0u) << "a faded reading would send its end to the device as a command";
}

TEST(LightWriteGate, TheEchoSwallowsExactlyOneWrite) {
  LightWriteGate gate;
  esphome::light::LightState state;
  gate.publish(&state, true, 0.4F);
  ASSERT_FALSE(writes_to_device(gate, state));

  user_sets(state, true, 0.4F);
  EXPECT_TRUE(gate.should_send(&state)) << "a later write with the same values is no echo any more";
}

TEST(LightWriteGate, PublishingTheSameReadingTwiceCausesNoCommand) {
  LightWriteGate gate;
  esphome::light::LightState state;

  gate.publish(&state, true, 0.4F);
  gate.publish(&state, true, 0.4F);

  EXPECT_FALSE(writes_to_device(gate, state));
  EXPECT_FALSE(state.next_write_) << "the second publish found the value shown and made no call";
}

TEST(LightWriteGate, AValueThatDiffersFromTheReadingArrivesAfterThePublishAndIsSent) {
  LightWriteGate gate;
  esphome::light::LightState state;
  gate.publish(&state, true, 0.4F);

  user_sets(state, true, 0.9F);  // between the publish and the light's loop

  EXPECT_TRUE(writes_to_device(gate, state));
}

TEST(LightWriteGate, AValueEqualToTheReadingChangesNothingOnTheDeviceAndIsSwallowed) {
  LightWriteGate gate;
  esphome::light::LightState state;
  gate.publish(&state, true, 0.4F);

  user_sets(state, true, 0.4F);

  EXPECT_FALSE(writes_to_device(gate, state));
}

TEST(LightWriteGate, TheBootWriteIsSwallowedWhateverItSaysAndOnlyOnce) {
  LightWriteGate gate;
  esphome::light::LightState state;
  gate.swallow_boot_write();

  user_sets(state, true, 1.0F);
  EXPECT_FALSE(writes_to_device(gate, state));
  user_sets(state, false, 1.0F);
  EXPECT_TRUE(writes_to_device(gate, state));
}

TEST(LightWriteGate, NothingIsPublishedAndNothingIsSentWhileATransitionRuns) {
  LightWriteGate gate;
  esphome::light::LightState state;
  state.test_transformer_active_ = true;

  gate.publish(&state, true, 0.4F);
  EXPECT_FALSE(state.current_values.is_on()) << "a call would end the transition and drop its end value";
  EXPECT_FALSE(state.next_write_);

  user_sets(state, true, 0.2F);  // a step of the transition
  EXPECT_FALSE(writes_to_device(gate, state));
}

TEST(LightWriteGate, TheEndOfATransitionIsSentAndTheNextReadingIsShown) {
  LightWriteGate gate;
  esphome::light::LightState state;
  state.test_transformer_active_ = true;
  user_sets(state, true, 0.2F);
  ASSERT_FALSE(writes_to_device(gate, state));

  state.test_transformer_active_ = false;  // the transition ended; its end values are written once
  user_sets(state, true, 0.8F);
  EXPECT_TRUE(writes_to_device(gate, state));

  gate.publish(&state, true, 0.5F);
  EXPECT_FLOAT_EQ(state.current_values.get_brightness(), 0.5F);
  EXPECT_FALSE(writes_to_device(gate, state));
}

TEST(LightWriteGate, ATransitionStepClearsTheExpectedEchoSoItCannotSwallowALaterValue) {
  LightWriteGate gate;
  esphome::light::LightState state;
  gate.publish(&state, true, 0.4F);

  state.test_transformer_active_ = true;
  ASSERT_FALSE(writes_to_device(gate, state));
  state.test_transformer_active_ = false;

  user_sets(state, true, 0.4F);
  EXPECT_TRUE(writes_to_device(gate, state));
}

// === The order that stays open ===

TEST(LightWriteGate, AValueStillWaitingToBeWrittenWhenAReadingIsPublishedIsReplacedByTheReading) {
  // The user's value is on the LightState and waits for the light's loop; a reading published before that loop
  // overwrites it, and the one write that follows reads as the echo. A value from Home Assistant's API waits like this
  // whenever a component that publishes runs after the API in the same pass: the call moves the LightState's loop to
  // the end of the pass, but ESPHome does not keep the order of the others fixed, so the hub can come after the API.
  // The component cannot see a waiting write without LightState::set_external_state(), so the value is lost (see
  // LightWriteGate).
  LightWriteGate gate;
  esphome::light::LightState state;
  user_sets(state, true, 0.5F);

  gate.publish(&state, true, 0.3F);

  EXPECT_FLOAT_EQ(state.current_values.get_brightness(), 0.3F);
  EXPECT_FALSE(writes_to_device(gate, state));
}
