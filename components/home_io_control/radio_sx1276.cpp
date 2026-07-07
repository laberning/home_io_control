/// @file radio_sx1276.cpp
/// @brief SX1276 radio driver implementation for IO-Homecontrol.
/// @ingroup hioc_radio

// This file is mostly chip-register programming and packed bit masks. Naming every literal that
// mirrors the datasheet hurts readability more than it helps, so the magic-number checks are
// suppressed at file scope here.
// NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)

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
  uint8_t const value = this->spi_->spi_read();
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
  uint8_t const current = this->read_register_(REG_OP_MODE);
  this->write_register_(REG_OP_MODE, (current & ~MODE_MASK) | (mode & MODE_MASK));
  uint32_t const start = millis();
  while (true) {
    uint8_t const cur_mode = this->read_register_(REG_OP_MODE) & MODE_MASK;
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
  uint32_t const start = millis();
  while ((this->read_register_(REG_IMAGE_CAL) & 0x20) != 0) {
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
  this->populate_capture_base_(blocking_wait, this->current_freq_, -(int16_t) rssi / 2, raw, raw_len, frame, frame_len);
  this->last_capture_.rx_done = (irq2 & 0x04) != 0;
  this->last_capture_.irq_flags1 = irq1;
  this->last_capture_.irq_flags2 = irq2;
  // IRQ_FLAGS2 bit 1 = PayloadCrcError. In IoHomeOn mode the chip filters
  // bad-CRC frames in hardware before raising RxDone, so this bit is never
  // set when a packet is delivered. Always false on SX1276.
  this->last_capture_.crc_error = false;
  this->last_capture_.reported_len = raw_len;
}

void RadioSX1276::change_frequency(uint32_t freq_hz) {
  uint64_t const frf = ((uint64_t) freq_hz << 19) / FXOSC;
  this->write_register_(REG_FRF_MSB, (uint8_t) ((frf >> 16) & 0xFF));
  this->write_register_(REG_FRF_MID, (uint8_t) ((frf >> 8) & 0xFF));
  this->write_register_(REG_FRF_LSB, (uint8_t) (frf & 0xFF));
  this->current_freq_ = freq_hz;
}

int16_t RadioSX1276::read_rssi() { return -(int16_t) this->read_register_(REG_RSSI_VALUE) / 2; }

bool RadioSX1276::is_sync_detected() {
  return (this->read_register_(REG_IRQ_FLAGS1) & 0x01) != 0;  // Bit 0 = SyncAddressMatch
}

bool RadioSX1276::is_preamble_detected() {
  return (this->read_register_(REG_IRQ_FLAGS1) & 0x02) != 0;  // Bit 1 = PreambleDetect
}

// === Initialization ===

bool RadioSX1276::init() {
  this->rst_pin_->setup();
  this->dio0_pin_->setup();
  // Attach DIO0 interrupt
  this->dio0_pin_->attach_interrupt(&RadioSX1276::gpio_intr, this, gpio::INTERRUPT_RISING_EDGE);
  if (this->dio4_pin_ != nullptr)
    this->dio4_pin_->setup();

  // Hardware reset
  this->reset_hardware_();

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
  // This routine programs the SX1276 into FSK mode with the IoHomeOn feature enabled.
  // IoHomeOn is critical: it enables hardware CRC (CCITT) and enforces the exact
  // frame structure expected by the IO‑Homecontrol protocol. Without it, the radio
  // would not recognize frames or compute correct CRCs. Every register value below
  // has been validated against working Somfy/Velux captures and Semtech's recommended
  // configuration for IoHomeOn. Do not change unless you have on‑air captures to
  // prove compatibility.
  this->write_register_(REG_OP_MODE, 0x00);  // FSK + Sleep
  delay(10);

  // Frequency: channel 2 (868.95 MHz)
  uint64_t const frf = ((uint64_t) FREQ_CH2 << 19) / FXOSC;
  this->write_register_(REG_FRF_MSB, (uint8_t) ((frf >> 16) & 0xFF));
  this->write_register_(REG_FRF_MID, (uint8_t) ((frf >> 8) & 0xFF));
  this->write_register_(REG_FRF_LSB, (uint8_t) (frf & 0xFF));

  this->set_mode_(MODE_STDBY);
  this->run_image_cal_();
  if (this->failed_)
    return;
  this->set_mode_(MODE_STDBY);

  this->write_register_(REG_OSC, 0x07);  // Clock out off
  this->write_register_(REG_PACKET_CONFIG1,
                        0x90);  // Variable len, CRC on, CCITT
                                // IoHomeOn is the crucial difference from a generic FSK setup: Semtech's SX1276 can
                                // speak the protocol natively enough to handle CRC and frame boundaries for us. This
                                // path serves as the baseline against which SX1262 captures are compared.
  this->write_register_(REG_PACKET_CONFIG2, 0x70);  // Packet mode, IoHomeOn, PowerFrame
  this->write_register_(REG_SYNC_CONFIG, 0x50);     // Auto restart PLL off, AA, sync on
  this->write_register_(REG_DIO_MAPPING1, 0x3D);    // DIO0: PayloadReady/PacketSent, DIO2: SyncAddress (for gated hop)
  this->write_register_(REG_DIO_MAPPING2, 0xF1);    // DIO4: PreambleDetect
  this->write_register_(REG_PLLHOP, this->read_register_(REG_PLLHOP) | 0x80);  // Fast hop
  this->write_register_(REG_PA_RAMP, 0x2D);          // Gaussian BT=1.0 shaping (0x20) | 15μs ramp (0x0D)
  this->write_register_(REG_FIFO_THRESH, 0x80);      // TX start FIFO not empty
  this->write_register_(REG_PAYLOAD_LENGTH, 0xFF);   // Max payload
  this->write_register_(REG_RSSI_CONFIG, 0x02);      // RSSI smoothing 8
  this->write_register_(REG_RX_CONFIG, 0x9E);        // Restart collision, AFC, AGC, preamble
  this->write_register_(REG_AFC_FEI, 0x01);          // AFC auto clear
  this->write_register_(REG_LNA, 0x23);              // Max gain, boost on
  this->write_register_(REG_PREAMBLE_DETECT, 0xAA);  // Detect on, 2 bytes, tol 10
  // RX/AFC bandwidth: sourced from the runtime-tunable rx_bandwidth_ (default BW_41_7_KHZ = 0x13).
  // Carson-rule bandwidth for 38400 bps + 19200 Hz deviation is ~77 kHz; the default is tighter
  // than theoretical but maximizes sensitivity by rejecting out-of-band noise. Validated against
  // real devices. See SX1276RxBandwidth for the selectable options.
  this->set_rx_bandwidth_(this->rx_bandwidth_);

  // Bitrate 38400 bps
  uint32_t const br = FXOSC / 38400;
  this->write_register_(REG_BITRATE_MSB, (br >> 8) & 0xFF);
  this->write_register_(REG_BITRATE_LSB, br & 0xFF);

  // Deviation 19200 Hz
  auto fd = (uint32_t) ((19200.0F / FXOSC) * (1 << 19));
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

  // Sync word: 0x55, 0xFF, 0x33 in registers → on‑air `55 FF 33`. This is the real-Deal confirmed-working sync for
  // IO‑Homecontrol with the SX1276. SyncSize=2 (REG_SYNC_CONFIG bits 2:0 = 0x2) matches the first 2 bytes {0x55, 0xFF}.
  // The 3rd byte 0x33 is transmitted on‑air but not compared by the chip in SyncSize=2 mode.
  uint8_t const sc = this->read_register_(REG_SYNC_CONFIG);
  this->write_register_(REG_SYNC_CONFIG, (sc & 0xF8) | 0x02);  // SyncSize=2: 2 matched bytes `55 FF`; 3rd `33` unused
  this->write_register_(REG_SYNC_VALUE1, 0x55);
  this->write_register_(REG_SYNC_VALUE1 + 1, 0xFF);
  this->write_register_(REG_SYNC_VALUE1 + 2, 0x33);

  this->set_mode_rx();
}

void RadioSX1276::set_rx_bandwidth_(SX1276RxBandwidth bandwidth) {
  this->rx_bandwidth_ = bandwidth;
  // The enum value is the RegRxBw register byte; AFC bandwidth uses the same encoding.
  this->write_register_(REG_RX_BW, static_cast<uint8_t>(bandwidth));
  this->write_register_(REG_AFC_BW, static_cast<uint8_t>(bandwidth));
}

// === Packet TX/RX ===

bool RadioSX1276::send_packet(const uint8_t *data, uint8_t len, const RadioTxConfig &tx_config) {
  if (len == 0 || len > RADIO_PACKET_BUFFER_SIZE)
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
  uint8_t const opmode = this->read_register_(REG_OP_MODE);
  this->write_register_(REG_OP_MODE, (opmode & 0xF8) | MODE_TX);

  uint32_t const start = millis();
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

bool RadioSX1276::poll_until_payload_ready_(uint32_t timeout_ms, bool &saw_dio0, uint8_t &irq1, uint8_t &irq2) {
  uint32_t const start = millis();
  while (true) {
    if (this->is_dio_fired()) {
      saw_dio0 = true;
      break;
    }
    irq1 = this->read_register_(REG_IRQ_FLAGS1);
    irq2 = this->read_register_(REG_IRQ_FLAGS2);
    if ((irq2 & 0x04) != 0)
      break;
    if (millis() - start > timeout_ms)
      return false;
    App.feed_wdt();
    delay(1);
  }
  this->clear_dio_fired();
  if (saw_dio0) {
    irq1 = this->read_register_(REG_IRQ_FLAGS1);
    irq2 = this->read_register_(REG_IRQ_FLAGS2);
  }
  return true;
}

uint8_t RadioSX1276::read_fifo_packet_(uint8_t *buf, uint8_t buf_size) {
  uint8_t len = 0;
  while (((this->read_register_(REG_IRQ_FLAGS2) & 0x40) == 0) && len < buf_size)
    buf[len++] = this->read_register_(REG_FIFO);
  return len;
}

bool RadioSX1276::wait_for_packet(RadioRxPacket &packet, uint32_t timeout_ms) {
  this->prepare_blocking_receive_(packet);
  this->clear_dio_fired();

  bool saw_dio0 = false;
  uint8_t irq1 = 0;
  uint8_t irq2 = 0;
  if (!this->poll_until_payload_ready_(timeout_ms, saw_dio0, irq1, irq2))
    return false;

  uint8_t const rssi = this->read_register_(REG_RSSI_VALUE);
  if ((irq2 & 0x04) == 0) {
    this->fill_capture_info_(true, irq1, irq2, rssi, nullptr, 0, nullptr, 0);
    return false;
  }

  packet.len = this->read_fifo_packet_(packet.data, sizeof(packet.data));
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
  this->prepare_nonblocking_receive_(packet);

  uint8_t const irq1 = this->read_register_(REG_IRQ_FLAGS1);
  uint8_t const irq2 = this->read_register_(REG_IRQ_FLAGS2);
  uint8_t const rssi = this->read_register_(REG_RSSI_VALUE);
  if ((irq2 & 0x04) != 0) {
    packet.len = this->read_fifo_packet_(packet.data, sizeof(packet.data));
    packet.freq_hz = this->current_freq_;
    this->fill_capture_info_(false, irq1, irq2, rssi, packet.data, packet.len, packet.data, packet.len);
#ifdef IOHOME_FRAME_LOG
    if (packet.len > 0)
      log_frame("RX", packet.data, packet.len, this->current_freq_);
#endif
    return packet.len > 0;
  }
  this->fill_capture_info_(false, irq1, irq2, rssi, nullptr, 0, nullptr, 0);
  if ((irq2 & 0x10) != 0) {  // FifoOverrun — clear it
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

// NOLINTEND(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
