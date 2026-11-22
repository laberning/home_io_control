/// @file platform_oneway_light_test.cpp
/// @brief Tests for the assumed-state light of a one-way controller (ADR 0128), run against a double of the hub's
/// control port and the manual clock: the commands of a slider value in both ways, the stop that is sent once and
/// never stale, and the level the light shows for its own commands and for a followed remote's presses.

#include "platform_oneway_entities.h"

#include "hub_port_doubles.h"

#include "esphome/core/application.h"

#include <gtest/gtest.h>

using namespace esphome::home_io_control;
using decisions::OneWaySliderWay;

namespace {

constexpr const char *IDENTITY = "lamp_remote";
constexpr const char *FOLLOWED_REMOTE = "9D6085";
constexpr const char *OTHER_REMOTE = "AABBCC";

constexpr uint32_t ON_MS = 20000;   ///< Off to full.
constexpr uint32_t OFF_MS = 10000;  ///< Full to off.

/// Wire values of the on and off commands and of 25 and 75 % on the wire (0 is full brightness).
constexpr uint8_t WIRE_ON = 0x00;
constexpr uint8_t WIRE_OFF = 0xC8;
constexpr uint8_t WIRE_25_PERCENT_BRIGHT = 0x96;  // wire 75
constexpr uint8_t WIRE_75_PERCENT_BRIGHT = 0x32;  // wire 25

OneWayFrameInfo wire_frame(uint8_t main0) {
  OneWayFrameInfo info{};
  info.has_intent = true;
  info.main0 = main0;
  return info;
}

/// The report of a command of @p identity that went out with the wire value @p main0.
OneWayCommandReport sent(uint8_t main0, bool transmitted = true, const char *identity = IDENTITY) {
  OneWayCommandReport report{};
  report.controller_id = identity;
  report.sequence_reserved = true;
  report.transmitted = transmitted;
  report.wire = wire_frame(main0);
  return report;
}

class TestableLight : public IOHomeOneWayLight {
 public:
  using IOHomeOneWayLight::suppress_write_;
};

class OneWayLightTest : public ::testing::Test {
 protected:
  void TearDown() override { esphome::test_scheduler::clear(); }

  void start(bool dimmable, bool timed, OneWaySliderWay way = OneWaySliderWay::TIMED) {
    light.set_parent(&hub);
    light.set_controller_id(IDENTITY);
    light.set_dimmable(dimmable);
    if (timed) {
      light.set_fade_durations(ON_MS, OFF_MS);
      light.set_slider_way(way);
    }
    light.add_follow(FOLLOWED_REMOTE);
    light.setup();
    light.setup_state(&state);
    light.write_state(&state);  // LightState::setup()'s boot-time restore write; swallowed
  }

  /// What Home Assistant asks for: on at @p brightness, or off; @p in_transition for a step of a transition.
  void ask(float brightness, bool in_transition = false) {
    esphome::light::LightState request;
    request.set_current_on(brightness > 0.0F);
    request.set_current_brightness(brightness);
    request.test_transformer_active_ = in_transition;
    light.write_state(&request);
  }

  /// Report the last queued command as having reached the radio, with wire value @p main0.
  void reached_radio(uint8_t main0) {
    hub.report_oneway_command(sent(main0));
    echo();
  }

  /// A remote's press, heard by the hub.
  void press(uint8_t main0) {
    hub.press_oneway_remote(FOLLOWED_REMOTE, wire_frame(main0));
    echo();
  }

  /// Real ESPHome re-enters write_state() while it applies a published level; the host stub does not, so this does
  /// it for the publishes that arm the guard. The guard must swallow it: a command here would be an echo.
  void echo() {
    if (light.suppress_write_)
      light.write_state(&state);
  }

  /// Let @p ms pass, firing what is due and running the loop once, as ESPHome does.
  void pass(uint32_t ms) {
    esphome::test_scheduler::run_until(esphome::millis() + ms);
    light.loop();
    echo();
  }

  [[nodiscard]] float shown() const {
    return state.current_values.is_on() ? state.current_values.get_brightness() : 0.0F;
  }

  esphome::test_clock::ManualClock clock;
  test::ControlPortDouble hub;
  TestableLight light;
  esphome::light::LightState state;
};

}  // namespace

// === Without fade times ===

TEST_F(OneWayLightTest, ABinaryLightOffersOnAndOffAndAbrightnessLightABrightnessSlider) {
  EXPECT_EQ(light.get_traits().get_color_mode(), esphome::light::ColorMode::ON_OFF);

  light.set_dimmable(true);

  EXPECT_EQ(light.get_traits().get_color_mode(), esphome::light::ColorMode::BRIGHTNESS);
}

TEST_F(OneWayLightTest, OnAndOffQueueTheSameOneWayCommandsAsTheButtons) {
  start(false, false);

  ask(1.0F);
  ask(0.0F);

  ASSERT_EQ(hub.oneway_actions.size(), 2u);
  EXPECT_EQ(hub.oneway_actions[0], std::make_pair(std::string(IDENTITY), OneWayButtonAction::OPEN));
  EXPECT_EQ(hub.oneway_actions[1], std::make_pair(std::string(IDENTITY), OneWayButtonAction::CLOSE));
  EXPECT_TRUE(hub.oneway_positions.empty());
}

TEST_F(OneWayLightTest, ThePublishedStateOfALightWithoutFadeTimesIsOnOrOffNothingBetween) {
  start(true, false);

  ask(0.3F);
  ask(0.6F);

  ASSERT_EQ(hub.oneway_actions.size(), 2u);
  EXPECT_EQ(hub.oneway_actions[0].second, OneWayButtonAction::CLOSE) << "a value goes to its nearer end";
  EXPECT_EQ(hub.oneway_actions[1].second, OneWayButtonAction::OPEN);
}

TEST_F(OneWayLightTest, TheShownStateChangesOnlyWhenTheCommandReachedTheRadio) {
  start(false, false);
  ask(1.0F);
  EXPECT_FALSE(state.current_values.is_on()) << "queued is not sent";

  hub.report_oneway_command(sent(WIRE_ON, /*transmitted=*/false));
  EXPECT_FALSE(state.current_values.is_on()) << "a burst that never left the radio changes nothing";

  reached_radio(WIRE_ON);
  EXPECT_TRUE(state.current_values.is_on());
  reached_radio(WIRE_OFF);
  EXPECT_FALSE(state.current_values.is_on());
}

TEST_F(OneWayLightTest, AReportWithoutACommandOrOfAnotherIdentityChangesNothing) {
  start(false, false);

  OneWayCommandReport enrollment = sent(WIRE_ON);
  enrollment.wire.has_intent = false;
  hub.report_oneway_command(enrollment);
  hub.report_oneway_command(sent(WIRE_ON, true, "other_identity"));

  EXPECT_FALSE(state.current_values.is_on());
}

TEST_F(OneWayLightTest, APressOfAFollowedRemoteSetsTheStateAndSendsNothingWhileAStopChangesNothing) {
  start(false, false);

  press(WIRE_ON);
  EXPECT_TRUE(state.current_values.is_on());
  press(POS_STOP);
  EXPECT_TRUE(state.current_values.is_on()) << "a stop leaves a level the hub cannot know";
  press(POS_FAVORITE);
  EXPECT_TRUE(state.current_values.is_on()) << "a favourite changes nothing";
  press(WIRE_OFF);
  EXPECT_FALSE(state.current_values.is_on());
  hub.press_oneway_remote(OTHER_REMOTE, wire_frame(WIRE_ON));
  EXPECT_FALSE(state.current_values.is_on()) << "a remote that is not listed never moves it";

  EXPECT_TRUE(hub.oneway_actions.empty());
}

TEST_F(OneWayLightTest, PublishingTheLevelIsNotSentBackAsACommand) {
  start(false, false);

  reached_radio(WIRE_ON);  // publishes the level; the echo of that publish re-enters write_state()

  EXPECT_TRUE(state.current_values.is_on());
  EXPECT_TRUE(hub.oneway_actions.empty());
}

// === With fade times, the timed way ===

TEST_F(OneWayLightTest, AFadeShowsTheLevelMovingOverTheFadeTimeAndEndingAtTheCommandedEnd) {
  start(true, true);

  reached_radio(WIRE_ON);
  pass(0);
  EXPECT_FLOAT_EQ(shown(), 0.0F);
  pass(5000);
  EXPECT_FLOAT_EQ(shown(), 0.25F);
  pass(15000);
  EXPECT_FLOAT_EQ(shown(), 1.0F);

  reached_radio(WIRE_OFF);
  pass(5000);
  EXPECT_FLOAT_EQ(shown(), 0.5F) << "the fall is faster than the rise";
  pass(5000);
  EXPECT_FLOAT_EQ(shown(), 0.0F);
  EXPECT_FALSE(state.current_values.is_on());
}

TEST_F(OneWayLightTest, AValueSendsOnTowardsItAndOneStopWhenTheEstimateReachesItScheduledFromTheReport) {
  start(true, true);

  ask(0.5F);
  ASSERT_EQ(hub.oneway_actions.size(), 1u);
  EXPECT_EQ(hub.oneway_actions[0].second, OneWayButtonAction::OPEN);

  pass(7000);  // queued for a while before the burst goes out: the delay must not start yet
  EXPECT_EQ(hub.oneway_actions.size(), 1u) << "no stop before the command reached the radio";
  reached_radio(WIRE_ON);
  pass(9999);
  EXPECT_EQ(hub.oneway_actions.size(), 1u);
  pass(1);

  ASSERT_EQ(hub.oneway_actions.size(), 2u) << "half of the 20 s rise, counted from the report";
  EXPECT_EQ(hub.oneway_actions[1].second, OneWayButtonAction::STOP);
  pass(60000);
  EXPECT_EQ(hub.oneway_actions.size(), 2u) << "the stop is sent once";
}

TEST_F(OneWayLightTest, TheStopsReportFreezesTheLevelAtTheValue) {
  start(true, true);
  ask(0.5F);
  reached_radio(WIRE_ON);
  pass(10000);
  ASSERT_EQ(hub.oneway_actions.size(), 2u);

  reached_radio(POS_STOP);
  pass(30000);

  EXPECT_FLOAT_EQ(shown(), 0.5F);
}

TEST_F(OneWayLightTest, AValueBelowTheLevelClosesWithTheFallTimeAndAFullOrOffValueSendsNoStop) {
  start(true, true);
  reached_radio(WIRE_ON);
  pass(ON_MS);  // full
  ask(0.4F);
  ASSERT_EQ(hub.oneway_actions.size(), 1u);
  EXPECT_EQ(hub.oneway_actions[0].second, OneWayButtonAction::CLOSE);
  reached_radio(WIRE_OFF);
  pass(5999);
  EXPECT_EQ(hub.oneway_actions.size(), 1u);
  pass(1);
  EXPECT_EQ(hub.oneway_actions.size(), 2u) << "0.6 of a 10 s fall";

  reached_radio(POS_STOP);
  ask(1.0F);
  reached_radio(WIRE_ON);
  ask(0.0F);
  reached_radio(WIRE_OFF);
  pass(120000);

  ASSERT_EQ(hub.oneway_actions.size(), 4u) << "on and off are plain commands, with no stop behind them";
  EXPECT_EQ(hub.oneway_actions[2].second, OneWayButtonAction::OPEN);
  EXPECT_EQ(hub.oneway_actions[3].second, OneWayButtonAction::CLOSE);
}

TEST_F(OneWayLightTest, ANewValueCancelsThePendingStopAndIsPlannedFromTheLevelReached) {
  start(true, true);
  ask(0.8F);
  reached_radio(WIRE_ON);
  pass(4000);  // 0.2 so far; the first stop was due at 16 s

  ask(0.4F);  // 0.2 more of a rise: 4 s
  ASSERT_EQ(hub.oneway_actions.size(), 2u);
  EXPECT_EQ(hub.oneway_actions[1].second, OneWayButtonAction::OPEN);
  reached_radio(WIRE_ON);
  pass(3999);
  EXPECT_EQ(hub.oneway_actions.size(), 2u);
  pass(1);

  ASSERT_EQ(hub.oneway_actions.size(), 3u);
  EXPECT_EQ(hub.oneway_actions[2].second, OneWayButtonAction::STOP);
  pass(60000);
  EXPECT_EQ(hub.oneway_actions.size(), 3u) << "the stop of the first value was cancelled, no stale stop follows";
}

TEST_F(OneWayLightTest, AReversalSendsTheOppositeCommandAndLosesTheOldStop) {
  start(true, true);
  ask(0.9F);
  reached_radio(WIRE_ON);
  pass(10000);  // at 0.5, rising

  ask(0.3F);
  ASSERT_EQ(hub.oneway_actions.size(), 2u);
  EXPECT_EQ(hub.oneway_actions[1].second, OneWayButtonAction::CLOSE);
  reached_radio(WIRE_OFF);
  pass(1999);
  EXPECT_EQ(hub.oneway_actions.size(), 2u);
  pass(1);
  EXPECT_EQ(hub.oneway_actions.size(), 3u) << "0.2 of a 10 s fall";
  EXPECT_EQ(hub.oneway_actions[2].second, OneWayButtonAction::STOP);
}

TEST_F(OneWayLightTest, AStopSentOrHeardCancelsThePendingStop) {
  start(true, true);
  ask(0.5F);
  reached_radio(WIRE_ON);
  pass(4000);

  reached_radio(POS_STOP);  // a button's stop, or a stop the hub sent itself
  pass(60000);

  EXPECT_EQ(hub.oneway_actions.size(), 1u) << "no second stop on top of the first";
}

TEST_F(OneWayLightTest, AFollowedRemotesPressCancelsThePendingStop) {
  start(true, true);
  ask(0.5F);
  reached_radio(WIRE_ON);
  pass(4000);

  press(WIRE_OFF);
  pass(60000);

  EXPECT_EQ(hub.oneway_actions.size(), 1u) << "the wall remote took over; the stop would be stale";
  EXPECT_FLOAT_EQ(shown(), 0.0F);
}

TEST_F(OneWayLightTest, AFollowedRemotesPressBeforeTheCommandReachedTheRadioDropsTheStopToo) {
  start(true, true);
  ask(0.5F);  // queued

  press(WIRE_ON);
  reached_radio(WIRE_ON);
  pass(60000);

  EXPECT_EQ(hub.oneway_actions.size(), 1u);
}

TEST_F(OneWayLightTest, ACommandThatDidNotReachTheRadioSchedulesNoStopAndChangesNothing) {
  start(true, true);
  ask(0.5F);

  hub.report_oneway_command(sent(WIRE_ON, /*transmitted=*/false));
  pass(60000);

  EXPECT_EQ(hub.oneway_actions.size(), 1u);
  EXPECT_FLOAT_EQ(shown(), 0.0F);
}

TEST_F(OneWayLightTest, AnotherCommandsReportDoesNotArmTheStopOfAValueItWasNotSentFor) {
  start(true, true);
  ask(0.5F);  // on, queued

  reached_radio(WIRE_OFF);  // an earlier "off" button press reports first
  pass(1000);
  EXPECT_EQ(hub.oneway_actions.size(), 1u) << "a stop planned for a rise is not armed by a fall";
  reached_radio(WIRE_ON);  // then the value's own command
  pass(60000);

  EXPECT_EQ(hub.oneway_actions.size(), 1u) << "the stop is lost, the fade runs to its end; never a stop out of time";
  EXPECT_FLOAT_EQ(shown(), 1.0F);
}

TEST_F(OneWayLightTest, TwoValuesQueuedTheSameWayStopOnceAtTheLatestTimedFromWhenTheFadeBegan) {
  start(true, true);
  ask(0.3F);
  ask(0.5F);  // before the first value's command went out
  ASSERT_EQ(hub.oneway_actions.size(), 2u);

  reached_radio(WIRE_ON);  // the first value's command: the fade begins
  pass(300);
  EXPECT_EQ(hub.oneway_actions.size(), 2u) << "the first value's stop was cancelled by the second";
  reached_radio(WIRE_ON);  // the latest value's command arms its stop, timed from the level the fade has reached
  pass(9699);
  EXPECT_EQ(hub.oneway_actions.size(), 2u);
  pass(1);

  ASSERT_EQ(hub.oneway_actions.size(), 3u) << "half of the 20 s rise after the fade began";
  EXPECT_EQ(hub.oneway_actions[2].second, OneWayButtonAction::STOP);
  pass(60000);
  EXPECT_EQ(hub.oneway_actions.size(), 3u) << "one stop";
}

TEST_F(OneWayLightTest, AValueAskedWhileTheTimedStopWaitsForItsReportIsArmedByItsOwnCommand) {
  start(true, true);
  ask(0.5F);
  reached_radio(WIRE_ON);
  pass(10000);  // the stop is queued
  ASSERT_EQ(hub.oneway_actions.size(), 2u);

  ask(0.25F);  // queued behind the stop
  ASSERT_EQ(hub.oneway_actions.size(), 3u);
  EXPECT_EQ(hub.oneway_actions[2].second, OneWayButtonAction::CLOSE);
  reached_radio(POS_STOP);  // the stop's report freezes the level and leaves the new value's plan alone
  reached_radio(WIRE_OFF);
  pass(2499);
  EXPECT_EQ(hub.oneway_actions.size(), 3u);
  pass(1);

  ASSERT_EQ(hub.oneway_actions.size(), 4u) << "0.25 of a 10 s fall";
  EXPECT_EQ(hub.oneway_actions[3].second, OneWayButtonAction::STOP);
}

TEST_F(OneWayLightTest, AFadeThatPassedTheValueWhileItsCommandWaitedTurnsBackAndStopsThere) {
  start(true, true);
  ask(0.9F);
  reached_radio(WIRE_ON);
  pass(10000);  // at 0.5, rising

  ask(0.55F);  // 1 s more of the rise
  ASSERT_EQ(hub.oneway_actions.size(), 2u);
  EXPECT_EQ(hub.oneway_actions[1].second, OneWayButtonAction::OPEN);
  pass(1500);              // the command waits longer than that for the radio: the fade is at 0.575
  reached_radio(WIRE_ON);  // its report finds the value behind

  ASSERT_EQ(hub.oneway_actions.size(), 3u) << "the way back, not a fade to full";
  EXPECT_EQ(hub.oneway_actions[2].second, OneWayButtonAction::CLOSE);
  pass(500);  // at 0.6 when the way back reaches the radio
  reached_radio(WIRE_OFF);
  pass(499);
  EXPECT_EQ(hub.oneway_actions.size(), 3u);
  pass(1);

  ASSERT_EQ(hub.oneway_actions.size(), 4u) << "0.05 of a 10 s fall, armed by the way back's own report";
  EXPECT_EQ(hub.oneway_actions[3].second, OneWayButtonAction::STOP);
  pass(60000);
  EXPECT_EQ(hub.oneway_actions.size(), 4u);
}

TEST_F(OneWayLightTest, AForeignCommandThatTakesTheValuesPlaceLosesTheStopOfTheWayBackButNeverSendsAStaleOne) {
  start(true, true);
  ask(0.9F);
  reached_radio(WIRE_ON);
  pass(10000);  // at 0.5, rising

  // A button of the identity queued just before the value: its report comes first and takes the value's place.
  ask(0.55F);
  pass(1500);              // at 0.575
  reached_radio(WIRE_ON);  // the button's report finds the value behind: the way back is queued
  ASSERT_EQ(hub.oneway_actions.size(), 3u);
  EXPECT_EQ(hub.oneway_actions[2].second, OneWayButtonAction::CLOSE);
  reached_radio(WIRE_ON);   // the value's own command, read as the way back's place: a rise, so the value is dropped
  reached_radio(WIRE_OFF);  // the way back
  pass(60000);

  EXPECT_EQ(hub.oneway_actions.size(), 3u) << "the stop is lost and the fade runs to an end; never a stop out of time";
  EXPECT_FLOAT_EQ(shown(), 0.0F);
}

TEST_F(OneWayLightTest, AFadeThatReachedTheValueWhileItsCommandWaitedStopsAtOnce) {
  start(true, true);
  ask(0.9F);
  reached_radio(WIRE_ON);
  pass(10000);  // at 0.5, rising

  ask(0.55F);
  pass(1000);  // at the value when the command reaches the radio
  reached_radio(WIRE_ON);

  ASSERT_EQ(hub.oneway_actions.size(), 3u);
  EXPECT_EQ(hub.oneway_actions[2].second, OneWayButtonAction::STOP);
  reached_radio(POS_STOP);
  pass(60000);
  EXPECT_EQ(hub.oneway_actions.size(), 3u);
  EXPECT_NEAR(shown(), 0.55F, 1e-4F);
}

TEST_F(OneWayLightTest, ATransitionSendsOnlyItsEnd) {
  start(true, true);

  ask(0.1F, /*in_transition=*/true);
  ask(0.2F, /*in_transition=*/true);
  EXPECT_TRUE(hub.oneway_actions.empty()) << "a step of a transition is not a command";
  ask(0.3F);

  ASSERT_EQ(hub.oneway_actions.size(), 1u);
  EXPECT_EQ(hub.oneway_actions[0].second, OneWayButtonAction::OPEN);
}

TEST_F(OneWayLightTest, ThePlanIsInTheUsersDirectionForAnInvertedIdentity) {
  hub.inverted_oneway_identities.insert(IDENTITY);
  start(true, true);
  ask(0.5F);

  reached_radio(0xC8);  // an inverted kind: the user's on is wire 100 %
  pass(10000);

  ASSERT_EQ(hub.oneway_actions.size(), 2u);
  EXPECT_EQ(hub.oneway_actions[1].second, OneWayButtonAction::STOP);
}

TEST_F(OneWayLightTest, AValueWithTheLevelAlreadyThereSendsNothingAndAStopWhileItFades) {
  start(true, true);
  ask(0.0F);
  reached_radio(WIRE_OFF);
  hub.oneway_actions.clear();

  ask(0.00001F);  // already as good as there
  EXPECT_TRUE(hub.oneway_actions.empty()) << "nothing to run, nothing sent";

  ask(1.0F);
  reached_radio(WIRE_ON);
  pass(10000);
  ask(0.5F);
  ASSERT_EQ(hub.oneway_actions.size(), 2u);
  EXPECT_EQ(hub.oneway_actions[1].second, OneWayButtonAction::STOP) << "where it is while it passes the value";
}

TEST_F(OneWayLightTest, AFadeAcrossTheMillisWrapStillArrives) {
  start(true, true);
  esphome::test_clock::set_ms(0xFFFFF000U);

  reached_radio(WIRE_ON);
  pass(ON_MS);

  EXPECT_FLOAT_EQ(shown(), 1.0F);
}

// === With fade times, the frame way ===

TEST_F(OneWayLightTest, AFrameValueSendsOnePositionFrameOnTheWireScaleAndNoStop) {
  start(true, true, OneWaySliderWay::FRAME);

  ask(0.25F);
  pass(120000);

  ASSERT_EQ(hub.oneway_positions.size(), 1u);
  EXPECT_EQ(hub.oneway_positions[0], std::make_pair(std::string(IDENTITY), uint8_t{75}))
      << "0 is full brightness on the wire";
  EXPECT_TRUE(hub.oneway_actions.empty()) << "no stop is ever sent in this way";
}

TEST_F(OneWayLightTest, ThePositionFramesReportSetsTheLevelAtOnceWhereTheCoverWouldTravel) {
  start(true, true, OneWaySliderWay::FRAME);

  reached_radio(WIRE_25_PERCENT_BRIGHT);

  EXPECT_FLOAT_EQ(shown(), 0.25F);
  pass(0);
  EXPECT_FLOAT_EQ(shown(), 0.25F) << "no fade to it";
  reached_radio(WIRE_75_PERCENT_BRIGHT);
  EXPECT_FLOAT_EQ(shown(), 0.75F);
}

TEST_F(OneWayLightTest, AFollowedRemotesLevelJumpsAndItsEndsFade) {
  start(true, true);

  press(WIRE_25_PERCENT_BRIGHT);
  EXPECT_FLOAT_EQ(shown(), 0.25F);
  press(WIRE_ON);
  pass(5000);
  EXPECT_FLOAT_EQ(shown(), 0.5F);
  press(POS_STOP);
  pass(30000);
  EXPECT_FLOAT_EQ(shown(), 0.5F);
  press(POS_FAVORITE);
  EXPECT_FLOAT_EQ(shown(), 0.5F) << "a favourite changes nothing";
}
