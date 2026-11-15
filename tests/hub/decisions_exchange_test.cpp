#include "decisions_exchange.h"

#include "test_helpers.h"

using namespace esphome::home_io_control;
using namespace test;

// ============================================================================
// Exchange reply classification: first and final responses, challenge answers, subscription answers.
// ============================================================================
// Pure helpers in decisions_exchange.h, tested without radio or timing.

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

namespace {

/// The RS100's answer to a subscription of @p event_id, sent from DST_ID to OWN_ID.
IoFrame subscription_answer(uint16_t event_id) {
  IoFrame f{};
  f.cmd = CMD_STATUS_UPDATE;
  memcpy(f.src, DST_ID, NODE_ID_SIZE);
  memcpy(f.dst, OWN_ID, NODE_ID_SIZE);
  const uint8_t payload[] = {0x05, 0x60, static_cast<uint8_t>(event_id >> 8), static_cast<uint8_t>(event_id & 0xFF),
                             0x01, 0x64};
  memcpy(f.data, payload, sizeof(payload));
  f.data_len = sizeof(payload);
  return f;
}

}  // namespace

TEST(Decisions, SubscriptionAnswerIsTheSameEventFromTheSameDevice) {
  IoFrame subscribe{};
  ASSERT_TRUE(create_subscribe_event(subscribe, OWN_ID, DST_ID, EVENT_BATTERY_LEVEL_CHANGE, false));
  const IoFrame answer = subscription_answer(EVENT_BATTERY_LEVEL_CHANGE);
  EXPECT_TRUE(decisions::is_subscription_answer(subscribe, answer));
  EXPECT_TRUE(decisions::is_device_event_report(answer)) << "still an event report everywhere else";

  EXPECT_FALSE(decisions::is_subscription_answer(subscribe, subscription_answer(EVENT_LOCAL_SENSOR)))
      << "another event";
  IoFrame other_device = answer;
  other_device.src[2] ^= 0x01;
  EXPECT_FALSE(decisions::is_subscription_answer(subscribe, other_device)) << "another device";

  IoFrame unsubscribe{};
  ASSERT_TRUE(create_unsubscribe_event(unsubscribe, OWN_ID, DST_ID, EVENT_BATTERY_LEVEL_CHANGE, false));
  EXPECT_FALSE(decisions::is_subscription_answer(unsubscribe, answer)) << "only the subscribe action draws an answer";
  IoFrame poll{};
  create_get_status(poll, OWN_ID, DST_ID, false);
  EXPECT_FALSE(decisions::is_subscription_answer(poll, answer)) << "not a subscription request";
}

// ========================================================================================
// Challenge answer classification
// ========================================================================================

TEST(Decisions, ChallengeAnswerFromTheChallengedDeviceToUsIsAccepted) {
  const IoFrame answer = make_frame(DST_ID, OWN_ID, CMD_CHALLENGE_RESP, HMAC_SIZE);

  EXPECT_EQ(decisions::classify_challenge_answer(answer, DST_ID, OWN_ID),
            decisions::ChallengeAnswerDisposition::ACCEPT);
}

TEST(Decisions, ChallengeAnswerFromAnotherDeviceIsIgnored) {
  const IoFrame answer = make_frame(FOREIGN_ID, OWN_ID, CMD_CHALLENGE_RESP, HMAC_SIZE);

  EXPECT_EQ(decisions::classify_challenge_answer(answer, DST_ID, OWN_ID),
            decisions::ChallengeAnswerDisposition::IGNORE_UNRELATED);
}

TEST(Decisions, ChallengeAnswerAddressedToAnotherControllerIsIgnored) {
  const IoFrame answer = make_frame(DST_ID, FOREIGN_ID, CMD_CHALLENGE_RESP, HMAC_SIZE);

  EXPECT_EQ(decisions::classify_challenge_answer(answer, DST_ID, OWN_ID),
            decisions::ChallengeAnswerDisposition::IGNORE_UNRELATED);
}

TEST(Decisions, ChallengeAnswerThatIsNotA0x3DIsIgnored) {
  const IoFrame status = make_frame(DST_ID, OWN_ID, CMD_STATUS_UPDATE, HMAC_SIZE);

  EXPECT_EQ(decisions::classify_challenge_answer(status, DST_ID, OWN_ID),
            decisions::ChallengeAnswerDisposition::IGNORE_UNRELATED);
}

TEST(Decisions, ChallengeAnswerShorterThanAFullHmacIsIgnored) {
  const IoFrame answer = make_frame(DST_ID, OWN_ID, CMD_CHALLENGE_RESP, HMAC_SIZE - 1);

  EXPECT_EQ(decisions::classify_challenge_answer(answer, DST_ID, OWN_ID),
            decisions::ChallengeAnswerDisposition::IGNORE_UNRELATED);
}

// ========================================================================================
// Broadcast reply filter
// ========================================================================================

namespace {

/// The Atlantic AGILIA heater's roll-call reply (0x2B from node 986111 to the hub 60F823), wire
/// bytes from tests/corpus/captures/pairing/atlantic_agilia_pairing_key_pull_heater_rollcall.yaml.
IoFrame corpus_roll_call_reply() {
  static constexpr uint8_t WIRE[] = {0xD1, 0x00, 0x60, 0xF8, 0x23, 0x98, 0x61, 0x11, 0x2B,
                                     0x0D, 0x01, 0x98, 0x61, 0x11, 0x0C, 0xCC, 0xF4, 0x39};
  IoFrame frame{};
  EXPECT_TRUE(parse(WIRE, sizeof(WIRE), frame)) << "the corpus frame must parse";
  return frame;
}

constexpr uint8_t CORPUS_HUB_ID[NODE_ID_SIZE] = {0x60, 0xF8, 0x23};

}  // namespace

TEST(Decisions, BroadcastReplyIsTheExpectedCommandAddressedToUs) {
  const IoFrame reply = corpus_roll_call_reply();

  EXPECT_TRUE(decisions::is_broadcast_reply_for(reply, CMD_DISCOVER_SPE_RESP, CORPUS_HUB_ID))
      << "a real roll-call reply is collected, whichever device sent it";
}

TEST(Decisions, BroadcastReplyForAnotherCommandIsNotCollected) {
  const IoFrame reply = corpus_roll_call_reply();

  EXPECT_FALSE(decisions::is_broadcast_reply_for(reply, CMD_DISCOVER_RESP, CORPUS_HUB_ID));
}

TEST(Decisions, BroadcastReplyAddressedToAnotherControllerIsNotCollected) {
  const IoFrame reply = corpus_roll_call_reply();

  EXPECT_FALSE(decisions::is_broadcast_reply_for(reply, CMD_DISCOVER_SPE_RESP, OWN_ID))
      << "a device answering a neighbouring controller's roll-call is not ours";
}

TEST(Decisions, BroadcastReplyIgnoresTheSource) {
  const IoFrame from_a = make_frame(DST_ID, OWN_ID, CMD_DISCOVER_SPE_RESP, 0);
  const IoFrame from_b = make_frame(FOREIGN_ID, OWN_ID, CMD_DISCOVER_SPE_RESP, 0);

  EXPECT_TRUE(decisions::is_broadcast_reply_for(from_a, CMD_DISCOVER_SPE_RESP, OWN_ID));
  EXPECT_TRUE(decisions::is_broadcast_reply_for(from_b, CMD_DISCOVER_SPE_RESP, OWN_ID))
      << "any device in range may answer; which ones did is the point of the roll-call";
}

// ============================================================================
// should_record_device_event(): the one rule for the "Last Device Event" sensor
// ============================================================================

TEST(Decisions, ADeviceEventReportFromARegisteredDeviceIsAlwaysRecorded) {
  const IoFrame report = subscription_answer(EVENT_LOCAL_SENSOR);
  ASSERT_TRUE(decisions::is_device_event_report(report));

  EXPECT_TRUE(decisions::should_record_device_event(report, /*source_is_registered=*/true,
                                                    /*reached_unhandled_fallback=*/false, /*probes_enabled=*/false));
}

TEST(Decisions, NothingFromAnUnregisteredSourceIsRecorded) {
  const IoFrame report = subscription_answer(EVENT_LOCAL_SENSOR);

  EXPECT_FALSE(decisions::should_record_device_event(report, false, false, false));
  EXPECT_FALSE(decisions::should_record_device_event(report, false, true, true));
}

TEST(Decisions, AnUnhandledFrameIsRecordedOnlyFromTheFallbackWithProbesOn) {
  const IoFrame other = make_frame(DST_ID, OWN_ID, CMD_PRIVATE_RESP, 6);
  ASSERT_FALSE(decisions::is_device_event_report(other));

  EXPECT_FALSE(decisions::should_record_device_event(other, true, /*fallback=*/false, /*probes=*/true));
  EXPECT_FALSE(decisions::should_record_device_event(other, true, /*fallback=*/true, /*probes=*/false));
  EXPECT_TRUE(decisions::should_record_device_event(other, true, /*fallback=*/true, /*probes=*/true));
}

// ============================================================================
// Idle receive: when the hub asks ESPHome for a continuous loop.
// ============================================================================

TEST(Decisions, ContinuousLoopIsWantedWhileTheRadioIsSetUpAndHealthy) {
  EXPECT_TRUE(decisions::wants_continuous_loop(/*radio_ready=*/true, /*radio_failed=*/false));
}

TEST(Decisions, ContinuousLoopIsNotWantedBeforeOrWithoutARadio) {
  EXPECT_FALSE(decisions::wants_continuous_loop(false, false)) << "setup failed or has not finished";
}

TEST(Decisions, ContinuousLoopIsReleasedWhenTheRadioFails) {
  EXPECT_FALSE(decisions::wants_continuous_loop(true, true));
}
