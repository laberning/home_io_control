#include "pairing_responder.h"
#include "proto_commands.h"
#include "proto_crypto.h"

#include "test_helpers.h"

using namespace esphome::home_io_control;
using namespace esphome::home_io_control::pairing_responder;

// ============================================================================
// PairingResponder test suite
// ============================================================================
// Pure state-machine transitions for the "Accept Foreign Pairing" (key-extraction) responder:
// ARMED_IDLE -> SENT_DISCOVER_RESP -> SENT_CONFIRM_ACK -> SENT_CHALLENGE -> EXTRACTED ->
// SENT_ADDRESS_RESP, including out-of-order frames, retries (missed-frame tolerance), the
// SENT_CONFIRM_ACK bypass some hubs take, and the crypto integration through on_key_transfer().

namespace {

ResponderContext make_armed_ctx() {
  ResponderContext ctx;
  ctx.state = ResponderState::ARMED_IDLE;
  memcpy(ctx.throwaway_id, test::OWN_ID, NODE_ID_SIZE);
  ctx.advertised_type = DeviceType::ROLLER_SHUTTER;
  ctx.advertised_subtype = 0;
  return ctx;
}

}  // namespace

// ========================================================================================
// Happy path
// ========================================================================================

TEST(PairingResponder, FullFlowTransitionsThroughAllStates) {
  ResponderContext ctx = make_armed_ctx();

  EXPECT_TRUE(on_discover_request(ctx)) << "discovery request should be accepted while ARMED_IDLE";
  EXPECT_EQ(ctx.state, ResponderState::SENT_DISCOVER_RESP);

  EXPECT_TRUE(on_discover_confirm(ctx)) << "discovery confirm should be accepted while SENT_DISCOVER_RESP";
  EXPECT_EQ(ctx.state, ResponderState::SENT_CONFIRM_ACK);

  const uint8_t hub_id[NODE_ID_SIZE] = {0xAA, 0xBB, 0xCC};
  EXPECT_TRUE(on_key_init(ctx, test::TEST_CHALLENGE, hub_id)) << "key-init should be accepted while SENT_DISCOVER_RESP";
  EXPECT_EQ(ctx.state, ResponderState::SENT_CHALLENGE);
  EXPECT_EQ(0, memcmp(ctx.challenge, test::TEST_CHALLENGE, HMAC_SIZE)) << "challenge should be stored";
  EXPECT_EQ(0, memcmp(ctx.hub_node_id, hub_id, NODE_ID_SIZE)) << "hub's real node ID should be captured from src";

  // Build a real key-transfer payload the way create_key_transfer() does, so the crypto
  // round-trip is exercised end-to-end through the state machine, not just proto_crypto directly.
  IoFrame key_init_frame{};
  create_key_init(key_init_frame, hub_id, ctx.throwaway_id);
  IoFrame key_transfer{};
  ASSERT_TRUE(create_key_transfer(key_transfer, key_init_frame, ctx.throwaway_id, hub_id, test::TEST_SYSTEM_KEY,
                                  ctx.challenge));

  EXPECT_TRUE(on_key_transfer(ctx, key_transfer.data)) << "key-transfer should be accepted while SENT_CHALLENGE";
  EXPECT_EQ(ctx.state, ResponderState::EXTRACTED);
  EXPECT_EQ(0, memcmp(ctx.recovered_key, test::TEST_SYSTEM_KEY, AES_KEY_SIZE))
      << "recovered_key should equal the original system key";
}

// ========================================================================================
// Retry / missed-frame tolerance (SX1262 slow turnaround)
// ========================================================================================

TEST(PairingResponder, DiscoverRequestRetryResendsWithoutRegenerating) {
  ResponderContext ctx = make_armed_ctx();
  ASSERT_TRUE(on_discover_request(ctx));
  ASSERT_EQ(ctx.state, ResponderState::SENT_DISCOVER_RESP);

  // Hub missed our first 0x29 and retries the 0x28.
  EXPECT_TRUE(on_discover_request(ctx)) << "a discovery retry while SENT_DISCOVER_RESP should still be accepted";
  EXPECT_EQ(ctx.state, ResponderState::SENT_DISCOVER_RESP) << "state should not regress or advance on a retry";
}

TEST(PairingResponder, DiscoverConfirmRetryResendsSameAck) {
  ResponderContext ctx = make_armed_ctx();
  ASSERT_TRUE(on_discover_request(ctx));
  ASSERT_TRUE(on_discover_confirm(ctx));
  ASSERT_EQ(ctx.state, ResponderState::SENT_CONFIRM_ACK);

  // Hub missed our first 0x2D and retries the 0x2C.
  EXPECT_TRUE(on_discover_confirm(ctx)) << "a discovery-confirm retry while SENT_CONFIRM_ACK should still be accepted";
  EXPECT_EQ(ctx.state, ResponderState::SENT_CONFIRM_ACK) << "state should not regress or advance on a retry";
}

/// Some hubs give up waiting for our 0x2D and send CMD_KEY_INIT anyway, so the key exchange must
/// be reachable without the discovery-confirm step ever completing.
TEST(PairingResponder, KeyInitAcceptedWithoutDiscoverConfirm) {
  ResponderContext ctx = make_armed_ctx();
  ASSERT_TRUE(on_discover_request(ctx));
  ASSERT_EQ(ctx.state, ResponderState::SENT_DISCOVER_RESP);

  const uint8_t hub_id[NODE_ID_SIZE] = {0xAA, 0xBB, 0xCC};
  EXPECT_TRUE(on_key_init(ctx, test::TEST_CHALLENGE, hub_id))
      << "key-init straight from SENT_DISCOVER_RESP must still be accepted";
  EXPECT_EQ(ctx.state, ResponderState::SENT_CHALLENGE);
  EXPECT_EQ(0, memcmp(ctx.challenge, test::TEST_CHALLENGE, HMAC_SIZE)) << "challenge should be stored";
  EXPECT_EQ(0, memcmp(ctx.hub_node_id, hub_id, NODE_ID_SIZE)) << "hub's real node ID should be captured from src";
}

TEST(PairingResponder, KeyInitRetryKeepsStoredChallenge) {
  ResponderContext ctx = make_armed_ctx();
  ASSERT_TRUE(on_discover_request(ctx));

  const uint8_t hub_id[NODE_ID_SIZE] = {0xAA, 0xBB, 0xCC};
  ASSERT_TRUE(on_key_init(ctx, test::TEST_CHALLENGE, hub_id));
  ASSERT_EQ(ctx.state, ResponderState::SENT_CHALLENGE);

  // Hub missed our first 0x3C and retries the 0x31; the caller would generate a *new* random
  // challenge here, but on_key_init() must ignore it and keep the one it already sent.
  const uint8_t different_challenge[HMAC_SIZE] = {0x99, 0x98, 0x97, 0x96, 0x95, 0x94};
  const uint8_t different_hub_id[NODE_ID_SIZE] = {0x01, 0x02, 0x03};
  EXPECT_TRUE(on_key_init(ctx, different_challenge, different_hub_id)) << "a key-init retry should still be accepted";
  EXPECT_EQ(ctx.state, ResponderState::SENT_CHALLENGE);
  EXPECT_EQ(0, memcmp(ctx.challenge, test::TEST_CHALLENGE, HMAC_SIZE))
      << "retry must not overwrite the already-sent challenge";
  EXPECT_EQ(0, memcmp(ctx.hub_node_id, hub_id, NODE_ID_SIZE)) << "retry must not overwrite the captured hub node ID";
}

// ========================================================================================
// Out-of-order / unexpected frames — must not crash or get stuck armed
// ========================================================================================

TEST(PairingResponder, KeyInitBeforeDiscoverIsIgnored) {
  ResponderContext ctx = make_armed_ctx();
  const uint8_t hub_id[NODE_ID_SIZE] = {0xAA, 0xBB, 0xCC};
  EXPECT_FALSE(on_key_init(ctx, test::TEST_CHALLENGE, hub_id)) << "key-init while ARMED_IDLE should be ignored";
  EXPECT_EQ(ctx.state, ResponderState::ARMED_IDLE) << "state should be unchanged";
}

TEST(PairingResponder, KeyTransferBeforeKeyInitIsIgnored) {
  ResponderContext ctx = make_armed_ctx();
  ASSERT_TRUE(on_discover_request(ctx));

  uint8_t payload[AES_KEY_SIZE] = {0};
  EXPECT_FALSE(on_key_transfer(ctx, payload)) << "key-transfer while SENT_DISCOVER_RESP should be ignored";
  EXPECT_EQ(ctx.state, ResponderState::SENT_DISCOVER_RESP) << "state should be unchanged";
}

TEST(PairingResponder, DiscoverConfirmBeforeDiscoverIsIgnored) {
  ResponderContext ctx = make_armed_ctx();
  EXPECT_FALSE(on_discover_confirm(ctx)) << "discovery confirm while ARMED_IDLE should be ignored";
  EXPECT_EQ(ctx.state, ResponderState::ARMED_IDLE) << "state should be unchanged";
}

TEST(PairingResponder, DiscoverConfirmAfterKeyInitIsIgnored) {
  ResponderContext ctx = make_armed_ctx();
  ASSERT_TRUE(on_discover_request(ctx));
  const uint8_t hub_id[NODE_ID_SIZE] = {0xAA, 0xBB, 0xCC};
  ASSERT_TRUE(on_key_init(ctx, test::TEST_CHALLENGE, hub_id));
  ASSERT_EQ(ctx.state, ResponderState::SENT_CHALLENGE);

  EXPECT_FALSE(on_discover_confirm(ctx)) << "a late 0x2C must not pull an in-flight key exchange back a phase";
  EXPECT_EQ(ctx.state, ResponderState::SENT_CHALLENGE) << "state should be unchanged";
}

TEST(PairingResponder, DiscoverRequestAfterExtractedIsIgnored) {
  ResponderContext ctx = make_armed_ctx();
  ctx.state = ResponderState::EXTRACTED;
  EXPECT_FALSE(on_discover_request(ctx)) << "a second discovery request after extraction must not re-arm the exchange";
  EXPECT_EQ(ctx.state, ResponderState::EXTRACTED) << "state should be unchanged";
}

TEST(PairingResponder, KeyInitAfterExtractedIsIgnored) {
  ResponderContext ctx = make_armed_ctx();
  ctx.state = ResponderState::EXTRACTED;
  const uint8_t hub_id[NODE_ID_SIZE] = {0xAA, 0xBB, 0xCC};
  EXPECT_FALSE(on_key_init(ctx, test::TEST_CHALLENGE, hub_id)) << "key-init after extraction must be ignored";
  EXPECT_EQ(ctx.state, ResponderState::EXTRACTED);
}

/// SENT_ADDRESS_RESP twin of DiscoverRequestAfterExtractedIsIgnored above: the new state widens
/// every existing on_*() guard's surface, so each must be re-pinned against it too.
TEST(PairingResponder, DiscoverRequestAfterAddressRespIsIgnored) {
  ResponderContext ctx = make_armed_ctx();
  ctx.state = ResponderState::SENT_ADDRESS_RESP;
  EXPECT_FALSE(on_discover_request(ctx)) << "a discovery request after address verification must not re-arm";
  EXPECT_EQ(ctx.state, ResponderState::SENT_ADDRESS_RESP) << "state should be unchanged";
}

/// SENT_ADDRESS_RESP twin of KeyInitAfterExtractedIsIgnored above.
TEST(PairingResponder, KeyInitAfterAddressRespIsIgnored) {
  ResponderContext ctx = make_armed_ctx();
  ctx.state = ResponderState::SENT_ADDRESS_RESP;
  const uint8_t hub_id[NODE_ID_SIZE] = {0xAA, 0xBB, 0xCC};
  EXPECT_FALSE(on_key_init(ctx, test::TEST_CHALLENGE, hub_id)) << "key-init after address verification must be ignored";
  EXPECT_EQ(ctx.state, ResponderState::SENT_ADDRESS_RESP);
}

TEST(PairingResponder, KeyTransferWhileDisarmedIsIgnored) {
  ResponderContext ctx;  // defaults to DISARMED
  uint8_t payload[AES_KEY_SIZE] = {0};
  EXPECT_FALSE(on_key_transfer(ctx, payload)) << "key-transfer while disarmed should be ignored";
  EXPECT_EQ(ctx.state, ResponderState::DISARMED);
}

// ========================================================================================
// Negative crypto case: a bogus key-transfer payload still transitions to EXTRACTED (there is
// no way to distinguish a wrong key from a right one at this layer — see recover_system_key_
// from_transfer()'s doxygen), but must not report the caller's real system key back.
// ========================================================================================

TEST(PairingResponder, KeyTransferWithBogusPayloadStillExtractsSomeKey) {
  ResponderContext ctx = make_armed_ctx();
  ASSERT_TRUE(on_discover_request(ctx));
  const uint8_t hub_id[NODE_ID_SIZE] = {0xAA, 0xBB, 0xCC};
  ASSERT_TRUE(on_key_init(ctx, test::TEST_CHALLENGE, hub_id));

  uint8_t bogus_payload[AES_KEY_SIZE] = {0};
  EXPECT_TRUE(on_key_transfer(ctx, bogus_payload)) << "crypt_key() only fails on an internal AES error";
  EXPECT_EQ(ctx.state, ResponderState::EXTRACTED);
  EXPECT_NE(0, memcmp(ctx.recovered_key, test::TEST_SYSTEM_KEY, AES_KEY_SIZE))
      << "a bogus payload should not happen to decode to the real test system key";
}

// ========================================================================================
// Address verification (0x36 -> 0x37, hub-issued 0x3C -> 0x3D): the Velux KLR200 round some
// hubs run after the key exchange to verify the backbone address they were handed.
// ========================================================================================

TEST(PairingResponder, AddressReqAcceptedAfterExtracted) {
  ResponderContext ctx = make_armed_ctx();
  ctx.state = ResponderState::EXTRACTED;
  EXPECT_TRUE(on_address_req(ctx)) << "an address request after extraction should be accepted";
  EXPECT_EQ(ctx.state, ResponderState::SENT_ADDRESS_RESP);
}

TEST(PairingResponder, AddressReqRetryResendsWithoutRegenerating) {
  ResponderContext ctx = make_armed_ctx();
  ctx.state = ResponderState::EXTRACTED;
  ASSERT_TRUE(on_address_req(ctx));
  ASSERT_EQ(ctx.state, ResponderState::SENT_ADDRESS_RESP);

  // Hub missed our first 0x37 and retries the 0x36.
  EXPECT_TRUE(on_address_req(ctx)) << "an address-request retry while SENT_ADDRESS_RESP should still be accepted";
  EXPECT_EQ(ctx.state, ResponderState::SENT_ADDRESS_RESP) << "state should not regress or advance on a retry";
}

TEST(PairingResponder, AddressReqBeforeExtractedIsIgnored) {
  ResponderContext ctx = make_armed_ctx();
  ctx.state = ResponderState::SENT_CHALLENGE;
  EXPECT_FALSE(on_address_req(ctx)) << "an address request before the key exchange completes must be ignored";
  EXPECT_EQ(ctx.state, ResponderState::SENT_CHALLENGE) << "state should be unchanged";
}

TEST(PairingResponder, AddressChallengeAcceptedAfterAddressReq) {
  ResponderContext ctx = make_armed_ctx();
  ctx.state = ResponderState::SENT_ADDRESS_RESP;
  EXPECT_TRUE(on_address_challenge(ctx)) << "a hub-issued challenge after our 0x37 should be accepted";
  EXPECT_EQ(ctx.state, ResponderState::SENT_ADDRESS_RESP)
      << "unlike every other on_*() here, this must NOT advance state -- it stays put so a retried "
         "0x3C is answered the same way";
}

TEST(PairingResponder, AddressChallengeBeforeAddressReqIsIgnored) {
  ResponderContext ctx = make_armed_ctx();
  ctx.state = ResponderState::EXTRACTED;
  EXPECT_FALSE(on_address_challenge(ctx)) << "a challenge before we've sent our 0x37 must be ignored";
  EXPECT_EQ(ctx.state, ResponderState::EXTRACTED) << "state should be unchanged";
}

TEST(PairingResponder, AddressChallengeAfterDisarmIsIgnored) {
  ResponderContext ctx;  // defaults to DISARMED
  EXPECT_FALSE(on_address_challenge(ctx)) << "a challenge while disarmed must be ignored";
  EXPECT_EQ(ctx.state, ResponderState::DISARMED);
}

// ========================================================================================
// Stage name lookup (for logging)
// ========================================================================================

TEST(PairingResponder, StageNameCoversEveryState) {
  EXPECT_STREQ(responder_stage_name(ResponderState::DISARMED), "disarmed");
  EXPECT_STREQ(responder_stage_name(ResponderState::ARMED_IDLE), "armed_idle");
  EXPECT_STREQ(responder_stage_name(ResponderState::SENT_DISCOVER_RESP), "sent_discover_resp");
  EXPECT_STREQ(responder_stage_name(ResponderState::SENT_CONFIRM_ACK), "sent_confirm_ack");
  EXPECT_STREQ(responder_stage_name(ResponderState::SENT_CHALLENGE), "sent_challenge");
  EXPECT_STREQ(responder_stage_name(ResponderState::EXTRACTED), "extracted");
  EXPECT_STREQ(responder_stage_name(ResponderState::SENT_ADDRESS_RESP), "sent_address_resp");
}
