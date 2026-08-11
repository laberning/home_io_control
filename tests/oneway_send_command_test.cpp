#include "oneway_transmitter.h"

#include "proto_codecs.h"
#include "proto_commands.h"
#include "proto_crypto.h"

#include "test_helpers.h"

#include <esphome/core/preferences.h>

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

/// Captures the frames a burst puts on air.
class BurstRecorder {
 public:
  OneWayTransmitFn fn() {
    return [this](const IoFrame &frame, uint32_t freq, uint16_t preamble) {
      (void) freq;
      (void) preamble;
      this->frames.push_back(frame);
      return true;
    };
  }

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
