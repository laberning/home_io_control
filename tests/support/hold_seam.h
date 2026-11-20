#pragma once

/// @file hold_seam.h
/// @brief Test seam for ADR 0111: records whether the frame log is held at every transmit and every
/// hand-over of a scripted reply on a MockRadio.
/// @ingroup hioc_hub

#include "log_frame.h"
#include "stubs/radio_test_common.h"

#include <gtest/gtest.h>

#include <functional>
#include <utility>
#include <vector>

namespace test {

/// Watches a MockRadio from the transmit that provokes a reply to the moment the reply is handed over. A step
/// that transmits and then waits is correct when the frame log is held at both (the hold depth is tracked in
/// every build, so this needs no frame-log build), and the hold must be gone again when the step returns.
/// It also reads the release count at each of those moments: a release is what prints the held lines in a
/// frame-log build, so a release between a reply and the next transmit of the same step (the #163 shape)
/// shows as differing counts, see quiet_from_delivery_to_next_send(). An inner release counts too although it prints
/// nothing, so the check is stricter than needed: a step holds nothing of its own between a reply and its answer.
///
/// It owns the radio's two callbacks; `extra_send` and `extra_deliver` run in them too, for a test that samples
/// something else (a LogCapture size) at the same two moments.
class HoldSeam {
 public:
  explicit HoldSeam(MockRadio &radio, std::function<void()> extra_send = {}, std::function<void()> extra_deliver = {}) {
    radio.set_on_send([this, extra = std::move(extra_send)] {
      this->depth_at_send.push_back(esphome::home_io_control::frame_log_hold_depth());
      this->events_.push_back({false, esphome::home_io_control::frame_log_release_count()});
      if (extra)
        extra();
    });
    radio.set_on_deliver([this, extra = std::move(extra_deliver)] {
      this->depth_at_deliver.push_back(esphome::home_io_control::frame_log_hold_depth());
      this->events_.push_back({true, esphome::home_io_control::frame_log_release_count()});
      if (extra)
        extra();
    });
  }

  /// Hold depth at each transmit, in send order.
  std::vector<uint8_t> depth_at_send;
  /// Hold depth at each hand-over of a scripted reply, in order.
  std::vector<uint8_t> depth_at_deliver;

  /// @return true when at least one transmit and one hand-over happened and the frame log was held at all of them.
  [[nodiscard]] bool held_at_every_send_and_delivery() const {
    return !depth_at_send.empty() && !depth_at_deliver.empty() && all_held(depth_at_send) && all_held(depth_at_deliver);
  }

  /// @param delivery Index into depth_at_deliver.
  /// @return true when the hold was not released between that hand-over and the next transmit; also true when
  /// no transmit follows (the step ended, and its release is expected).
  [[nodiscard]] bool quiet_from_delivery_to_next_send(size_t delivery) const {
    size_t seen = 0;
    for (size_t i = 0; i < events_.size(); i++) {
      if (!events_[i].is_delivery || seen++ != delivery)
        continue;
      for (size_t j = i + 1; j < events_.size(); j++)
        if (!events_[j].is_delivery)
          return events_[j].releases == events_[i].releases;
      return true;
    }
    ADD_FAILURE() << "no hand-over number " << delivery;
    return false;
  }

 private:
  /// One transmit or hand-over, with the release count at that moment.
  struct Event {
    bool is_delivery;
    uint32_t releases;
  };
  std::vector<Event> events_;

  static bool all_held(const std::vector<uint8_t> &depths) {
    for (const uint8_t depth : depths)
      if (depth == 0)
        return false;
    return true;
  }
};

/// @return true when no hold is left over: every step released what it held.
inline bool frame_log_released() { return esphome::home_io_control::frame_log_hold_depth() == 0; }

}  // namespace test
