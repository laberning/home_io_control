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
         "unbounded skip can walk past what a strict receiver accepts and desync just as badly";
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

// ========================================================================================
// Boot rule: initial_sequence against the persisted value (ADR 0114)
// ========================================================================================

TEST(OneWayBootSequenceRuleTest, InitialSequenceWinsWhenNothingIsPersisted) {
  EXPECT_EQ(resolve_boot_sequence(std::nullopt, 0), 0);
  EXPECT_EQ(resolve_boot_sequence(std::nullopt, 40000), 40000);
}

TEST(OneWayBootSequenceRuleTest, InitialSequenceWinsOnlyWhenAboveByLessThanHalfTheRange) {
  EXPECT_EQ(resolve_boot_sequence(1000, 1001), 1001) << "one above is ahead";
  EXPECT_EQ(resolve_boot_sequence(1000, 1000 + 0x7FFF), 1000 + 0x7FFF) << "just under half the range is still ahead";
  EXPECT_EQ(resolve_boot_sequence(1000, 1000 + 0x8000), 1000) << "exactly half the range is not ahead";
  EXPECT_EQ(resolve_boot_sequence(1000, 1000), 1000) << "equal changes nothing";
  EXPECT_EQ(resolve_boot_sequence(1000, 999), 1000) << "behind never wins";
  EXPECT_EQ(resolve_boot_sequence(1000, 0), 1000);
}

TEST(OneWayBootSequenceRuleTest, TheDefaultInitialSequenceNeverResetsAHighCounter) {
  // On the 16-bit ring 0 is "ahead" of every value above 0x8000; a ring comparison would reset such a counter to the
  // default initial_sequence 0 at every boot and replay numbers the device has seen.
  for (const uint16_t persisted : {0x8000, 0x8001, 40000, 0xFFF0, 0xFFFF})
    EXPECT_EQ(resolve_boot_sequence(persisted, 0), persisted) << "persisted " << persisted;
}

TEST(OneWayBootSequenceRuleTest, TheResultIsNeverBehindThePersistedValueForAnyInitialSequence) {
  // Exhaustive over initial_sequence, for persisted values across the range. The result is always one of the two
  // inputs, and only a value 1..0x7FFF above the persisted one replaces it.
  for (const uint16_t persisted : {0x0000, 0x0001, 0x000A, 0x7FFF, 0x8000, 0xFFF0, 0xFFFF}) {
    for (uint32_t initial = 0; initial <= 0xFFFF; initial++) {
      const auto initial_sequence = static_cast<uint16_t>(initial);
      const bool replaces = initial > persisted && initial - persisted <= 0x7FFF;
      const uint16_t result = resolve_boot_sequence(persisted, initial_sequence);
      ASSERT_EQ(result, replaces ? initial_sequence : persisted) << "persisted " << persisted << " initial " << initial;
      ASSERT_GE(result, persisted) << "the counter never moves back at boot";
    }
  }
}

TEST_F(OneWaySequenceStoreTest, ACounterThatWrappedResumesFromItsPersistedValueForAnyBehindInitialSequence) {
  // Run an identity across 65535 -> 0, reboot, and register it again with each initial_sequence. Only a value above
  // the small post-wrap counter by less than half the range moves it (forward); everything else leaves it alone.
  {
    OneWaySequenceStore store;
    store.add_identity(NODE_A, 0);
    ASSERT_TRUE(store.seed(NODE_A, 0xFFFA));
    for (int i = 0; i < 10; i++)
      take(store, NODE_A);  // 0xFFFA .. 0xFFFF, 0 .. 3: the counter has wrapped.
  }
  esphome::test_preferences::simulate_reboot();

  // The persisted value is the end of the reserved block, 10, which is ahead of everything handed out.
  constexpr uint16_t PERSISTED_AFTER_WRAP = 10;
  for (uint32_t initial = 0; initial <= 0xFFFF; initial += 7) {
    // The reboot must not change flash: every iteration starts from the same persisted block end.
    OneWaySequenceStore rebooted;
    rebooted.add_identity(NODE_A, static_cast<uint16_t>(initial));
    uint16_t resumed = 0;
    ASSERT_TRUE(rebooted.peek(NODE_A, resumed));
    const bool above_by_less_than_half = initial > PERSISTED_AFTER_WRAP && initial - PERSISTED_AFTER_WRAP < 0x8000;
    EXPECT_EQ(resumed, above_by_less_than_half ? initial : PERSISTED_AFTER_WRAP) << "initial " << initial;
    // Independent of the rule's formula: none of the ten numbers sent around the wrap comes back.
    EXPECT_FALSE(resumed >= 0xFFFA || resumed <= 3) << "initial " << initial << " resumed at a sent number";
  }
}

TEST_F(OneWaySequenceStoreTest, AReservationBlockThatStraddlesTheWrapSurvivesAReboot) {
  // The last numbers sent are 0xFFFA and 0xFFFB, but the reservation persisted before them already wrapped to 2.
  // At boot the persisted value is plainly *below* the numbers sent, so no initial_sequence may pull the counter
  // back to them; every listed value must resume at 2, seven steps past the last number sent.
  {
    OneWaySequenceStore store;
    store.add_identity(NODE_A, 0);
    ASSERT_TRUE(store.seed(NODE_A, 0xFFFA));
    ASSERT_EQ(take(store, NODE_A), 0xFFFA);
    ASSERT_EQ(take(store, NODE_A), 0xFFFB);
  }
  esphome::test_preferences::simulate_reboot();

  for (const uint16_t initial : {0x0000, 0x0001, 0x0002, 0xEA60, 0xFFFA, 0xFFFB, 0xFFFC, 0xFFFF}) {
    // peek(), not next(): a reservation here would change flash for the following iterations.
    OneWaySequenceStore rebooted;
    rebooted.add_identity(NODE_A, initial);
    uint16_t resumed = 0;
    ASSERT_TRUE(rebooted.peek(NODE_A, resumed));
    EXPECT_EQ(resumed, 2) << "initial " << initial;
  }
}

TEST_F(OneWaySequenceStoreTest, AConfiguredValueFarBehindAWrappedCounterNeverReplaysEarlierNumbers) {
  // The reported fault in one case: initial_sequence 60000, counter wrapped to a small value.
  {
    OneWaySequenceStore store;
    store.add_identity(NODE_A, 60000);
    ASSERT_TRUE(store.seed(NODE_A, 0xFFFE));
    for (int i = 0; i < 5; i++)
      take(store, NODE_A);  // 0xFFFE, 0xFFFF, 0, 1, 2
  }
  esphome::test_preferences::simulate_reboot();

  OneWaySequenceStore rebooted;
  rebooted.add_identity(NODE_A, 60000);
  const uint16_t value = take(rebooted, NODE_A);
  EXPECT_LT(value, 100) << "the counter carries on past the wrap instead of restarting at 60000";
  EXPECT_GE(value, 3) << "and it never repeats a number already handed out";
}

// ========================================================================================
// Storage key
// ========================================================================================

TEST_F(OneWaySequenceStoreTest, TheStorageKeyOfAKnownAddressIsPinned) {
  // The counter must follow the address it belongs to across builds and releases: a changed key derivation
  // would start every user's counter from initial_sequence again and replay numbers the device has seen.
  // 0x409AE91F is the FNV-1a hash over '1', 'W' and the three address bytes A1 B2 C3 (see preference_key()).
  OneWaySequenceStore store;
  store.add_identity(NODE_A, 0);
  ASSERT_TRUE(store.seed(NODE_A, 0x1234));

  const auto &durable = esphome::test_preferences::committed();
  ASSERT_EQ(durable.size(), 1U);
  EXPECT_EQ(durable.begin()->first, 0x409AE91FU);
}

// ========================================================================================
// A failed flash write refuses the sequence
// ========================================================================================

TEST_F(OneWaySequenceStoreTest, AFailedCommitRefusesToHandOutASequence) {
  OneWaySequenceStore store;
  store.add_identity(NODE_A, 300);
  esphome::test_preferences::faults().fail_sync = true;

  uint16_t value = 0xFFFF;
  EXPECT_FALSE(store.next(NODE_A, value)) << "a reservation that did not reach flash must not be handed out";
  EXPECT_EQ(value, 0xFFFF) << "a refused call must not write to the caller's output";
  EXPECT_FALSE(store.seed(NODE_A, 500)) << "seed() reports a failed write too";
  uint16_t peeked = 0;
  ASSERT_TRUE(store.peek(NODE_A, peeked));
  EXPECT_EQ(peeked, 300) << "a failed seed() must not move the counter";
}

TEST_F(OneWaySequenceStoreTest, AFailedStagedWriteRefusesToHandOutASequence) {
  OneWaySequenceStore store;
  store.add_identity(NODE_A, 300);
  esphome::test_preferences::faults().fail_save = true;

  uint16_t value = 0xFFFF;
  EXPECT_FALSE(store.next(NODE_A, value));
  EXPECT_EQ(value, 0xFFFF);
}

TEST_F(OneWaySequenceStoreTest, TheStoreRecoversOnceFlashWorksAgainWithoutSkippingOrRepeating) {
  OneWaySequenceStore store;
  store.add_identity(NODE_A, 300);
  esphome::test_preferences::faults().fail_sync = true;
  uint16_t value = 0;
  ASSERT_FALSE(store.next(NODE_A, value));

  esphome::test_preferences::faults() = {};
  EXPECT_EQ(take(store, NODE_A), 300) << "nothing was handed out during the failure, so nothing is lost";
  EXPECT_EQ(take(store, NODE_A), 301);
}
