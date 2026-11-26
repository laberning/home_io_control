#pragma once

/// @file radio_stand_in.h
/// @brief One scripting surface for the radios the hub is tested over: MockRadio and the Bridle
///        driver on a scripted co-processor.
///
/// A test written against RadioStandIn asserts the same rules for an SPI radio and for the bridge:
/// what the hub transmitted, what it listened for, what it did about a busy channel. Only the
/// subset both can express is here; what only one models (the ordering rule of a Bridle link, a
/// strobe) is tested on that stand-in alone.

#include <cstdint>
#include <memory>
#include <vector>

#include "radio_interface.h"
#include "stubs/radio_test_common.h"

class RadioStandIn {
 public:
  virtual ~RadioStandIn() = default;

  /// The driver the hub is handed.
  virtual esphome::home_io_control::RadioDriver &driver() = 0;

  // === Scripting: what the peer does ===

  /// The next frame the peer sends after the hub's next transmission. The channel may be the hub's
  /// logical channel or, for a 2.4 GHz frame, its air frequency.
  virtual void queue_rx(const esphome::home_io_control::RadioRxPacket &packet) = 0;
  /// The peer says nothing after the hub's next transmission.
  virtual void queue_rx_silence(uint8_t count = 1) = 0;
  /// A frame that arrives `offset_us` after the start of the `send_index`-th transmission (the main
  /// frame's start, where a transmission has a strobe).
  virtual void queue_rx_timed_after_send(const esphome::home_io_control::RadioRxPacket &packet, int send_index,
                                         uint64_t offset_us) = 0;
  /// The same anchored at the first transmission of exactly these io-frame bytes.
  virtual void queue_rx_timed_after_bytes(const esphome::home_io_control::RadioRxPacket &packet,
                                          std::vector<uint8_t> anchor_frame, uint64_t offset_us) = 0;
  /// The next `times` clear-channel checks find the channel at `level_dbm`.
  virtual void queue_channel_busy(int16_t level_dbm, uint8_t times = 1) = 0;
  /// Make a transmission take its time on air. The bridge always does.
  virtual void set_model_tx_airtime(bool on) = 0;

  // === What the hub did ===

  /// The io frames of the transmissions that went out, in order.
  virtual std::vector<std::vector<uint8_t>> sent_frames() const = 0;
  /// The config of each of those transmissions.
  virtual const std::vector<esphome::home_io_control::RadioTxConfig> &tx_configs() const = 0;
  /// Every channel the hub retuned to.
  virtual const std::vector<uint32_t> &freq_history() const = 0;
  /// Manual-clock milliseconds at each transmission (a busy one included on the bridge).
  virtual std::vector<uint32_t> send_times_ms() const = 0;
  /// Every timeout the hub handed to wait_for_packet().
  virtual const std::vector<uint32_t> &wait_timeouts() const = 0;
  [[nodiscard]] int send_count() const { return static_cast<int>(sent_frames().size()); }
};

/// MockRadio behind the stand-in interface. `radio` may be a MockRadio subclass.
class MockStandIn : public RadioStandIn {
 public:
  explicit MockStandIn(std::unique_ptr<MockRadio> radio = std::make_unique<MockRadio>()) : radio_(std::move(radio)) {}

  esphome::home_io_control::RadioDriver &driver() override { return *radio_; }
  MockRadio &mock() { return *radio_; }

  void queue_rx(const esphome::home_io_control::RadioRxPacket &packet) override { radio_->queue_rx(packet); }
  void queue_rx_silence(uint8_t count = 1) override { radio_->queue_rx_silence(count); }
  void queue_rx_timed_after_send(const esphome::home_io_control::RadioRxPacket &packet, int send_index,
                                 uint64_t offset_us) override {
    radio_->queue_rx_timed_after_send(packet, send_index, offset_us);
  }
  void queue_rx_timed_after_bytes(const esphome::home_io_control::RadioRxPacket &packet,
                                  std::vector<uint8_t> anchor_frame, uint64_t offset_us) override {
    radio_->queue_rx_timed_after_bytes(packet, std::move(anchor_frame), offset_us);
  }
  void queue_channel_busy(int16_t level_dbm, uint8_t times = 1) override {
    for (uint8_t i = 0; i < times; i++)
      radio_->queue_rssi(level_dbm);
  }
  void set_model_tx_airtime(bool on) override { radio_->set_model_tx_airtime(on); }

  std::vector<std::vector<uint8_t>> sent_frames() const override { return radio_->get_sent_data(); }
  const std::vector<esphome::home_io_control::RadioTxConfig> &tx_configs() const override {
    return radio_->get_tx_configs();
  }
  const std::vector<uint32_t> &freq_history() const override { return radio_->freq_history(); }
  std::vector<uint32_t> send_times_ms() const override { return radio_->send_times_ms(); }
  const std::vector<uint32_t> &wait_timeouts() const override { return radio_->wait_timeouts(); }

 private:
  std::unique_ptr<MockRadio> radio_;
};
