#include "proto_codecs.h"
#include "proto_commands.h"
#include "proto_crypto.h"
#include "proto_frame.h"

#include "test_helpers.h"

#include <cstring>

using namespace esphome::home_io_control;

// ============================================================================
// 1W execute builder tests
// ============================================================================
// A 1W execute frame is fire-and-forget: no challenge, no reply, a device *class* for a
// destination, and a rolling sequence in place of the 2W challenge. Every byte a builder emits
// is therefore unverifiable at runtime — nothing answers to say it was wrong — so these tests
// pin the output against real frames instead: three captures from this project's own Somfy
// awning remote and one published worked example.

namespace {

/// Header + payload + sequence of a 1W execute, i.e. everything before the MAC. The captures
/// below can only pin this prefix: their keys are unknown (`key: unknown` in the corpus YAML),
/// so their MACs cannot be recomputed. The MAC is pinned separately against create_1w_hmac().
constexpr uint8_t EXECUTE_PREFIX_LEN = 17;
/// 1W execute data payload: origin + acei + main[2] + fp1 + fp2 + sequence[2] + MAC[6].
constexpr uint8_t EXECUTE_DATA_LEN = 14;
/// Full 1W execute body: 9-byte header + the 14 data bytes.
constexpr uint8_t EXECUTE_FRAME_LEN = 23;
/// Offset of the MAC within the data payload (after the parameters and the sequence).
constexpr uint8_t EXECUTE_MAC_OFFSET = 8;

/// tests/corpus/captures/reference_1w_vectors/oneway_execute_iv_vector.yaml — the worked
/// initial-value example from Velocet/iown-homecontrol docs/linklayer.md. STOP from node
/// 385762, sequence 0x0599, typed-broadcast to all.
const uint8_t PUBLISHED_STOP_SRC[NODE_ID_SIZE] = {0x38, 0x57, 0x62};
const uint8_t PUBLISHED_STOP_PREFIX[EXECUTE_PREFIX_LEN] = {0xF6, 0x00, 0x00, 0x00, 0x3F, 0x38, 0x57, 0x62, 0x00,
                                                           0x01, 0x43, 0xD2, 0x00, 0x00, 0x00, 0x05, 0x99};
/// The MAC that frame carries is the source document's filler value, not a signature — the same
/// bytes appear as the challenge in its 2W examples, and no key is published for this frame. It
/// is used here only to reconstitute the exact 23 bytes the document's CRC covers.
const uint8_t PUBLISHED_STOP_PLACEHOLDER_MAC[HMAC_SIZE] = {0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC};
/// Trailing CRC bytes of that frame, in wire order (LSB first).
constexpr uint8_t PUBLISHED_STOP_CRC_LO = 0x5F;
constexpr uint8_t PUBLISHED_STOP_CRC_HI = 0xB0;

/// tests/corpus/captures/somfy_awning/oneway_remote_{open,close,stop}_sx1276.yaml — three
/// presses of this project's own remote, node 9D6085, captured on an SX1276.
const uint8_t SOMFY_REMOTE_SRC[NODE_ID_SIZE] = {0x9D, 0x60, 0x85};
const uint8_t SOMFY_OPEN_PREFIX[EXECUTE_PREFIX_LEN] = {0xF6, 0x00, 0x00, 0x00, 0x3F, 0x9D, 0x60, 0x85, 0x00,
                                                       0x01, 0x43, 0x00, 0x00, 0x00, 0x00, 0x59, 0x97};
const uint8_t SOMFY_CLOSE_PREFIX[EXECUTE_PREFIX_LEN] = {0xF6, 0x00, 0x00, 0x00, 0x3F, 0x9D, 0x60, 0x85, 0x00,
                                                        0x01, 0x43, 0xC8, 0x00, 0x00, 0x00, 0x59, 0x84};
const uint8_t SOMFY_STOP_PREFIX[EXECUTE_PREFIX_LEN] = {0xF6, 0x00, 0x00, 0x00, 0x3F, 0x9D, 0x60, 0x85, 0x00,
                                                       0x01, 0x43, 0xD2, 0x00, 0x00, 0x00, 0x59, 0x94};

/// Serialize a built frame and return its length, failing the test if it does not serialize.
uint8_t serialize_execute(const IoFrame &frame, uint8_t *out, uint8_t out_size) {
  const uint8_t len = serialize(frame, out, out_size);
  EXPECT_EQ(len, EXECUTE_FRAME_LEN) << "a 1W execute serializes to 9 header + 14 data bytes";
  return len;
}

/// Recompute the MAC a 1W execute must carry, independently of the builder: the span is the
/// command byte through fp2 (7 bytes), stopping before the sequence.
void expected_execute_mac(uint8_t main0, uint8_t main1, uint16_t sequence, const uint8_t key[AES_KEY_SIZE],
                          uint8_t out[HMAC_SIZE]) {
  const uint8_t span[7] = {CMD_EXECUTE, ORIGINATOR_USER_REMOTE, 0x43, main0, main1, 0x00, 0x00};
  ASSERT_TRUE(crypto::create_1w_hmac(span, sizeof(span), sequence, key, out));
}

}  // namespace

// ========================================================================================
// Byte-exact reproduction of real frames
// ========================================================================================

TEST(OneWayCommands, StopReproducesThePublishedVectorPrefix) {
  IoFrame frame{};
  ASSERT_TRUE(create_1w_execute_command(frame, PUBLISHED_STOP_SRC, DeviceType::UNKNOWN, CoverCommand::STOP, 0x0599,
                                        test::TEST_SYSTEM_KEY))
      << "building a 1W STOP should succeed";

  uint8_t wire[FRAME_MAX_WIRE_SIZE] = {0};
  ASSERT_EQ(serialize_execute(frame, wire, sizeof(wire)), EXECUTE_FRAME_LEN);
  EXPECT_EQ(0, memcmp(wire, PUBLISHED_STOP_PREFIX, EXECUTE_PREFIX_LEN))
      << "every byte through the sequence must match the published frame";
}

TEST(OneWayCommands, StopMacMatchesTheIndependentlyComputedSignature) {
  // The published frame's own MAC is a placeholder, so the signature is pinned against
  // create_1w_hmac() instead — which is itself pinned by a published KAT (proto_crypto_test).
  IoFrame frame{};
  ASSERT_TRUE(create_1w_execute_command(frame, PUBLISHED_STOP_SRC, DeviceType::UNKNOWN, CoverCommand::STOP, 0x0599,
                                        test::TEST_SYSTEM_KEY));

  uint8_t expected[HMAC_SIZE] = {0};
  expected_execute_mac(POS_STOP, 0x00, 0x0599, test::TEST_SYSTEM_KEY, expected);
  EXPECT_EQ(0, memcmp(&frame.data[EXECUTE_MAC_OFFSET], expected, HMAC_SIZE))
      << "the MAC must sign the command byte through fp2, and nothing else";
}

TEST(OneWayCommands, FramingReproducesThePublishedCrc) {
  // The CRC covers all 23 body bytes, so reconstituting the document's frame from our own
  // framing plus its placeholder MAC and getting its published CRC back proves the header,
  // lengths and field order are byte-for-byte what a real receiver checksums.
  IoFrame frame{};
  ASSERT_TRUE(create_1w_execute_command(frame, PUBLISHED_STOP_SRC, DeviceType::UNKNOWN, CoverCommand::STOP, 0x0599,
                                        test::TEST_SYSTEM_KEY));

  uint8_t wire[FRAME_MAX_WIRE_SIZE] = {0};
  ASSERT_EQ(serialize_execute(frame, wire, sizeof(wire)), EXECUTE_FRAME_LEN);
  memcpy(&wire[EXECUTE_PREFIX_LEN], PUBLISHED_STOP_PLACEHOLDER_MAC, HMAC_SIZE);

  const uint16_t crc = crc_ccitt(wire, EXECUTE_FRAME_LEN);
  EXPECT_EQ(static_cast<uint8_t>(crc & 0xFF), PUBLISHED_STOP_CRC_LO) << "CRC low byte must match the published frame";
  EXPECT_EQ(static_cast<uint8_t>(crc >> 8), PUBLISHED_STOP_CRC_HI) << "CRC high byte must match the published frame";
}

TEST(OneWayCommands, PositionZeroReproducesTheRemotesOpenPress) {
  // OPEN is not a distinct wire command: it is position 0, which encodes to main 00 00. Pinning
  // it against a real press is what proves the position path and the named-command path agree
  // with one physical remote.
  IoFrame frame{};
  ASSERT_TRUE(
      create_1w_execute_position(frame, SOMFY_REMOTE_SRC, DeviceType::UNKNOWN, 0, 0x5997, test::TEST_SYSTEM_KEY))
      << "position 0 (fully open) should build";

  uint8_t wire[FRAME_MAX_WIRE_SIZE] = {0};
  ASSERT_EQ(serialize_execute(frame, wire, sizeof(wire)), EXECUTE_FRAME_LEN);
  EXPECT_EQ(0, memcmp(wire, SOMFY_OPEN_PREFIX, EXECUTE_PREFIX_LEN))
      << "an OPEN press from node 9D6085 at sequence 5997 has exactly these bytes";
}

TEST(OneWayCommands, PositionHundredReproducesTheRemotesClosePress) {
  IoFrame frame{};
  ASSERT_TRUE(
      create_1w_execute_position(frame, SOMFY_REMOTE_SRC, DeviceType::UNKNOWN, 100, 0x5984, test::TEST_SYSTEM_KEY))
      << "position 100 (fully closed) should build";

  uint8_t wire[FRAME_MAX_WIRE_SIZE] = {0};
  ASSERT_EQ(serialize_execute(frame, wire, sizeof(wire)), EXECUTE_FRAME_LEN);
  EXPECT_EQ(0, memcmp(wire, SOMFY_CLOSE_PREFIX, EXECUTE_PREFIX_LEN))
      << "CLOSE encodes as main C8 00, i.e. position 100 doubled";
}

TEST(OneWayCommands, StopReproducesTheRemotesStopPress) {
  IoFrame frame{};
  ASSERT_TRUE(create_1w_execute_command(frame, SOMFY_REMOTE_SRC, DeviceType::UNKNOWN, CoverCommand::STOP, 0x5994,
                                        test::TEST_SYSTEM_KEY));

  uint8_t wire[FRAME_MAX_WIRE_SIZE] = {0};
  ASSERT_EQ(serialize_execute(frame, wire, sizeof(wire)), EXECUTE_FRAME_LEN);
  EXPECT_EQ(0, memcmp(wire, SOMFY_STOP_PREFIX, EXECUTE_PREFIX_LEN))
      << "a STOP press from node 9D6085 at sequence 5994 has exactly these bytes";
}

// ========================================================================================
// Frame shape
// ========================================================================================

TEST(OneWayCommands, FrameCarriesTheOneWayShape) {
  IoFrame frame{};
  ASSERT_TRUE(create_1w_execute_command(frame, SOMFY_REMOTE_SRC, DeviceType::AWNING, CoverCommand::STOP, 1,
                                        test::TEST_SYSTEM_KEY));

  EXPECT_NE(0, frame.ctrl0 & CTRL0_PROTOCOL_1W) << "a 1W frame must declare the 1W protocol bit";
  EXPECT_TRUE(is_start(frame) && is_end(frame)) << "a 1W command is a single self-contained frame";
  EXPECT_EQ(frame.cmd, CMD_EXECUTE) << "execute commands are CMD_EXECUTE (0x00)";
  EXPECT_EQ(frame.data_len, EXECUTE_DATA_LEN) << "payload(6) + sequence(2) + MAC(6)";
  EXPECT_EQ(frame_length(frame), EXECUTE_FRAME_LEN) << "9-byte header plus 14 data bytes";
  EXPECT_FALSE(frame.has_mac)
      << "a 1W execute's MAC lives inside the declared length; the out-of-length trailer is CMD 0x30's";
}

TEST(OneWayCommands, LowPowerBitIsNeverSet) {
  // Real remotes leave CTRL1 at zero: three own-hardware captures, one third-party capture and
  // the published example all carry ctrl1=0x00. (The reference implementation's forgePacket()
  // sets its LPM bit; the captures outvote it.) Setting it would put a byte on air that no
  // observed 1W transmitter sends.
  IoFrame frame{};
  ASSERT_TRUE(create_1w_execute_position(frame, SOMFY_REMOTE_SRC, DeviceType::UNKNOWN, 50, 1, test::TEST_SYSTEM_KEY));
  EXPECT_EQ(frame.ctrl1, 0x00) << "CTRL1 must be zero, matching every captured 1W execute";
}

// ========================================================================================
// Class addressing
// ========================================================================================

TEST(OneWayCommands, DestinationIsTheTypedBroadcastForTheClass) {
  IoFrame frame{};
  ASSERT_TRUE(create_1w_execute_command(frame, SOMFY_REMOTE_SRC, DeviceType::LIGHT, CoverCommand::STOP, 1,
                                        test::TEST_SYSTEM_KEY));

  EXPECT_EQ(frame.dst[0], 0x00);
  EXPECT_EQ(frame.dst[1], 0x01) << "the light class (0x06) spills into byte 1: 00 01 BF";
  EXPECT_EQ(frame.dst[2], 0xBF);
  EXPECT_EQ(broadcast_target_type(frame.dst), DeviceType::LIGHT) << "the destination must decode back to the class";
}

TEST(OneWayCommands, EveryClassRoundTripsThroughTheDestination) {
  // The class spans bits [9:2], so a single-byte encoding is right for classes 0-3 and wrong
  // from 4 upward — a low-class spot check would not catch it.
  for (uint16_t raw = 0; raw <= static_cast<uint16_t>(DeviceType::SWINGING_SHUTTER); raw++) {
    const auto type = static_cast<DeviceType>(raw);
    IoFrame frame{};
    ASSERT_TRUE(create_1w_execute_command(frame, SOMFY_REMOTE_SRC, type, CoverCommand::STOP, 1, test::TEST_SYSTEM_KEY));
    EXPECT_EQ(broadcast_target_type(frame.dst), type)
        << "class 0x" << std::hex << raw << " must survive the round trip";
  }
}

// ========================================================================================
// Sequence handling
// ========================================================================================

TEST(OneWayCommands, SequenceIsBigEndianAfterTheParameters) {
  IoFrame frame{};
  ASSERT_TRUE(create_1w_execute_command(frame, SOMFY_REMOTE_SRC, DeviceType::UNKNOWN, CoverCommand::STOP, 0xABCD,
                                        test::TEST_SYSTEM_KEY));
  EXPECT_EQ(frame.data[6], 0xAB) << "sequence high byte follows fp2";
  EXPECT_EQ(frame.data[7], 0xCD) << "sequence low byte follows the high byte";
}

TEST(OneWayCommands, TheSameCommandAtTwoSequencesSignsDifferently) {
  // The sequence is 1W's entire replay defence, and it reaches the MAC only through the IV —
  // never through the checksummed span. If two sequences produced one signature, a captured
  // frame would replay forever.
  IoFrame first{};
  IoFrame second{};
  ASSERT_TRUE(create_1w_execute_command(first, SOMFY_REMOTE_SRC, DeviceType::UNKNOWN, CoverCommand::STOP, 100,
                                        test::TEST_SYSTEM_KEY));
  ASSERT_TRUE(create_1w_execute_command(second, SOMFY_REMOTE_SRC, DeviceType::UNKNOWN, CoverCommand::STOP, 101,
                                        test::TEST_SYSTEM_KEY));

  EXPECT_EQ(0, memcmp(first.data, second.data, 6)) << "the command parameters are identical";
  EXPECT_NE(0, memcmp(&first.data[EXECUTE_MAC_OFFSET], &second.data[EXECUTE_MAC_OFFSET], HMAC_SIZE))
      << "but the signatures must differ";
}

TEST(OneWayCommands, TwoKeysSignTheSameCommandDifferently) {
  // Per-identity keys are the reason a hub can drive an adopted foreign network alongside its
  // own; if the key did not reach the signature, adoption would be decorative.
  uint8_t other_key[AES_KEY_SIZE];
  memcpy(other_key, test::TEST_SYSTEM_KEY, AES_KEY_SIZE);
  other_key[0] ^= 0xFF;

  IoFrame mine{};
  IoFrame theirs{};
  ASSERT_TRUE(create_1w_execute_command(mine, SOMFY_REMOTE_SRC, DeviceType::UNKNOWN, CoverCommand::STOP, 7,
                                        test::TEST_SYSTEM_KEY));
  ASSERT_TRUE(
      create_1w_execute_command(theirs, SOMFY_REMOTE_SRC, DeviceType::UNKNOWN, CoverCommand::STOP, 7, other_key));
  EXPECT_NE(0, memcmp(&mine.data[EXECUTE_MAC_OFFSET], &theirs.data[EXECUTE_MAC_OFFSET], HMAC_SIZE))
      << "the controller key must reach the signature";
}

// ========================================================================================
// Decode round-trip — the builder and the shipped decoder must agree
// ========================================================================================

TEST(OneWayCommands, NamedCommandsDecodeBackToTheirIntent) {
  struct Case {
    CoverCommand cmd;
    uint8_t main0;
    uint8_t main1;
    const char *intent;
  };
  const Case cases[] = {
      {CoverCommand::STOP, POS_STOP, 0x00, "STOP"},
      {CoverCommand::FAVORITE, POS_FAVORITE, 0x00, "FAVORITE"},
      {CoverCommand::VENT, POS_FAVORITE, POS_VENT_MODIFIER, "VENT"},
      {CoverCommand::FORCE_OPEN, POS_FORCE_OPEN, 0x00, "FORCE_OPEN"},
  };

  for (const auto &c : cases) {
    IoFrame frame{};
    ASSERT_TRUE(
        create_1w_execute_command(frame, SOMFY_REMOTE_SRC, DeviceType::AWNING, c.cmd, 42, test::TEST_SYSTEM_KEY))
        << "building " << c.intent << " should succeed";
    EXPECT_EQ(frame.data[2], c.main0) << c.intent << " main0";
    EXPECT_EQ(frame.data[3], c.main1) << c.intent << " main1";

    const OneWayFrameInfo info = decode_1w_frame(frame);
    EXPECT_TRUE(info.has_intent) << c.intent << " must decode as an intent-bearing frame";
    EXPECT_EQ(info.target_type, DeviceType::AWNING) << c.intent << " target class";
    EXPECT_EQ(info.originator, ORIGINATOR_USER_REMOTE) << c.intent << " originator";
    EXPECT_EQ(info.acei_level, ACEI_LEVEL_USER_HIGH) << c.intent << " ACEI level";
    EXPECT_STREQ(info.intent, c.intent) << "the shipped decoder must read back what the builder wrote";
    EXPECT_EQ(0, memcmp(info.src, SOMFY_REMOTE_SRC, NODE_ID_SIZE)) << c.intent << " source";
  }
}

TEST(OneWayCommands, PositionsDecodeBackToTheSamePercentage) {
  for (uint8_t position = 0; position <= 100; position += 5) {
    // 50 is skipped deliberately, not because the builder gets it wrong — see
    // PositionFiftyCollidesWithForceOpen for the wire-format collision it exposes.
    if (position == 50)
      continue;
    IoFrame frame{};
    ASSERT_TRUE(
        create_1w_execute_position(frame, SOMFY_REMOTE_SRC, DeviceType::AWNING, position, 42, test::TEST_SYSTEM_KEY))
        << "position " << static_cast<int>(position) << " should build";

    const OneWayFrameInfo info = decode_1w_frame(frame);
    ASSERT_TRUE(info.has_intent);
    const auto target = oneway_intent_to_target(info.main0, info.main1);
    ASSERT_TRUE(target.has_value()) << "position " << static_cast<int>(position) << " must decode to a target";
    EXPECT_FLOAT_EQ(*target, static_cast<float>(position))
        << "the wire encoding doubles the position; the decoder must halve it back";
  }
}

TEST(OneWayCommands, PositionFiftyCollidesWithForceOpen) {
  // Not a builder defect — the wire format overloads the byte. 2 * 50 is 0x64, which is also
  // POS_FORCE_OPEN's main code, and the reference implementation emits the same byte for both.
  // Pinned here so the collision is a documented property with a test behind it rather than a
  // surprise someone rediscovers from a device that force-opens when asked for half-closed.
  IoFrame position_frame{};
  IoFrame force_frame{};
  ASSERT_TRUE(
      create_1w_execute_position(position_frame, SOMFY_REMOTE_SRC, DeviceType::AWNING, 50, 42, test::TEST_SYSTEM_KEY));
  ASSERT_TRUE(create_1w_execute_command(force_frame, SOMFY_REMOTE_SRC, DeviceType::AWNING, CoverCommand::FORCE_OPEN, 42,
                                        test::TEST_SYSTEM_KEY));

  EXPECT_EQ(0, memcmp(position_frame.data, force_frame.data, EXECUTE_DATA_LEN))
      << "position 50 and FORCE_OPEN produce identical frames, signature included";
  EXPECT_STREQ(decode_1w_frame(position_frame).intent, "FORCE_OPEN")
      << "the decoder resolves the ambiguity toward FORCE_OPEN, and a device likely does too";
}

// ========================================================================================
// Rejection
// ========================================================================================

TEST(OneWayCommands, PositionAboveHundredIsRejectedWithoutTouchingTheFrame) {
  // 1W has no reply, so a half-built frame that a caller ignored the return value for would go
  // on air and fail silently. Refuse before writing anything.
  IoFrame frame{};
  memset(&frame, 0xEE, sizeof(frame));
  EXPECT_FALSE(create_1w_execute_position(frame, SOMFY_REMOTE_SRC, DeviceType::AWNING, 101, 1, test::TEST_SYSTEM_KEY))
      << "positions run 0-100";
  EXPECT_EQ(frame.ctrl0, 0xEE) << "a rejected build must leave the caller's frame untouched";
}
