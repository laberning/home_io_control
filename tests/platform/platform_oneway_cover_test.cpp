/// @file platform_oneway_cover_test.cpp
/// @brief Tests for the assumed-state cover of a one-way controller (ADR 0117), run against a double of the hub's
/// control port.

#include "platform_oneway_entities.h"

#include "hub_port_doubles.h"

#include "esphome/core/application.h"

#include <gtest/gtest.h>

using namespace esphome::home_io_control;
using namespace esphome::cover;
using decisions::OneWaySliderWay;

namespace {

constexpr const char *IDENTITY = "awning_remote";
constexpr const char *FOLLOWED_REMOTE = "9D6085";
constexpr const char *OTHER_REMOTE = "AABBCC";

class TestableOneWayCover : public IOHomeOneWayCover {
 public:
  using IOHomeOneWayCover::control;
};

/// A decoded frame as the hub reports it: main byte @p main0 as a wire value, carrying a command.
OneWayFrameInfo wire_frame(uint8_t main0) {
  OneWayFrameInfo info{};
  info.has_intent = true;
  info.main0 = main0;
  return info;
}

/// The report of a command of @p identity that went out with the wire value @p main0.
OneWayCommandReport sent(const std::string &identity, uint8_t main0, bool transmitted = true) {
  OneWayCommandReport report{};
  report.controller_id = identity;
  report.sequence_reserved = true;
  report.transmitted = transmitted;
  report.wire = wire_frame(main0);
  return report;
}

constexpr uint8_t WIRE_OPEN = 0x00;
constexpr uint8_t WIRE_CLOSED = 0xC8;

class OneWayCoverTest : public ::testing::Test {
 protected:
  /// Wire the cover to the double; `follows` lists the remotes it follows.
  void start(std::initializer_list<const char *> follows = {FOLLOWED_REMOTE}) {
    cover.set_parent(&hub);
    cover.set_controller_id(IDENTITY);
    for (const char *sender : follows)
      cover.add_follow(sender);
    cover.setup();
  }

  test::ControlPortDouble hub;
  TestableOneWayCover cover;
};

}  // namespace

TEST_F(OneWayCoverTest, ItOffersOpenCloseAndStopWithoutAPositionAndShowsAssumedState) {
  const auto traits = cover.get_traits();

  EXPECT_FALSE(traits.get_supports_position());
  EXPECT_TRUE(traits.get_is_assumed_state()) << "no read exists, so the shown state is a guess";
}

TEST_F(OneWayCoverTest, OpenCloseAndStopQueueTheSameOneWayCommandsAsTheButtons) {
  start();

  CoverCall open(&cover);
  open.set_position(COVER_OPEN);
  cover.control(open);
  CoverCall close(&cover);
  close.set_position(COVER_CLOSED);
  cover.control(close);
  CoverCall stop(&cover);
  stop.set_stop(true);
  cover.control(stop);

  ASSERT_EQ(hub.oneway_actions.size(), 3u) << "one sequence each";
  EXPECT_EQ(hub.oneway_actions[0], std::make_pair(std::string(IDENTITY), OneWayButtonAction::OPEN));
  EXPECT_EQ(hub.oneway_actions[1], std::make_pair(std::string(IDENTITY), OneWayButtonAction::CLOSE));
  EXPECT_EQ(hub.oneway_actions[2], std::make_pair(std::string(IDENTITY), OneWayButtonAction::STOP));
}

TEST_F(OneWayCoverTest, ACommandChangesNothingUntilItsReportSaysItReachedTheRadio) {
  start();

  CoverCall close(&cover);
  close.set_position(COVER_CLOSED);
  cover.control(close);

  EXPECT_EQ(cover.position, COVER_OPEN) << "queued is not sent";
}

TEST_F(OneWayCoverTest, ACommandThatReachedTheRadioShowsItsEndState) {
  start();

  hub.report_oneway_command(sent(IDENTITY, WIRE_CLOSED));
  EXPECT_EQ(cover.position, COVER_CLOSED);
  EXPECT_EQ(cover.current_operation, COVER_OPERATION_IDLE) << "there is no travel time, so no moving phase";

  hub.report_oneway_command(sent(IDENTITY, WIRE_OPEN));
  EXPECT_EQ(cover.position, COVER_OPEN);
}

TEST_F(OneWayCoverTest, AStopLeavesTheCoverShownAsNotClosed) {
  start();
  hub.report_oneway_command(sent(IDENTITY, WIRE_CLOSED));

  hub.report_oneway_command(sent(IDENTITY, POS_STOP));

  EXPECT_EQ(cover.position, COVER_OPEN) << "stopped half-way is not closed";
  EXPECT_EQ(cover.current_operation, COVER_OPERATION_IDLE);
}

TEST_F(OneWayCoverTest, ACommandThatDidNotReachTheRadioChangesNothing) {
  start();
  hub.report_oneway_command(sent(IDENTITY, WIRE_CLOSED));

  hub.report_oneway_command(sent(IDENTITY, WIRE_OPEN, /*transmitted=*/false));

  EXPECT_EQ(cover.position, COVER_CLOSED) << "a failed send is not a command the device may have heard";
}

TEST_F(OneWayCoverTest, AReportWithoutACommandOrOfAnotherIdentityChangesNothing) {
  start();
  hub.report_oneway_command(sent(IDENTITY, WIRE_CLOSED));

  OneWayCommandReport enrollment = sent(IDENTITY, WIRE_OPEN);
  enrollment.wire.has_intent = false;
  hub.report_oneway_command(enrollment);
  hub.report_oneway_command(sent("window_remote", WIRE_OPEN));

  EXPECT_EQ(cover.position, COVER_CLOSED);
}

TEST_F(OneWayCoverTest, ANamedPositionSuchAsFavoriteChangesNothing) {
  start();
  hub.report_oneway_command(sent(IDENTITY, WIRE_CLOSED));

  hub.report_oneway_command(sent(IDENTITY, POS_FAVORITE));

  EXPECT_EQ(cover.position, COVER_CLOSED);
}

TEST_F(OneWayCoverTest, APositionIsShownAsItsNearerEnd) {
  start();

  hub.report_oneway_command(sent(IDENTITY, 0x8C));  // 70 %
  EXPECT_EQ(cover.position, COVER_CLOSED);
  hub.report_oneway_command(sent(IDENTITY, 0x28));  // 20 %
  EXPECT_EQ(cover.position, COVER_OPEN);
}

TEST_F(OneWayCoverTest, APressOfAFollowedRemoteMovesTheStateAndSendsNothing) {
  start();

  hub.press_oneway_remote(FOLLOWED_REMOTE, wire_frame(WIRE_CLOSED));
  EXPECT_EQ(cover.position, COVER_CLOSED);
  hub.press_oneway_remote(FOLLOWED_REMOTE, wire_frame(WIRE_OPEN));
  EXPECT_EQ(cover.position, COVER_OPEN);
  hub.press_oneway_remote(FOLLOWED_REMOTE, wire_frame(WIRE_CLOSED));
  hub.press_oneway_remote(FOLLOWED_REMOTE, wire_frame(POS_STOP));
  EXPECT_EQ(cover.position, COVER_OPEN);

  EXPECT_TRUE(hub.oneway_actions.empty()) << "the press is the remote's own command; nothing is sent for it";
}

TEST_F(OneWayCoverTest, APressOfARemoteThatIsNotListedNeverMovesTheState) {
  start();
  ASSERT_EQ(hub.oneway_press_subscriptions.size(), 1u);
  EXPECT_EQ(hub.oneway_press_subscriptions[0].senders, std::vector<std::string>{FOLLOWED_REMOTE});

  hub.press_oneway_remote(OTHER_REMOTE, wire_frame(WIRE_CLOSED));

  EXPECT_EQ(cover.position, COVER_OPEN) << "a neighbour's remote of the same class must not move the cover";
}

TEST_F(OneWayCoverTest, WithoutListedRemotesItSubscribesToNoPresses) {
  start({});

  EXPECT_TRUE(hub.oneway_press_subscriptions.empty());
  EXPECT_EQ(hub.oneway_report_callbacks.size(), 1u) << "its own commands are still followed";
}

TEST_F(OneWayCoverTest, AnInvertedIdentityReadsRemotePressesAndOwnCommandsAsTheUserMeansThem) {
  hub.inverted_oneway_identities.insert(IDENTITY);
  start();

  hub.press_oneway_remote(FOLLOWED_REMOTE, wire_frame(WIRE_OPEN));
  EXPECT_EQ(cover.position, COVER_CLOSED) << "wire 0 is closed on an inverted kind";
  hub.press_oneway_remote(FOLLOWED_REMOTE, wire_frame(WIRE_CLOSED));
  EXPECT_EQ(cover.position, COVER_OPEN);

  hub.report_oneway_command(sent(IDENTITY, WIRE_OPEN));
  EXPECT_EQ(cover.position, COVER_CLOSED) << "the identity's own mirrored CLOSE goes out as wire 0";
  hub.report_oneway_command(sent(IDENTITY, POS_STOP));
  EXPECT_EQ(cover.position, COVER_OPEN) << "a stop is not mirrored";
}

// ============================================================================
// With travel times (ADR 0127)
// ============================================================================

namespace {

constexpr uint32_t OPEN_MS = 20000;
constexpr uint32_t CLOSE_MS = 10000;

class OneWayTimedCoverTest : public OneWayCoverTest {
 protected:
  void TearDown() override { esphome::test_scheduler::clear(); }

  void start_timed(OneWaySliderWay way = OneWaySliderWay::TIMED) {
    cover.set_travel_durations(OPEN_MS, CLOSE_MS);
    cover.set_slider_way(way);
    start();
  }

  /// Let @p ms pass, firing what is due and running the entity's loop once, as ESPHome does.
  void pass(uint32_t ms) {
    esphome::test_scheduler::run_until(esphome::millis() + ms);
    cover.loop();
  }

  /// What Home Assistant asks for: the slider at @p position.
  void ask(float position) {
    CoverCall call(&cover);
    call.set_position(position);
    cover.control(call);
  }

  /// Home Assistant's stop.
  void ask_stop() {
    CoverCall call(&cover);
    call.set_stop(true);
    cover.control(call);
  }

  /// Report the oldest queued command not yet reported (the queue reports in order) as sent with wire value @p main0;
  /// @p transmitted false for a burst that never reached the radio.
  void reached_radio(uint8_t main0, bool transmitted = true) {
    OneWayCommandReport report = sent(IDENTITY, main0, transmitted);
    report.ticket = hub.next_unreported_oneway_ticket();
    hub.report_oneway_command(report);
  }

  /// A remote's press, heard by the hub.
  void press(uint8_t main0) { hub.press_oneway_remote(FOLLOWED_REMOTE, wire_frame(main0)); }

  esphome::test_clock::ManualClock clock;
};

}  // namespace

TEST_F(OneWayTimedCoverTest, ItOffersAPositionOnlyWithTravelTimes) {
  EXPECT_FALSE(cover.get_traits().get_supports_position());

  cover.set_travel_durations(OPEN_MS, CLOSE_MS);

  EXPECT_TRUE(cover.get_traits().get_supports_position());
  EXPECT_TRUE(cover.get_traits().get_is_assumed_state());
}

TEST_F(OneWayTimedCoverTest, ACloseShowsClosingWhileThePositionFallsAndEndsClosedAndIdle) {
  start_timed();

  hub.report_oneway_command(sent(IDENTITY, WIRE_CLOSED));
  EXPECT_EQ(cover.current_operation, COVER_OPERATION_CLOSING);
  EXPECT_FLOAT_EQ(cover.position, 1.0F);

  pass(5000);
  EXPECT_FLOAT_EQ(cover.position, 0.5F);
  EXPECT_EQ(cover.current_operation, COVER_OPERATION_CLOSING);

  pass(5000);
  EXPECT_FLOAT_EQ(cover.position, COVER_CLOSED);
  EXPECT_EQ(cover.current_operation, COVER_OPERATION_IDLE);
}

TEST_F(OneWayTimedCoverTest, ItPublishesAboutOnceASecondWhileMovingAndOnTheStartAndTheEnd) {
  start_timed();
  const int before = cover.publish_count;
  hub.report_oneway_command(sent(IDENTITY, WIRE_CLOSED));
  EXPECT_EQ(cover.publish_count, before + 1) << "the start";

  for (int i = 0; i < 9; ++i)
    pass(100);
  EXPECT_EQ(cover.publish_count, before + 1) << "less than a second since the start";
  pass(100);
  EXPECT_EQ(cover.publish_count, before + 2) << "a second after the start";

  pass(9000);
  EXPECT_EQ(cover.publish_count, before + 3) << "the end of the run, not a publish per pass";
  pass(30000);
  EXPECT_EQ(cover.publish_count, before + 3) << "idle publishes nothing";
}

TEST_F(OneWayTimedCoverTest, AStopHeardMidRunFreezesThePositionAndShowsIdle) {
  start_timed();
  hub.report_oneway_command(sent(IDENTITY, WIRE_CLOSED));
  pass(3000);

  hub.press_oneway_remote(FOLLOWED_REMOTE, wire_frame(POS_STOP));

  EXPECT_FLOAT_EQ(cover.position, 0.7F);
  EXPECT_EQ(cover.current_operation, COVER_OPERATION_IDLE);
  pass(20000);
  EXPECT_FLOAT_EQ(cover.position, 0.7F);
}

TEST_F(OneWayTimedCoverTest, AReversalPublishesTheChangeOfDirectionAtOnce) {
  start_timed();
  hub.report_oneway_command(sent(IDENTITY, WIRE_CLOSED));
  pass(4000);  // 0.6
  const int before = cover.publish_count;

  hub.press_oneway_remote(FOLLOWED_REMOTE, wire_frame(WIRE_OPEN));

  EXPECT_EQ(cover.publish_count, before + 1);
  EXPECT_EQ(cover.current_operation, COVER_OPERATION_OPENING);
  pass(2000);
  EXPECT_FLOAT_EQ(cover.position, 0.7F) << "20 s for a full opening run: 2 s is 0.1";
}

TEST_F(OneWayTimedCoverTest, AFullRunEndsAtTheEndStopWhateverTheEstimateWasBefore) {
  start_timed();
  hub.report_oneway_command(sent(IDENTITY, WIRE_CLOSED));
  pass(4000);
  hub.press_oneway_remote(FOLLOWED_REMOTE, wire_frame(POS_STOP));  // 0.6, and say the cover really sat at 0.1

  hub.press_oneway_remote(FOLLOWED_REMOTE, wire_frame(WIRE_CLOSED));
  pass(CLOSE_MS);

  EXPECT_FLOAT_EQ(cover.position, COVER_CLOSED);
  EXPECT_EQ(cover.current_operation, COVER_OPERATION_IDLE);
}

TEST_F(OneWayTimedCoverTest, APositionFrameTravelsToItsPercentageAndStopsThereWhetherOwnOrHeard) {
  start_timed();

  hub.report_oneway_command(sent(IDENTITY, 0x8C));  // 70 % closed
  pass(10000);
  EXPECT_FLOAT_EQ(cover.position, 0.3F) << "reached in 7 s and held";
  EXPECT_EQ(cover.current_operation, COVER_OPERATION_IDLE);

  hub.press_oneway_remote(FOLLOWED_REMOTE, wire_frame(0x28));  // 20 % closed, so 0.8 open
  EXPECT_EQ(cover.current_operation, COVER_OPERATION_OPENING);
  pass(10000);
  EXPECT_FLOAT_EQ(cover.position, 0.8F);
  EXPECT_EQ(cover.current_operation, COVER_OPERATION_IDLE);
}

TEST_F(OneWayTimedCoverTest, AnInvertedIdentityEstimatesTheUsersDirection) {
  hub.inverted_oneway_identities.insert(IDENTITY);
  start_timed();

  hub.press_oneway_remote(FOLLOWED_REMOTE, wire_frame(WIRE_OPEN));  // wire 0 is closed here

  EXPECT_EQ(cover.current_operation, COVER_OPERATION_CLOSING);
}

TEST_F(OneWayTimedCoverTest, AFavoriteOrAnUnlistedRemoteOrAFailedSendChangesNothing) {
  start_timed();
  hub.report_oneway_command(sent(IDENTITY, WIRE_CLOSED));
  pass(3000);
  const int before = cover.publish_count;

  hub.press_oneway_remote(FOLLOWED_REMOTE, wire_frame(POS_FAVORITE));
  hub.press_oneway_remote(OTHER_REMOTE, wire_frame(POS_STOP));
  hub.report_oneway_command(sent(IDENTITY, WIRE_OPEN, /*transmitted=*/false));

  EXPECT_EQ(cover.publish_count, before);
  EXPECT_EQ(cover.current_operation, COVER_OPERATION_CLOSING) << "still running down as before";
  pass(2000);
  EXPECT_FLOAT_EQ(cover.position, 0.5F);
}

TEST_F(OneWayTimedCoverTest, ARunAcrossTheMillisWrapStillArrives) {
  start_timed();
  esphome::test_clock::set_ms(0xFFFFF000U);
  hub.report_oneway_command(sent(IDENTITY, WIRE_CLOSED));

  pass(10000);

  EXPECT_FLOAT_EQ(cover.position, COVER_CLOSED);
  EXPECT_EQ(cover.current_operation, COVER_OPERATION_IDLE);
}

TEST_F(OneWayTimedCoverTest, WithoutTravelTimesTheLoopDoesNothingAndTheStateIsUnchanged) {
  start();
  hub.report_oneway_command(sent(IDENTITY, WIRE_CLOSED));
  const int before = cover.publish_count;

  pass(5000);

  EXPECT_EQ(cover.publish_count, before);
  EXPECT_EQ(cover.position, COVER_CLOSED);
}

// ============================================================================
// A slider value with travel times (ADR 0128)
// ============================================================================

TEST_F(OneWayCoverTest, WithoutTravelTimesASliderValueGoesToItsNearerEnd) {
  start();

  CoverCall low(&cover);
  low.set_position(0.3F);
  cover.control(low);
  CoverCall high(&cover);
  high.set_position(0.5F);
  cover.control(high);

  ASSERT_EQ(hub.oneway_actions.size(), 2u);
  EXPECT_EQ(hub.oneway_actions[0].second, OneWayButtonAction::CLOSE);
  EXPECT_EQ(hub.oneway_actions[1].second, OneWayButtonAction::OPEN);
  EXPECT_TRUE(hub.oneway_positions.empty());
}

TEST_F(OneWayTimedCoverTest, AValueSendsCloseTowardsItAndOneStopWhenTheEstimateReachesItScheduledFromTheReport) {
  start_timed();

  ask(0.5F);
  ASSERT_EQ(hub.oneway_actions.size(), 1u);
  EXPECT_EQ(hub.oneway_actions[0].second, OneWayButtonAction::CLOSE);

  pass(7000);  // queued for a while before the burst goes out: the delay must not start yet
  EXPECT_EQ(hub.oneway_actions.size(), 1u) << "no stop before the command reached the radio";
  reached_radio(WIRE_CLOSED);
  pass(4999);
  EXPECT_EQ(hub.oneway_actions.size(), 1u);
  pass(1);

  ASSERT_EQ(hub.oneway_actions.size(), 2u) << "half of the 10 s fall, counted from the report";
  EXPECT_EQ(hub.oneway_actions[1].second, OneWayButtonAction::STOP);
  reached_radio(POS_STOP);
  pass(60000);
  EXPECT_EQ(hub.oneway_actions.size(), 2u) << "the stop is sent once";
  EXPECT_FLOAT_EQ(cover.position, 0.5F);
  EXPECT_EQ(cover.current_operation, COVER_OPERATION_IDLE);
}

TEST_F(OneWayTimedCoverTest, AValueAboveThePositionOpensWithTheOpenTime) {
  start_timed();
  reached_radio(WIRE_CLOSED);
  pass(CLOSE_MS);  // closed
  ask(0.25F);
  ASSERT_EQ(hub.oneway_actions.size(), 1u);
  EXPECT_EQ(hub.oneway_actions[0].second, OneWayButtonAction::OPEN);

  reached_radio(WIRE_OPEN);
  pass(4999);
  EXPECT_EQ(hub.oneway_actions.size(), 1u);
  pass(1);

  ASSERT_EQ(hub.oneway_actions.size(), 2u) << "a quarter of the 20 s rise";
  EXPECT_EQ(hub.oneway_actions[1].second, OneWayButtonAction::STOP);
}

TEST_F(OneWayTimedCoverTest, AnEndValueSendsAPlainCommandWithNoStop) {
  start_timed();

  ask(0.0F);
  reached_radio(WIRE_CLOSED);
  ask(1.0F);
  reached_radio(WIRE_OPEN);
  pass(120000);

  ASSERT_EQ(hub.oneway_actions.size(), 2u);
  EXPECT_EQ(hub.oneway_actions[0].second, OneWayButtonAction::CLOSE);
  EXPECT_EQ(hub.oneway_actions[1].second, OneWayButtonAction::OPEN);
  EXPECT_FLOAT_EQ(cover.position, COVER_OPEN) << "the end stop re-anchors the estimate";
}

TEST_F(OneWayTimedCoverTest, ANewValueCancelsThePendingStopAndIsPlannedFromThePositionReached) {
  start_timed();
  ask(0.2F);
  reached_radio(WIRE_CLOSED);
  pass(2000);  // 0.8 so far; the first stop was due at 8 s

  ask(0.5F);  // 0.3 more of a fall: 3 s
  ASSERT_EQ(hub.oneway_actions.size(), 2u);
  EXPECT_EQ(hub.oneway_actions[1].second, OneWayButtonAction::CLOSE);
  reached_radio(WIRE_CLOSED);
  pass(2999);
  EXPECT_EQ(hub.oneway_actions.size(), 2u);
  pass(1);

  ASSERT_EQ(hub.oneway_actions.size(), 3u);
  EXPECT_EQ(hub.oneway_actions[2].second, OneWayButtonAction::STOP);
  pass(60000);
  EXPECT_EQ(hub.oneway_actions.size(), 3u) << "the stop of the first value was cancelled, no stale stop follows";
}

TEST_F(OneWayTimedCoverTest, AReversalSendsTheOppositeCommand) {
  start_timed();
  ask(0.1F);
  reached_radio(WIRE_CLOSED);
  pass(5000);  // at 0.5, falling

  ask(0.7F);

  ASSERT_EQ(hub.oneway_actions.size(), 2u);
  EXPECT_EQ(hub.oneway_actions[1].second, OneWayButtonAction::OPEN);
  reached_radio(WIRE_OPEN);
  pass(3999);
  EXPECT_EQ(hub.oneway_actions.size(), 2u);
  pass(1);
  EXPECT_EQ(hub.oneway_actions.size(), 3u) << "0.2 of a 20 s rise";
}

TEST_F(OneWayTimedCoverTest, AStopFromHomeAssistantCancelsThePendingStopAndSendsItsOwn) {
  start_timed();
  ask(0.2F);
  reached_radio(WIRE_CLOSED);
  pass(3000);

  ask_stop();
  reached_radio(POS_STOP);
  pass(60000);

  ASSERT_EQ(hub.oneway_actions.size(), 2u);
  EXPECT_EQ(hub.oneway_actions[1].second, OneWayButtonAction::STOP) << "the user's stop only, none of the plan's";
  EXPECT_NEAR(cover.position, 0.7F, 1e-4F);
}

TEST_F(OneWayTimedCoverTest, AStopHeardFromAButtonOrAnotherSenderCancelsThePendingStop) {
  start_timed();
  ask(0.2F);
  reached_radio(WIRE_CLOSED);
  pass(3000);

  reached_radio(POS_STOP);  // a button's stop, reported by the hub
  pass(60000);

  EXPECT_EQ(hub.oneway_actions.size(), 1u) << "no second stop on top of the first";
}

TEST_F(OneWayTimedCoverTest, AFollowedRemotesPressCancelsThePendingStop) {
  start_timed();
  ask(0.2F);
  reached_radio(WIRE_CLOSED);
  pass(3000);

  press(WIRE_OPEN);
  pass(120000);

  EXPECT_EQ(hub.oneway_actions.size(), 1u) << "the wall remote took over; the stop would be stale";
  EXPECT_FLOAT_EQ(cover.position, COVER_OPEN);
}

TEST_F(OneWayTimedCoverTest, AFollowedRemotesPressBeforeTheCommandReachedTheRadioDropsTheStopToo) {
  start_timed();
  ask(0.5F);  // queued

  press(WIRE_OPEN);
  reached_radio(WIRE_CLOSED);
  pass(60000);

  EXPECT_EQ(hub.oneway_actions.size(), 1u);
}

TEST_F(OneWayTimedCoverTest, ACommandThatDidNotReachTheRadioSchedulesNoStopAndChangesNothing) {
  start_timed();
  ask(0.5F);

  reached_radio(WIRE_CLOSED, /*transmitted=*/false);
  pass(60000);

  EXPECT_EQ(hub.oneway_actions.size(), 1u);
  EXPECT_FLOAT_EQ(cover.position, COVER_OPEN);
}

TEST_F(OneWayTimedCoverTest, TwoValuesQueuedTheSameWayStopOnceAtTheLatest) {
  start_timed();
  ask(0.7F);
  ask(0.5F);  // before the first value's command went out
  ASSERT_EQ(hub.oneway_actions.size(), 2u);

  reached_radio(WIRE_CLOSED);  // the first value's command: the fall begins
  pass(300);
  EXPECT_EQ(hub.oneway_actions.size(), 2u) << "the first value's stop was cancelled by the second";
  reached_radio(WIRE_CLOSED);  // the latest value's command arms its stop, timed from the position reached
  pass(4699);
  EXPECT_EQ(hub.oneway_actions.size(), 2u);
  pass(1);

  ASSERT_EQ(hub.oneway_actions.size(), 3u);
  EXPECT_EQ(hub.oneway_actions[2].second, OneWayButtonAction::STOP);
  pass(60000);
  EXPECT_EQ(hub.oneway_actions.size(), 3u) << "one stop";
}

TEST_F(OneWayTimedCoverTest, ARunThatPassedTheValueWhileItsCommandWaitedTurnsBackAndStopsThere) {
  start_timed();
  ask(0.1F);
  reached_radio(WIRE_CLOSED);
  pass(5000);  // at 0.5, falling

  ask(0.45F);  // 0.5 s more of the fall
  pass(1000);  // the command waits longer than that for the radio: at 0.4
  reached_radio(WIRE_CLOSED);

  ASSERT_EQ(hub.oneway_actions.size(), 3u) << "the way back, not a run to closed";
  EXPECT_EQ(hub.oneway_actions[2].second, OneWayButtonAction::OPEN);
}

TEST_F(OneWayTimedCoverTest, AForeignCommandQueuedJustBeforeAValueDoesNotLoseItsStop) {
  start_timed();
  hub.send_oneway_action(IDENTITY, OneWayButtonAction::OPEN);  // a button of the identity, queued just before the value
  ask(0.5F);

  reached_radio(WIRE_OPEN);  // the button's report arms no stop
  pass(1000);
  EXPECT_EQ(hub.oneway_actions.size(), 2u);
  reached_radio(WIRE_CLOSED);  // the value's own report
  pass(4999);
  EXPECT_EQ(hub.oneway_actions.size(), 2u);
  pass(1);

  ASSERT_EQ(hub.oneway_actions.size(), 3u) << "half of the 10 s fall, counted from the value's own report";
  EXPECT_EQ(hub.oneway_actions[2].second, OneWayButtonAction::STOP);
}

TEST_F(OneWayTimedCoverTest, AForeignCommandQueuedJustAfterAValueIsTheLastCommandAndWins) {
  start_timed();
  ask(0.5F);
  hub.send_oneway_action(IDENTITY, OneWayButtonAction::CLOSE);  // a button of the identity, queued just after the value

  reached_radio(WIRE_CLOSED);  // the value's own report arms its stop
  pass(100);
  reached_radio(WIRE_CLOSED);  // the button's report comes last and cancels it
  pass(120000);

  EXPECT_EQ(hub.oneway_actions.size(), 2u) << "the last queued command wins: the cover closes fully, no stop follows";
}

TEST_F(OneWayTimedCoverTest, AForeignCommandQueuedJustBeforeAValueDoesNotLoseTheStopOfTheWayBack) {
  start_timed();
  ask(0.1F);
  reached_radio(WIRE_CLOSED);
  pass(5000);  // at 0.5, falling

  hub.send_oneway_action(IDENTITY, OneWayButtonAction::CLOSE);  // a button, just before the value
  ask(0.45F);
  pass(1000);                  // at 0.4 when the burst goes out
  reached_radio(WIRE_CLOSED);  // the button's report leaves the value waiting
  reached_radio(WIRE_CLOSED);  // the value's report finds the value behind: the way back is queued
  ASSERT_EQ(hub.oneway_actions.size(), 4u);
  EXPECT_EQ(hub.oneway_actions[3].second, OneWayButtonAction::OPEN);
  reached_radio(WIRE_OPEN);  // the way back's own report arms its stop, 0.05 of a 20 s rise
  pass(999);
  EXPECT_EQ(hub.oneway_actions.size(), 4u);
  pass(1);

  ASSERT_EQ(hub.oneway_actions.size(), 5u);
  EXPECT_EQ(hub.oneway_actions[4].second, OneWayButtonAction::STOP);
}

TEST_F(OneWayTimedCoverTest, AForeignCommandQueuedJustAfterTheWayBackLeavesTheWayBacksStop) {
  start_timed();
  ask(0.1F);
  reached_radio(WIRE_CLOSED);
  pass(5000);  // at 0.5, falling

  ask(0.45F);
  pass(1000);                  // at 0.4
  reached_radio(WIRE_CLOSED);  // the value's report finds the value behind: the way back is queued
  ASSERT_EQ(hub.oneway_actions.size(), 3u);
  hub.send_oneway_action(IDENTITY, OneWayButtonAction::CLOSE);  // a button, queued behind the way back
  reached_radio(WIRE_OPEN);                                     // the way back's report arms its stop
  pass(100);
  reached_radio(WIRE_CLOSED);  // the button's report comes last and cancels the stop that was armed
  pass(120000);

  EXPECT_EQ(hub.oneway_actions.size(), 4u) << "the button was the last command: no stop follows it";
}

TEST_F(OneWayTimedCoverTest, ThePlanIsInTheUsersDirectionForAnInvertedIdentity) {
  hub.inverted_oneway_identities.insert(IDENTITY);
  start_timed();
  ask(0.5F);

  reached_radio(WIRE_OPEN);  // an inverted kind: the user's close is wire 0
  pass(5000);

  ASSERT_EQ(hub.oneway_actions.size(), 2u);
  EXPECT_EQ(hub.oneway_actions[1].second, OneWayButtonAction::STOP);
}

TEST_F(OneWayTimedCoverTest, AFrameValueSendsOnePositionFrameOnTheWireScaleAndNoStop) {
  start_timed(OneWaySliderWay::FRAME);

  ask(0.25F);
  pass(120000);

  ASSERT_EQ(hub.oneway_positions.size(), 1u);
  EXPECT_EQ(hub.oneway_positions[0], std::make_pair(std::string(IDENTITY), uint8_t{75}))
      << "0 is open on the wire, so 25 % open is 75";
  EXPECT_TRUE(hub.oneway_actions.empty()) << "no open, close or stop";
}

TEST_F(OneWayTimedCoverTest, ThePositionFramesReportMakesTheCoverTravelToItAtTheTravelRate) {
  start_timed(OneWaySliderWay::FRAME);
  ask(0.25F);

  reached_radio(0x96);  // wire 75 %
  EXPECT_EQ(cover.current_operation, COVER_OPERATION_CLOSING);
  EXPECT_FLOAT_EQ(cover.position, 1.0F) << "it does not jump, unlike a dimmer";
  pass(7500);  // 0.75 of the 10 s fall

  EXPECT_NEAR(cover.position, 0.25F, 1e-4F);
  EXPECT_EQ(cover.current_operation, COVER_OPERATION_IDLE);
}
