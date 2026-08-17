#include "proto_codecs.h"
#include "proto_commands.h"
#include "proto_crypto.h"
#include "proto_frame.h"

#include "corpus_generated.h"
#include "corpus_test_helpers.h"
#include "log_frame.h"
#include "redaction.h"
#include "test_helpers.h"

#include <cstring>
#include <string>

using namespace esphome::home_io_control;

// ============================================================================
// 1W execute builder tests
// ============================================================================
// A 1W execute frame is fire-and-forget: no challenge, no reply, a device *class* for a
// destination, and a rolling sequence in place of the 2W challenge. Every byte a builder emits
// is therefore unverifiable at runtime — nothing answers to say it was wrong — so these tests
// pin the output against real frames instead: three captures from this project's own Somfy
// awning remote and one published worked example, plus a fourth own-hardware capture
// (oneway_remote_favorite_sx1276) used the other way around, as counter-evidence that the
// builder's FAVORITE encoding is *not* what that button actually sends on air. Expected bytes are
// read out of the corpus by id (corpus_test::capture_by_id(), corpus_test_helpers.h) rather than
// transcribed, so a corrected capture automatically corrects what these tests check.

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
/// 385762, typed-broadcast to all. Only the node address is transcribed here; everything else
/// (sequence, prefix bytes, MAC, CRC) is read from the corpus by load_captured_execute() below —
/// see its comment for why.
const uint8_t PUBLISHED_STOP_SRC[NODE_ID_SIZE] = {0x38, 0x57, 0x62};

/// tests/corpus/captures/somfy_awning/oneway_remote_{open,close,stop}_sx1276.yaml — three
/// presses of this project's own remote, node 9D6085, captured on an SX1276. Same note as above:
/// only the shared node address is transcribed.
const uint8_t SOMFY_REMOTE_SRC[NODE_ID_SIZE] = {0x9D, 0x60, 0x85};

/// tests/corpus/captures/reference_1w_vectors/oneway_add_controller_kat.yaml — the published
/// CMD 0x30 known-answer example (Velocet/iown-homecontrol docs/linklayer.md). Only the inputs a
/// caller of create_1w_add_controller() must supply are transcribed here; the frame it must
/// reproduce is read from the corpus by id, same discipline as the execute vectors above.
const uint8_t ADD_CONTROLLER_VECTOR_SRC[NODE_ID_SIZE] = {0xAB, 0xCD, 0xEF};
const uint8_t ADD_CONTROLLER_VECTOR_KEY[AES_KEY_SIZE] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
                                                         0x09, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16};
constexpr uint8_t ADD_CONTROLLER_VECTOR_MANUFACTURER = 0x02;
constexpr uint16_t ADD_CONTROLLER_VECTOR_SEQUENCE = 0x1234;

/// Scan `text` for the hex-byte rendering of `key` (space-separated, uppercase — matches
/// bytes_to_hex()'s format), mirroring redaction_test.cpp's own helper of the same name.
bool text_contains_key_hex(const std::string &text, const uint8_t *key, size_t key_len) {
  char hex[FRAME_LOG_HEX_BUFFER_SIZE];
  bytes_to_hex(key, static_cast<uint8_t>(key_len), hex, sizeof(hex));
  std::string needle(hex);
  if (!needle.empty() && needle.back() == ' ')
    needle.pop_back();
  return text.find(needle) != std::string::npos;
}

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

/// Everything a byte-exact reproduction test needs to pin a built frame against, read straight
/// out of one corpus capture's sole frame rather than transcribed by hand. A correction to a
/// capture must change what these tests check, not leave a stale transcription behind — that is
/// the entire point of going through the corpus instead of a local C array (see the file comment
/// at the top of this file). ASSERTs the id resolves and the frame is long enough to hold a 1W
/// execute, so a renamed or truncated capture fails the calling test loudly instead of comparing
/// against garbage or silently checking nothing.
struct CapturedExecute {
  uint8_t prefix[EXECUTE_PREFIX_LEN];  ///< header(9) + origin/acei/main[2]/fp1/fp2/sequence(8).
  uint16_t sequence;                   ///< The last two bytes of `prefix`, decoded.
  uint8_t mac[HMAC_SIZE];              ///< The captured frame's own MAC — a placeholder for the
                                       ///< published vector (see its corpus entry's `key:`).
  bool has_crc;
  uint8_t crc_lo;  ///< Trailing CRC, wire order (LSB first). Only valid when has_crc.
  uint8_t crc_hi;
};

void load_captured_execute(const char *id, CapturedExecute &out) {
  const auto *capture = corpus_test::capture_by_id(id);
  ASSERT_NE(capture, nullptr) << "corpus capture '" << id << "' not found — was it renamed?";
  ASSERT_EQ(capture->frame_count, 1) << id << " must have exactly one frame";
  const auto &cf = capture->frames[0];
  ASSERT_GE(cf.len, static_cast<uint8_t>(EXECUTE_PREFIX_LEN + HMAC_SIZE))
      << id << " frame is shorter than a 1W execute";
  memcpy(out.prefix, cf.bytes, EXECUTE_PREFIX_LEN);
  out.sequence = static_cast<uint16_t>((out.prefix[EXECUTE_PREFIX_LEN - 2] << 8) | out.prefix[EXECUTE_PREFIX_LEN - 1]);
  memcpy(out.mac, &cf.bytes[EXECUTE_PREFIX_LEN], HMAC_SIZE);
  out.has_crc = cf.crc_present;
  if (cf.crc_present) {
    out.crc_lo = cf.bytes[cf.len - 2];
    out.crc_hi = cf.bytes[cf.len - 1];
  }
}

}  // namespace

// ========================================================================================
// Byte-exact reproduction of real frames
// ========================================================================================

TEST(OneWayCommands, StopReproducesThePublishedVectorPrefix) {
  CapturedExecute pub;
  ASSERT_NO_FATAL_FAILURE(load_captured_execute("reference_1w_execute_iv_vector", pub));

  IoFrame frame{};
  ASSERT_TRUE(create_1w_execute_command(frame, PUBLISHED_STOP_SRC, DeviceType::UNKNOWN, CoverCommand::STOP,
                                        pub.sequence, test::TEST_SYSTEM_KEY))
      << "building a 1W STOP should succeed";

  uint8_t wire[FRAME_MAX_WIRE_SIZE] = {0};
  ASSERT_EQ(serialize_execute(frame, wire, sizeof(wire)), EXECUTE_FRAME_LEN);
  EXPECT_EQ(0, memcmp(wire, pub.prefix, EXECUTE_PREFIX_LEN))
      << "every byte through the sequence must match the published frame";
}

TEST(OneWayCommands, StopMacMatchesTheIndependentlyComputedSignature) {
  // The published frame's own MAC is a placeholder, so the signature is pinned against
  // create_1w_hmac() instead — which is itself pinned by a published KAT (proto_crypto_test).
  CapturedExecute pub;
  ASSERT_NO_FATAL_FAILURE(load_captured_execute("reference_1w_execute_iv_vector", pub));
  IoFrame frame{};
  ASSERT_TRUE(create_1w_execute_command(frame, PUBLISHED_STOP_SRC, DeviceType::UNKNOWN, CoverCommand::STOP,
                                        pub.sequence, test::TEST_SYSTEM_KEY));

  uint8_t expected[HMAC_SIZE] = {0};
  expected_execute_mac(POS_STOP, 0x00, pub.sequence, test::TEST_SYSTEM_KEY, expected);
  EXPECT_EQ(0, memcmp(&frame.data[EXECUTE_MAC_OFFSET], expected, HMAC_SIZE))
      << "the MAC must sign the command byte through fp2, and nothing else";
}

TEST(OneWayCommands, FramingReproducesThePublishedCrc) {
  // The CRC covers all 23 body bytes, so reconstituting the document's frame from our own
  // framing plus its placeholder MAC and getting its published CRC back proves the header,
  // lengths and field order are byte-for-byte what a real receiver checksums.
  CapturedExecute pub;
  ASSERT_NO_FATAL_FAILURE(load_captured_execute("reference_1w_execute_iv_vector", pub));
  ASSERT_TRUE(pub.has_crc) << "the published vector capture must carry a CRC to pin against";

  IoFrame frame{};
  ASSERT_TRUE(create_1w_execute_command(frame, PUBLISHED_STOP_SRC, DeviceType::UNKNOWN, CoverCommand::STOP,
                                        pub.sequence, test::TEST_SYSTEM_KEY));

  uint8_t wire[FRAME_MAX_WIRE_SIZE] = {0};
  ASSERT_EQ(serialize_execute(frame, wire, sizeof(wire)), EXECUTE_FRAME_LEN);
  memcpy(&wire[EXECUTE_PREFIX_LEN], pub.mac, HMAC_SIZE);

  const uint16_t crc = crc_ccitt(wire, EXECUTE_FRAME_LEN);
  EXPECT_EQ(static_cast<uint8_t>(crc & 0xFF), pub.crc_lo) << "CRC low byte must match the published frame";
  EXPECT_EQ(static_cast<uint8_t>(crc >> 8), pub.crc_hi) << "CRC high byte must match the published frame";
}

TEST(OneWayCommands, PositionZeroReproducesTheRemotesOpenPress) {
  // OPEN is not a distinct wire command: it is position 0, which encodes to main 00 00. Pinning
  // it against a real press is what proves the position path and the named-command path agree
  // with one physical remote.
  CapturedExecute open;
  ASSERT_NO_FATAL_FAILURE(load_captured_execute("oneway_remote_open_sx1276", open));
  IoFrame frame{};
  ASSERT_TRUE(
      create_1w_execute_position(frame, SOMFY_REMOTE_SRC, DeviceType::UNKNOWN, 0, open.sequence, test::TEST_SYSTEM_KEY))
      << "position 0 (fully open) should build";

  uint8_t wire[FRAME_MAX_WIRE_SIZE] = {0};
  ASSERT_EQ(serialize_execute(frame, wire, sizeof(wire)), EXECUTE_FRAME_LEN);
  EXPECT_EQ(0, memcmp(wire, open.prefix, EXECUTE_PREFIX_LEN))
      << "an OPEN press from node 9D6085 at sequence 5997 has exactly these bytes";
}

TEST(OneWayCommands, PositionHundredReproducesTheRemotesClosePress) {
  CapturedExecute close_press;
  ASSERT_NO_FATAL_FAILURE(load_captured_execute("oneway_remote_close_sx1276", close_press));
  IoFrame frame{};
  ASSERT_TRUE(create_1w_execute_position(frame, SOMFY_REMOTE_SRC, DeviceType::UNKNOWN, 100, close_press.sequence,
                                         test::TEST_SYSTEM_KEY))
      << "position 100 (fully closed) should build";

  uint8_t wire[FRAME_MAX_WIRE_SIZE] = {0};
  ASSERT_EQ(serialize_execute(frame, wire, sizeof(wire)), EXECUTE_FRAME_LEN);
  EXPECT_EQ(0, memcmp(wire, close_press.prefix, EXECUTE_PREFIX_LEN))
      << "CLOSE encodes as main C8 00, i.e. position 100 doubled";
}

TEST(OneWayCommands, StopReproducesTheRemotesStopPress) {
  CapturedExecute stop_press;
  ASSERT_NO_FATAL_FAILURE(load_captured_execute("oneway_remote_stop_sx1276", stop_press));
  IoFrame frame{};
  ASSERT_TRUE(create_1w_execute_command(frame, SOMFY_REMOTE_SRC, DeviceType::UNKNOWN, CoverCommand::STOP,
                                        stop_press.sequence, test::TEST_SYSTEM_KEY));

  uint8_t wire[FRAME_MAX_WIRE_SIZE] = {0};
  ASSERT_EQ(serialize_execute(frame, wire, sizeof(wire)), EXECUTE_FRAME_LEN);
  EXPECT_EQ(0, memcmp(wire, stop_press.prefix, EXECUTE_PREFIX_LEN))
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
    // 50 is skipped deliberately, not because the builder gets it wrong: main0 == POS_FORCE_OPEN
    // (0x64) is excluded by oneway_intent_to_target()'s special-code list, a decode-side choice
    // out of scope here — see ForceOpenHasNoOneWayEncoding for what the builder actually does
    // with position 50 (an ordinary doubled-position frame).
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

TEST(OneWayCommands, ForceOpenHasNoOneWayEncoding) {
  // 0x64 outbound was hardware-tested as "move to 50%" (see POS_FORCE_OPEN in
  // proto_constants.h), not a lock bypass. A 1W FORCE_OPEN button would therefore have been a
  // 50% button wearing the wrong name, so it was never built: position 50 is an ordinary
  // position command, and create_1w_execute_command() rejects CoverCommand::FORCE_OPEN outright
  // rather than reuse that byte under a misleading label.
  IoFrame position_frame{};
  ASSERT_TRUE(
      create_1w_execute_position(position_frame, SOMFY_REMOTE_SRC, DeviceType::AWNING, 50, 42, test::TEST_SYSTEM_KEY));
  EXPECT_EQ(position_frame.data[2], static_cast<uint8_t>(2 * 50)) << "position 50 is an ordinary doubled-position byte";
  EXPECT_EQ(position_frame.data[3], 0x00);

  IoFrame force_frame{};
  memset(reinterpret_cast<uint8_t *>(&force_frame), 0xAA, sizeof(force_frame));
  const IoFrame untouched = force_frame;
  EXPECT_FALSE(create_1w_execute_command(force_frame, SOMFY_REMOTE_SRC, DeviceType::AWNING, CoverCommand::FORCE_OPEN,
                                         42, test::TEST_SYSTEM_KEY))
      << "1W has no known force-open encoding";
  EXPECT_EQ(0, memcmp(&force_frame, &untouched, sizeof(IoFrame)))
      << "a rejected command must leave the frame untouched";
}

// ========================================================================================
// Counter-evidence — the capture this builder does NOT reproduce
// ========================================================================================

TEST(OneWayCommands, FavoriteButtonCaptureIsWritePrivateNotExecute) {
  // Direct counter-evidence for the FAVORITE/VENT caveat in create_1w_execute_command()'s doxygen
  // (proto_commands.h): this project's own capture of a real My/favorite button press is not what
  // this builder emits for CoverCommand::FAVORITE, or anything close to it. If the builder is
  // ever changed to match this capture, or the caveat above it is deleted, this test is what
  // still says the capture disagrees.
  const auto *capture = corpus_test::capture_by_id("oneway_remote_favorite_sx1276");
  ASSERT_NE(capture, nullptr) << "corpus capture 'oneway_remote_favorite_sx1276' not found — was it renamed?";
  ASSERT_EQ(capture->frame_count, 1);

  const IoFrame frame = corpus_test::parse_capture_frame(capture->frames[0]);
  EXPECT_EQ(frame.cmd, CMD_WRITE_PRIVATE) << "the My/favorite press is CMD_WRITE_PRIVATE (0x20)";
  EXPECT_NE(frame.cmd, CMD_EXECUTE) << "in particular, it is not the CMD_EXECUTE this builder emits for FAVORITE";
  EXPECT_TRUE(is_start(frame) && is_end(frame)) << "still a single self-contained 1W frame";
  EXPECT_EQ(broadcast_target_type(frame.dst), DeviceType::UNKNOWN)
      << "dst 00 00 3F is the all-classes broadcast, not a typed one";

  const OneWayFrameInfo info = decode_1w_frame(frame);
  EXPECT_FALSE(info.has_intent) << "decode_1w_frame() only decodes CMD_EXECUTE/CMD_ACTIVATE_MODE payloads — "
                                   "WRITE_PRIVATE has a different layout and is not read as an intent, let "
                                   "alone as POS_FAVORITE";
}

// ========================================================================================
// Rejection
// ========================================================================================

TEST(OneWayCommands, PositionAboveHundredIsRejectedWithoutTouchingTheFrame) {
  // 1W has no reply, so a half-built frame that a caller ignored the return value for would go
  // on air and fail silently. Refuse before writing anything.
  IoFrame frame{};
  memset(reinterpret_cast<uint8_t *>(&frame), 0xEE, sizeof(frame));
  EXPECT_FALSE(create_1w_execute_position(frame, SOMFY_REMOTE_SRC, DeviceType::AWNING, 101, 1, test::TEST_SYSTEM_KEY))
      << "positions run 0-100";
  EXPECT_EQ(frame.ctrl0, 0xEE) << "a rejected build must leave the caller's frame untouched";
}

// ============================================================================
// 1W enrollment builder tests (CMD 0x30 add-controller / CMD 0x39 remove-controller)
// ============================================================================
// These two builders register (or un-register) a controller identity on a device — the
// precondition for anything the execute builders above produce ever being obeyed at all (see
// ADR 0026). Same discipline as the execute tests: expected bytes come from the
// corpus by id, never transcribed, except for the published vector's own public inputs (node,
// key, manufacturer, sequence), which a caller must supply and so are legitimately local
// constants rather than "expected" bytes.

TEST(OneWayEnrollment, AddControllerReproducesPublishedVector) {
  const auto *capture = corpus_test::capture_by_id("reference_1w_add_controller_kat");
  ASSERT_NE(capture, nullptr) << "corpus capture 'reference_1w_add_controller_kat' not found — was it renamed?";
  ASSERT_EQ(capture->frame_count, 1);
  const IoFrame expected = corpus_test::parse_capture_frame(capture->frames[0]);
  ASSERT_TRUE(expected.has_mac) << "the published 0x30 frame must parse with its out-of-length MAC trailer";

  IoFrame frame{};
  ASSERT_TRUE(create_1w_add_controller(frame, ADD_CONTROLLER_VECTOR_SRC, DeviceType::UNKNOWN,
                                       ADD_CONTROLLER_VECTOR_MANUFACTURER, ADD_CONTROLLER_VECTOR_SEQUENCE,
                                       ADD_CONTROLLER_VECTOR_KEY))
      << "building the published 0x30 vector should succeed";

  uint8_t wire[FRAME_MAX_WIRE_SIZE] = {0};
  uint8_t expected_wire[FRAME_MAX_WIRE_SIZE] = {0};
  const uint8_t len = serialize(frame, wire, sizeof(wire));
  const uint8_t expected_len = serialize(expected, expected_wire, sizeof(expected_wire));
  ASSERT_EQ(len, expected_len) << "built and published frames must serialize to the same length";
  EXPECT_EQ(0, memcmp(wire, expected_wire, len))
      << "node ABCDEF, key 0102...1516, seq 0x1234, man 0x02 must reproduce the published frame "
         "byte-for-byte, including the out-of-length trailer and its MAC";
}

TEST(OneWayEnrollment, AddControllerCarriesOutOfLengthMacTrailer) {
  IoFrame frame{};
  ASSERT_TRUE(create_1w_add_controller(frame, ADD_CONTROLLER_VECTOR_SRC, DeviceType::UNKNOWN,
                                       ADD_CONTROLLER_VECTOR_MANUFACTURER, ADD_CONTROLLER_VECTOR_SEQUENCE,
                                       ADD_CONTROLLER_VECTOR_KEY));

  EXPECT_TRUE(frame.has_mac) << "0x30's MAC does not fit inside CTRL0's 5-bit length field alongside its "
                                "20-byte payload, so it must ride as the out-of-length trailer";
  EXPECT_EQ(frame_length(frame), FRAME_MIN_SIZE + 20) << "declared length is 9 header + 20 payload bytes only";

  uint8_t wire[FRAME_MAX_WIRE_SIZE] = {0};
  const uint8_t len = serialize(frame, wire, sizeof(wire));
  EXPECT_EQ(len, FRAME_MIN_SIZE + 20 + HMAC_SIZE) << "serialize() must fold the trailer into the returned length "
                                                     "so a caller's CRC covers it";
}

TEST(OneWayEnrollment, AddControllerCanOmitMacTrailer) {
  // with_mac=false is the shape every real hardware capture this project holds actually uses
  // (see AddControllerReproducesRealHardwareCapture below) — the published-vector shape above is
  // the parameter's default, kept for backward compatibility with that vector's own tests, not
  // evidence either shape is "more correct" than the other.
  IoFrame frame{};
  ASSERT_TRUE(create_1w_add_controller(frame, ADD_CONTROLLER_VECTOR_SRC, DeviceType::UNKNOWN,
                                       ADD_CONTROLLER_VECTOR_MANUFACTURER, ADD_CONTROLLER_VECTOR_SEQUENCE,
                                       ADD_CONTROLLER_VECTOR_KEY, /*with_mac=*/false));

  EXPECT_FALSE(frame.has_mac) << "with_mac=false must leave no out-of-length trailer";
  EXPECT_EQ(frame_length(frame), FRAME_MIN_SIZE + 20) << "declared length is unchanged either way";

  uint8_t wire[FRAME_MAX_WIRE_SIZE] = {0};
  const uint8_t len = serialize(frame, wire, sizeof(wire));
  EXPECT_EQ(len, FRAME_MIN_SIZE + 20) << "no trailer folded in when with_mac is false";
}

TEST(OneWayEnrollment, AddControllerReproducesRealHardwareCapture) {
  // Byte-exact reproduction of a real Somfy Smoove 0x30, not a documentation example: the corpus
  // capture was re-keyed (scripts/corpus/ingest.py --rekey) so its enc_key unwraps under
  // test::TEST_SYSTEM_KEY instead of the remote's real key, but every other byte — including the
  // absence of a MAC trailer — is exactly what that remote transmitted. This is the test that
  // would catch with_mac's default ever silently flipping to false, or vice versa, without
  // someone updating this capture's own note that it carries no trailer.
  const auto *capture = corpus_test::capture_by_id("somfy_smoove_9d6085_oneway_add_and_remove_controller");
  ASSERT_NE(capture, nullptr) << "corpus capture not found — was it renamed?";
  ASSERT_EQ(capture->frame_count, 3) << "expected 2x 0x39 followed by 1x 0x30";
  const IoFrame expected = corpus_test::parse_capture_frame(capture->frames[2]);
  ASSERT_EQ(expected.cmd, CMD_ONEWAY_ADD_CONTROLLER);
  ASSERT_FALSE(expected.has_mac) << "this specific real capture carries no MAC trailer — see its own description";

  const uint8_t src[NODE_ID_SIZE] = {0x9D, 0x60, 0x85};
  IoFrame frame{};
  ASSERT_TRUE(create_1w_add_controller(frame, src, DeviceType::UNKNOWN, /*manufacturer=*/0x02, /*sequence=*/0x5E18,
                                       test::TEST_SYSTEM_KEY, /*with_mac=*/false));

  uint8_t wire[FRAME_MAX_WIRE_SIZE] = {0};
  uint8_t expected_wire[FRAME_MAX_WIRE_SIZE] = {0};
  const uint8_t len = serialize(frame, wire, sizeof(wire));
  const uint8_t expected_len = serialize(expected, expected_wire, sizeof(expected_wire));
  ASSERT_EQ(len, expected_len);
  EXPECT_EQ(0, memcmp(wire, expected_wire, len))
      << "node 9D6085, corpus key, seq 0x5E18, man 0x02, no MAC must reproduce this real capture byte-for-byte";
}

TEST(OneWayEnrollment, AddControllerWithMacReproducesRealHardwareCapture) {
  // Companion to AddControllerReproducesRealHardwareCapture above: that one pins the no-MAC
  // shape against a real Somfy Smoove; this one pins the MAC-trailer shape against a real Somfy
  // Izymo dimmer, so with_mac's default can never silently flip to true without this test
  // catching the mismatch. Both shapes are confirmed accepted by real hardware — see
  // create_1w_add_controller()'s `@warning` (proto_commands.h).
  const auto *capture = corpus_test::capture_by_id("somfy_izymo_dimmer_oneway_add_controller_with_mac");
  ASSERT_NE(capture, nullptr) << "corpus capture not found — was it renamed?";
  ASSERT_EQ(capture->frame_count, 1);
  const IoFrame expected = corpus_test::parse_capture_frame(capture->frames[0]);
  ASSERT_EQ(expected.cmd, CMD_ONEWAY_ADD_CONTROLLER);
  ASSERT_TRUE(expected.has_mac) << "this specific real capture carries the out-of-length MAC trailer — see its "
                                   "own description";

  const uint8_t src[NODE_ID_SIZE] = {0x7F, 0x84, 0xA2};
  IoFrame frame{};
  ASSERT_TRUE(create_1w_add_controller(frame, src, DeviceType::LIGHT, /*manufacturer=*/0x02, /*sequence=*/0x0060,
                                       test::TEST_SYSTEM_KEY, /*with_mac=*/true));

  uint8_t wire[FRAME_MAX_WIRE_SIZE] = {0};
  uint8_t expected_wire[FRAME_MAX_WIRE_SIZE] = {0};
  const uint8_t len = serialize(frame, wire, sizeof(wire));
  const uint8_t expected_len = serialize(expected, expected_wire, sizeof(expected_wire));
  ASSERT_EQ(len, expected_len);
  EXPECT_EQ(0, memcmp(wire, expected_wire, len))
      << "node 7F84A2, corpus key, seq 0x0060, man 0x02, with MAC must reproduce this real capture byte-for-byte";
}

TEST(OneWayEnrollment, RemoveControllerKeepsMacInsideDeclaredLength) {
  IoFrame frame{};
  ASSERT_TRUE(create_1w_remove_controller(frame, SOMFY_REMOTE_SRC, DeviceType::AWNING, 1, test::TEST_SYSTEM_KEY));

  EXPECT_FALSE(frame.has_mac) << "0x39's MAC sits inside the declared payload, unlike 0x30's out-of-length trailer";
  EXPECT_EQ(frame.data_len, 9) << "data(1) + sequence(2) + mac(6)";
  EXPECT_EQ(frame_length(frame), FRAME_MIN_SIZE + 9);

  uint8_t wire[FRAME_MAX_WIRE_SIZE] = {0};
  const uint8_t len = serialize(frame, wire, sizeof(wire));
  EXPECT_EQ(len, frame_length(frame)) << "no trailer to fold in, unlike create_1w_add_controller()";
}

TEST(OneWayEnrollment, RemoveControllerReproducesRealHardwareCapture) {
  // Byte-exact reproduction of a real Somfy Smoove 0x39, the same corpus capture
  // AddControllerReproducesRealHardwareCapture uses. This is the direct proof behind the
  // `cmd + data` MAC span create_1w_remove_controller() uses: if the span were wrong, this frame
  // would not match, even though frame *shape* alone (RemoveControllerKeepsMacInsideDeclaredLength
  // above) can't tell a wrong span from a right one.
  const auto *capture = corpus_test::capture_by_id("somfy_smoove_9d6085_oneway_add_and_remove_controller");
  ASSERT_NE(capture, nullptr) << "corpus capture not found — was it renamed?";
  ASSERT_EQ(capture->frame_count, 3) << "expected 2x 0x39 followed by 1x 0x30";
  const IoFrame expected = corpus_test::parse_capture_frame(capture->frames[0]);
  ASSERT_EQ(expected.cmd, CMD_ONEWAY_REMOVE);

  const uint8_t src[NODE_ID_SIZE] = {0x9D, 0x60, 0x85};
  IoFrame frame{};
  ASSERT_TRUE(create_1w_remove_controller(frame, src, DeviceType::UNKNOWN, /*sequence=*/0x5E0F, test::TEST_SYSTEM_KEY));

  uint8_t wire[FRAME_MAX_WIRE_SIZE] = {0};
  uint8_t expected_wire[FRAME_MAX_WIRE_SIZE] = {0};
  const uint8_t len = serialize(frame, wire, sizeof(wire));
  const uint8_t expected_len = serialize(expected, expected_wire, sizeof(expected_wire));
  ASSERT_EQ(len, expected_len);
  EXPECT_EQ(0, memcmp(wire, expected_wire, len))
      << "node 9D6085, corpus key, seq 0x5E0F must reproduce this real capture byte-for-byte";
}

TEST(OneWayEnrollment, AddControllerRoundTripsThroughAdoptionDecoder) {
  // Closes the loop with decode_1w_add_controller(): our own 0x30 must decode back to the key we
  // put in, with a verifying MAC — which would catch a key-wrap IV derived from the wrong node
  // (e.g. the destination, like the rejected reference divergence this builder's doxygen warns
  // against) just as surely as a hardware test would.
  IoFrame frame{};
  ASSERT_TRUE(create_1w_add_controller(frame, SOMFY_REMOTE_SRC, DeviceType::LIGHT, 0x07, 42, test::TEST_SYSTEM_KEY));

  OneWayAdoptedKey adopted{};
  const auto err = decode_1w_add_controller(frame, adopted);
  ASSERT_EQ(err, OneWayAddControllerDecodeError::NONE) << "the shipped decoder must accept our own frame";
  EXPECT_EQ(0, memcmp(adopted.system_key, test::TEST_SYSTEM_KEY, AES_KEY_SIZE))
      << "the recovered key must be the key we registered with";
  EXPECT_EQ(adopted.manufacturer, 0x07);
  EXPECT_EQ(0, memcmp(adopted.sender_node, SOMFY_REMOTE_SRC, NODE_ID_SIZE));
  EXPECT_EQ(adopted.sequence, 42);
  EXPECT_EQ(adopted.mac_status, OneWayMacStatus::VERIFIED)
      << "a MAC that fails to verify here would mean the key-wrap IV or the MAC span disagrees "
         "with what the decoder expects";
}

TEST(OneWayEnrollment, LowPowerBitIsNeverSet) {
  // Same rule as every other 1W builder in this file (OneWayCommands.LowPowerBitIsNeverSet) —
  // stated here too because it is easy to reintroduce by analogy with the reference
  // implementation's forgePacket(), which does set it.
  IoFrame add{};
  ASSERT_TRUE(create_1w_add_controller(add, ADD_CONTROLLER_VECTOR_SRC, DeviceType::UNKNOWN,
                                       ADD_CONTROLLER_VECTOR_MANUFACTURER, ADD_CONTROLLER_VECTOR_SEQUENCE,
                                       ADD_CONTROLLER_VECTOR_KEY));
  EXPECT_EQ(add.ctrl1, 0x00);

  IoFrame remove{};
  ASSERT_TRUE(create_1w_remove_controller(remove, SOMFY_REMOTE_SRC, DeviceType::AWNING, 1, test::TEST_SYSTEM_KEY));
  EXPECT_EQ(remove.ctrl1, 0x00);
}

TEST(OneWayEnrollment, KeyMaterialIsMaskedInFrameLogs) {
  IoFrame frame{};
  ASSERT_TRUE(create_1w_add_controller(frame, ADD_CONTROLLER_VECTOR_SRC, DeviceType::UNKNOWN,
                                       ADD_CONTROLLER_VECTOR_MANUFACTURER, ADD_CONTROLLER_VECTOR_SEQUENCE,
                                       ADD_CONTROLLER_VECTOR_KEY));
  uint8_t wire[FRAME_MAX_WIRE_SIZE] = {0};
  const uint8_t len = serialize(frame, wire, sizeof(wire));
  ASSERT_GT(len, 0);

  char out[FRAME_LOG_HEX_BUFFER_SIZE];
  render_frame_hex_redacted(wire, len, out, sizeof(out));
  std::string rendered(out);

  EXPECT_NE(rendered.find("bytes masked"), std::string::npos)
      << "command_carries_key_material() already covers CMD_ONEWAY_ADD_CONTROLLER (redaction.h); "
         "this pins that a TX-side frame actually goes through the masking path";
  EXPECT_FALSE(text_contains_key_hex(rendered, wire + FRAME_MIN_SIZE, len - FRAME_MIN_SIZE))
      << "the wrapped key and MAC must not leak into the rendered text";
  EXPECT_FALSE(contains_key_material(reinterpret_cast<const uint8_t *>(rendered.data()), rendered.size(),
                                     ADD_CONTROLLER_VECTOR_KEY))
      << "the plaintext controller key must never appear in rendered log text";
}
