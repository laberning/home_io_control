#include "oneway_transmitter.h"

#include "proto_codecs.h"
#include "proto_commands.h"
#include "proto_crypto.h"

#include "corpus_generated.h"
#include "corpus_test_helpers.h"
#include "test_helpers.h"

#include <esphome/core/preferences.h>

#include <array>
#include <cstring>
#include <vector>

using namespace esphome::home_io_control;

// ============================================================================
// OneWayTransmitter::send_command / send_position
// ============================================================================
// Where the identity model, the sequence store and the frame builders meet. The properties worth
// pinning are the ones no runtime feedback could ever reveal, because 1W has none: that one press
// consumes exactly one sequence, that consecutive presses never reuse one, and that each identity
// signs with its own key.

namespace {

const uint8_t OWN_NET_NODE[NODE_ID_SIZE] = {0x11, 0x22, 0x33};
const uint8_t ADOPTED_NET_NODE[NODE_ID_SIZE] = {0x44, 0x55, 0x66};

/// Captures the frames a burst puts on air. `transmit_result` is what `fn()` reports back to the
/// transmitter — set it false to simulate a radio that never accepts a copy.
class BurstRecorder {
 public:
  OneWayTransmitFn fn() {
    return [this](const IoFrame &frame, uint32_t freq, uint16_t preamble) {
      (void) freq;
      (void) preamble;
      this->frames.push_back(frame);
      return this->transmit_result;
    };
  }

  bool transmit_result{true};

  /// Sequences seen, one entry per transmitted frame.
  [[nodiscard]] std::vector<uint16_t> sequences() const {
    std::vector<uint16_t> out;
    out.reserve(this->frames.size());
    for (const auto &frame : this->frames)
      out.push_back(static_cast<uint16_t>((frame.data[6] << 8) | frame.data[7]));
    return out;
  }

  std::vector<IoFrame> frames;
};

OneWayControllerIdentity make_identity(const std::string &id, const uint8_t node_id[NODE_ID_SIZE], DeviceType type,
                                       uint8_t key_fill, uint16_t initial_sequence = 0) {
  OneWayControllerIdentity identity{};
  identity.id = id;
  memcpy(identity.node_id, node_id, NODE_ID_SIZE);
  memset(identity.system_key, key_fill, AES_KEY_SIZE);
  identity.io_device_type = type;
  identity.initial_sequence = initial_sequence;
  return identity;
}

class OneWaySendCommandTest : public ::testing::Test {
 protected:
  void SetUp() override { esphome::test_preferences::wipe(); }
  void TearDown() override { esphome::test_preferences::wipe(); }
};

}  // namespace

TEST_F(OneWaySendCommandTest, OneCommandConsumesExactlyOneSequence) {
  // The rule the whole burst design rests on: a device treats one sequence as one command, so
  // four copies bearing four sequences would be four commands, three of them rejected as replays.
  BurstRecorder recorder;
  OneWayTransmitter transmitter(recorder.fn());
  transmitter.add_identity(make_identity("awning", OWN_NET_NODE, DeviceType::AWNING, 0x11, 500));
  transmitter.setup();

  ASSERT_TRUE(transmitter.send_command("awning", CoverCommand::STOP));

  const auto sequences = recorder.sequences();
  ASSERT_EQ(sequences.size(), ONEWAY_BURST_REPEATS);
  for (const uint16_t sequence : sequences)
    EXPECT_EQ(sequence, 500) << "every copy of one command must carry the same sequence";
}

TEST_F(OneWaySendCommandTest, ConsecutiveCommandsNeverReuseASequence) {
  BurstRecorder recorder;
  OneWayTransmitter transmitter(recorder.fn());
  transmitter.add_identity(make_identity("awning", OWN_NET_NODE, DeviceType::AWNING, 0x11, 500));
  transmitter.setup();

  ASSERT_TRUE(transmitter.send_command("awning", CoverCommand::STOP));
  ASSERT_TRUE(transmitter.send_position("awning", 100));
  ASSERT_TRUE(transmitter.send_command("awning", CoverCommand::FAVORITE));

  const auto sequences = recorder.sequences();
  ASSERT_EQ(sequences.size(), ONEWAY_BURST_REPEATS * 3);
  EXPECT_EQ(sequences[0], 500);
  EXPECT_EQ(sequences[ONEWAY_BURST_REPEATS], 501) << "the counter advances once per command, not once per frame";
  EXPECT_EQ(sequences[ONEWAY_BURST_REPEATS * 2], 502)
      << "and it keeps advancing across command types, matching what a device tracks";
}

TEST_F(OneWaySendCommandTest, InterleavedIdentitiesKeepSeparateSequences) {
  BurstRecorder recorder;
  OneWayTransmitter transmitter(recorder.fn());
  transmitter.add_identity(make_identity("own", OWN_NET_NODE, DeviceType::AWNING, 0x11, 100));
  transmitter.add_identity(make_identity("adopted", ADOPTED_NET_NODE, DeviceType::ROLLER_SHUTTER, 0x22, 900));
  transmitter.setup();

  ASSERT_TRUE(transmitter.send_command("own", CoverCommand::STOP));
  ASSERT_TRUE(transmitter.send_command("adopted", CoverCommand::STOP));
  ASSERT_TRUE(transmitter.send_command("own", CoverCommand::STOP));

  const auto sequences = recorder.sequences();
  EXPECT_EQ(sequences[0], 100);
  EXPECT_EQ(sequences[ONEWAY_BURST_REPEATS], 900) << "the second identity has its own counter, not the first's";
  EXPECT_EQ(sequences[ONEWAY_BURST_REPEATS * 2], 101)
      << "and the first identity resumes where it left off, undisturbed";
}

TEST_F(OneWaySendCommandTest, EachIdentitySignsWithItsOwnKey) {
  // The reason per-identity keys exist: an adopted foreign network's key has to coexist with the
  // hub's own. If the transmitter used one key for both, adoption would achieve nothing.
  BurstRecorder recorder;
  OneWayTransmitter transmitter(recorder.fn());
  transmitter.add_identity(make_identity("own", OWN_NET_NODE, DeviceType::AWNING, 0x11, 7));
  transmitter.add_identity(make_identity("adopted", ADOPTED_NET_NODE, DeviceType::AWNING, 0x22, 7));
  transmitter.setup();

  ASSERT_TRUE(transmitter.send_command("own", CoverCommand::STOP));
  ASSERT_TRUE(transmitter.send_command("adopted", CoverCommand::STOP));

  const IoFrame &own = recorder.frames[0];
  const IoFrame &adopted = recorder.frames[ONEWAY_BURST_REPEATS];
  ASSERT_EQ(0, memcmp(own.data, adopted.data, 8)) << "same command, same sequence — only the key differs";
  EXPECT_NE(0, memcmp(&own.data[8], &adopted.data[8], HMAC_SIZE)) << "so the signatures must differ";

  // Verified against the key directly, not just against each other: matching two wrong signatures
  // would look identical to this test otherwise.
  const uint8_t span[7] = {CMD_EXECUTE, ORIGINATOR_USER_REMOTE, 0x43, POS_STOP, 0x00, 0x00, 0x00};
  uint8_t expected[HMAC_SIZE] = {0};
  uint8_t own_key[AES_KEY_SIZE];
  memset(own_key, 0x11, AES_KEY_SIZE);
  ASSERT_TRUE(crypto::create_1w_hmac(span, sizeof(span), 7, own_key, expected));
  EXPECT_EQ(0, memcmp(&own.data[8], expected, HMAC_SIZE)) << "the identity's own key must be the one that signed";
}

TEST_F(OneWaySendCommandTest, CommandsAddressTheIdentitysDeviceClass) {
  BurstRecorder recorder;
  OneWayTransmitter transmitter(recorder.fn());
  transmitter.add_identity(make_identity("light", OWN_NET_NODE, DeviceType::LIGHT, 0x11));
  transmitter.setup();

  ASSERT_TRUE(transmitter.send_command("light", CoverCommand::STOP));

  const OneWayFrameInfo info = decode_1w_frame(recorder.frames[0]);
  EXPECT_EQ(info.target_type, DeviceType::LIGHT) << "the identity's class is the destination; there is no node to name";
  EXPECT_EQ(0, memcmp(recorder.frames[0].src, OWN_NET_NODE, NODE_ID_SIZE))
      << "and the identity's address is the source";
}

TEST_F(OneWaySendCommandTest, AnUnknownIdentityTransmitsNothing) {
  BurstRecorder recorder;
  OneWayTransmitter transmitter(recorder.fn());
  transmitter.add_identity(make_identity("awning", OWN_NET_NODE, DeviceType::AWNING, 0x11));
  transmitter.setup();

  EXPECT_FALSE(transmitter.send_command("typo", CoverCommand::STOP));
  EXPECT_FALSE(transmitter.send_position("typo", 50));
  EXPECT_TRUE(recorder.frames.empty()) << "an unresolvable handle must not put anything on air";
}

TEST_F(OneWaySendCommandTest, AnIdentityRegisteredWithoutSetupTransmitsNothing) {
  // setup() is what opens the counters. Without it there is no reserved sequence, and a frame
  // signed with an unreserved one is exactly the replay risk the store exists to prevent.
  BurstRecorder recorder;
  OneWayTransmitter transmitter(recorder.fn());
  transmitter.add_identity(make_identity("awning", OWN_NET_NODE, DeviceType::AWNING, 0x11));

  EXPECT_FALSE(transmitter.send_command("awning", CoverCommand::STOP));
  EXPECT_TRUE(recorder.frames.empty());
}

TEST_F(OneWaySendCommandTest, SequencesSurviveAReboot) {
  BurstRecorder recorder;
  {
    OneWayTransmitter transmitter(recorder.fn());
    transmitter.add_identity(make_identity("awning", OWN_NET_NODE, DeviceType::AWNING, 0x11, 300));
    transmitter.setup();
    ASSERT_TRUE(transmitter.send_command("awning", CoverCommand::STOP));
  }

  esphome::test_preferences::simulate_reboot();

  BurstRecorder after_reboot;
  OneWayTransmitter rebooted(after_reboot.fn());
  rebooted.add_identity(make_identity("awning", OWN_NET_NODE, DeviceType::AWNING, 0x11, 300));
  rebooted.setup();
  ASSERT_TRUE(rebooted.send_command("awning", CoverCommand::STOP));

  EXPECT_GT(after_reboot.sequences()[0], recorder.sequences()[0])
      << "a reboot must not replay a sequence the device has already accepted";
}

// ============================================================================
// OneWayTransmitter::send_enrollment / send_unenrollment
// ============================================================================
// Same seam as send_command()/send_position() above -- one sequence per burst, per-identity
// keys, unknown-identity refusal -- plus properties specific to enrollment: it is two bursts back
// to back (0x39 self-directed remove, then 0x30 add -- the documented 1W pairing handshake, see
// linklayer.md:396 and ADR 0026), with_mac=false on the 0x30 half (see create_1w_add_controller()'s
// @warning, proto_commands.h), and explicit "ENROLL"/"UNENROLL" report labels, since
// decode_1w_frame() cannot read an intent out of a 0x30/0x39.

TEST_F(OneWaySendCommandTest, EnrollmentConsumesTwoSequencesOneEach) {
  BurstRecorder recorder;
  OneWayTransmitter transmitter(recorder.fn());
  transmitter.add_identity(make_identity("awning", OWN_NET_NODE, DeviceType::AWNING, 0x11, 500));
  transmitter.setup();

  ASSERT_TRUE(transmitter.send_enrollment("awning"));

  ASSERT_EQ(recorder.frames.size(), 2 * ONEWAY_BURST_REPEATS) << "0x39 burst then 0x30 burst, back to back";
  for (uint8_t i = 0; i < ONEWAY_BURST_REPEATS; i++) {
    const auto &frame = recorder.frames[i];
    EXPECT_EQ(frame.cmd, CMD_ONEWAY_REMOVE);
    const uint16_t sequence = static_cast<uint16_t>((frame.data[1] << 8) | frame.data[2]);
    EXPECT_EQ(sequence, 500) << "every copy of the 0x39 prelude must carry the same sequence";
  }
  for (uint8_t i = ONEWAY_BURST_REPEATS; i < 2 * ONEWAY_BURST_REPEATS; i++) {
    const auto &frame = recorder.frames[i];
    ASSERT_EQ(frame.data_len, 20) << "0x30 declared payload is enc_key(16)+man(1)+data(1)+seq(2)";
    EXPECT_EQ(frame.cmd, CMD_ONEWAY_ADD_CONTROLLER);
    const uint16_t sequence = static_cast<uint16_t>((frame.data[18] << 8) | frame.data[19]);
    EXPECT_EQ(sequence, 501) << "the 0x30 half consumes the next sequence after the 0x39 prelude";
  }
}

TEST_F(OneWaySendCommandTest, EnrollmentAndUnenrollmentShareTheCommandSequenceCounter) {
  BurstRecorder recorder;
  OneWayTransmitter transmitter(recorder.fn());
  transmitter.add_identity(make_identity("awning", OWN_NET_NODE, DeviceType::AWNING, 0x11, 500));
  transmitter.setup();

  ASSERT_TRUE(transmitter.send_command("awning", CoverCommand::STOP));
  ASSERT_TRUE(transmitter.send_enrollment("awning"));
  ASSERT_TRUE(transmitter.send_unenrollment("awning"));

  // Layout: [0..3] STOP (seq 500), [4..7] enrollment's 0x39 prelude (seq 501),
  // [8..11] enrollment's 0x30 (seq 502), [12..15] the standalone 0x39 (seq 503).
  const uint16_t enroll_prelude_seq = static_cast<uint16_t>((recorder.frames[ONEWAY_BURST_REPEATS].data[1] << 8) |
                                                            recorder.frames[ONEWAY_BURST_REPEATS].data[2]);
  const uint16_t enroll_seq = static_cast<uint16_t>((recorder.frames[ONEWAY_BURST_REPEATS * 2].data[18] << 8) |
                                                    recorder.frames[ONEWAY_BURST_REPEATS * 2].data[19]);
  const uint16_t unenroll_seq = static_cast<uint16_t>((recorder.frames[ONEWAY_BURST_REPEATS * 3].data[1] << 8) |
                                                      recorder.frames[ONEWAY_BURST_REPEATS * 3].data[2]);
  EXPECT_EQ(enroll_prelude_seq, 501) << "one counter per node, shared across every command type it sends";
  EXPECT_EQ(enroll_seq, 502);
  EXPECT_EQ(unenroll_seq, 503);
}

TEST_F(OneWaySendCommandTest, EnrollmentSendsNoMacTrailerOnTheAddHalf) {
  // with_mac=false: every real hardware capture this project holds carries no MAC trailer, unlike
  // the published documentation vector create_1w_add_controller() defaults to. The enroll button
  // is the one caller that transmits to real hardware, so it must not use that default.
  BurstRecorder recorder;
  OneWayTransmitter transmitter(recorder.fn());
  transmitter.add_identity(make_identity("awning", OWN_NET_NODE, DeviceType::AWNING, 0x11, 1));
  transmitter.setup();

  ASSERT_TRUE(transmitter.send_enrollment("awning"));

  const auto &add_frame = recorder.frames[ONEWAY_BURST_REPEATS];
  EXPECT_FALSE(add_frame.has_mac) << "enrollment must use the no-MAC shape, not the builder's own default";
  EXPECT_EQ(add_frame.cmd, CMD_ONEWAY_ADD_CONTROLLER);
}

TEST_F(OneWaySendCommandTest, EnrollmentSendsRemoveThenAddBackToBack) {
  // The documented 1W pairing handshake (linklayer.md:396, "1W Discovery"): 0x39 then 0x30, both
  // from the same identity, one burst each, no gap beyond the bursts' own airtime.
  BurstRecorder recorder;
  OneWayTransmitter transmitter(recorder.fn());
  transmitter.add_identity(make_identity("awning", OWN_NET_NODE, DeviceType::AWNING, 0x11, 1));
  transmitter.setup();

  ASSERT_TRUE(transmitter.send_enrollment("awning"));

  ASSERT_EQ(recorder.frames.size(), 2 * ONEWAY_BURST_REPEATS);
  EXPECT_EQ(recorder.frames[0].cmd, CMD_ONEWAY_REMOVE);
  EXPECT_EQ(recorder.frames[ONEWAY_BURST_REPEATS].cmd, CMD_ONEWAY_ADD_CONTROLLER);
  for (const auto &frame : recorder.frames)
    EXPECT_EQ(0, memcmp(frame.src, OWN_NET_NODE, NODE_ID_SIZE)) << "both halves transmit as the same identity";
}

TEST_F(OneWaySendCommandTest, EnrollmentMatchesTheBuildersDirectly) {
  BurstRecorder recorder;
  OneWayTransmitter transmitter(recorder.fn());
  transmitter.add_identity(make_identity("awning", OWN_NET_NODE, DeviceType::AWNING, 0x11, 7));
  transmitter.setup();

  ASSERT_TRUE(transmitter.send_enrollment("awning"));

  uint8_t key[AES_KEY_SIZE];
  memset(key, 0x11, AES_KEY_SIZE);

  IoFrame expected_remove{};
  ASSERT_TRUE(create_1w_remove_controller(expected_remove, OWN_NET_NODE, DeviceType::AWNING, 7, key));
  EXPECT_EQ(0, memcmp(recorder.frames[0].data, expected_remove.data, expected_remove.data_len))
      << "the 0x39 prelude must match create_1w_remove_controller() at the sequence it actually consumed";

  IoFrame expected_add{};
  ASSERT_TRUE(create_1w_add_controller(expected_add, OWN_NET_NODE, DeviceType::AWNING, 0, 8, key, /*with_mac=*/false));
  EXPECT_EQ(0, memcmp(recorder.frames[ONEWAY_BURST_REPEATS].data, expected_add.data, expected_add.data_len))
      << "the 0x30 half must match create_1w_add_controller() at the sequence it actually consumed";
}

TEST_F(OneWaySendCommandTest, EnrollmentWithMacConfiguredTrueAddsTheTrailer) {
  // enrollment_with_mac: true -- the YAML escape hatch for hardware that needs the MAC-bearing
  // 0x30 shape (the published documentation vector's own shape) instead of the no-MAC default.
  BurstRecorder recorder;
  OneWayTransmitter transmitter(recorder.fn());
  OneWayControllerIdentity identity = make_identity("awning", OWN_NET_NODE, DeviceType::AWNING, 0x11, 3);
  identity.enrollment_with_mac = true;
  transmitter.add_identity(identity);
  transmitter.setup();

  ASSERT_TRUE(transmitter.send_enrollment("awning"));

  const auto &add_frame = recorder.frames[ONEWAY_BURST_REPEATS];
  EXPECT_TRUE(add_frame.has_mac) << "enrollment_with_mac: true must reach create_1w_add_controller()'s with_mac";
  EXPECT_EQ(add_frame.cmd, CMD_ONEWAY_ADD_CONTROLLER);
}

// ============================================================================
// VELUX enrollment gesture (ADR 0032): 0x39 -> class sweep -> STOP + DOWN
// ============================================================================

namespace {
constexpr uint8_t ALL_DEVICES_DST[NODE_ID_SIZE] = {0x00, 0x00, 0x3F};
constexpr uint8_t ROLLER_SHUTTER_DST[NODE_ID_SIZE] = {0x00, 0x00, 0xBF};
constexpr uint8_t AWNING_DST[NODE_ID_SIZE] = {0x00, 0x00, 0xFF};
constexpr uint8_t DUAL_SHUTTER_DST[NODE_ID_SIZE] = {0x00, 0x03, 0x7F};

OneWayControllerIdentity make_velux_identity(uint16_t initial_sequence) {
  OneWayControllerIdentity identity = make_identity("velux", OWN_NET_NODE, DeviceType::SCREEN, 0x22, initial_sequence);
  identity.manufacturer = MANUFACTURER_VELUX;
  return identity;
}

uint16_t execute_seq(const IoFrame &f) { return static_cast<uint16_t>((f.data[6] << 8) | f.data[7]); }
uint16_t add_seq(const IoFrame &f) { return static_cast<uint16_t>((f.data[18] << 8) | f.data[19]); }
uint16_t remove_seq(const IoFrame &f) { return static_cast<uint16_t>((f.data[1] << 8) | f.data[2]); }
}  // namespace

TEST_F(OneWaySendCommandTest, VeluxEnrollmentSweepsThreeClassesThenStopsAndCloses) {
  BurstRecorder recorder;
  OneWayTransmitter transmitter(recorder.fn());
  transmitter.add_identity(make_velux_identity(10));
  transmitter.setup();

  ASSERT_TRUE(transmitter.send_enrollment("velux"));

  // 0x39 + (0x30 x3 sweep) + STOP + DOWN = 6 logical bursts.
  ASSERT_EQ(recorder.frames.size(), 6u * ONEWAY_BURST_REPEATS);
  const auto &f = recorder.frames;

  // 0x39 clear -> all-devices, not the identity's typed SCREEN class.
  EXPECT_EQ(f[0].cmd, CMD_ONEWAY_REMOVE);
  EXPECT_EQ(0, memcmp(f[0].dst, ALL_DEVICES_DST, NODE_ID_SIZE)) << "VELUX broadcasts its 0x39, unlike Somfy";

  // The 0x30 sweep: roller_shutter, awning, dual_shutter -- never SCREEN -- all under one sequence.
  EXPECT_EQ(f[1 * ONEWAY_BURST_REPEATS].cmd, CMD_ONEWAY_ADD_CONTROLLER);
  EXPECT_EQ(0, memcmp(f[1 * ONEWAY_BURST_REPEATS].dst, ROLLER_SHUTTER_DST, NODE_ID_SIZE));
  EXPECT_EQ(0, memcmp(f[2 * ONEWAY_BURST_REPEATS].dst, AWNING_DST, NODE_ID_SIZE));
  EXPECT_EQ(0, memcmp(f[3 * ONEWAY_BURST_REPEATS].dst, DUAL_SHUTTER_DST, NODE_ID_SIZE));
  EXPECT_FALSE(f[1 * ONEWAY_BURST_REPEATS].has_mac) << "no-MAC 0x30 form, matching real VELUX (#74)";

  // STOP then DOWN -> all-devices, VELUX ACEI (0x61), main0 POS_STOP then 0xC8 (fully closed).
  const auto &stop = f[4 * ONEWAY_BURST_REPEATS];
  const auto &down = f[5 * ONEWAY_BURST_REPEATS];
  EXPECT_EQ(stop.cmd, CMD_EXECUTE);
  EXPECT_EQ(0, memcmp(stop.dst, ALL_DEVICES_DST, NODE_ID_SIZE));
  EXPECT_EQ(stop.data[1], 0x61) << "STOP follow-up carries the VELUX ACEI";
  EXPECT_EQ(stop.data[2], POS_STOP);
  EXPECT_EQ(down.cmd, CMD_EXECUTE);
  EXPECT_EQ(0, memcmp(down.dst, ALL_DEVICES_DST, NODE_ID_SIZE));
  EXPECT_EQ(down.data[1], 0x61);
  EXPECT_EQ(down.data[2], 0xC8) << "DOWN = position 100, wire value 0xC8";
}

TEST_F(OneWaySendCommandTest, VeluxEnrollmentConsumesFourSequencesOneEachExceptTheSharedSweep) {
  BurstRecorder recorder;
  OneWayTransmitter transmitter(recorder.fn());
  transmitter.add_identity(make_velux_identity(10));
  transmitter.setup();

  ASSERT_TRUE(transmitter.send_enrollment("velux"));
  const auto &f = recorder.frames;

  EXPECT_EQ(remove_seq(f[0]), 10) << "0x39 gets the seed";
  EXPECT_EQ(add_seq(f[1 * ONEWAY_BURST_REPEATS]), 11) << "the whole 0x30 sweep shares one sequence";
  EXPECT_EQ(add_seq(f[2 * ONEWAY_BURST_REPEATS]), 11);
  EXPECT_EQ(add_seq(f[3 * ONEWAY_BURST_REPEATS]), 11);
  EXPECT_EQ(execute_seq(f[4 * ONEWAY_BURST_REPEATS]), 12) << "STOP is the next sequence after the sweep";
  EXPECT_EQ(execute_seq(f[5 * ONEWAY_BURST_REPEATS]), 13) << "DOWN the one after that";
}

TEST_F(OneWaySendCommandTest, VeluxEnrollmentClassesOverrideNarrowsTheSweep) {
  BurstRecorder recorder;
  OneWayTransmitter transmitter(recorder.fn());
  OneWayControllerIdentity identity = make_velux_identity(1);
  identity.enrollment_classes = {DeviceType::AWNING, DeviceType::UNKNOWN, DeviceType::UNKNOWN};
  transmitter.add_identity(identity);
  transmitter.setup();

  ASSERT_TRUE(transmitter.send_enrollment("velux"));

  // 0x39 + one 0x30 (awning only) + STOP + DOWN = 4 bursts.
  ASSERT_EQ(recorder.frames.size(), 4u * ONEWAY_BURST_REPEATS);
  EXPECT_EQ(recorder.frames[ONEWAY_BURST_REPEATS].cmd, CMD_ONEWAY_ADD_CONTROLLER);
  EXPECT_EQ(0, memcmp(recorder.frames[ONEWAY_BURST_REPEATS].dst, AWNING_DST, NODE_ID_SIZE));
  EXPECT_EQ(recorder.frames[2 * ONEWAY_BURST_REPEATS].cmd, CMD_EXECUTE) << "STOP follows the (single-class) sweep";
}

TEST_F(OneWaySendCommandTest, VeluxEnrollmentReportsLabelEachLeg) {
  BurstRecorder recorder;
  OneWayTransmitter transmitter(recorder.fn());
  transmitter.add_identity(make_velux_identity(1));
  transmitter.setup();

  std::vector<OneWayCommandReport> reports;
  transmitter.set_command_report_callback([&](const OneWayCommandReport &r) { reports.push_back(r); });

  ASSERT_TRUE(transmitter.send_enrollment("velux"));

  ASSERT_EQ(reports.size(), 4u) << "0x39, the sweep (one report), STOP, DOWN";
  EXPECT_EQ(reports[0].intent, "UNENROLL");
  EXPECT_EQ(reports[1].intent, "ENROLL");
  EXPECT_EQ(reports[1].target_type, DeviceType::SCREEN) << "the sweep report names the identity's own class";
  EXPECT_EQ(reports[2].intent, "ENROLL STOP");
  EXPECT_EQ(reports[3].intent, "ENROLL DOWN");
}

TEST_F(OneWaySendCommandTest, SomfyManufacturerStillUsesTheUnchangedTwoFrameGesture) {
  BurstRecorder recorder;
  OneWayTransmitter transmitter(recorder.fn());
  OneWayControllerIdentity identity = make_identity("somfy", OWN_NET_NODE, DeviceType::AWNING, 0x11, 5);
  identity.manufacturer = MANUFACTURER_SOMFY;
  transmitter.add_identity(identity);
  transmitter.setup();

  ASSERT_TRUE(transmitter.send_enrollment("somfy"));

  // Exactly the pre-ADR-0032 shape: 0x39 then 0x30, both to the identity's typed class.
  ASSERT_EQ(recorder.frames.size(), 2u * ONEWAY_BURST_REPEATS);
  EXPECT_EQ(recorder.frames[0].cmd, CMD_ONEWAY_REMOVE);
  EXPECT_EQ(recorder.frames[ONEWAY_BURST_REPEATS].cmd, CMD_ONEWAY_ADD_CONTROLLER);
  const uint8_t awning_typed[NODE_ID_SIZE] = {0x00, 0x00, 0xFF};
  EXPECT_EQ(0, memcmp(recorder.frames[0].dst, awning_typed, NODE_ID_SIZE)) << "Somfy 0x39 stays typed, not broadcast";
  EXPECT_EQ(0, memcmp(recorder.frames[ONEWAY_BURST_REPEATS].dst, awning_typed, NODE_ID_SIZE));
}

TEST_F(OneWaySendCommandTest, VeluxEnrollmentReproducesTheSyntheticGoldenGesture) {
  // Byte-for-byte against tests/corpus/captures/enrollment/synthetic_enrollment_velux_kli_prog_sweep.yaml
  // (key: corpus, src AA BB CC, seqs 1/2/2/2/3/4). Pins ctrl0/ctrl1, every destination, man_id,
  // the no-MAC 0x30 form, the shared sweep sequence, the ACEI, and all four 1W MACs in one test --
  // the strongest check available for a path with no hardware confirmation.
  BurstRecorder recorder;
  OneWayTransmitter transmitter(recorder.fn());
  OneWayControllerIdentity identity{};
  identity.id = "golden";
  const uint8_t src[NODE_ID_SIZE] = {0xAA, 0xBB, 0xCC};
  memcpy(identity.node_id, src, NODE_ID_SIZE);
  memcpy(identity.system_key, test::TEST_SYSTEM_KEY, AES_KEY_SIZE);
  identity.manufacturer = MANUFACTURER_VELUX;
  identity.io_device_type = DeviceType::AWNING;  // irrelevant: sweep uses the profile list, STOP/DOWN broadcast
  identity.initial_sequence = 1;
  transmitter.add_identity(identity);
  transmitter.setup();

  ASSERT_TRUE(transmitter.send_enrollment("golden"));

  const auto *cap = corpus_test::capture_by_id("synthetic_enrollment_velux_kli_prog_sweep");
  ASSERT_NE(cap, nullptr);
  ASSERT_EQ(cap->frame_count, 6u);
  ASSERT_EQ(recorder.frames.size(), 6u * ONEWAY_BURST_REPEATS);
  for (uint8_t i = 0; i < 6; i++) {
    uint8_t wire[FRAME_MAX_WIRE_SIZE] = {0};
    const uint8_t len = serialize(recorder.frames[i * ONEWAY_BURST_REPEATS], wire, sizeof(wire));
    ASSERT_EQ(len, cap->frames[i].len) << "burst " << static_cast<int>(i) << " length";
    EXPECT_EQ(0, memcmp(wire, cap->frames[i].bytes, len))
        << "burst " << static_cast<int>(i) << " does not reproduce the golden capture";
  }
}

TEST_F(OneWaySendCommandTest, VeluxEnrollmentSkipsStopAndDownWhenTheSweepTransmittedNothing) {
  // A failed sweep must NOT be followed by a real DOWN broadcast to every 1W device on the network.
  BurstRecorder recorder;
  recorder.transmit_result = false;  // the radio never accepts a copy
  OneWayTransmitter transmitter(recorder.fn());
  transmitter.add_identity(make_velux_identity(1));
  transmitter.setup();

  EXPECT_FALSE(transmitter.send_enrollment("velux"));

  // 0x39 prelude + the 3-class 0x30 sweep were attempted; STOP and DOWN were not.
  EXPECT_EQ(recorder.frames.size(), 4u * ONEWAY_BURST_REPEATS);
  for (const auto &f : recorder.frames)
    EXPECT_NE(f.cmd, CMD_EXECUTE) << "no STOP/DOWN EXECUTE frame should have been built after a dead sweep";
}

TEST_F(OneWaySendCommandTest, UnenrollmentBuildsARemoveControllerFrame) {
  BurstRecorder recorder;
  OneWayTransmitter transmitter(recorder.fn());
  transmitter.add_identity(make_identity("awning", OWN_NET_NODE, DeviceType::AWNING, 0x11, 9));
  transmitter.setup();

  ASSERT_TRUE(transmitter.send_unenrollment("awning"));

  EXPECT_EQ(recorder.frames[0].cmd, CMD_ONEWAY_REMOVE);
  EXPECT_FALSE(recorder.frames[0].has_mac) << "0x39's MAC lives inside the declared payload";
}

TEST_F(OneWaySendCommandTest, AnUnknownIdentityEnrollsAndUnenrollsNothing) {
  BurstRecorder recorder;
  OneWayTransmitter transmitter(recorder.fn());
  transmitter.add_identity(make_identity("awning", OWN_NET_NODE, DeviceType::AWNING, 0x11));
  transmitter.setup();

  std::vector<OneWayCommandReport> reports;
  transmitter.set_command_report_callback([&](const OneWayCommandReport &r) { reports.push_back(r); });

  EXPECT_FALSE(transmitter.send_enrollment("typo"));
  EXPECT_FALSE(transmitter.send_unenrollment("typo"));
  EXPECT_TRUE(recorder.frames.empty());
  // The dispatcher resolves the identity once up front, so an unknown handle produces exactly one
  // failure report per call -- not one per would-be leg of the gesture.
  EXPECT_EQ(reports.size(), 2u);
}

TEST_F(OneWaySendCommandTest, ReportsCarryExplicitEnrollUnenrollLabels) {
  // decode_1w_frame() has no intent to read out of a 0x30/0x39 (it only understands
  // CMD_EXECUTE/CMD_ACTIVATE_MODE), so without an explicit label the "Last 1W Command" sensor
  // would show a blank intent for the one feature whose whole diagnostic story is that sensor.
  BurstRecorder recorder;
  OneWayTransmitter transmitter(recorder.fn());
  transmitter.add_identity(make_identity("awning", OWN_NET_NODE, DeviceType::AWNING, 0x11, 1));
  transmitter.setup();

  std::vector<OneWayCommandReport> reports;
  transmitter.set_command_report_callback([&](const OneWayCommandReport &report) { reports.push_back(report); });

  ASSERT_TRUE(transmitter.send_enrollment("awning"));
  ASSERT_TRUE(transmitter.send_unenrollment("awning"));

  // send_enrollment() itself reports twice -- once for its 0x39 prelude, once for the 0x30 that
  // follows -- then the standalone send_unenrollment() call reports a third time.
  ASSERT_EQ(reports.size(), 3u);
  EXPECT_EQ(reports[0].intent, "UNENROLL");
  EXPECT_EQ(reports[0].target_type, DeviceType::AWNING);
  EXPECT_EQ(reports[1].intent, "ENROLL");
  EXPECT_EQ(reports[1].target_type, DeviceType::AWNING);
  EXPECT_EQ(reports[2].intent, "UNENROLL");
}

TEST_F(OneWaySendCommandTest, PositionsAreEncodedAsTheBuilderWould) {
  BurstRecorder recorder;
  OneWayTransmitter transmitter(recorder.fn());
  transmitter.add_identity(make_identity("awning", OWN_NET_NODE, DeviceType::AWNING, 0x11, 1));
  transmitter.setup();

  ASSERT_TRUE(transmitter.send_position("awning", 25));

  IoFrame expected{};
  uint8_t key[AES_KEY_SIZE];
  memset(key, 0x11, AES_KEY_SIZE);
  ASSERT_TRUE(create_1w_execute_position(expected, OWN_NET_NODE, DeviceType::AWNING, 25, 1, key));
  EXPECT_EQ(0, memcmp(recorder.frames[0].data, expected.data, expected.data_len))
      << "send_position() must add nothing to what the builder produces";
}

// ============================================================================
// 1W wire profile (ADR 0031): manufacturer: -> CMD_EXECUTE ACEI; execute_broadcast: all -> dst.
// ============================================================================

TEST_F(OneWaySendCommandTest, VeluxIdentityEmitsLevel3Acei) {
  BurstRecorder recorder;
  OneWayTransmitter transmitter(recorder.fn());
  OneWayControllerIdentity id = make_identity("velux", OWN_NET_NODE, DeviceType::AWNING, 0x11, 1);
  id.manufacturer = MANUFACTURER_VELUX;
  transmitter.add_identity(id);
  transmitter.setup();

  ASSERT_TRUE(transmitter.send_command("velux", CoverCommand::STOP));
  EXPECT_EQ(recorder.frames[0].data[1], 0x61) << "a VELUX identity's EXECUTE carries ACEI 0x61";
}

TEST_F(OneWaySendCommandTest, IdentityWithoutManufacturerStillEmitsTheSomfyAcei) {
  // The back-compat promise: an identity that never set manufacturer: is byte-for-byte what it
  // was before ADR 0031.
  BurstRecorder recorder;
  OneWayTransmitter transmitter(recorder.fn());
  transmitter.add_identity(make_identity("plain", OWN_NET_NODE, DeviceType::AWNING, 0x11, 1));
  transmitter.setup();

  ASSERT_TRUE(transmitter.send_command("plain", CoverCommand::STOP));
  EXPECT_EQ(recorder.frames[0].data[1], 0x43);
}

TEST_F(OneWaySendCommandTest, ExecuteAceiOverrideBeatsTheProfile) {
  BurstRecorder recorder;
  OneWayTransmitter transmitter(recorder.fn());
  OneWayControllerIdentity id = make_identity("odd", OWN_NET_NODE, DeviceType::AWNING, 0x11, 1);
  id.manufacturer = MANUFACTURER_VELUX;
  id.execute_acei = 0x55;
  transmitter.add_identity(id);
  transmitter.setup();

  ASSERT_TRUE(transmitter.send_command("odd", CoverCommand::STOP));
  EXPECT_EQ(recorder.frames[0].data[1], 0x55) << "execute_acei: overrides the manufacturer profile";
}

TEST_F(OneWaySendCommandTest, ExecuteBroadcastAllTargetsAllDevicesRegardlessOfClass) {
  BurstRecorder recorder;
  OneWayTransmitter transmitter(recorder.fn());
  OneWayControllerIdentity id = make_identity("screen", OWN_NET_NODE, DeviceType::SCREEN, 0x11, 1);
  id.manufacturer = MANUFACTURER_VELUX;
  id.execute_broadcast_all = true;
  transmitter.add_identity(id);
  transmitter.setup();

  ASSERT_TRUE(transmitter.send_command("screen", CoverCommand::STOP));

  const uint8_t all_devices[NODE_ID_SIZE] = {0x00, 0x00, 0x3F};
  EXPECT_EQ(0, memcmp(recorder.frames[0].dst, all_devices, NODE_ID_SIZE))
      << "execute_broadcast: all -> 00 00 3F even though io_device_type is SCREEN (0x0B)";

  // The frame is still well-formed and self-consistent (MAC over the same assembled payload).
  IoFrame expected{};
  uint8_t key[AES_KEY_SIZE];
  memset(key, 0x11, AES_KEY_SIZE);
  ASSERT_TRUE(create_1w_execute_command(expected, OWN_NET_NODE, DeviceType::SCREEN, CoverCommand::STOP, 1, key, 0x61,
                                        /*broadcast_all=*/true));
  EXPECT_EQ(0, memcmp(recorder.frames[0].data, expected.data, expected.data_len));
}

TEST_F(OneWaySendCommandTest, ExecuteBroadcastAllDoesNotDegradeTheReportToUnknown) {
  // With `execute_broadcast: all` the wire dst decodes to DeviceType::UNKNOWN. The report must
  // still name the identity's class, or the "Last 1W Command" sensor and the TX log read
  // "STOP -> unknown".
  BurstRecorder recorder;
  OneWayTransmitter transmitter(recorder.fn());
  OneWayControllerIdentity id = make_identity("screen", OWN_NET_NODE, DeviceType::SCREEN, 0x11, 1);
  id.execute_broadcast_all = true;
  transmitter.add_identity(id);
  transmitter.setup();

  std::vector<OneWayCommandReport> reports;
  transmitter.set_command_report_callback([&](const OneWayCommandReport &report) { reports.push_back(report); });

  ASSERT_TRUE(transmitter.send_command("screen", CoverCommand::STOP));
  ASSERT_EQ(reports.size(), 1u);
  EXPECT_EQ(reports[0].target_type, DeviceType::SCREEN) << "the identity's class, not the frame's decoded UNKNOWN";
  EXPECT_EQ(reports[0].intent, "STOP") << "intent still decodes from the built frame";
}
