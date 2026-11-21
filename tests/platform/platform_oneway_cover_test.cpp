/// @file platform_oneway_cover_test.cpp
/// @brief Tests for the assumed-state cover of a one-way controller (ADR 0117), run against a double of the hub's
/// control port.

#include "platform_oneway_entities.h"

#include "hub_port_doubles.h"

#include <gtest/gtest.h>

using namespace esphome::home_io_control;
using namespace esphome::cover;

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
