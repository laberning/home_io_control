#pragma once

/// @file timed_replay.h
/// @brief Replays a corpus capture's received frames at their captured times, relative to what
/// the code under test actually transmits.
///
/// A captured `rx` frame answers the `tx` frame before it. Its `t_ms` gap to that frame is the
/// device's timing as observed on the day: TX-start log stamp to reception, so it includes the
/// capture's own airtime. Replaying it as "arrives that long after *our* matching send started"
/// keeps the device's timing while letting the engine choose its own. The engine only
/// succeeds when its listens really cover each reply, and a reply the capture shows arriving after
/// a retry needs the engine to really make that retry. The release rule itself lives in
/// timed_release.h (SendTimeline), fed by MockRadio::queue_rx_timed_after_send()/
/// queue_rx_timed_after_bytes().
///
/// Offsets are held in microseconds. A capture stamps either `t_ms` or, when its sniffer is finer
/// than a millisecond (2.4 GHz), `t_us`; both give the same offsets. A stamp is where the capturing
/// tool put the frame, not where the radio's main frame started, so the replay anchors on the
/// radio's own view of the send (a Bridle transmission's main frame, after its wake-up strobe).

#include "corpus_generated.h"
#include "corpus_test_helpers.h"
#include "stubs/radio_test_common.h"

#include "proto_timing.h"  // FREQ_CH2

#include <cstring>
#include <string>
#include <vector>

namespace corpus_test {
using namespace esphome::home_io_control;

/// How a replayed reply finds the send it answers.
enum class ReplayAnchor : uint8_t {
  /// The N-th send, N counting the capture's own tx frames. For replays whose transmissions match
  /// the capture one to one (the exchange replay asserts exactly that).
  SEND_INDEX,
  /// The first send with the same bytes as the preceding captured tx. For replays that may retry a
  /// different number of times than the capture shows; needs byte-exact tx (`key: corpus`).
  FIRST_MATCHING_SEND,
};

/// The captured frame as the radio would hand it to the engine: CRC stripped, frequency defaulted
/// to CH2 when the capture didn't record one.
inline RadioRxPacket to_rx_packet(
    const corpus::CorpusFrame &cf) {  // freq: as captured (see logical_freq() for 2.4 GHz)
  RadioRxPacket packet{};
  packet.len = wire_len(cf);
  std::memcpy(packet.data, cf.bytes, packet.len);
  packet.freq_hz = cf.freq_hz != 0 ? cf.freq_hz : FREQ_CH2;
  return packet;
}

/// A frame's stamp in microseconds, from whichever time base its capture uses.
inline uint64_t stamp_us(const corpus::CorpusFrame &cf) {
  return cf.has_t_us ? cf.t_us : static_cast<uint64_t>(cf.t_ms) * 1000u;
}

/// True when the capture can be replayed on its own timeline: real, not synthetic, and every frame
/// timestamped.
inline bool capture_is_timed(const corpus::CorpusCapture &capture) {
  if (std::string(capture.source_origin) == "synthetic-bootstrap" || std::string(capture.captured_with) == "synthetic")
    return false;
  for (uint8_t i = 0; i < capture.frame_count; i++) {
    if (!capture.frames[i].has_t_ms && !capture.frames[i].has_t_us)
      return false;
  }
  return capture.frame_count > 0;
}

/// Queues every `rx` frame of @p capture on @p radio at its captured offset from the `tx` frame
/// it answers. Within a run of byte-identical consecutive `tx` frames (retries), the offset is
/// taken from the run's last frame, the one the reply actually followed. `rx` frames before the
/// first `tx` (traffic overheard before the exchange began, e.g. the remote press that triggered a
/// poll) are not queued: nothing was listening for the exchange yet.
/// @tparam Radio MockRadio or a RadioStandIn: anything with queue_rx_timed_after_send()/_after_bytes().
/// @return empty on success, else what made the capture unreplayable.
template<class Radio>
std::string queue_timed_rx(Radio &radio, const corpus::CorpusCapture &capture, ReplayAnchor anchor) {
  if (!capture_is_timed(capture))
    return "capture has frames without t_ms or t_us (or is synthetic)";
  int tx_index = -1;
  const corpus::CorpusFrame *last_tx = nullptr;
  for (uint8_t i = 0; i < capture.frame_count; i++) {
    const corpus::CorpusFrame &cf = capture.frames[i];
    if (cf.tx) {
      tx_index++;
      last_tx = &cf;
      continue;
    }
    if (last_tx == nullptr)
      continue;
    if (stamp_us(cf) < stamp_us(*last_tx))
      return "rx frame " + std::to_string(i) + " is stamped before the tx frame it follows";
    const uint64_t offset_us = stamp_us(cf) - stamp_us(*last_tx);
    if (anchor == ReplayAnchor::SEND_INDEX) {
      radio.queue_rx_timed_after_send(to_rx_packet(cf), tx_index, offset_us);
    } else {
      radio.queue_rx_timed_after_bytes(
          to_rx_packet(cf), std::vector<uint8_t>(last_tx->bytes, last_tx->bytes + wire_len(*last_tx)), offset_us);
    }
  }
  return {};
}

}  // namespace corpus_test
