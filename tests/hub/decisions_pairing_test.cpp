#include "decisions_pairing.h"

#include "test_helpers.h"

using namespace esphome::home_io_control;
using namespace test;

// ============================================================================
// Pairing classification: discovery, key challenge, discover-confirm, key-confirm, the device behind a pairing node.
// ============================================================================
// Pure helpers in decisions_pairing.h, tested without radio or timing.

// ========================================================================================
// Pairing discovery response classification
// ========================================================================================

TEST(Decisions, DiscoveryRequestStaysOnCh2UnlessTheChannelsRotate) {
  for (uint8_t attempt = 0; attempt <= 6; attempt++) {
    EXPECT_EQ(decisions::discovery_request_freq(false, attempt, ChannelSet::all()), FREQ_CH2)
        << "attempt " << static_cast<int>(attempt);
  }
}

TEST(Decisions, DiscoveryRequestRotatesCh2Ch1Ch3AndStartsOver) {
  const ChannelSet all = ChannelSet::all();
  EXPECT_EQ(decisions::discovery_request_freq(true, 1, all), FREQ_CH2);
  EXPECT_EQ(decisions::discovery_request_freq(true, 2, all), FREQ_CH1);
  EXPECT_EQ(decisions::discovery_request_freq(true, 3, all), FREQ_CH3);
  EXPECT_EQ(decisions::discovery_request_freq(true, 4, all), FREQ_CH2);
  EXPECT_EQ(decisions::discovery_request_freq(true, 0, all), FREQ_CH2) << "attempts count from 1";
}

TEST(Decisions, DiscoveryRotationSkipsAChannelTheSetExcludes) {
  const ChannelSet without_ch1 = ChannelSet::all().without_index(1);  // IO_CHANNELS = CH2, CH1, CH3
  EXPECT_EQ(decisions::discovery_request_freq(true, 1, without_ch1), FREQ_CH2);
  EXPECT_EQ(decisions::discovery_request_freq(true, 2, without_ch1), FREQ_CH3);
  EXPECT_EQ(decisions::discovery_request_freq(true, 3, without_ch1), FREQ_CH2);
}

TEST(Decisions, DiscoveryRequestLeavesCh2WhenTheSetExcludesIt) {
  const ChannelSet without_ch2 = ChannelSet::all().without_index(0);
  for (uint8_t attempt = 1; attempt <= 4; attempt++) {
    EXPECT_EQ(decisions::discovery_request_freq(false, attempt, without_ch2), FREQ_CH1)
        << "the default goes to the first allowed channel, attempt " << static_cast<int>(attempt);
  }
  EXPECT_EQ(decisions::discovery_request_freq(true, 1, without_ch2), FREQ_CH1);
  EXPECT_EQ(decisions::discovery_request_freq(true, 2, without_ch2), FREQ_CH3);
}

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

// ========================================================================================
// Pairing discover-confirm reply classification
// ========================================================================================

TEST(Decisions, PairingDiscoverConfirmAckAccepted) {
  const IoFrame ack = make_frame(DST_ID, OWN_ID, CMD_DISCOVER_CONFIRM_ACK, 0);

  EXPECT_EQ(decisions::classify_pairing_discover_confirm_reply(ack, DST_ID, OWN_ID),
            decisions::PairingDiscoverConfirmDisposition::ACK)
      << "matching 0x2D should be classified as ACK";
}

TEST(Decisions, PairingDiscoverConfirmIgnoresWrongSrc) {
  const IoFrame foreign = make_frame(FOREIGN_ID, OWN_ID, CMD_DISCOVER_CONFIRM_ACK, 0);

  EXPECT_EQ(decisions::classify_pairing_discover_confirm_reply(foreign, DST_ID, OWN_ID),
            decisions::PairingDiscoverConfirmDisposition::IGNORE)
      << "a 0x2D from a different node should be ignored";
}

TEST(Decisions, PairingDiscoverConfirmIgnoresWrongDst) {
  const IoFrame wrong_dst = make_frame(DST_ID, FOREIGN_ID, CMD_DISCOVER_CONFIRM_ACK, 0);

  EXPECT_EQ(decisions::classify_pairing_discover_confirm_reply(wrong_dst, DST_ID, OWN_ID),
            decisions::PairingDiscoverConfirmDisposition::IGNORE)
      << "a 0x2D addressed to someone else should be ignored";
}

TEST(Decisions, PairingDiscoverConfirmErrorReply) {
  const IoFrame error = make_frame(DST_ID, OWN_ID, CMD_ERROR_RESP, 1);

  EXPECT_EQ(decisions::classify_pairing_discover_confirm_reply(error, DST_ID, OWN_ID),
            decisions::PairingDiscoverConfirmDisposition::ERROR)
      << "a matching CMD_ERROR_RESP should be classified as ERROR";
}

TEST(Decisions, PairingDiscoverConfirmIgnoresUnrelatedCommands) {
  const IoFrame repeated_discover_resp = make_frame(DST_ID, OWN_ID, CMD_DISCOVER_RESP, 2);
  EXPECT_EQ(decisions::classify_pairing_discover_confirm_reply(repeated_discover_resp, DST_ID, OWN_ID),
            decisions::PairingDiscoverConfirmDisposition::IGNORE)
      << "a repeated 0x29 is not an answer to the discover-confirm step";

  const IoFrame challenge = make_frame(DST_ID, OWN_ID, CMD_CHALLENGE_REQ, HMAC_SIZE);
  EXPECT_EQ(decisions::classify_pairing_discover_confirm_reply(challenge, DST_ID, OWN_ID),
            decisions::PairingDiscoverConfirmDisposition::IGNORE)
      << "a 0x3C is not an answer to the discover-confirm step";
}

// ========================================================================================
// Pairing key-confirm reply classification (slow-turnaround wait_for_key_confirm_() path)
// ========================================================================================

TEST(Decisions, PairingKeyConfirmConfirmAccepted) {
  const IoFrame request = make_frame(OWN_ID, DST_ID, CMD_KEY_TRANSFER, 16);
  const IoFrame confirm = make_frame(DST_ID, OWN_ID, CMD_KEY_CONFIRM, 0);

  EXPECT_EQ(decisions::classify_pairing_key_confirm_reply(request, confirm),
            decisions::PairingKeyConfirmDisposition::CONFIRM)
      << "matching 0x33 should be classified as CONFIRM";
}

TEST(Decisions, PairingKeyConfirmChallengeFromFreshChallenge) {
  const IoFrame request = make_frame(OWN_ID, DST_ID, CMD_KEY_TRANSFER, 16);
  const IoFrame challenge = make_frame(DST_ID, OWN_ID, CMD_CHALLENGE_REQ, HMAC_SIZE);

  EXPECT_EQ(decisions::classify_pairing_key_confirm_reply(request, challenge),
            decisions::PairingKeyConfirmDisposition::CHALLENGE)
      << "a well-formed 0x3C (6-byte challenge) should be classified as CHALLENGE";
}

TEST(Decisions, PairingKeyConfirmRefusesShortChallenge) {
  const IoFrame request = make_frame(OWN_ID, DST_ID, CMD_KEY_TRANSFER, 16);
  const IoFrame short_challenge = make_frame(DST_ID, OWN_ID, CMD_CHALLENGE_REQ, HMAC_SIZE - 1);

  EXPECT_EQ(decisions::classify_pairing_key_confirm_reply(request, short_challenge),
            decisions::PairingKeyConfirmDisposition::REFUSE)
      << "a malformed 0x3C does not satisfy classify_pairing_key_challenge(), so it falls to REFUSE";
}

TEST(Decisions, PairingKeyConfirmRefusesErrorResponse) {
  const IoFrame request = make_frame(OWN_ID, DST_ID, CMD_KEY_TRANSFER, 16);
  const IoFrame error = make_frame(DST_ID, OWN_ID, CMD_ERROR_RESP, 1);

  EXPECT_EQ(decisions::classify_pairing_key_confirm_reply(request, error),
            decisions::PairingKeyConfirmDisposition::REFUSE)
      << "CMD_ERROR_RESP should be classified as REFUSE, an explicit refusal";
}

TEST(Decisions, PairingKeyConfirmIgnoresForeignEndpoints) {
  const IoFrame request = make_frame(OWN_ID, DST_ID, CMD_KEY_TRANSFER, 16);
  const IoFrame foreign = make_frame(FOREIGN_ID, OWN_ID, CMD_KEY_CONFIRM, 0);

  EXPECT_EQ(decisions::classify_pairing_key_confirm_reply(request, foreign),
            decisions::PairingKeyConfirmDisposition::IGNORE)
      << "a reply from a node other than the one we transferred the key to should be ignored";
}

// ========================================================================================
// Discover-confirm try-rotation mapping
// ========================================================================================

TEST(Decisions, DiscoverConfirmTryRotationMapping) {
  EXPECT_FALSE(decisions::discover_confirm_try_rotates(1)) << "try 1 should hold the request channel";
  EXPECT_TRUE(decisions::discover_confirm_try_rotates(2)) << "try 2 should rotate all channels";
  EXPECT_FALSE(decisions::discover_confirm_try_rotates(3)) << "try 3 should hold the request channel";
}

// === Pairing: the device behind a pairing node ===

TEST(PairingNextAddress, OnlyAnAtlanticRemoteControllerHasAHeaterAtTheNextAddress) {
  const auto heater = decisions::heater_behind_pairing_node(DeviceType::REMOTE_CONTROLLER, MANUFACTURER_ATLANTIC_GROUP);
  ASSERT_TRUE(heater.has_value());
  EXPECT_EQ(heater->type, DeviceType::ELECTRICAL_HEATER);
  EXPECT_EQ(heater->subtype, SUBTYPE_HEATER_WITH_SETPOINT);
  EXPECT_FALSE(decisions::heater_behind_pairing_node(DeviceType::REMOTE_CONTROLLER, MANUFACTURER_SOMFY));
  EXPECT_FALSE(decisions::heater_behind_pairing_node(DeviceType::REMOTE_CONTROLLER, 0));
  EXPECT_FALSE(decisions::heater_behind_pairing_node(DeviceType::ELECTRICAL_HEATER, MANUFACTURER_ATLANTIC_GROUP));
}

TEST(PairingNextAddress, NextNodeIdCountsUpAsOneTwentyFourBitNumber) {
  // The three radiators of issue #165.
  const uint8_t radiators[][NODE_ID_SIZE] = {{0xB6, 0xBD, 0xFB}, {0x98, 0x61, 0x10}, {0xF6, 0xBE, 0x8D}};
  const uint8_t heaters[][NODE_ID_SIZE] = {{0xB6, 0xBD, 0xFC}, {0x98, 0x61, 0x11}, {0xF6, 0xBE, 0x8E}};
  for (size_t i = 0; i < 3; i++) {
    uint8_t next[NODE_ID_SIZE] = {};
    ASSERT_TRUE(decisions::next_node_id(radiators[i], next));
    EXPECT_EQ(0, memcmp(next, heaters[i], NODE_ID_SIZE)) << i;
  }
  const uint8_t carry[NODE_ID_SIZE] = {0x12, 0x34, 0xFF};
  uint8_t next[NODE_ID_SIZE] = {};
  ASSERT_TRUE(decisions::next_node_id(carry, next));
  EXPECT_EQ(next[0], 0x12);
  EXPECT_EQ(next[1], 0x35);
  EXPECT_EQ(next[2], 0x00);
  const uint8_t last[NODE_ID_SIZE] = {0xFF, 0xFF, 0xFF};
  EXPECT_FALSE(decisions::next_node_id(last, next));
}

// ========================================================================================
// Key pull reply classification
// ========================================================================================

TEST(Decisions, KeyPullReplyWithAFullKeyIsTheKey) {
  const IoFrame key = make_frame(DST_ID, OWN_ID, CMD_KEY_TRANSFER, AES_KEY_SIZE);

  EXPECT_EQ(decisions::classify_key_pull_reply(key, DST_ID, OWN_ID), decisions::PairingKeyPullReplyDisposition::KEY);
}

TEST(Decisions, KeyPullReplyWithAShortKeyIsRejectedNotTaken) {
  const IoFrame short_key = make_frame(DST_ID, OWN_ID, CMD_KEY_TRANSFER, AES_KEY_SIZE - 1);

  EXPECT_EQ(decisions::classify_key_pull_reply(short_key, DST_ID, OWN_ID),
            decisions::PairingKeyPullReplyDisposition::REJECT);
}

TEST(Decisions, KeyPullReplyThatIsAnErrorIsARefusal) {
  const IoFrame refusal = make_frame(DST_ID, OWN_ID, CMD_ERROR_RESP, 1);

  EXPECT_EQ(decisions::classify_key_pull_reply(refusal, DST_ID, OWN_ID),
            decisions::PairingKeyPullReplyDisposition::REFUSE);
}

TEST(Decisions, KeyPullReplyOfAnotherCommandBetweenTheTwoNodesIsRejected) {
  const IoFrame other = make_frame(DST_ID, OWN_ID, CMD_DISCOVER_RESP, 8);

  EXPECT_EQ(decisions::classify_key_pull_reply(other, DST_ID, OWN_ID),
            decisions::PairingKeyPullReplyDisposition::REJECT);
}

TEST(Decisions, KeyPullReplyBetweenOtherNodesIsIgnored) {
  const IoFrame from_foreign = make_frame(FOREIGN_ID, OWN_ID, CMD_KEY_TRANSFER, AES_KEY_SIZE);
  const IoFrame to_foreign = make_frame(DST_ID, FOREIGN_ID, CMD_KEY_TRANSFER, AES_KEY_SIZE);

  EXPECT_EQ(decisions::classify_key_pull_reply(from_foreign, DST_ID, OWN_ID),
            decisions::PairingKeyPullReplyDisposition::IGNORE);
  EXPECT_EQ(decisions::classify_key_pull_reply(to_foreign, DST_ID, OWN_ID),
            decisions::PairingKeyPullReplyDisposition::IGNORE);
}

// ========================================================================================
// Direct key confirm
// ========================================================================================

TEST(Decisions, DirectKeyConfirmIsA0x33FromTheDeviceToUs) {
  EXPECT_TRUE(decisions::is_direct_key_confirm(make_frame(DST_ID, OWN_ID, CMD_KEY_CONFIRM, 0), DST_ID, OWN_ID));
}

TEST(Decisions, DirectKeyConfirmNeedsTheRightCommandAndEndpoints) {
  EXPECT_FALSE(decisions::is_direct_key_confirm(make_frame(DST_ID, OWN_ID, CMD_ERROR_RESP, 1), DST_ID, OWN_ID));
  EXPECT_FALSE(decisions::is_direct_key_confirm(make_frame(FOREIGN_ID, OWN_ID, CMD_KEY_CONFIRM, 0), DST_ID, OWN_ID))
      << "another device's confirm";
  EXPECT_FALSE(decisions::is_direct_key_confirm(make_frame(DST_ID, FOREIGN_ID, CMD_KEY_CONFIRM, 0), DST_ID, OWN_ID))
      << "a confirm for another controller";
}

// ========================================================================================
// Key verify reply classification
// ========================================================================================

TEST(Decisions, KeyVerifyNameAnswerAfterAChallengeMeansTheKeyIsHeld) {
  const IoFrame name = make_frame(DST_ID, OWN_ID, CMD_GET_NAME_RESP, 4);

  EXPECT_EQ(decisions::classify_key_verify_reply(true, true, name), decisions::PairingKeyVerifyDisposition::HELD);
}

TEST(Decisions, KeyVerifyNameAnswerWithoutAChallengeDoesNotProveTheKey) {
  const IoFrame name = make_frame(DST_ID, OWN_ID, CMD_GET_NAME_RESP, 4);

  EXPECT_EQ(decisions::classify_key_verify_reply(true, false, name), decisions::PairingKeyVerifyDisposition::NOT_HELD)
      << "an unauthenticated answer says nothing about the key";
}

TEST(Decisions, KeyVerifyErrorWithACodeIsAnOutrightRefusal) {
  const IoFrame refusal = make_frame(DST_ID, OWN_ID, CMD_ERROR_RESP, 1);

  EXPECT_EQ(decisions::classify_key_verify_reply(true, false, refusal),
            decisions::PairingKeyVerifyDisposition::REFUSED);
  EXPECT_EQ(decisions::classify_key_verify_reply(true, true, refusal), decisions::PairingKeyVerifyDisposition::REFUSED);
}

TEST(Decisions, KeyVerifyErrorWithoutACodeIsNotAVerdict) {
  const IoFrame bare_error = make_frame(DST_ID, OWN_ID, CMD_ERROR_RESP, 0);

  EXPECT_EQ(decisions::classify_key_verify_reply(true, false, bare_error),
            decisions::PairingKeyVerifyDisposition::NOT_HELD);
}

TEST(Decisions, KeyVerifyWithoutAnAnswerIsNotHeldWhateverTheStaleFrameSays) {
  // The reply buffer keeps the last frame of an earlier step; a silent exchange must not read it.
  const IoFrame stale_name = make_frame(DST_ID, OWN_ID, CMD_GET_NAME_RESP, 4);
  const IoFrame stale_error = make_frame(DST_ID, OWN_ID, CMD_ERROR_RESP, 1);

  EXPECT_EQ(decisions::classify_key_verify_reply(false, true, stale_name),
            decisions::PairingKeyVerifyDisposition::NOT_HELD);
  EXPECT_EQ(decisions::classify_key_verify_reply(false, false, stale_error),
            decisions::PairingKeyVerifyDisposition::NOT_HELD);
}
