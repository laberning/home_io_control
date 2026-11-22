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

// === A value that waits to be written ===

TEST(LightValueWaiting, IsSeenFromAShownValueThatDiffersFromTheLastSeenOne) {
  EXPECT_FALSE(is_value_waiting(std::nullopt, true, 0.5F)) << "nothing is known before the first sight";
  EXPECT_FALSE(is_value_waiting(LightShown{true, 0.5F}, true, 0.5F));
  EXPECT_TRUE(is_value_waiting(LightShown{true, 0.5F}, false, 0.5F));
  EXPECT_TRUE(is_value_waiting(LightShown{true, 0.5F}, true, 0.6F));
}

TEST(LightWriteGate, AReadingPublishedWhileAnInstantValueWaitsIsDroppedAndTheValueIsSent) {
  // The API sets a value; a component that publishes runs before the light's loop. The reading must not replace it.
  LightWriteGate gate;
  esphome::light::LightState state;
  gate.publish(&state, true, 0.4F);
  ASSERT_FALSE(writes_to_device(gate, state));
  user_sets(state, true, 0.9F);

  gate.publish(&state, true, 0.3F);

  EXPECT_FLOAT_EQ(state.current_values.get_brightness(), 0.9F) << "the reading did not replace the value";
  EXPECT_TRUE(writes_to_device(gate, state)) << "the value goes to the device";
}

TEST(LightWriteGate, AReadingPublishedWhileATransitionCallWaitsIsDroppedAndItsEndIsSent) {
  LightWriteGate gate;
  esphome::light::LightState state;
  gate.publish(&state, true, 0.4F);
  ASSERT_FALSE(writes_to_device(gate, state));
  auto call = state.make_call();  // a transition: the shown values are set, the transformer starts in the next loop
  call.set_state(true);
  call.set_brightness(0.9F);
  call.set_transition_length(2000);
  call.perform();
  ASSERT_FALSE(state.is_transformer_active());

  gate.publish(&state, true, 0.3F);

  // A call of the gate would make the change instant and drop the transformer (set_immediately_()).
  EXPECT_FALSE(state.next_write_) << "the reading did not end the transition call";
  EXPECT_FLOAT_EQ(state.remote_values.get_brightness(), 0.9F);

  state.test_transformer_active_ = true;  // the light's loops: a step of the transition, not sent
  state.current_values.set_brightness(0.6F);
  state.next_write_ = true;
  EXPECT_FALSE(writes_to_device(gate, state));
  state.test_transformer_active_ = false;  // the transition's end, written once
  state.current_values.set_brightness(0.9F);
  state.next_write_ = true;
  EXPECT_TRUE(writes_to_device(gate, state)) << "the transition's end value goes to the device";
}

TEST(LightWriteGate, ADroppedReadingIsShownWithTheNextOneOnceTheValueHasBeenWritten) {
  LightWriteGate gate;
  esphome::light::LightState state;
  gate.publish(&state, true, 0.4F);
  ASSERT_FALSE(writes_to_device(gate, state));
  user_sets(state, true, 0.9F);
  gate.publish(&state, true, 0.3F);
  ASSERT_TRUE(writes_to_device(gate, state));

  gate.publish(&state, true, 0.3F);

  EXPECT_FLOAT_EQ(state.current_values.get_brightness(), 0.3F);
  EXPECT_FALSE(writes_to_device(gate, state)) << "the shown reading causes no command";
}

TEST(LightWriteGate, NothingIsDroppedBeforeTheGateHasSeenTheLight) {
  LightWriteGate gate;
  esphome::light::LightState state;
  user_sets(state, true, 0.5F);  // a value waits, but the gate knows nothing of the light yet

  gate.publish(&state, true, 0.3F);

  EXPECT_FLOAT_EQ(state.current_values.get_brightness(), 0.3F);
}

TEST(LightWriteGate, ACallThatRepeatsTheShownValueInTheSameMomentIsNotSeenAsWaiting) {
  // The case that stays open: the call changes nothing the gate can compare, so a reading published before its write
  // replaces it, and the write reads as the echo (see LightWriteGate; LightState::set_external_state() closes it).
  LightWriteGate gate;
  esphome::light::LightState state;
  gate.publish(&state, true, 0.4F);
  ASSERT_FALSE(writes_to_device(gate, state));
  user_sets(state, true, 0.4F);

  gate.publish(&state, true, 0.3F);

  EXPECT_FLOAT_EQ(state.current_values.get_brightness(), 0.3F);
  EXPECT_FALSE(writes_to_device(gate, state));
}
