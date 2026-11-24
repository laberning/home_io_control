/// @file radio_bridle_session_replay_test.cpp
/// @brief Recorded Bridle sessions replayed through the driver alone.
///
/// A fixture (tests/corpus/sessions/) is a conversation between the hub's driver and a co-processor. Here the
/// co-processor is the recording: FakeCoprocessor plays every recorded message at the gap it had after the command
/// it followed, and compares each command the driver sends with the recorded one (type and body, not `seq` or CRC).
/// A replay therefore fails when the driver's command sequence changes, and passes exactly when it is the recorded
/// one. The link and boot fixtures replay here, the 868 MHz one over the `Io868GfskPhy`, the other over the 2.4 GHz
/// profile; the exchange fixtures replay through the engine in corpus_bridle_session_replay_test.cpp.
/// @ingroup hioc_radio

#include <cstring>
#include <string>

#include <gtest/gtest.h>

#include "bridle_session_playback.h"

using namespace esphome::home_io_control;
using namespace esphome::home_io_control::bridle;
using bridle_playback::PlaybackRig;
using bridle_playback::refused_by_version_rule;
using bridle_playback::session_named;

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

TEST(BridleSessionReplay, AChangedCommandIsReportedWithBothMessages) {
  const Session &session = session_named("bridle_868_boot_configure_dual_band");
  SKIP_IF_REFUSED(session);
  PlaybackRig rig(session);  // the recording configures the gfsk radio; this driver takes radio 0, the 802.15.4 one
  rig.radio.init();
  ASSERT_FALSE(rig.cp.playback_errors().empty());
  EXPECT_NE(rig.errors().find("differs"), std::string::npos);
  EXPECT_NE(rig.errors().find("sent:"), std::string::npos);
  EXPECT_NE(rig.errors().find("recorded:"), std::string::npos);
}

TEST(BridleSessionReplay, ACommandBeyondTheRecordingIsReported) {
  const Session &session = session_named("bridle_868_boot_configure_dual_band");
  SKIP_IF_REFUSED(session);
  PlaybackRig rig(session, io868_gfsk_phy());
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
  // The recording's boot `ready` with the previous minor, as an old recording would be (the CRC is not read here).
  uint8_t ready[sizeof(SESSION_0_MESSAGE_0)];
  std::memcpy(ready, SESSION_0_MESSAGE_0, sizeof(ready));
  ready[4] = PROTO_MINOR - 1;
  const SessionMessage message{false, 0, ready, sizeof(ready)};
  const Session old{"old", "", "", "", "", &message, 1};
  EXPECT_TRUE(refused_by_version_rule(old));
  EXPECT_FALSE(refused_by_version_rule(SESSIONS[0]));
}
