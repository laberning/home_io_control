/// @file corpus_bridle_session_replay_test.cpp
/// @brief Recorded Bridle sessions replayed through the layer that recorded them.
///
/// A fixture (tests/corpus/sessions/) is a conversation between the hub's driver and a co-processor.
/// Here the co-processor is the recording: FakeCoprocessor plays every recorded message at the gap it
/// had after the command it followed, and compares each command the driver sends with the recorded
/// one (type and body, not `seq` or CRC). A replay therefore fails when the driver's command
/// sequence changes, and passes exactly when it is the recorded one.
///
/// Every fixture replays through the layer that recorded it: the exchange engine for the exchange
/// fixtures, the driver alone for the link and boot fixtures. The 868 MHz fixtures replay over the
/// `Io868GfskPhy`, the others over the 2.4 GHz profile.

#include <cstring>
#include <string>

#include <gtest/gtest.h>

#include "bridle_session_runs.h"
#include "exchange_engine.h"
#include "fake_coprocessor.h"
#include "testable_radio_bridle.h"

using namespace esphome::home_io_control;
using namespace esphome::home_io_control::bridle;
using esphome::test_clock::ManualClock;

namespace {

const Session &session_named(const std::string &name) {
  for (size_t i = 0; i < SESSION_COUNT; i++) {
    if (name == SESSIONS[i].name)
      return SESSIONS[i];
  }
  ADD_FAILURE() << "no session fixture named " << name << "; regenerate with `make bridle-sessions-regen`";
  static const Session none{};
  return none;
}

/// Whether the host's version rule (spec §10.2) refuses the co-processor of a recording: its first `ready` names a
/// protocol this build does not accept. Such a recording is migrated (`bridle-session migrate`); until then its
/// replay is skipped by name, never dropped.
bool refused_by_version_rule(const Session &session) {
  for (size_t i = 0; i < session.message_count; i++) {
    const SessionMessage &m = session.messages[i];
    Message decoded;
    if (!m.from_host && decode_message(m.bytes, m.len, decoded) == DecodeStatus::OK) {
      if (const auto *ready = std::get_if<Ready>(&decoded.body))
        return !version_accepted(PROTO_MAJOR, PROTO_MINOR, ready->proto_major, ready->proto_minor);
    }
  }
  return false;
}

#define SKIP_IF_REFUSED(session) \
  if (refused_by_version_rule(session)) \
  GTEST_SKIP() << "recording refused by the version rule, not replayed: " << (session).name

/// The driver over a recording. No reset hook: the recording holds the boot `ready`.
struct PlaybackRig {
  ManualClock clock;
  FakeByteStream stream;
  FakeCoprocessor cp;
  FakeResetLine reset;
  TestableRadioBridle radio;

  explicit PlaybackRig(const Session &session, const BridlePhy &phy = io2g4_phy(),
                       std::optional<int8_t> tx_power_dbm = std::nullopt)
      : cp(stream), radio(&stream, phy, &reset, tx_power_dbm) {
    cp.play_session(session);
  }

  /// The failures of the replay, one per line, for a test to print.
  [[nodiscard]] std::string errors() const {
    std::string text;
    for (const std::string &e : cp.playback_errors())
      text += e + "\n";
    return text;
  }
};

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

TEST(BridleLinkSessionReplay, TheDriverReconfiguresAfterTheRecordedReboot) {
  const Session &session = session_named("bridle_link_unsolicited_ready");
  SKIP_IF_REFUSED(session);
  PlaybackRig rig(session);
  ASSERT_TRUE(rig.radio.init());
  rig.radio.set_mode_rx();
  // The recording's reboot arrives a few ms later; passes as the hub's loop makes them carry the
  // driver through the reconfiguration. The recording ends with the test's own rx_on after it.
  RadioRxPacket packet;
  for (int pass = 0; pass < 1000 && !(rig.cp.playback_events_pending() == 0 && rig.radio.link_running()); pass++) {
    esphome::test_clock::advance_ms(5);
    rig.radio.check_for_packet(packet);
  }
  EXPECT_TRUE(rig.radio.link_running());
  rig.radio.set_mode_rx();
  rig.radio.check_for_packet(packet);  // takes the ack the recording ends with
  EXPECT_TRUE(rig.cp.playback_finished());
  EXPECT_TRUE(rig.cp.playback_errors().empty()) << rig.errors();
}

TEST(BridleLinkSessionReplay, TheDriverConfiguresTheGfskRadioOfTheRecordedDualBandBoard) {
  const Session &session = session_named("bridle_868_boot_configure_dual_band");
  SKIP_IF_REFUSED(session);
  PlaybackRig rig(session, io868_gfsk_phy());
  ASSERT_TRUE(rig.radio.init()) << (rig.radio.failure_reason() ? rig.radio.failure_reason() : "");
  EXPECT_TRUE(rig.cp.playback_finished());
  EXPECT_TRUE(rig.cp.playback_errors().empty()) << rig.errors();
}

/// The recording of the hardware bench: the driver and the exchange engine over a real dual-band
/// co-processor, four exchanges to phantom devices that never answer.
TEST(BridleHardwareSessionReplay, TheEngineSendsTheCommandsOfFourExchangesToPhantomDevices) {
  static constexpr uint8_t OWN[3] = {0xC0, 0xFF, 0xEE};
  static constexpr uint8_t ALWAYS_ALIVE[3] = {0xFE, 0xEB, 0x1E};
  static constexpr uint8_t LOW_POWER[3] = {0xFE, 0xEB, 0x2E};
  // The bench host runs the 868 board package: 4 dBm, and the clear-channel check before every send.
  const Session &session = session_named("bridle_868_hardware_phantom_exchanges");
  SKIP_IF_REFUSED(session);
  PlaybackRig rig(session, io868_gfsk_phy(), 4);
  ASSERT_TRUE(rig.radio.init()) << (rig.radio.failure_reason() ? rig.radio.failure_reason() : "");
  rig.radio.run_idle_scan();
  RadioRxPacket packet;
  // The bench host idled for five seconds before its first exchange, with a foreign preamble heard and dropped
  // meanwhile (events only, no frame): the playback releases them in the recorded order.
  for (int i = 0; i < 1200; i++) {
    esphome::test_clock::advance_ms(5);
    rig.radio.check_for_packet(packet);
  }

  RadioDriver *driver = &rig.radio;
  TuningConfig tuning;
  tuning.lbt_max_retries = 3;
  tuning.normal_start_preamble = 48;  // what the 868 board package sets, and what the recording shows
  ExchangeEngine engine(&driver, OWN, test::TEST_SYSTEM_KEY, &tuning);
  IoFrame request{};
  IoFrame response{};
  // The order the hub asked in on the bench: the names first, then the statuses, each device in turn.
  create_get_name(request, OWN, ALWAYS_ALIVE, false);
  EXPECT_EQ(engine.send_and_receive(request, response, FREQ_CH2), ExchangeOutcome::FAILED);
  rig.radio.run_idle_scan();
  create_get_name(request, OWN, LOW_POWER, true);
  EXPECT_EQ(engine.send_and_receive(request, response, FREQ_CH2), ExchangeOutcome::FAILED);
  rig.radio.run_idle_scan();
  create_get_status(request, OWN, LOW_POWER, true);
  EXPECT_EQ(engine.send_and_receive(request, response, FREQ_CH2), ExchangeOutcome::FAILED);
  rig.radio.run_idle_scan();
  create_get_status(request, OWN, ALWAYS_ALIVE, false);
  EXPECT_EQ(engine.send_and_receive(request, response, FREQ_CH2), ExchangeOutcome::FAILED);
  rig.radio.run_idle_scan();
  esphome::test_clock::advance_ms(5);
  rig.radio.check_for_packet(packet);  // takes the ack the recording ends with

  EXPECT_TRUE(rig.cp.playback_errors().empty()) << rig.errors();
  EXPECT_TRUE(rig.cp.playback_finished()) << "the driver stopped before the recording did";
}

TEST(BridleSessionReplay, AChangedCommandIsReportedWithBothMessages) {
  const Session &session = session_named("bridle_exchange_execute_challenge");
  SKIP_IF_REFUSED(session);
  PlaybackRig rig(session);
  ASSERT_TRUE(rig.radio.init());
  RadioDriver *driver = &rig.radio;
  TuningConfig tuning;
  ExchangeEngine engine(&driver, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);
  IoFrame request{};
  create_execute_position(request, test::OWN_ID, test::DST_ID, false, 50);  // the recording says 100
  IoFrame response{};
  engine.send_and_receive(request, response, FREQ_CH2);

  ASSERT_FALSE(rig.cp.playback_errors().empty());
  EXPECT_NE(rig.errors().find("differs"), std::string::npos);
  EXPECT_NE(rig.errors().find("sent:"), std::string::npos);
  EXPECT_NE(rig.errors().find("recorded:"), std::string::npos);
}

TEST(BridleSessionReplay, ACommandBeyondTheRecordingIsReported) {
  const Session &session = session_named("bridle_exchange_execute_challenge");
  SKIP_IF_REFUSED(session);
  PlaybackRig rig(session);
  ASSERT_TRUE(rig.radio.init());
  rig.radio.set_mode_rx();  // the recording holds no rx_on
  EXPECT_FALSE(rig.cp.playback_errors().empty());
}

TEST(BridleSessionReplay, NoFixtureIsRefusedByTheVersionRule) {
  // The release gate: a refused recording would be skipped above, so none may be left.
  for (size_t i = 0; i < SESSION_COUNT; i++)
    EXPECT_FALSE(refused_by_version_rule(SESSIONS[i])) << SESSIONS[i].name << " needs `bridle-session migrate`";
}

TEST(BridleSessionReplay, ARecordingOfAnotherProtocolVersionIsRefused) {
  // The recording of the 0.5 boot `ready` with the previous minor, as an old recording would be (the CRC is not read
  // here).
  uint8_t ready[sizeof(SESSION_0_MESSAGE_0)];
  std::memcpy(ready, SESSION_0_MESSAGE_0, sizeof(ready));
  ready[4] = PROTO_MINOR - 1;
  const SessionMessage message{false, 0, ready, sizeof(ready)};
  const Session old{"old", "", "", "", "", &message, 1};
  EXPECT_TRUE(refused_by_version_rule(old));
  EXPECT_FALSE(refused_by_version_rule(SESSIONS[0]));
}

TEST(BridleSessionReplay, EveryFixtureHasAReplayHereAndTheOriginIsTheOneItWasRecordedWith) {
  const char *replayed[] = {"bridle_868_boot_configure_dual_band", "bridle_868_exchange_execute_challenge",
                            "bridle_868_exchange_long_preamble",   "bridle_868_hardware_phantom_exchanges",
                            "bridle_exchange_execute_challenge",   "bridle_exchange_long_strobe",
                            "bridle_link_unsolicited_ready"};
  ASSERT_EQ(SESSION_COUNT, sizeof(replayed) / sizeof(replayed[0]))
      << "a new fixture needs a replay in this file (and, if a test records it, one entry in "
         "scripts/regen-bridle-sessions.py)";
  for (size_t i = 0; i < SESSION_COUNT; i++) {
    EXPECT_STREQ(SESSIONS[i].name, replayed[i]);
    // The hardware recording has its own origin and is kept out of `make bridle-sessions-regen`; every other
    // fixture is a host test's own trace.
    const bool hardware = std::string(SESSIONS[i].name) == "bridle_868_hardware_phantom_exchanges";
    EXPECT_STREQ(SESSIONS[i].origin, hardware ? "own-hardware" : "synthetic-bootstrap") << SESSIONS[i].name;
    EXPECT_STRNE(SESSIONS[i].description, "");
  }
}
