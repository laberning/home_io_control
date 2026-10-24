#pragma once

/// @file bridle_session_runs.h
/// @brief What the hub asks of the driver in the session fixtures, defined once.
///
/// A session fixture (tests/corpus/sessions/) is recorded from a test that drives the hub over a
/// scripted co-processor and replayed by a test that drives the same hub over the recording. Both
/// must ask for the same thing or the replay would compare unlike conversations, so the requests
/// live here.

#include "proto_commands.h"
#include "proto_frame.h"
#include "test_helpers.h"

namespace bridle_runs {

/// The EXECUTE the exchange fixtures send: 100 % (closed) to the test device, to a low-power
/// receiver (long wake-up train) or an always-alive one.
inline esphome::home_io_control::IoFrame execute_request(bool low_power) {
  esphome::home_io_control::IoFrame request{};
  esphome::home_io_control::create_execute_position(request, test::OWN_ID, test::DST_ID, low_power, 100);
  return request;
}

}  // namespace bridle_runs
