#include "hub_decisions.h"

#include "test_helpers.h"

using namespace esphome::home_io_control;
using namespace test;

// ============================================================================
// Decisions test suite
// ============================================================================
// Pure frame-classification logic: exchange state-machine transitions (first
// response handling, final acceptance) and pairing discovery/challenge decision
// trees. These are constexpr-friendly inline functions, testable without radio.

// ========================================================================================
// Exchange first-response classification
// ========================================================================================

TEST(Decisions, ExchangeFirstResponseDirect) {
  const IoFrame request = make_execute(100);
  const IoFrame direct = make_frame(DST_ID, OWN_ID, CMD_PRIVATE_RESP, 6);

  EXPECT_EQ(decisions::classify_exchange_first_response(request, direct),
            decisions::ExchangeFirstResponseDisposition::COMPLETE_DIRECT)
      << "matching non-challenge first response should complete directly";
}

TEST(Decisions, ExchangeFirstResponseRequiresAuth) {
  const IoFrame request = make_execute(100);
  const IoFrame challenge = make_frame(DST_ID, OWN_ID, CMD_CHALLENGE_REQ, HMAC_SIZE);

  EXPECT_EQ(decisions::classify_exchange_first_response(request, challenge),
            decisions::ExchangeFirstResponseDisposition::REQUIRE_AUTH)
      << "matching challenge first response should require auth";
}

TEST(Decisions, ExchangeFirstResponseIgnoreUnrelated) {
  const IoFrame request = make_execute(100);
  const IoFrame unrelated = make_frame(FOREIGN_ID, OWN_ID, CMD_PRIVATE_RESP, 6);

  EXPECT_EQ(decisions::classify_exchange_first_response(request, unrelated),
            decisions::ExchangeFirstResponseDisposition::IGNORE_UNRELATED)
      << "unrelated first response should be ignored";
}

// ========================================================================================
// Exchange final-response classification
// ========================================================================================

TEST(Decisions, ExchangeFinalResponseAccept) {
  const IoFrame request = make_execute(100);
  const IoFrame direct = make_frame(DST_ID, OWN_ID, CMD_PRIVATE_RESP, 6);

  EXPECT_EQ(decisions::classify_exchange_final_response(request, direct),
            decisions::ExchangeFinalResponseDisposition::ACCEPT)
      << "matching final response should be accepted";
}

TEST(Decisions, ExchangeFinalResponseIgnoreUnrelated) {
  const IoFrame request = make_execute(100);
  const IoFrame unrelated = make_frame(FOREIGN_ID, OWN_ID, CMD_PRIVATE_RESP, 6);

  EXPECT_EQ(decisions::classify_exchange_final_response(request, unrelated),
            decisions::ExchangeFinalResponseDisposition::IGNORE_UNRELATED)
      << "unrelated final response should be ignored";
}

// ========================================================================================
// Pairing discovery response classification
// ========================================================================================

TEST(Decisions, PairingDiscoveryAccept) {
  const IoFrame discovery = make_frame(DST_ID, OWN_ID, CMD_DISCOVER_RESP, 2);

  EXPECT_EQ(decisions::classify_pairing_discovery_response(discovery, OWN_ID),
            decisions::PairingDiscoveryDisposition::ACCEPT)
      << "discovery response addressed to this controller should be accepted during discovery wait";
}

TEST(Decisions, PairingDiscoveryInvalidNonDiscovery) {
  const IoFrame ignored = make_frame(DST_ID, OWN_ID, CMD_PRIVATE_RESP, 6);

  EXPECT_EQ(decisions::classify_pairing_discovery_response(ignored, OWN_ID),
            decisions::PairingDiscoveryDisposition::INVALID)
      << "non-discovery frame should be invalid during pairing discovery wait";
}

TEST(Decisions, PairingDiscoveryInvalidWrongDestination) {
  // DISCOVER_REQ goes out to a shared broadcast address, so a well-formed 0x29 arriving during
  // the discovery window may be a device answering a different, concurrent controller's request
  // rather than ours. Real hardware addresses its response back to the requesting controller, so
  // a mismatched dst means this frame was never meant for us.
  const IoFrame discovery_for_someone_else = make_frame(DST_ID, FOREIGN_ID, CMD_DISCOVER_RESP, 2);

  EXPECT_EQ(decisions::classify_pairing_discovery_response(discovery_for_someone_else, OWN_ID),
            decisions::PairingDiscoveryDisposition::INVALID)
      << "a discovery response addressed to a different controller must not be accepted as ours";
}

// ========================================================================================
// Pairing key challenge classification
// ========================================================================================

TEST(Decisions, PairingKeyChallengeAccept) {
  const IoFrame key_challenge = make_frame(DST_ID, OWN_ID, CMD_CHALLENGE_REQ, HMAC_SIZE);

  EXPECT_EQ(decisions::classify_pairing_key_challenge(key_challenge, DST_ID, OWN_ID),
            decisions::PairingKeyChallengeDisposition::ACCEPT)
      << "matching key challenge should be accepted during pairing key wait";
}

TEST(Decisions, PairingKeyChallengeIgnoreWrongCmd) {
  const IoFrame wrong_cmd = make_frame(DST_ID, OWN_ID, CMD_PRIVATE_RESP, HMAC_SIZE);

  EXPECT_EQ(decisions::classify_pairing_key_challenge(wrong_cmd, DST_ID, OWN_ID),
            decisions::PairingKeyChallengeDisposition::IGNORE)
      << "wrong command should be ignored during pairing key wait";
}

TEST(Decisions, PairingKeyChallengeIgnoreWrongLen) {
  const IoFrame wrong_len = make_frame(DST_ID, OWN_ID, CMD_CHALLENGE_REQ, HMAC_SIZE - 1);

  EXPECT_EQ(decisions::classify_pairing_key_challenge(wrong_len, DST_ID, OWN_ID),
            decisions::PairingKeyChallengeDisposition::IGNORE)
      << "wrong length challenge should be ignored during pairing key wait";
}

TEST(Decisions, PairingKeyChallengeIgnoreWrongNodes) {
  const IoFrame wrong_nodes = make_frame(FOREIGN_ID, OWN_ID, CMD_CHALLENGE_REQ, HMAC_SIZE);

  EXPECT_EQ(decisions::classify_pairing_key_challenge(wrong_nodes, DST_ID, OWN_ID),
            decisions::PairingKeyChallengeDisposition::IGNORE)
      << "wrong node pairing challenge should be ignored during pairing key wait";
}

// ============================================================================
// 1W burst suppression — is_duplicate_1w_frame()
// ============================================================================
// The key includes the decoded intent, not just the command byte: a move and a
// stop are both CMD_EXECUTE and differ only in main0.

namespace {

decisions::OneWayDedupState dedup_state(const char *src, uint8_t cmd, bool has_intent, uint8_t main0,
                                        uint32_t timestamp) {
  return decisions::OneWayDedupState{src, cmd, has_intent, main0, 0x00, timestamp};
}

constexpr uint32_t DEDUP_WINDOW_MS = 2000;

}  // namespace

TEST(Decisions, OneWayDedupFirstFrameIsNeverADuplicate) {
  const decisions::OneWayDedupState nothing_seen_yet{};
  const auto incoming = dedup_state("AABBCC", CMD_EXECUTE, true, 0xC8, 1000);

  EXPECT_FALSE(decisions::is_duplicate_1w_frame(nothing_seen_yet, incoming, DEDUP_WINDOW_MS))
      << "with no previous frame recorded the src IDs differ, so nothing can be suppressed";
}

TEST(Decisions, OneWayDedupSuppressesIdenticalRepeatInsideWindow) {
  const auto last = dedup_state("AABBCC", CMD_EXECUTE, true, 0xC8, 1000);
  const auto repeat = dedup_state("AABBCC", CMD_EXECUTE, true, 0xC8, 1040);

  EXPECT_TRUE(decisions::is_duplicate_1w_frame(last, repeat, DEDUP_WINDOW_MS))
      << "the 4x/40ms reliability burst must collapse into one logical press";
}

TEST(Decisions, OneWayDedupAllowsIdenticalRepeatAfterWindow) {
  const auto last = dedup_state("AABBCC", CMD_EXECUTE, true, 0xC8, 1000);
  const auto later = dedup_state("AABBCC", CMD_EXECUTE, true, 0xC8, 1000 + DEDUP_WINDOW_MS);

  EXPECT_FALSE(decisions::is_duplicate_1w_frame(last, later, DEDUP_WINDOW_MS))
      << "a genuine second press after the window is a new logical press";
}

TEST(Decisions, OneWayDedupDoesNotSuppressStopAfterMove) {
  // Both frames are CMD_EXECUTE; only main0 differs. Keying on the command byte alone would
  // discard the stop, losing the sender event, the optimistic clear, and the immediate poll.
  const auto move = dedup_state("AABBCC", CMD_EXECUTE, true, 0xC8, 1000);
  const auto stop = dedup_state("AABBCC", CMD_EXECUTE, true, POS_STOP, 1500);

  EXPECT_FALSE(decisions::is_duplicate_1w_frame(move, stop, DEDUP_WINDOW_MS))
      << "a stop pressed shortly after a move carries a different intent and must be processed";
}

TEST(Decisions, OneWayDedupDoesNotSuppressDifferentRemote) {
  const auto last = dedup_state("AABBCC", CMD_EXECUTE, true, 0xC8, 1000);
  const auto other_remote = dedup_state("DDEEFF", CMD_EXECUTE, true, 0xC8, 1040);

  EXPECT_FALSE(decisions::is_duplicate_1w_frame(last, other_remote, DEDUP_WINDOW_MS))
      << "two remotes pressed at once are two presses";
}

TEST(Decisions, OneWayDedupIgnoresIntentBytesWhenNoIntentWasDecoded) {
  // Commands without a decodable intent (e.g. write-private) carry stale main0/main1; the key
  // must fall back to src+cmd for them rather than comparing meaningless bytes.
  const auto last = dedup_state("AABBCC", CMD_WRITE_PRIVATE, false, 0x11, 1000);
  const auto repeat = dedup_state("AABBCC", CMD_WRITE_PRIVATE, false, 0x99, 1040);

  EXPECT_TRUE(decisions::is_duplicate_1w_frame(last, repeat, DEDUP_WINDOW_MS))
      << "intent-less commands dedup on src+cmd alone";
}

TEST(Decisions, OneWayDedupSurvivesMillisWrap) {
  const auto last = dedup_state("AABBCC", CMD_EXECUTE, true, 0xC8, 0xFFFFFF00);
  const auto after_wrap = dedup_state("AABBCC", CMD_EXECUTE, true, 0xC8, 0x00000040);

  EXPECT_TRUE(decisions::is_duplicate_1w_frame(last, after_wrap, DEDUP_WINDOW_MS))
      << "unsigned subtraction must keep the window correct across the millis() wrap";
}

// ============================================================================
// Background-poll deferral — defer_background_poll_for_1w_activity()
// ============================================================================

TEST(Decisions, DeferHoldsBackgroundPollDuringRemoteActivity) {
  EXPECT_TRUE(decisions::defer_background_poll_for_1w_activity(/*next_op_is_background=*/true,
                                                               /*first_1w_activity_ms=*/1000,
                                                               /*last_1w_activity_ms=*/1000,
                                                               /*now=*/1100, /*quiet_ms=*/700,
                                                               /*max_defer_ms=*/5000))
      << "a poll must not seize the radio while the remote that triggered it is still transmitting";
}

TEST(Decisions, DeferReleasesBackgroundPollAfterQuietPeriod) {
  EXPECT_FALSE(decisions::defer_background_poll_for_1w_activity(true, 1000, 1000, 1700, 700, 5000))
      << "the hold must expire exactly at the quiet period, not linger";
}

TEST(Decisions, DeferNeverHoldsBackControlOperations) {
  EXPECT_FALSE(decisions::defer_background_poll_for_1w_activity(/*next_op_is_background=*/false,
                                                                /*first_1w_activity_ms=*/1000,
                                                                /*last_1w_activity_ms=*/1000,
                                                                /*now=*/1100, /*quiet_ms=*/700,
                                                                /*max_defer_ms=*/5000))
      << "a user command must not wait on a remote the user may not even own";
}

TEST(Decisions, DeferIsInactiveBeforeAnyRemoteIsHeard) {
  EXPECT_FALSE(decisions::defer_background_poll_for_1w_activity(true, /*first_1w_activity_ms=*/0,
                                                                /*last_1w_activity_ms=*/0,
                                                                /*now=*/500, /*quiet_ms=*/700,
                                                                /*max_defer_ms=*/5000))
      << "0 means no 1W frame has ever been seen, not 'heard one at boot'";
}

TEST(Decisions, DeferHoldsThroughRepeatedReArmingUnderTheCap) {
  // Burst started at 0; a frame every 500ms keeps re-arming the quiet-period hold, but the cap
  // (5000ms from burst start) has not been reached yet.
  EXPECT_TRUE(decisions::defer_background_poll_for_1w_activity(true, /*first_1w_activity_ms=*/0,
                                                               /*last_1w_activity_ms=*/4500,
                                                               /*now=*/4600, /*quiet_ms=*/700,
                                                               /*max_defer_ms=*/5000))
      << "sustained sub-quiet_ms traffic must keep deferring right up to the cap";
}

TEST(Decisions, DeferReleasesAtTheCapEvenMidBurst) {
  // Same sustained traffic, but now() has reached the cap measured from first_1w_activity_ms —
  // the poll must be let through even though the most recent frame is still within quiet_ms.
  EXPECT_FALSE(decisions::defer_background_poll_for_1w_activity(true, /*first_1w_activity_ms=*/0,
                                                                /*last_1w_activity_ms=*/4900,
                                                                /*now=*/5000, /*quiet_ms=*/700,
                                                                /*max_defer_ms=*/5000))
      << "sustained 1W traffic must not starve a background poll past max_defer_ms";
}

// ============================================================================
// Burst start-of-window detection — oneway_burst_started_fresh()
// ============================================================================

TEST(Decisions, BurstStartsFreshWhenNoneSeenYet) {
  EXPECT_TRUE(decisions::oneway_burst_started_fresh(/*last_1w_activity_ms=*/0, /*now=*/500, /*quiet_ms=*/700));
}

TEST(Decisions, BurstStartsFreshAfterQuietGap) {
  EXPECT_TRUE(decisions::oneway_burst_started_fresh(/*last_1w_activity_ms=*/1000, /*now=*/1700, /*quiet_ms=*/700))
      << "a gap reaching quiet_ms means the previous burst already ended";
}

TEST(Decisions, BurstContinuesWithinQuietGap) {
  EXPECT_FALSE(decisions::oneway_burst_started_fresh(/*last_1w_activity_ms=*/1000, /*now=*/1699, /*quiet_ms=*/700))
      << "a frame arriving just inside quiet_ms extends the current burst rather than starting a new one";
}
