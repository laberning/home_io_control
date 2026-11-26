#pragma once

/// @file bridle_session_playback.h
/// @brief What every replay of a recorded Bridle session shares: finding a fixture, the version rule, and the driver
///        over a recording.
///
/// A session fixture (tests/corpus/sessions/) is replayed by the driver alone (radio_bridle_session_replay_test.cpp)
/// and through the exchange engine (corpus_bridle_session_replay_test.cpp); both start from the pieces here.

#include <cstddef>
#include <optional>
#include <string>

#include <gtest/gtest.h>

#include "fake_byte_stream.h"
#include "fake_coprocessor.h"
#include "testable_radio_bridle.h"

namespace bridle_playback {

using namespace esphome::home_io_control::bridle;

/// The fixture named @p name; a failure (naming the missing fixture) and an empty session when there is none.
inline const Session &session_named(const std::string &name) {
  for (size_t i = 0; i < SESSION_COUNT; i++) {
    if (name == SESSIONS[i].name)
      return SESSIONS[i];
  }
  ADD_FAILURE() << "no session fixture named " << name << "; record it from the test that writes its trace";
  static const Session none{};
  return none;
}

/// Whether the host's version rule (spec §10.2) refuses the co-processor of a recording: its first `ready` names a
/// protocol this build does not accept. Such a recording is migrated (`bridle-session migrate`); until then its
/// replay is skipped by name, never dropped.
inline bool refused_by_version_rule(const Session &session) {
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

/// The driver over a recording. No reset hook: the recording holds the boot `ready`.
struct PlaybackRig {
  esphome::test_clock::ManualClock clock;
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

}  // namespace bridle_playback

/// Skips the calling test, by name, when its recording is refused by the version rule.
#define SKIP_IF_REFUSED(session) \
  if (bridle_playback::refused_by_version_rule(session)) \
  GTEST_SKIP() << "recording refused by the version rule, not replayed: " << (session).name
