/// @file radio_sx1276.cpp
/// @brief SX1276 radio driver implementation for IO-Homecontrol.

#include "radio_sx1276.h"
#include "log_frame.h"
#include "esphome/core/log.h"
#include "esphome/core/application.h"

namespace esphome {
namespace home_io_control {

static const char *const TAG = "home_io_control.sx1276";

// === SPI register access ===

uint8_t RadioSX1276::read_register_(uint8_t reg) {
  this->spi_->spi_enable();
  this->spi_->spi_write(reg & 0x7F);
  uint8_t value = this->spi_->spi_read();
  this->spi_->spi_disable();
  return value;
}

void RadioSX1276::write_register_(uint8_t reg, uint8_t value) {
  this->spi_->spi_enable();
  this->spi_->spi_write(reg | 0x80);
  this->spi_->spi_write(value);
  this->spi_->spi_disable();
}

// === Radio mode control ===

void RadioSX1276::set_mode_(uint8_t mode) {
  uint8_t current = this->read_register_(REG_OP_MODE);
  this->write_register_(REG_OP_MODE, (current & ~MODE_MASK) | (mode & MODE_MASK));
  uint32_t start = millis();
  while (true) {
    uint8_t cur_mode = this->read_register_(REG_OP_MODE) & MODE_MASK;
    if (cur_mode == mode || (mode == MODE_RX && cur_mode == 0x04))
      break;
    if (millis() - start > 50) {
      ESP_LOGE(TAG, "Radio mode change timeout");
      this->failed_ = true;
      return;
    }
  }
}

void RadioSX1276::set_mode_rx() { this->set_mode_(MODE_RX); }
void RadioSX1276::set_mode_standby() { this->set_mode_(MODE_STDBY); }

void RadioSX1276::run_image_cal_() {
  this->set_mode_(MODE_STDBY);
  this->write_register_(REG_IMAGE_CAL, 0x40);
  uint32_t start = millis();
  while (this->read_register_(REG_IMAGE_CAL) & 0x20) {
    if (millis() - start > 20) {
      ESP_LOGE(TAG, "Image calibration timeout");
      this->failed_ = true;
      return;
    }
  }
}

void IRAM_ATTR RadioSX1276::gpio_intr(RadioSX1276 *arg) { arg->mark_dio_fired_from_isr(); }

void RadioSX1276::fill_capture_info_(bool blocking_wait, uint8_t irq1, uint8_t irq2, uint8_t rssi, const uint8_t *raw,
                                     uint8_t raw_len, const uint8_t *frame, uint8_t frame_len) {
  this->last_capture_ = RadioCaptureInfo{};
  this->last_capture_.valid = true;
  this->last_capture_.blocking_wait = blocking_wait;
  this->last_capture_.rx_done = (irq2 & 0x04) != 0;
  this->last_capture_.timestamp_ms = millis();
  this->last_capture_.freq_hz = this->current_freq_;
  this->last_capture_.irq_flags1 = irq1;
  this->last_capture_.irq_flags2 = irq2;
  this->last_capture_.rssi_dbm = -157 + (rssi / 2);
  this->last_capture_.crc_error = (irq2 & 0x02) == 0;
  this->last_capture_.reported_len = raw_len;
  this->last_capture_.raw_len = raw_len;
  this->last_capture_.frame_len = frame_len;
  if (raw != nullptr && raw_len > 0)
    memcpy(this->last_capture_.raw, raw, raw_len);
  if (frame != nullptr && frame_len > 0)
    memcpy(this->last_capture_.frame, frame, frame_len);
}

void RadioSX1276::change_frequency(uint32_t freq_hz) {
  uint64_t frf = ((uint64_t) freq_hz << 19) / FXOSC;
  this->write_register_(REG_FRF_MSB, (uint8_t) ((frf >> 16) & 0xFF));
  this->write_register_(REG_FRF_MID, (uint8_t) ((frf >> 8) & 0xFF));
  this->write_register_(REG_FRF_LSB, (uint8_t) (frf & 0xFF));
  this->current_freq_ = freq_hz;
}

// === Initialization ===

bool RadioSX1276::init() {
  this->rst_pin_->setup();
  this->dio0_pin_->setup();
  this->dio0_pin_->attach_interrupt(&RadioSX1276::gpio_intr, this, gpio::INTERRUPT_RISING_EDGE);
  if (this->dio4_pin_ != nullptr)
    this->dio4_pin_->setup();

  // Hardware reset
  this->rst_pin_->digital_write(false);
  delay(1);
  this->rst_pin_->digital_write(true);
  delay(10);

  // Version check — SX1276 should return 0x12
  if (this->read_register_(REG_VERSION) != 0x12) {
    ESP_LOGE(TAG, "SX1276 not found");
    this->failed_ = true;
    return false;
  }

  this->configure_radio_();
  if (this->failed_)
    return false;

  ESP_LOGI(TAG, "SX1276 initialized");
  return true;
}

void RadioSX1276::configure_radio_() {
  this->write_register_(REG_OP_MODE, 0x00);  // FSK + Sleep
  delay(10);

  // Frequency: channel 2 (868.95 MHz)
  uint64_t frf = ((uint64_t) FREQ_CH2 << 19) / FXOSC;
  this->write_register_(REG_FRF_MSB, (uint8_t) ((frf >> 16) & 0xFF));
  this->write_register_(REG_FRF_MID, (uint8_t) ((frf >> 8) & 0xFF));
  this->write_register_(REG_FRF_LSB, (uint8_t) (frf & 0xFF));

  this->set_mode_(MODE_STDBY);
  this->run_image_cal_();
  if (this->failed_)
    return;
  this->set_mode_(MODE_STDBY);

  this->write_register_(REG_OSC, 0x07);             // Clock out off
  this->write_register_(REG_PACKET_CONFIG1, 0x90);  // Variable len, CRC on, CCITT
  // IoHomeOn is the crucial difference from a generic FSK setup: Semtech's SX1276 can speak the
  // protocol natively enough to handle CRC and frame boundaries for us. This path is therefore the
  // reference implementation used to judge whether SX1262 captures are faithful.
  this->write_register_(REG_PACKET_CONFIG2, 0x70);                             // Packet mode, IoHomeOn, PowerFrame
  this->write_register_(REG_SYNC_CONFIG, 0x50);                                // Auto restart PLL off, AA, sync on
  this->write_register_(REG_DIO_MAPPING1, 0x39);                               // DIO0: PayloadReady/PacketSent
  this->write_register_(REG_DIO_MAPPING2, 0xF1);                               // DIO4: PreambleDetect
  this->write_register_(REG_PLLHOP, this->read_register_(REG_PLLHOP) | 0x80);  // Fast hop
  this->write_register_(REG_PA_RAMP, 0x0E);                                    // No shaping, 12us
  this->write_register_(REG_FIFO_THRESH, 0x80);                                // TX start FIFO not empty
  this->write_register_(REG_PAYLOAD_LENGTH, 0xFF);                             // Max payload
  this->write_register_(REG_RSSI_CONFIG, 0x02);                                // RSSI smoothing 8
  this->write_register_(REG_RX_CONFIG, 0x9E);                                  // Restart collision, AFC, AGC, preamble
  this->write_register_(REG_AFC_FEI, 0x01);                                    // AFC auto clear
  this->write_register_(REG_LNA, 0x23);                                        // Max gain, boost on
  this->write_register_(REG_PREAMBLE_DETECT, 0xAA);                            // Detect on, 2 bytes, tol 10
  this->write_register_(REG_RX_BW, 0x01);                                      // 250 kHz

  // Bitrate 38400 bps
  uint32_t br = FXOSC / 38400;
  this->write_register_(REG_BITRATE_MSB, (br >> 8) & 0xFF);
  this->write_register_(REG_BITRATE_LSB, br & 0xFF);

  // Deviation 19200 Hz
  auto fd = (uint32_t) ((19200.0f / FXOSC) * (1 << 19));
  this->write_register_(REG_FDEV_MSB, (fd >> 8) & 0xFF);
  this->write_register_(REG_FDEV_LSB, fd & 0xFF);

  // PA config
  if (this->pa_pin_ == 0x80) {
    uint8_t p = std::max(this->tx_power_, (uint8_t) 2);
    p = std::min(p, (uint8_t) 17);
    this->write_register_(REG_PA_CONFIG, 0x80 | (p - 2));
  } else {
    this->write_register_(REG_PA_CONFIG, std::min(this->tx_power_, (uint8_t) 14));
  }
  this->write_register_(0x0B, 0x3B);  // OCP on, 240mA

  // Preamble 1024 bytes default (changed per-packet)
  this->write_register_(REG_PREAMBLE_MSB, 0x04);
  this->write_register_(REG_PREAMBLE_LSB, 0x00);

  // Sync word: 0x55, 0xFF, 0x33 (SyncSize=2 with IoHomeOn = 2 bytes used)
  uint8_t sc = this->read_register_(REG_SYNC_CONFIG);
  this->write_register_(REG_SYNC_CONFIG, (sc & 0xF8) | 0x02);
  this->write_register_(REG_SYNC_VALUE1, 0x55);
  this->write_register_(REG_SYNC_VALUE1 + 1, 0xFF);
  this->write_register_(REG_SYNC_VALUE1 + 2, 0x33);

  this->set_mode_rx();
}

// === Packet TX/RX ===

bool RadioSX1276::send_packet(const uint8_t *data, uint8_t len, const RadioTxConfig &tx_config) {
  if (len == 0 || len > 64)
    return false;
#ifdef IOHOME_FRAME_LOG
  log_frame("TX", data, len, tx_config.freq_hz, tx_config.preamble_len);
#endif
  this->set_mode_standby();
  this->write_register_(REG_PREAMBLE_MSB, tx_config.preamble_len >> 8);
  this->write_register_(REG_PREAMBLE_LSB, tx_config.preamble_len & 0xFF);
  this->change_frequency(tx_config.freq_hz);

  // Write data to FIFO
  this->spi_->spi_enable();
  this->spi_->spi_write(REG_FIFO | 0x80);
  for (uint8_t i = 0; i < len; i++)
    this->spi_->spi_transfer(data[i]);
  this->spi_->spi_disable();

  this->clear_dio_fired();
  uint8_t opmode = this->read_register_(REG_OP_MODE);
  this->write_register_(REG_OP_MODE, (opmode & 0xF8) | MODE_TX);

  uint32_t start = millis();
  while (!this->is_dio_fired()) {
    if (millis() - start > 4000) {
      ESP_LOGE(TAG, "TX timeout");
      this->set_mode_standby();
      return false;
    }
    App.feed_wdt();
    delayMicroseconds(100);
  }

  // Reset preamble to short for RX
  this->write_register_(REG_PREAMBLE_MSB, 0x00);
  this->write_register_(REG_PREAMBLE_LSB, 0x08);
  this->set_mode_rx();
  return true;
}

bool RadioSX1276::wait_for_packet(RadioRxPacket &packet, uint32_t timeout_ms) {
  this->clear_last_capture_();
  packet = RadioRxPacket{};
  this->clear_dio_fired();
  uint32_t start = millis();
  bool saw_dio0 = false;
  uint8_t irq1 = 0;
  uint8_t irq2 = 0;
  while (true) {
    if (this->is_dio_fired()) {
      saw_dio0 = true;
      break;
    }

    irq1 = this->read_register_(REG_IRQ_FLAGS1);
    irq2 = this->read_register_(REG_IRQ_FLAGS2);
    if (irq2 & 0x04)
      break;

    if (millis() - start > timeout_ms) {
      return false;
    }
    App.feed_wdt();
    delay(1);
  }
  this->clear_dio_fired();
  if (saw_dio0) {
    irq1 = this->read_register_(REG_IRQ_FLAGS1);
    irq2 = this->read_register_(REG_IRQ_FLAGS2);
  }
  uint8_t rssi = this->read_register_(REG_RSSI_VALUE);
  if (!(irq2 & 0x04)) {
    this->fill_capture_info_(true, irq1, irq2, rssi, nullptr, 0, nullptr, 0);
    return false;
  }
  // In IoHomeOn mode the FIFO already contains protocol bytes, so RX is just a straight FIFO read.
  while (!(this->read_register_(REG_IRQ_FLAGS2) & 0x40) && packet.len < sizeof(packet.data))  // FifoEmpty bit
    packet.data[packet.len++] = this->read_register_(REG_FIFO);
  packet.freq_hz = this->current_freq_;
  this->fill_capture_info_(true, irq1, irq2, rssi, packet.data, packet.len, packet.data, packet.len);
#ifdef IOHOME_FRAME_LOG
  if (packet.len > 0)
    log_frame("RX", packet.data, packet.len, this->current_freq_);
#endif
  return packet.len > 0;
}

bool RadioSX1276::check_for_packet(RadioRxPacket &packet) {
  if (!this->is_dio_fired())
    return false;
  this->clear_last_capture_();
  packet = RadioRxPacket{};
  this->clear_dio_fired();
  uint8_t irq1 = this->read_register_(REG_IRQ_FLAGS1);
  uint8_t irq2 = this->read_register_(REG_IRQ_FLAGS2);
  uint8_t rssi = this->read_register_(REG_RSSI_VALUE);
  if (irq2 & 0x04) {                                                                            // PayloadReady
    while (!(this->read_register_(REG_IRQ_FLAGS2) & 0x40) && packet.len < sizeof(packet.data))  // FifoEmpty
      packet.data[packet.len++] = this->read_register_(REG_FIFO);
    packet.freq_hz = this->current_freq_;
    this->fill_capture_info_(false, irq1, irq2, rssi, packet.data, packet.len, packet.data, packet.len);
#ifdef IOHOME_FRAME_LOG
    if (packet.len > 0)
      log_frame("RX", packet.data, packet.len, this->current_freq_);
#endif
    return packet.len > 0;
  }
  this->fill_capture_info_(false, irq1, irq2, rssi, nullptr, 0, nullptr, 0);
  if (irq2 & 0x10) {  // FifoOverrun — clear it
    this->write_register_(REG_IRQ_FLAGS2, 0x10);
  }
  return false;
}

void RadioSX1276::dump_debug() {
  ESP_LOGCONFIG(TAG, "  SX1276 Diagnostic:");
  ESP_LOGCONFIG(TAG, "    Opmode=0x%02X irq1=0x%02X irq2=0x%02X freq=%u", this->read_register_(REG_OP_MODE),
                this->read_register_(REG_IRQ_FLAGS1), this->read_register_(REG_IRQ_FLAGS2), this->current_freq_);
}

}  // namespace home_io_control
}  // namespace esphome
