#include "oneway_sequence_store.h"

#include "test_helpers.h"

#include <set>

using namespace esphome::home_io_control;

// ============================================================================
// OneWaySequenceStore test suite
// ============================================================================
// 1W's replay defence is a counter the *device* remembers. Everything here exists to pin one
// asymmetry: handing out a sequence twice is unrecoverable and invisible, while skipping some is
// free. So the store may always run ahead and may never fall back.
//
// These tests lean on the host preferences stub modelling ESP32's staged writes: save() alone is
// not durable, and simulate_reboot() discards anything that was never sync()ed. Code that forgets
// to sync fails here rather than on a user's roof.

namespace {

const uint8_t NODE_A[NODE_ID_SIZE] = {0xA1, 0xB2, 0xC3};
const uint8_t NODE_B[NODE_ID_SIZE] = {0xD4, 0xE5, 0xF6};
const uint8_t NODE_UNREGISTERED[NODE_ID_SIZE] = {0x00, 0x11, 0x22};

/// Fresh flash for each test — otherwise a counter persisted by one test seeds the next.
class OneWaySequenceStoreTest : public ::testing::Test {
 protected:
  void SetUp() override { esphome::test_preferences::wipe(); }
  void TearDown() override { esphome::test_preferences::wipe(); }
};

uint16_t take(OneWaySequenceStore &store, const uint8_t node_id[NODE_ID_SIZE]) {
  uint16_t value = 0;
  EXPECT_TRUE(store.next(node_id, value)) << "next() should succeed for a registered identity";
  return value;
}

}  // namespace

// ========================================================================================
// Monotonicity
// ========================================================================================

TEST_F(OneWaySequenceStoreTest, HandsOutStrictlyIncreasingValuesAcrossBlockBoundaries) {
  OneWaySequenceStore store;
  store.add_identity(NODE_A, 0);

  // Well past one stride, so the refill path is exercised several times rather than once.
  const uint16_t count = ONEWAY_SEQUENCE_STRIDE * 3 + 1;
  std::set<uint16_t> seen;
  uint16_t previous = 0;
  for (uint16_t i = 0; i < count; i++) {
    const uint16_t value = take(store, NODE_A);
    if (i > 0) {
      EXPECT_EQ(value, static_cast<uint16_t>(previous + 1)) << "sequences must advance by exactly one per command";
    }
    EXPECT_TRUE(seen.insert(value).second) << "a sequence must never be handed out twice";
    previous = value;
  }
}

TEST_F(OneWaySequenceStoreTest, IdentitiesKeepIndependentCounters) {
  OneWaySequenceStore store;
  store.add_identity(NODE_A, 100);
  store.add_identity(NODE_B, 500);

  EXPECT_EQ(take(store, NODE_A), 100);
  EXPECT_EQ(take(store, NODE_B), 500);
  EXPECT_EQ(take(store, NODE_A), 101) << "node A must not be advanced by node B's traffic";
  EXPECT_EQ(take(store, NODE_B), 501);
}

// ========================================================================================
// Reboot behaviour — the load-bearing property
// ========================================================================================

TEST_F(OneWaySequenceStoreTest, ReloadsForwardAfterARebootMidBlock) {
  uint16_t last_handed_out = 0;
  {
    OneWaySequenceStore store;
    store.add_identity(NODE_A, 0);
    // Stop partway into a block, which is where a real power cut lands.
    for (uint16_t i = 0; i < 3; i++)
      last_handed_out = take(store, NODE_A);
  }

  esphome::test_preferences::simulate_reboot();

  OneWaySequenceStore rebooted;
  rebooted.add_identity(NODE_A, 0);
  const uint16_t after = take(rebooted, NODE_A);
  EXPECT_GT(after, last_handed_out) << "a reboot must never re-issue a sequence that was already transmitted";
}

TEST_F(OneWaySequenceStoreTest, ReloadsForwardAfterARebootOnABlockBoundary) {
  uint16_t last_handed_out = 0;
  {
    OneWaySequenceStore store;
    store.add_identity(NODE_A, 0);
    // Consume exactly one whole block: the boundary case where the reservation is fully used.
    for (uint16_t i = 0; i < ONEWAY_SEQUENCE_STRIDE; i++)
      last_handed_out = take(store, NODE_A);
  }

  esphome::test_preferences::simulate_reboot();

  OneWaySequenceStore rebooted;
  rebooted.add_identity(NODE_A, 0);
  EXPECT_EQ(take(rebooted, NODE_A), static_cast<uint16_t>(last_handed_out + 1))
      << "a clean block boundary should cost no skipped sequences at all";
}

TEST_F(OneWaySequenceStoreTest, ARebootSkipsAtMostOneStride) {
  uint16_t last_handed_out = 0;
  {
    OneWaySequenceStore store;
    store.add_identity(NODE_A, 0);
    last_handed_out = take(store, NODE_A);  // Worst case: one value used out of a full block.
  }

  esphome::test_preferences::simulate_reboot();

  OneWaySequenceStore rebooted;
  rebooted.add_identity(NODE_A, 0);
  const uint16_t after = take(rebooted, NODE_A);
  EXPECT_LE(after - last_handed_out, ONEWAY_SEQUENCE_STRIDE)
      << "the forward skip is what a reboot costs, and it must stay bounded by the stride — an "
         "unbounded skip walks outside the device's acceptance window and desyncs just as badly";
}

TEST_F(OneWaySequenceStoreTest, AnUnsyncedWriteWouldNotSurviveAReboot) {
  // Guards the stub itself: if save() were modelled as immediately durable, every reboot test
  // above would pass even for an implementation that never calls sync().
  esphome::ESPPreferenceObject pref = esphome::global_preferences->make_preference<uint16_t>(0xDEADBEEF);
  const uint16_t staged = 4242;
  ASSERT_TRUE(pref.save(&staged));

  esphome::test_preferences::simulate_reboot();

  uint16_t loaded = 0;
  EXPECT_FALSE(pref.load(&loaded)) << "an unsynced write must be lost by a reboot, as it is on ESP32";
}

// ========================================================================================
// initial_sequence and seed()
// ========================================================================================

TEST_F(OneWaySequenceStoreTest, InitialSequenceSeedsAFirstUseCounter) {
  OneWaySequenceStore store;
  store.add_identity(NODE_A, 1234);
  EXPECT_EQ(take(store, NODE_A), 1234) << "with nothing persisted the configured value is where we start";
}

TEST_F(OneWaySequenceStoreTest, ALowerInitialSequenceCannotWalkTheCounterBackwards) {
  {
    OneWaySequenceStore store;
    store.add_identity(NODE_A, 5000);
    for (uint16_t i = 0; i < ONEWAY_SEQUENCE_STRIDE + 2; i++)
      take(store, NODE_A);
  }

  esphome::test_preferences::simulate_reboot();

  // The user edits initial_sequence downward and reflashes — a plausible mistake that would
  // otherwise replay every sequence between the two values.
  OneWaySequenceStore rebooted;
  rebooted.add_identity(NODE_A, 10);
  uint16_t value = 0;
  ASSERT_TRUE(rebooted.next(NODE_A, value));
  EXPECT_GT(value, 5000) << "the persisted counter must win over a lower configured value";
}

TEST_F(OneWaySequenceStoreTest, AHigherInitialSequenceJumpsTheCounterForward) {
  {
    OneWaySequenceStore store;
    store.add_identity(NODE_A, 100);
    take(store, NODE_A);
  }

  esphome::test_preferences::simulate_reboot();

  // The documented day-one remedy for a desynced device: raise initial_sequence and reflash.
  OneWaySequenceStore rebooted;
  rebooted.add_identity(NODE_A, 9000);
  EXPECT_EQ(take(rebooted, NODE_A), 9000) << "forward is always safe, so a higher configured value must take effect";
}

TEST_F(OneWaySequenceStoreTest, SeedMovesTheCounterBackwardsAndPersists) {
  {
    OneWaySequenceStore store;
    store.add_identity(NODE_A, 8000);
    take(store, NODE_A);
    // seed() is the one path allowed to go backwards — the resync remedy.
    ASSERT_TRUE(store.seed(NODE_A, 42));
    EXPECT_EQ(take(store, NODE_A), 42) << "seed() takes effect immediately";
  }

  esphome::test_preferences::simulate_reboot();

  OneWaySequenceStore rebooted;
  rebooted.add_identity(NODE_A, 0);
  uint16_t value = 0;
  ASSERT_TRUE(rebooted.next(NODE_A, value));
  EXPECT_LT(value, 8000) << "a seeded value must survive the reboot, or the resync did not stick";
  EXPECT_GE(value, 42);
}

// ========================================================================================
// Unregistered identities and wraparound
// ========================================================================================

TEST_F(OneWaySequenceStoreTest, UnregisteredIdentitiesAreRefused) {
  OneWaySequenceStore store;
  store.add_identity(NODE_A, 0);

  uint16_t value = 0xFFFF;
  EXPECT_FALSE(store.next(NODE_UNREGISTERED, value)) << "an unknown address must not silently get a counter";
  EXPECT_EQ(value, 0xFFFF) << "a refused call must not write to the caller's output";
  EXPECT_FALSE(store.seed(NODE_UNREGISTERED, 5));
  EXPECT_FALSE(store.peek(NODE_UNREGISTERED, value));
}

TEST_F(OneWaySequenceStoreTest, RegisteringAnAddressTwiceKeepsOneCounter) {
  // Two counters for one address would each believe they own the sequence space and would hand
  // out the same values — the same failure as two identities sharing an address.
  OneWaySequenceStore store;
  store.add_identity(NODE_A, 700);
  store.add_identity(NODE_A, 900);
  EXPECT_EQ(take(store, NODE_A), 700) << "the second registration must be ignored, not stack a second counter";
}

TEST_F(OneWaySequenceStoreTest, WrapsAroundRatherThanStalling) {
  OneWaySequenceStore store;
  store.add_identity(NODE_A, 0xFFFD);

  EXPECT_EQ(take(store, NODE_A), 0xFFFD);
  EXPECT_EQ(take(store, NODE_A), 0xFFFE);
  EXPECT_EQ(take(store, NODE_A), 0xFFFF);
  EXPECT_EQ(take(store, NODE_A), 0x0000) << "the counter is a 16-bit ring; it must wrap, not saturate or refuse";
  EXPECT_EQ(take(store, NODE_A), 0x0001);
}

TEST_F(OneWaySequenceStoreTest, PeekDoesNotConsumeASequence) {
  OneWaySequenceStore store;
  store.add_identity(NODE_A, 77);

  uint16_t peeked = 0;
  ASSERT_TRUE(store.peek(NODE_A, peeked));
  EXPECT_EQ(peeked, 77);
  EXPECT_EQ(take(store, NODE_A), 77) << "peek() is for diagnostics; it must not reserve anything";
}
