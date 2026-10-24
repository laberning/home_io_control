#pragma once

/// @file bridle_stand_in.h
/// @brief RadioBridle over a FakeCoprocessor, behind the RadioStandIn scripting surface.
///
/// The hub sees a real RadioBridle; the peer is a scripted co-processor of one band: 2.4 GHz
/// (`BridleStandIn`) or 868 MHz on a `gfsk_raw` radio (`BridleStandIn868`). A queued frame is an
/// `rx` event: the peer's answer to the hub's next transmission, released when that transmission
/// has ended plus the turnaround of a real device (spec C6: a reply 300 µs after the main frame).
/// On 868 MHz the event comes later still: a `gfsk_raw` radio reports a frame only once it has
/// captured `rx_len` bytes behind the sync word (10 ms at 38.4 kb/s), whatever the frame's length.
/// Needs an active ManualClock, like everything with a clock of its own.

#include <deque>
#include <optional>

#include "fake_byte_stream.h"
#include "fake_coprocessor.h"
#include "radio_stand_in.h"
#include "radio_io868_gfsk_phy.h"
#include "testable_radio_bridle.h"

/// What differs between the bands a Bridle stand-in can be built for.
struct Bridge2g4 {
  using Phy = esphome::home_io_control::bridle::Io2g4Phy;
  static const Phy &phy() { return esphome::home_io_control::bridle::io2g4_phy(); }
  /// The co-processor: the reference 802.15.4 radio.
  static FakeCoprocessor::Config config() { return {}; }
  /// The payload of the `tx` the hub sends for an io frame: the PSDU without its FCS.
  static std::vector<uint8_t> request_payload(const std::vector<uint8_t> &frame) {
    std::vector<uint8_t> psdu(esphome::home_io_control::bridle::IO2G4_PSDU_HEADER,
                              esphome::home_io_control::bridle::IO2G4_PSDU_HEADER + 3);
    psdu.insert(psdu.end(), frame.begin(), frame.end());
    return psdu;
  }
  /// The `rx` payload for a reply: header, the io frame, the FCS the radio appends.
  static std::vector<uint8_t> reply_payload(const std::vector<uint8_t> &frame) {
    std::vector<uint8_t> psdu = request_payload(frame);
    uint16_t const fcs = esphome::home_io_control::crc_ccitt(psdu.data(), static_cast<uint8_t>(psdu.size()));
    psdu.push_back(static_cast<uint8_t>(fcs));
    psdu.push_back(static_cast<uint8_t>(fcs >> 8));
    return psdu;
  }
  static constexpr esphome::home_io_control::bridle::CrcState REPLY_CRC =
      esphome::home_io_control::bridle::CrcState::OK;
  /// From the end of the hub's frame to the moment the co-processor reports the reply.
  static constexpr uint64_t REPLY_DELAY_US = 300;
  /// Added to a reply scripted at a time of its own: nothing, the radio reports a frame when its last bit is in.
  static constexpr uint64_t TIMED_REPORT_DELAY_US = 0;
};

struct Bridge868 {
  using Phy = esphome::home_io_control::bridle::Io868GfskPhy;
  static const Phy &phy() { return esphome::home_io_control::bridle::io868_gfsk_phy(); }
  /// The co-processor: one `gfsk_raw` radio, as the SX1262 behind a Kite.
  static FakeCoprocessor::Config config() {
    FakeCoprocessor::Config c;
    c.radios = {FakeCoprocessor::reference_gfsk_radio(0)};
    c.anchor_replies_at_tx_end = true;  // the corpus stamps an 868 MHz frame once its transmission is over
    c.chip_id = static_cast<uint16_t>(esphome::home_io_control::bridle::ChipId::NRF52840);
    return c;
  }
  /// The payload of the `tx` the hub sends for an io frame: the frame and its CRC, UART-coded.
  static std::vector<uint8_t> request_payload(const std::vector<uint8_t> &frame) {
    uint8_t out[esphome::home_io_control::RADIO_PACKET_BUFFER_SIZE];
    size_t const n = phy().wrap(frame.data(), static_cast<uint8_t>(frame.size()), out, sizeof(out));
    return {out, out + n};
  }
  /// The `rx` payload for a reply: exactly `rx_len` bytes behind the sync word, the coded frame first and then
  /// what else the radio hears (deterministic noise that is no frame).
  static std::vector<uint8_t> reply_payload(const std::vector<uint8_t> &frame) {
    std::vector<uint8_t> raw = request_payload(frame);
    for (size_t i = raw.size(); i < esphome::home_io_control::bridle::IO868_RX_LEN; i++)
      raw.push_back(static_cast<uint8_t>(0x5A + 37 * i));
    return raw;
  }
  static constexpr esphome::home_io_control::bridle::CrcState REPLY_CRC =
      esphome::home_io_control::bridle::CrcState::NOT_CHECKED;
  /// The reply starts 300 µs after the hub's frame, takes its preamble and sync word to reach its first byte, and is
  /// reported once `rx_len` bytes have arrived behind the sync word.
  static constexpr uint64_t REPLY_DELAY_US =
      300 +
      esphome::home_io_control::soft_phy_air_time_us(esphome::home_io_control::SHORT_PREAMBLE +
                                                     esphome::home_io_control::IO868_SYNC_WORD_BYTES) +
      esphome::home_io_control::soft_phy_air_time_us(esphome::home_io_control::bridle::IO868_RX_LEN);
  /// A reply scripted at the time its frame was captured (a corpus capture) is reported `rx_len` bytes after
  /// the sync word, which is later than the frame ended for any frame shorter than that.
  static constexpr uint64_t TIMED_REPORT_DELAY_US =
      esphome::home_io_control::soft_phy_air_time_us(esphome::home_io_control::bridle::IO868_RX_LEN);
};

template<class Band> class BasicBridleStandIn : public RadioStandIn {
 public:
  /// A device answers ~300 µs after the frame it answers ends (spec C6), later where the radio reports a
  /// frame only after capturing `rx_len` bytes: `Band::REPLY_DELAY_US` is what the stand-in uses.
  static constexpr uint64_t REPLY_TURNAROUND_US = Band::REPLY_DELAY_US;

  /// @param trace_to_file Write the driver's trace when HIOC_BRIDLE_TRACE_DIR is set. A test that
  ///        makes the co-processor misbehave on purpose passes false: the trace lint should not judge it.
  explicit BasicBridleStandIn(bool trace_to_file = true)
      : cp_(stream_, Band::config()), radio_(&stream_, Band::phy(), &reset_, std::nullopt) {
    if (!esphome::test_clock::is_manual())
      std::abort();  // the stream's lockstep ticks and the co-processor's air time need the manual clock
    reset_.on_release = [this] { cp_.boot(); };
    cp_.set_tx_observer([this](const std::vector<uint8_t> &, uint64_t, uint64_t end_us) { on_transmission_(end_us); });
    if (trace_to_file)
      radio_.trace_to_environment_dir();
    initialised_ = radio_.init();
  }

  /// Whether the driver came up; a test that needs the link asserts it.
  [[nodiscard]] bool initialised() const { return initialised_; }
  FakeCoprocessor &coprocessor() { return cp_; }
  TestableRadioBridle &bridle() { return radio_; }

  esphome::home_io_control::RadioDriver &driver() override { return radio_; }

  void queue_rx(const esphome::home_io_control::RadioRxPacket &packet) override { replies_.push_back(packet); }
  void queue_rx_silence(uint8_t count = 1) override {
    for (uint8_t i = 0; i < count; i++)
      replies_.push_back(std::nullopt);
  }
  void queue_rx_timed_after_send(const esphome::home_io_control::RadioRxPacket &packet, int send_index,
                                 uint64_t offset_us) override {
    cp_.inject_rx_after_send(send_index, offset_us + Band::TIMED_REPORT_DELAY_US, air_freq_(packet), psdu_of_(packet));
  }
  void queue_rx_timed_after_bytes(const esphome::home_io_control::RadioRxPacket &packet,
                                  std::vector<uint8_t> anchor_frame, uint64_t offset_us) override {
    cp_.inject_rx_after_bytes(with_header_(anchor_frame), offset_us + Band::TIMED_REPORT_DELAY_US, air_freq_(packet),
                              psdu_of_(packet));
  }
  void queue_channel_busy(int16_t level_dbm, uint8_t times = 1) override {
    for (uint8_t i = 0; i < times; i++)
      cp_.queue_channel_level_dbm(level_dbm);
  }
  void set_model_tx_airtime(bool) override {}

  std::vector<std::vector<uint8_t>> sent_frames() const override { return radio_.sent_frames; }
  const std::vector<esphome::home_io_control::RadioTxConfig> &tx_configs() const override { return radio_.tx_configs; }
  const std::vector<uint32_t> &freq_history() const override { return radio_.freq_history; }
  std::vector<uint32_t> send_times_ms() const override { return radio_.send_times_ms; }
  const std::vector<uint32_t> &wait_timeouts() const override { return radio_.wait_timeouts; }

 private:
  static const auto &phy_() { return Band::phy(); }

  /// The air frequency of a packet named by its logical channel or already by its air frequency.
  static uint32_t air_freq_(const esphome::home_io_control::RadioRxPacket &packet) {
    return phy_().channel_hz(packet.freq_hz) != 0 ? packet.freq_hz : phy_().air_freq_hz(packet.freq_hz);
  }
  /// The payload of the `tx` the hub sends for `frame`.
  static std::vector<uint8_t> with_header_(const std::vector<uint8_t> &frame) { return Band::request_payload(frame); }
  /// The payload the radio would report for the peer's `packet`.
  static std::vector<uint8_t> psdu_of_(const esphome::home_io_control::RadioRxPacket &packet) {
    return Band::reply_payload(std::vector<uint8_t>(packet.data, packet.data + packet.len));
  }

  /// The hub has transmitted: the peer's scripted answer (or silence) follows.
  void on_transmission_(uint64_t end_us) {
    if (replies_.empty())
      return;
    std::optional<esphome::home_io_control::RadioRxPacket> reply = replies_.front();
    replies_.pop_front();
    if (!reply.has_value())
      return;
    uint64_t const now = FakeCoprocessor::now_us();
    cp_.inject_rx(air_freq_(*reply), psdu_of_(*reply), Band::REPLY_CRC, end_us + REPLY_TURNAROUND_US - now);
  }

  FakeByteStream stream_;
  FakeCoprocessor cp_;
  FakeResetLine reset_;
  TestableRadioBridle radio_;
  std::deque<std::optional<esphome::home_io_control::RadioRxPacket>> replies_;
  bool initialised_{false};
};

/// The 2.4 GHz stand-in.
using BridleStandIn = BasicBridleStandIn<Bridge2g4>;
/// The 868 MHz stand-in, on a `gfsk_raw` radio.
using BridleStandIn868 = BasicBridleStandIn<Bridge868>;
