#pragma once

#include "corpus_generated.h"
#include "proto_frame.h"

#include <gtest/gtest.h>

#include <cstring>
#include <functional>
#include <string>
#include <vector>

/// Shared plumbing for the corpus_*_test.cpp suites: every suite parses capture
/// frames the same way and filters/instantiates the same CorpusCapture pointer arrays. Kept
/// here once so a new suite starts from a single include instead of another copy-paste.
namespace corpus_test {
using namespace esphome::home_io_control;

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
