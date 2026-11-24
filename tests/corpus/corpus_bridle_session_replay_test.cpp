/// @file corpus_bridle_session_replay_test.cpp
/// @brief Recorded Bridle sessions replayed through the layer that recorded them.
///
/// A fixture (tests/corpus/sessions/) is a conversation between the hub's driver and a co-processor.
/// Here the co-processor is the recording: FakeCoprocessor plays every recorded message at the gap it
/// had after the command it followed, and compares each command the driver sends with the recorded
/// one (type and body, not `seq` or CRC). A replay therefore fails when the driver's command
/// sequence changes, and passes exactly when it is the recorded one.
///
/// The exchange fixtures replay through the exchange engine here; the link and boot fixtures
/// replay through the driver alone (radio_bridle_session_replay_test.cpp). The 868 MHz fixtures replay over the
/// `Io868GfskPhy`, the others over the 2.4 GHz profile.

#include <cstring>
#include <string>

#include <gtest/gtest.h>

#include "bridle_session_playback.h"
#include "bridle_session_runs.h"
#include "exchange_engine.h"

using namespace esphome::home_io_control;
using namespace esphome::home_io_control::bridle;
using bridle_playback::PlaybackRig;
using bridle_playback::session_named;

namespace {

/// An exchange fixture, whether its request goes to a low-power device, and the band's profile.
struct ExchangeFixture {
  const char *name;
  bool low_power;
  const BridlePhy &(*phy)();
  /// Whether the fake's send timeline can decode the recorded `tx` payloads (it reads the 2.4 GHz PSDU, not the
  /// UART-coded gfsk payload).
  bool decodes_sends;
};

const BridlePhy &phy_868() { return io868_gfsk_phy(); }
const BridlePhy &phy_2g4() { return io2g4_phy(); }

}  // namespace

class BridleExchangeSessionReplay : public ::testing::TestWithParam<ExchangeFixture> {};

TEST_P(BridleExchangeSessionReplay, TheEngineSendsExactlyTheRecordedCommands) {
  const Session &session = session_named(GetParam().name);
  SKIP_IF_REFUSED(session);
  PlaybackRig rig(session, GetParam().phy());
  ASSERT_TRUE(rig.radio.init()) << (rig.radio.failure_reason() ? rig.radio.failure_reason() : "");

  RadioDriver *driver = &rig.radio;
  TuningConfig tuning;
  ExchangeEngine engine(&driver, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);
  engine.set_random_source([]() -> uint32_t { return 0; });  // the recording's draw (exchange_over_bridle_test.cpp)
  IoFrame response{};
  EXPECT_EQ(engine.send_and_receive(bridle_runs::execute_request(GetParam().low_power), response, FREQ_CH2),
            ExchangeOutcome::SUCCESS_WITH_RESPONSE);

  EXPECT_TRUE(rig.cp.playback_errors().empty()) << rig.errors();
  EXPECT_TRUE(rig.cp.playback_finished()) << "the driver stopped before the recording did";
  if (GetParam().decodes_sends) {
    EXPECT_EQ(rig.cp.send_timeline().sent_data().size(), 2u) << "the request and the answer to the challenge";
  }
}

INSTANTIATE_TEST_SUITE_P(
    BridleSessions, BridleExchangeSessionReplay,
    ::testing::Values(ExchangeFixture{"bridle_exchange_execute_challenge", false, &phy_2g4, true},
                      ExchangeFixture{"bridle_exchange_long_strobe", true, &phy_2g4, true},
                      ExchangeFixture{"bridle_868_exchange_execute_challenge", false, &phy_868, false},
                      ExchangeFixture{"bridle_868_exchange_long_preamble", true, &phy_868, false}),
    [](const ::testing::TestParamInfo<ExchangeFixture> &info) { return std::string(info.param.name); });

TEST(BridleSessionReplay, EveryFixtureHasAReplayAndTheOriginIsTheOneItWasRecordedWith) {
  const char *replayed[] = {"bridle_868_boot_configure_dual_band", "bridle_868_exchange_execute_challenge",
                            "bridle_868_exchange_long_preamble",   "bridle_exchange_execute_challenge",
                            "bridle_exchange_long_strobe",         "bridle_link_unsolicited_ready"};
  ASSERT_EQ(SESSION_COUNT, sizeof(replayed) / sizeof(replayed[0]))
      << "a new fixture needs a replay in this file or in radio_bridle_session_replay_test.cpp (and, if a test records "
         "it, one entry in "
         "scripts/regen-bridle-sessions.py)";
  for (size_t i = 0; i < SESSION_COUNT; i++) {
    EXPECT_STREQ(SESSIONS[i].name, replayed[i]);
    // Every fixture is a host test's own trace.
    EXPECT_STREQ(SESSIONS[i].origin, "synthetic-bootstrap") << SESSIONS[i].name;
    EXPECT_STRNE(SESSIONS[i].description, "");
  }
}
