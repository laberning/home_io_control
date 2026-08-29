/// @file radio_soft_phy_driver_base.cpp
/// @brief Shared driver flow implementation for the software-PHY radios.
/// @ingroup hioc_radio
///
/// See radio_soft_phy_driver_base.h for the architectural context. This file holds the shared
/// RX/TX orchestration for both software-PHY drivers, parameterized over the small set of virtual
/// primitives/hooks the two chips genuinely differ on.

// Opcode payloads and recovery thresholds are written in the same shape as the chip protocol
// and on-air framing they reproduce.
// NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)

#include "radio_soft_phy_driver_base.h"
#include "log_frame.h"
#include "esphome/core/log.h"
#include "esphome/core/application.h"

#include <cinttypes>

namespace esphome {
namespace home_io_control {

static const char *const TAG = "home_io_control.soft_phy";

namespace {

/// Block until `raw_bytes` (plus @ref SOFT_PHY_EARLY_READ_MARGIN_BYTES) have had time to arrive
/// since the sync word was observed at `sync_us`. Pure wall-clock waiting against the protocol's
/// line rate — it reads no chip state, so it lives here rather than on the driver.
/// @return false if the caller's `timeout_ms` window closed first.
bool wait_for_air_time(uint32_t sync_us, uint8_t raw_bytes, uint32_t start_ms, uint32_t timeout_ms) {
  uint32_t const needed_us = soft_phy_air_time_us((uint32_t) raw_bytes + SOFT_PHY_EARLY_READ_MARGIN_BYTES);
  while (micros() - sync_us < needed_us) {
    // Never outstay the window the caller asked for: a receive that has run out of time falls back
    // to the RX_DONE path (which will time out on its own terms) rather than silently overrunning.
    if (millis() - start_ms > timeout_ms)
      return false;
    App.feed_wdt();
    delayMicroseconds(SOFT_PHY_EARLY_POLL_US);
  }
  return true;
}

}  // namespace

// === SPI transport helper shared by both chips ===

void SoftPhyDriverBase::wait_busy_() {
  // Once a BUSY timeout has failed the driver, every later call would otherwise re-run the same
  // timeout again — init()'s remaining configure_radio_() steps would each block for
  // busy_timeout_ms_ before init() finally returns false. Short-circuit instead: the chip is
  // already known-unresponsive, so there's nothing to wait for.
  if (this->failed_)
    return;
  uint32_t const start = millis();
  while (this->busy_pin_->digital_read()) {
    if (millis() - start > this->busy_timeout_ms_) {
      ESP_LOGE(TAG, "BUSY timeout");
      this->failed_ = true;
      return;
    }
    App.feed_wdt();
  }
}

// === Packet RX (blocking) ===

bool SoftPhyDriverBase::wait_for_packet(RadioRxPacket &packet, uint32_t timeout_ms) {
  // Blocking receive with timeout. This orchestrator decomposes the state machine into three
  // low-complexity helpers, shared verbatim between SX1262 and LR1121.
  this->prepare_blocking_receive_(packet);

  uint32_t const start = millis();
  uint32_t irq = 0;

  // Phase 1: wait for first *terminal* activity (see activity_irq_mask()'s doc comment).
  if (!this->poll_until_activity_(start, timeout_ms, irq)) {
    return false;
  }

  // Phase 2: resolve the SYNC_WORD_VALID → RX_DONE race condition, and take the length-driven
  // shortcut when the chip supports it.
  bool early_completed = false;
  if (!this->resolve_sync_race_(start, timeout_ms, irq, packet, early_completed)) {
    return false;
  }
  if (early_completed) {
    return true;
  }

  // Phase 3: finalize — either read the packet or treat as a failure.
  return this->finalize_receive_(packet, irq);
}

/// Poll for first terminal radio activity (IRQ pin or an IRQ status bit within
/// activity_irq_mask()) within timeout. `irq` is always freshly read before this returns, so
/// callers never need a separate refresh step. A reading outside activity_irq_mask() (e.g.
/// LR1121's preamble-only case) keeps the loop going rather than returning — the loop re-reads
/// chip status via SPI every iteration regardless of the DIO edge, so nothing is lost.
bool SoftPhyDriverBase::poll_until_activity_(uint32_t start, uint32_t timeout_ms, uint32_t &irq) {
  while (true) {
    if (this->is_dio_fired())
      this->clear_dio_fired();
    irq = this->read_irq_status_raw();
    if ((irq & this->activity_irq_mask()) != 0) {
      // The flag must never outlive the poll that set it, so every exit path resets it here too.
      this->preamble_latched_at_timeout_ = false;
      return true;
    }
    if (millis() - start > timeout_ms) {
      // Snapshot before the reset below wipes it: reset_rx_state_() clears the whole IRQ word, so
      // this is the last point PreambleDetected can be observed for this dwell (see
      // preamble_latched_at_timeout_'s doc comment for why the bit can't just be re-read afterward).
      this->preamble_latched_at_timeout_ = (irq & this->preamble_detected_bit()) != 0;
      this->clear_dio_fired();
      this->reset_rx_state_();
      return false;
    }
    App.feed_wdt();
    delay(1);
  }
}

/// Resolve the SYNC_WORD_VALID → RX_DONE race condition common to both chips: the sync-word IRQ
/// can assert before the packet is fully received. If we observe SYNC without RX_DONE, clear the
/// sticky SYNC flag and spin until RX_DONE arrives or the remaining timeout elapses.
bool SoftPhyDriverBase::resolve_sync_race_(uint32_t start, uint32_t timeout_ms, uint32_t &irq, RadioRxPacket &packet,
                                           bool &early_completed) {
  early_completed = false;
  // If RX_DONE already set or SYNC not set, nothing to resolve.
  if ((irq & this->sync_word_valid_bit()) == 0 || (irq & this->rx_done_bit()) != 0) {
    return true;
  }
  // SYNC is the frame's own start marker, so from here everything about the frame's arrival is a
  // question of air time. Timestamp it before any SPI work so the length-driven receive below
  // measures from as close to the on-air event as this loop can see.
  uint32_t const sync_us = micros();
  // SYNC seen without RX_DONE — clear sticky SYNC and wait for RX_DONE.
  this->clear_irq_status(this->sync_word_valid_bit());

  if (this->try_early_completion_(packet, sync_us, irq, start, timeout_ms)) {
    early_completed = true;
    return true;
  }
  while (millis() - start <= timeout_ms) {
    if (!this->is_dio_fired()) {
      irq = this->read_irq_status_raw();
      if ((irq & this->rx_done_bit()) != 0)
        return true;
      App.feed_wdt();
      delay(1);
      continue;
    }
    this->clear_dio_fired();
    irq = this->read_irq_status_raw();
    if ((irq & this->rx_done_bit()) != 0)
      return true;
    if (irq != 0)
      this->clear_irq_status(irq);
  }
  return false;  // timeout
}

/// Finish a reception on the frame's own air time instead of the chip's fixed-length RX_DONE.
///
/// Three stages, each of which can bail out harmlessly:
///   1. wait out the first UART cell and read it — CTRL0 carries the whole frame's length;
///   2. wait out exactly that frame's air time and read exactly its bytes;
///   3. run the normal UART probe and accept only a CRC-valid frame.
///
/// Stage 3 is the safety property that makes this worth doing at all. CRC-CCITT is a 1-in-65536
/// gate, so a frame accepted here is a frame that would have been accepted at RX_DONE anyway —
/// and *any* failure (a chip that turns out not to expose its buffer mid-reception, a spurious
/// sync detect, a mis-guessed length, a bit error) simply returns false and leaves the caller on
/// the original RX_DONE path, which re-reads the full buffer from scratch. A wrong guess here
/// costs some latency; it can never cost a frame.
bool SoftPhyDriverBase::try_early_completion_(RadioRxPacket &packet, uint32_t sync_us, uint32_t irq_status,
                                              uint32_t start_ms, uint32_t timeout_ms) {
  int16_t const base = this->early_rx_read_offset();
  if (base < 0)
    return false;  // chip does not expose its data buffer mid-reception
  if (timeout_ms < SOFT_PHY_EARLY_MIN_WINDOW_MS)
    return false;  // no room left in this window to receive a whole frame either way
  auto const offset = (uint8_t) base;

  // Stage 1: ten bits of air time is all it takes to learn how long the frame will be.
  if (!wait_for_air_time(sync_us, SOFT_PHY_EARLY_HEADER_RAW_BYTES, start_ms, timeout_ms))
    return false;
  uint8_t header[SOFT_PHY_EARLY_HEADER_RAW_BYTES] = {0};
  this->read_rx_buffer(offset, header, sizeof(header));
  uint8_t const frame_len = soft_phy_peek_frame_length(header, sizeof(header));
  if (frame_len == 0)
    return false;

  // Stage 2: wait out only what this frame needs, then take the whole thing. The margin bytes
  // have been waited for either way, so read them too — they give the probe slack to work with
  // when the stream is not byte-aligned.
  uint8_t const frame_raw_len = soft_phy_raw_bytes_for_frame(frame_len);
  if (!wait_for_air_time(sync_us, frame_raw_len, start_ms, timeout_ms))
    return false;
  uint8_t raw[RADIO_PACKET_BUFFER_SIZE] = {0};
  auto const read_len =
      (uint8_t) std::min<uint16_t>((uint16_t) frame_raw_len + SOFT_PHY_EARLY_READ_MARGIN_BYTES, sizeof(raw));
  this->read_rx_buffer(offset, raw, read_len);

  // Stage 3: CRC decides.
  UartProbeResult const probe = find_uart_probe(raw, read_len);
  if (!probe.valid)
    return false;

  memcpy(packet.data, probe.decoded + probe.frame_start, probe.frame_len);
  packet.len = probe.frame_len;
  packet.freq_hz = this->current_freq_;
  // Reading packet status this early is still sound: the RSSI a driver reports from it is latched
  // at sync-word detection, which by definition has already happened.
  this->fill_capture_info(true, irq_status, offset, read_len, raw, read_len, packet.data, packet.len);

#ifdef IOHOME_FRAME_LOG
  ESP_LOGD(TAG, "Early RX: frame_len=%u raw_len=%u air_us=%" PRIu32, frame_len, read_len,
           (uint32_t) (micros() - sync_us));
  log_frame("RX", packet.data, packet.len, this->current_freq_);
#endif

  // The reception this latch refers to is being torn down deliberately, so drop it rather than
  // let a stale edge look like a fresh packet to the next wait.
  this->clear_dio_fired();
  this->reset_rx_state_();
  return true;
}

/// Finalize receive: read the packet if RX_DONE is set, otherwise record failure.
bool SoftPhyDriverBase::finalize_receive_(RadioRxPacket &packet, uint32_t irq) {
  if ((irq & this->rx_done_bit()) == 0) {
    this->fill_capture_info(true, irq, 0, 0, nullptr, 0, nullptr, 0);
    this->reset_rx_state_();
    return false;
  }
  return this->read_rx_packet(packet, true, irq);
}

void SoftPhyDriverBase::rearm_rx_after_tx_() {
  // The minimum needed to be listening again, because the peer can answer within a millisecond or
  // two of our carrier dropping. Deliberately *not* reset_rx_state_(): that also issues SetStandby
  // and re-writes the buffer base address, and on this path both are dead weight on the one code
  // path where microseconds decide whether a fast reply is heard at all --
  //   - both drivers program SetRxTxFallbackMode = STDBY_XOSC at init, so the chip is already in
  //     standby the instant TxDone fires;
  //   - the buffer base is written at init and nothing since has moved it.
  // What genuinely must happen: clear the latched TxDone (it shares the IRQ word with RX events),
  // restore the RX packet params that this transmission overwrote, and re-enter RX. Also drop the
  // hop holdoff (issue #81): whatever it was tracking belonged to the reception that this TX just
  // destroyed by transmitting over it, and RX is genuinely re-arming here same as it does through
  // reset_rx_state_() — unlike that call's SetStandby/buffer-base work, clearing the flag costs
  // nothing, so there is no reason to leave it stale on this path.
  //
  // invalidate_stale_rx_content_after_tx() is the same shape: no-op on SX1262 (its buffer split
  // already keeps TX content away from where a length-driven receive reads), so this path pays
  // nothing extra there; only LR1121, whose shared buffer needs it, does real work here.
  this->clear_reception_in_progress_();
  this->clear_irq_status(0xFFFFFFFF);
  this->invalidate_stale_rx_content_after_tx();
  this->set_rx_packet_params();
  this->set_mode_rx();
}

void SoftPhyDriverBase::build_gfsk_packet_params(const SoftPhyPacketParams &p, uint8_t out[GFSK_PACKET_PARAMS_LEN]) {
  // The SetPacketParams preamble field is bit-denominated on both chips (sx126x's
  // preamble_len_in_bits / lr11xx's pbl_len_in_bit), but preamble_bytes arrives in bytes, matching
  // every other layer in this codebase (LONG_PREAMBLE, SHORT_PREAMBLE, the tuning defaults, the
  // SX1276's byte-wide RegPreambleMsb/Lsb). Convert here, once, at the last step before the wire —
  // this is the conversion the 63e2502 fix had to patch separately in each driver.
  const uint16_t preamble_bits = p.preamble_bytes * 8;
  out[0] = static_cast<uint8_t>(preamble_bits >> 8);  // Preamble length MSB
  out[1] = static_cast<uint8_t>(preamble_bits);       // Preamble length LSB
  out[2] = p.preamble_detector;                       // Preamble detector length (chip constant)
  out[3] = p.sync_word_param;                         // Sync word length: 24 bits (chip constant)
  out[4] = 0x00;                                      // Address comparison: off
  out[5] = p.packet_type;                             // GFSK packet type: known/fixed length
  out[6] = p.payload_len;                             // Configured payload length
  out[7] = p.crc_type;                                // CRC mode (chip constant)
  out[8] = 0x00;                                      // Whitening: off
}

void SoftPhyDriverBase::reset_rx_state_(bool force_standby) {
  // Whatever was arriving is over — delivered, timed out, or deliberately discarded. Drop the hop
  // holdoff with it, rather than leaving the next ~12 ms of hopping waiting on a deadline that no
  // longer refers to anything (issue #81). This funnel covers every "RX torn down and re-armed"
  // path except rearm_rx_after_tx_(), which clears the same flag itself for the same reason (see
  // its own comment for why it can't just call this function): poll_until_activity_()'s timeout,
  // try_early_completion_()'s success, read_rx_packet()'s end, finalize_receive_()'s failure, and
  // check_for_packet()'s catch-all.
  this->clear_reception_in_progress_();
  if (force_standby)
    this->set_mode_standby();
  this->clear_irq_status(0xFFFFFFFF);
  this->configure_buffer_base();
  this->set_rx_packet_params();
  this->set_mode_rx();
}

bool SoftPhyDriverBase::read_rx_packet(RadioRxPacket &packet, bool blocking_wait, uint32_t irq_status) {
  uint8_t raw_reported_len = 0;
  uint8_t rx_offset = 0;
  this->get_rx_buffer_status(raw_reported_len, rx_offset);

  uint8_t rx_buf[RADIO_PACKET_BUFFER_SIZE] = {0};
  uint8_t recovered_buf[RADIO_PACKET_BUFFER_SIZE] = {0};
  uint8_t const reported_len = std::min(raw_reported_len, (uint8_t) sizeof(rx_buf));
  uint8_t raw_probe_len = reported_len;
  if (reported_len > 0 && reported_len < 32) {
    // When the chip reports a short packet length, still pull the full raw window: the useful
    // UART-packed tail (e.g. a post-auth response) can sit past the chip-reported boundary, so
    // trimming the probe to that boundary would lose it.
    raw_probe_len = sizeof(rx_buf);
  }
  if (reported_len == SOFT_PHY_RX_PROBE_PACKET_LEN)
    raw_probe_len = SOFT_PHY_RX_PROBE_PACKET_LEN;
  if (raw_probe_len > 0)
    this->read_rx_buffer(rx_offset, rx_buf, raw_probe_len);

  // Neither chip exposes the already-decoded IO-homecontrol frame the way SX1276 does. We first
  // capture the raw bytes exactly as reported by the chip, then recover the UART-packed protocol
  // stream in software and only pass a plausible frame up to the parser. This software recovery
  // path is the soft-PHY-specific adaptation to the same protocol.
  UartProbeResult probe = find_uart_probe(rx_buf, raw_probe_len);
  if (probe.valid) {
    memcpy(recovered_buf, probe.decoded + probe.frame_start, probe.frame_len);
    memcpy(packet.data, recovered_buf, probe.frame_len);
    packet.len = probe.frame_len;
#ifdef IOHOME_FRAME_LOG
    // rx_offset is the chip-reported buffer offset this reception was read from — logged here
    // (issue #81) because it is otherwise populated by every driver's fill_capture_info() and read
    // by nothing, and it is the one fact that would confirm or correct LR1121_RX_BUFFER_BASE
    // against a real LR1121 (see RadioLR1121::early_rx_read_offset's doc comment).
    ESP_LOGD(TAG, "UART probe: valid=1 bit_offset=%u frame_start=%u frame_len=%u decoded_len=%u rx_offset=%u",
             probe.bit_offset, probe.frame_start, probe.frame_len, probe.decoded_len, rx_offset);
#endif
  } else {
#ifdef IOHOME_FRAME_LOG
    // Log diagnostic info when CRC validation rejects all decode attempts — helps identify
    // whether post-TX RX corruption is being correctly caught by CRC or slipping through.
    char hex_buf[97] = {0};  // 32 bytes * 3 chars + null
    uint8_t dump_len = std::min(raw_probe_len, (uint8_t) 32);
    for (uint8_t i = 0; i < dump_len; i++)
      snprintf(hex_buf + (i * 3), 4, "%02X ", rx_buf[i]);
    ESP_LOGW(TAG, "UART probe: valid=0 decoded_len=%u raw_probe_len=%u", probe.decoded_len, raw_probe_len);
    ESP_LOGW(TAG, "  raw[0..%u]: %s", dump_len - 1, hex_buf);
    // Try to show why CRC failed at best offset
    if (probe.decoded_len >= FRAME_MIN_SIZE) {
      // FRAME_MAX_WIRE_SIZE, not FRAME_MAX_SIZE: this is the diagnostic that explains *why* a CRC
      // check failed, so it must be able to reach a MAC-bearing 1W frame's longer non-CRC length
      // (see IoFrame::has_mac) too — capped at the declared-only bound, this log would report a
      // spurious mismatch for a frame that is actually fine, exactly during the bring-up it exists
      // to help with.
      int best_len = std::min<int>(probe.decoded_len, FRAME_MAX_WIRE_SIZE);
      IoFrame test_frame;
      for (int cl = best_len; cl >= FRAME_MIN_SIZE; cl--) {
        if (!parse(probe.decoded, cl, test_frame))
          continue;
        if (cl + 2 <= (int) probe.decoded_len) {
          uint16_t computed = crc_ccitt(probe.decoded, cl);
          uint16_t received = (uint16_t) probe.decoded[cl] | ((uint16_t) probe.decoded[cl + 1] << 8);
          ESP_LOGW(TAG, "  CRC check: candidate_len=%d cmd=0x%02X crc_computed=0x%04X crc_received=0x%04X %s", cl,
                   test_frame.cmd, computed, received, computed == received ? "MATCH" : "MISMATCH");
        }
        break;
      }
    }
#endif
    // FRAME_MAX_WIRE_SIZE, not FRAME_MAX_SIZE: this fallback runs when no CRC-valid frame was
    // found, so what's being copied is raw chip-reported bytes on a best-effort basis, not a
    // frame known to lack a MAC trailer — capping to the declared-only bound would silently
    // truncate a genuine MAC-bearing frame's tail before it ever reached find_uart_probe again.
    uint8_t const copy_len = std::min(reported_len, FRAME_MAX_WIRE_SIZE);
    if (copy_len > 0)
      memcpy(packet.data, rx_buf, copy_len);
    packet.len = copy_len;
  }
  packet.freq_hz = this->current_freq_;
  this->fill_capture_info(blocking_wait, irq_status, rx_offset, reported_len, rx_buf, raw_probe_len, packet.data,
                          packet.len);

#ifdef IOHOME_FRAME_LOG
  if (packet.len > 0)
    log_frame("RX", packet.data, packet.len, this->current_freq_);
#endif
  this->reset_rx_state_();
  return packet.len > 0;
}

// === Packet RX (non-blocking) ===

bool SoftPhyDriverBase::check_for_packet(RadioRxPacket &packet) {
  if (!this->is_dio_fired())
    return false;
  this->prepare_nonblocking_receive_(packet);

  uint32_t const irq = this->read_irq_status_raw();

  if ((irq & this->activity_irq_mask()) == 0) {
    // A reading outside activity_irq_mask() (e.g. preamble-only on a chip that routes it to the
    // IRQ pin) means a frame may still be arriving. Clear just that bit instead of calling
    // reset_rx_state_() below, which would tear down RX mid-reception. Unlike
    // poll_until_activity_(), this function has no internal retry loop, so leaving the chip-level
    // bit set would starve later loop() ticks of the DIO edge they need to notice the real
    // RX_DONE. On chips whose activity_irq_mask() is "any bit" (the default), this branch can
    // never trigger — that hardware has already filtered such readings out before they get here.
    if (irq != 0)
      this->clear_irq_status(irq);
    return false;
  }

  if ((irq & this->sync_word_valid_bit()) != 0 && (irq & this->rx_done_bit()) == 0) {
    // A frame is genuinely arriving. Record it so maybe_hop() (which runs a few lines up the
    // caller's stack in loop()) holds the idle-path hop off instead of retuning under it —
    // change_frequency() clears the whole IRQ word and the DIO latch, so a frame that loses that
    // race is destroyed outright, not merely delayed (issue #81). A fresh is_sync_detected() read
    // from maybe_hop() would not work here: the clear_irq_status() call right below has already
    // dropped the sync bit by the time maybe_hop() could look at it, so the observation has to be
    // captured now, before it disappears. The holdoff expires on its own; see RX_HOP_HOLDOFF_US.
    //
    // Past the holdoff arm above, finish the reception here instead of leaving it to the RX_DONE
    // ~10 ms away and a loop() pass after that (issue #81, Mechanism B). sync_us is timestamped now,
    // not when the sync word actually landed on air, so it can be a whole loop period stale — that
    // only ever makes wait_for_air_time() (inside try_early_completion_()) wait longer than the
    // frame needed, never shorter, so reading the buffer late is harmless while reading it early
    // would not be. Timestamped before the SPI clear below for the same reason resolve_sync_race_()
    // does it: keep the reference as close to the on-air event as this loop can see.
    uint32_t const sync_us = micros();
    uint32_t const start_ms = millis();
    this->note_reception_in_progress_();
    this->clear_irq_status(this->sync_word_valid_bit());
    // Falling through here is not a lost frame: try_early_completion_()'s failure paths issue only
    // reads; nothing re-arms, retunes, or clears an IRQ, so the caller is free to fall back to the
    // ordinary RX_DONE path. try_early_completion_() can itself block for up to
    // idle_rx_completion_budget_ms() (~9.4 ms worst case), which eats into the holdoff armed above
    // before this function even returns — on the fall-through path, re-arm it fresh right here so
    // maybe_hop() (a few lines up the caller's stack in loop()) still sees the full holdoff window
    // instead of whatever fraction survived this call, and can't retune under a frame whose RX_DONE
    // hasn't been read yet. The success path does not need this: try_early_completion_() already
    // clears the holdoff itself via reset_rx_state_() once the frame is fully recovered.
    bool const completed =
        this->try_early_completion_(packet, sync_us, irq, start_ms, this->idle_rx_completion_budget_ms());
    if (!completed)
      this->note_reception_in_progress_();
    return completed;
  }

  if ((irq & this->rx_done_bit()) != 0) {
    return this->read_rx_packet(packet, false, irq);
  }

  this->fill_capture_info(false, irq, 0, 0, nullptr, 0, nullptr, 0);
  this->reset_rx_state_();
  return false;
}

// === Packet TX ===

bool SoftPhyDriverBase::send_packet(const uint8_t *data, uint8_t len, const RadioTxConfig &tx_config) {
  if (len == 0)
    return false;

#ifdef IOHOME_FRAME_LOG
  log_frame("TX", data, len, tx_config.freq_hz, tx_config.preamble_len);
#endif

  this->set_mode_standby();
  this->set_frequency_register(tx_config.freq_hz);

  // FRAME_MAX_WIRE_SIZE already includes the CRC bytes (declared + trailer + CRC), so this holds
  // `len` (which may itself include a serialize()-emitted MAC trailer, see IoFrame::has_mac) plus
  // its CRC without truncating a frame this driver is asked to transmit.
  uint8_t frame_with_crc[FRAME_MAX_WIRE_SIZE] = {0};
  uint8_t tx_buf[RADIO_PACKET_BUFFER_SIZE];
  if ((uint16_t) len + 2 > (uint16_t) sizeof(frame_with_crc))
    return false;

  memcpy(frame_with_crc, data, len);
  const uint16_t crc = crc_ccitt(data, len);
  frame_with_crc[len] = crc & 0xFF;
  frame_with_crc[len + 1] = (crc >> 8) & 0xFF;

  const uint8_t encoded_len = uart_encode_packet(frame_with_crc, len + 2, tx_buf, sizeof(tx_buf));
  if (encoded_len == 0)
    return false;

  this->set_tx_packet_params(tx_config.preamble_len, encoded_len);

  this->clear_irq_status(0xFFFFFFFF);
  this->write_tx_buffer(tx_buf, encoded_len);

  // Chip-specific pre-TX workaround hook (no-op on chips that don't need one).
  this->before_tx_arm();

  this->clear_dio_fired();
  this->start_tx();

  // Wait for an actual TxDone IRQ. The IRQ pin is shared with RX-related events, so a stale or
  // unrelated interrupt must not be treated as TX completion.
  uint32_t const start = millis();
  uint32_t tx_irq = 0;
  while (true) {
    if (!this->is_dio_fired()) {
      if (millis() - start > 4000) {
        ESP_LOGE(TAG, "TX timeout — DIO/IRQ pin never fired");
        this->set_mode_standby();
        return false;
      }
      App.feed_wdt();
      delayMicroseconds(100);
      continue;
    }

    this->clear_dio_fired();
    tx_irq = this->read_irq_status_raw();
    if ((tx_irq & this->tx_done_bit()) != 0)
      break;

    if (tx_irq != 0) {
      this->clear_irq_status(tx_irq);
    }

    if (millis() - start > 4000) {
      ESP_LOGE(TAG, "TX timeout — no TX_DONE IRQ (last_irq=0x%08" PRIX32 ")", tx_irq);
      this->set_mode_standby();
      return false;
    }
  }
  // TxDone used the same DIO/IRQ latch as RX. Clear the local latch before re-arming RX so an
  // immediate reply remains visible to wait_for_packet().
  this->clear_dio_fired();

#ifdef IOHOME_FRAME_LOG
  uint32_t const tx_done_us = micros();
#endif
  this->rearm_rx_after_tx_();

  // Post-TX settling delay: the GFSK demodulator needs time to stabilize after the TX→STDBY→RX
  // transition. Without this, frames received immediately after TX (e.g. the 0x3C challenge
  // during pairing) can suffer UART decode bit errors before the demodulator's frequency
  // discrimination has settled. Runtime-tunable per chip. The radio is already armed by this
  // point, so a frame arriving during the delay is still captured in hardware.
  delayMicroseconds(this->post_tx_settle_us_);

  // A peer can reply within a millisecond or two of our carrier dropping, so re-arm time is the
  // margin the whole exchange lives on: if it outlasts the peer's turnaround, the reply is not
  // late, it is never heard at all, and no response-window length can recover it. Behind the
  // frame-log flag with the rest of the PHY-level instrumentation because it fires on *every*
  // transmission; the timing calls compile out with it.
#ifdef IOHOME_FRAME_LOG
  ESP_LOGD(TAG, "TX->RX re-arm: %" PRIu32 " us (+%u us settle)", micros() - tx_done_us, this->post_tx_settle_us_);
#endif

  return true;
}

// === Frequency control ===

void SoftPhyDriverBase::change_frequency(uint32_t freq_hz) {
  this->set_mode_standby();
  this->set_frequency_register(freq_hz);
  this->clear_irq_status(0xFFFFFFFF);  // Clear stale preamble/sync bits from previous channel
  this->clear_dio_fired();             // Clear stale IRQ latch from previous channel activity
  this->set_mode_rx();
}

// === RSSI / sync / preamble ===

int16_t SoftPhyDriverBase::read_rssi() { return -(int16_t) this->read_rssi_raw_byte() / 2; }

bool SoftPhyDriverBase::is_sync_detected() { return (this->read_irq_status_raw() & this->sync_word_valid_bit()) != 0; }

bool SoftPhyDriverBase::is_preamble_detected() {
  if (this->preamble_latched_at_timeout_) {
    this->preamble_latched_at_timeout_ = false;
    return true;
  }
  return (this->read_irq_status_raw() & this->preamble_detected_bit()) != 0;
}

}  // namespace home_io_control
}  // namespace esphome

// NOLINTEND(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
