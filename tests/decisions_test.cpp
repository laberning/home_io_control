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

  EXPECT_EQ(decisions::classify_pairing_discovery_response(discovery), decisions::PairingDiscoveryDisposition::ACCEPT)
      << "discovery response should be accepted during pairing discovery wait";
}

TEST(Decisions, PairingDiscoveryIgnoreNonDiscovery) {
  const IoFrame ignored = make_frame(DST_ID, OWN_ID, CMD_PRIVATE_RESP, 6);

  EXPECT_EQ(decisions::classify_pairing_discovery_response(ignored), decisions::PairingDiscoveryDisposition::IGNORE)
      << "non-discovery frame should be ignored during pairing discovery wait";
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
