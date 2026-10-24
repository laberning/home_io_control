#pragma once

#include "bridle_stand_in.h"
#include "corpus_generated.h"
#include "proto_frame.h"
#include "radio_io2g4_phy.h"
#include "radio_stand_in.h"

#include <gtest/gtest.h>

#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

/// Shared plumbing for the corpus_*_test.cpp suites: every suite parses capture
/// frames the same way and filters/instantiates the same CorpusCapture pointer arrays. Kept
/// here once so a new suite starts from a single include instead of another copy-paste.
namespace corpus_test {
using namespace esphome::home_io_control;
// `bridle::` alone is ambiguous here: the global ::bridle (the Bridle release) is visible too.
namespace hioc_bridle = esphome::home_io_control::bridle;

/// Length of a captured frame's wire bytes with any trailing CRC stripped.
inline uint8_t wire_len(const corpus::CorpusFrame &cf) {
  return cf.crc_present ? static_cast<uint8_t>(cf.len - 2) : cf.len;
}

/// Parse a captured frame's wire bytes (CRC-stripped) into an IoFrame.
inline IoFrame parse_capture_frame(const corpus::CorpusFrame &cf) {
  IoFrame frame{};
  EXPECT_TRUE(parse(cf.bytes, wire_len(cf), frame)) << "failed to parse captured frame bytes";
  return frame;
}

/// Why the pairing replay cannot drive @p capture, or nullptr when it can. It replays through
/// MockRadio, which models the 868 MHz radios only, and no 2.4 GHz pairing capture exists to check
/// a 2.4 GHz model against: such a capture is skipped by name (GTEST_SKIP with this reason)
/// rather than dropped from the parameter list, where it would vanish without a trace. The
/// exchange replay has no such limit: it picks its radio stand-in by the capture's `phy`.
inline const char *pairing_replay_skip_reason(const corpus::CorpusCapture &capture) {
  if (capture.phy == corpus::Phy::IO_2G4)
    return "phy io_2g4: the pairing replay models 868 MHz radio timing only";
  return nullptr;
}

/// The channel the hub knows a captured frame's frequency as. A 2.4 GHz capture records the
/// frequency on air; the hub works in the logical channels FREQ_CH1..3, which the 2.4 GHz profile
/// maps onto 2425/2450/2475 MHz. 0 stays 0 (not recorded).
inline uint32_t logical_freq(const corpus::CorpusFrame &cf, const corpus::CorpusCapture &capture) {
  if (capture.phy != corpus::Phy::IO_2G4 || cf.freq_hz == 0)
    return cf.freq_hz;
  return hioc_bridle::io2g4_phy().channel_hz(cf.freq_hz);
}

/// The radio stand-in a capture is replayed over: the Bridle driver on a scripted 2.4 GHz
/// co-processor for a `phy: io_2g4` capture (which needs an active ManualClock), else the mock of
/// the chip it was captured with. "sx1262" is the only chip whose mock (MockRadioSX1262) differs in
/// exchange-timing behaviour (response_preamble/has_fast_tx_rx_turnaround); every other
/// captured_with value (sx1276, lr1121, other, synthetic) is fine on the generic MockRadio, which
/// already mirrors the fast-hopping SX1276-like reference behaviour (radio_test_common.h). LR1121
/// shares its exchange-flow implementation with SX1262 (SoftPhyDriverBase) but has not been shown
/// to need SX1262's distinct timing mock for replay purposes -- revisit if an LR1121-specific
/// exchange-timing difference is ever found.
inline std::unique_ptr<RadioStandIn> make_stand_in(const corpus::CorpusCapture &capture) {
  if (capture.phy == corpus::Phy::IO_2G4)
    return std::make_unique<BridleStandIn>();
  if (std::string(capture.captured_with) == "sx1262")
    return std::make_unique<MockStandIn>(std::make_unique<MockRadioSX1262>());
  return std::make_unique<MockStandIn>();
}

/// The 868 MHz bridge a capture is replayed over: the Bridle driver on a scripted `gfsk_raw` co-processor,
/// which reports each reply `rx_len` bytes after its sync word. Needs an active ManualClock.
inline std::unique_ptr<RadioStandIn> make_868_bridge_stand_in() { return std::make_unique<BridleStandIn868>(); }

/// Corpus captures matching `pred`, e.g. captures_where([](auto *c) { return c->has_exchange; }).
inline std::vector<const corpus::CorpusCapture *> captures_where(
    const std::function<bool(const corpus::CorpusCapture *)> &pred) {
  std::vector<const corpus::CorpusCapture *> result;
  for (size_t i = 0; i < corpus::CAPTURE_COUNT; i++) {
    if (pred(&corpus::CAPTURES[i]))
      result.push_back(&corpus::CAPTURES[i]);
  }
  return result;
}

/// Every corpus capture, unfiltered.
inline std::vector<const corpus::CorpusCapture *> all_captures() {
  return captures_where([](const corpus::CorpusCapture *) { return true; });
}

/// The single capture with the given `id`, or nullptr if none matches. For tests that pin a
/// specific frame's bytes rather than sweeping the whole corpus — those tests should source their
/// expected bytes from here instead of transcribing them, so a corrected capture automatically
/// corrects what they check instead of leaving a stale transcription behind. Callers should
/// ASSERT on a non-null result: a missing id almost always means a capture got renamed, and that
/// should fail the test loudly rather than silently check nothing.
inline const corpus::CorpusCapture *capture_by_id(const char *id) {
  for (size_t i = 0; i < corpus::CAPTURE_COUNT; i++) {
    if (std::strcmp(corpus::CAPTURES[i].id, id) == 0)
      return &corpus::CAPTURES[i];
  }
  return nullptr;
}

/// gtest instantiation name generator: uses the capture's own `id` as the test name.
inline std::string capture_name_generator(const ::testing::TestParamInfo<const corpus::CorpusCapture *> &info) {
  return std::string(info.param->id);
}

}  // namespace corpus_test
