#include "oneway_receiver.h"

#include <algorithm>
#include <cstring>

/// @file oneway_receiver.cpp
/// @brief State and rules for frames overheard from one-way (1W) remotes.
/// @ingroup hioc_hub

namespace esphome {
namespace home_io_control {

void OneWayReceiver::note_activity(uint32_t now) {
  if (decisions::oneway_burst_started_fresh(this->last_activity_ms_, now, ONEWAY_QUIET_PERIOD_MS))
    this->first_activity_ms_ = now;
  this->last_activity_ms_ = now;
}

void OneWayReceiver::note_frame(const IoFrame &frame, uint32_t now, int16_t rssi_dbm) {
  this->note_activity(now);
  if (!decisions::is_one_way_pairing_gesture((frame.ctrl0 & CTRL0_PROTOCOL_1W) != 0, frame.dst, frame.cmd))
    return;
  memcpy(this->sighting_.src, frame.src, NODE_ID_SIZE);
  memcpy(this->sighting_.dst, frame.dst, NODE_ID_SIZE);
  this->sighting_.cmd = frame.cmd;
  this->sighting_.rssi = rssi_dbm;
  this->sighting_.seen_ms = now;
}

bool OneWayReceiver::is_new_press(const IoFrame &frame, const OneWayFrameInfo &info, uint32_t now) {
  decisions::OneWayDedupState incoming{
      node_id_to_string(frame.src), frame.cmd, info.has_intent, info.main0, info.main1, now};
  memcpy(incoming.dst, frame.dst, NODE_ID_SIZE);
  if (decisions::is_duplicate_1w_frame(this->last_press_, incoming, ONEWAY_DEDUP_WINDOW_MS))
    return false;
  this->last_press_ = incoming;
  return true;
}

bool OneWayReceiver::defers_background_poll(bool next_is_background, uint32_t now) const {
  return decisions::defer_background_poll_for_1w_activity(next_is_background, this->first_activity_ms_,
                                                          this->last_activity_ms_, now, ONEWAY_QUIET_PERIOD_MS,
                                                          ONEWAY_POLL_DEFER_CAP_MS);
}

bool OneWayReceiver::is_exposed(const std::string &sender_id) const {
  return std::find(this->exposed_senders_.begin(), this->exposed_senders_.end(), sender_id) !=
         this->exposed_senders_.end();
}

}  // namespace home_io_control
}  // namespace esphome
