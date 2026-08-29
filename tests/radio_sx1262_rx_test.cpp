#include "radio_sx1262.h"
#include "radio_interface.h"
#include "proto_constants.h"

#include "esphome/core/application.h"

#include "test_helpers.h"
#include "stubs/radio_test_common.h"
#include "stubs/scripted_spi.h"
#include "stubs/soft_phy_test_driver.h"

#include <algorithm>
#include <cstring>
#include <gtest/gtest.h>
#include <vector>

using namespace esphome::home_io_control;

// ============================================================================
// Testable subclass of RadioSX1262
// ============================================================================
//
// The scriptable test double is shared with the LR1121 suite — see
// tests/stubs/soft_phy_test_driver.h. SX1262 needs no chip-specific test hooks, so it is a plain
// alias of the template.
using TestableRadioSX1262 = test::TestableSoftPhy<RadioSX1262>;

namespace {
// Append the protocol CRC to `frame` and UART-encode the result — the exact on-air transform
// SoftPhyDriverBase::send_packet() performs, so its length is what set_packet_params_() must
// program into SetPacketParams' payload-length field. Mirrors encode_frame_with_crc() in
// radio_lr1121_test.cpp.
uint8_t sx1262_encoded_frame_len(const uint8_t *frame, uint8_t frame_len) {
  const uint16_t crc = crc_ccitt(frame, frame_len);
  uint8_t frame_with_crc[FRAME_MAX_WIRE_SIZE];
  memcpy(frame_with_crc, frame, frame_len);
  frame_with_crc[frame_len] = crc & 0xFF;
  frame_with_crc[frame_len + 1] = (crc >> 8) & 0xFF;
  uint8_t encoded[FRAME_MAX_WIRE_SIZE] = {0};
  return uart_encode_packet(frame_with_crc, static_cast<uint8_t>(frame_len + 2), encoded, sizeof(encoded));
}
}  // namespace

// UART encode/decode and CRC validation tests
// ============================================================================

TEST(RadioSX1262, UartEncodeDecodeRoundTrip) {
  // A valid IO-homecontrol frame with CRC appended should encode and decode cleanly.
  const uint8_t frame[] = {0xCE, 0x00, 0xC0, 0xFF, 0xEE, 0xAA, 0xBB, 0xCC, 0x3C, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
  const uint8_t frame_len = sizeof(frame);

  // Append CRC
  uint16_t crc = crc_ccitt(frame, frame_len);
  uint8_t frame_with_crc[32];
  memcpy(frame_with_crc, frame, frame_len);
  frame_with_crc[frame_len] = crc & 0xFF;
  frame_with_crc[frame_len + 1] = (crc >> 8) & 0xFF;

  // UART-encode
  uint8_t encoded[64] = {0};
  uint8_t encoded_len = uart_encode_packet(frame_with_crc, frame_len + 2, encoded, sizeof(encoded));
  ASSERT_GT(encoded_len, 0u);

  // Decode and verify via find_uart_probe (which includes CRC validation)
  UartProbeResult probe = find_uart_probe(encoded, encoded_len);
  ASSERT_TRUE(probe.valid) << "CRC-valid frame should be accepted by find_uart_probe";
  EXPECT_EQ(probe.frame_len, frame_len);
  EXPECT_EQ(memcmp(probe.decoded + probe.frame_start, frame, frame_len), 0)
      << "Decoded frame should match original byte-for-byte";
}

TEST(RadioSX1262, UartProbeRejectsBitErrors) {
  // Encode a valid frame, then flip a bit — CRC should reject it.
  const uint8_t frame[] = {0xCE, 0x00, 0xC0, 0xFF, 0xEE, 0xAA, 0xBB, 0xCC, 0x3C, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
  const uint8_t frame_len = sizeof(frame);

  uint16_t crc = crc_ccitt(frame, frame_len);
  uint8_t frame_with_crc[32];
  memcpy(frame_with_crc, frame, frame_len);
  frame_with_crc[frame_len] = crc & 0xFF;
  frame_with_crc[frame_len + 1] = (crc >> 8) & 0xFF;

  uint8_t encoded[64] = {0};
  uint8_t encoded_len = uart_encode_packet(frame_with_crc, frame_len + 2, encoded, sizeof(encoded));
  ASSERT_GT(encoded_len, 0u);

  // Flip a bit in the middle of the encoded stream (simulates demodulator error)
  encoded[encoded_len / 2] ^= 0x20;

  UartProbeResult probe = find_uart_probe(encoded, encoded_len);
  // The probe should either not find a valid frame, or find one at a wrong offset
  // that doesn't match our original. In either case, it must NOT return our frame
  // with corrupted bytes.
  if (probe.valid) {
    // If something was found, it should NOT match the original (wrong CRC would reject the real frame)
    bool matches_original =
        (probe.frame_len == frame_len && memcmp(probe.decoded + probe.frame_start, frame, frame_len) == 0);
    EXPECT_FALSE(matches_original) << "Bit-corrupted frame should not pass CRC validation";
  }
  // Either probe.valid==false (rejected entirely) or it found a different candidate — both are acceptable
}

TEST(RadioSX1262, UartProbeAcceptsMacTrailerFrame) {
  // Full RX path for a 1W CMD 0x30 "add controller" frame (IoFrame::has_mac), not just parse() in
  // isolation: find_uart_probe() runs the CRC search (find_crc_valid_frame, which must reach the
  // wider declared_len + HMAC_SIZE candidate length to recover a trailer-bearing frame) and
  // classifies the candidate as a plausible frame (is_plausible_uart_frame(), which accepts it via
  // the 1W bit) before this test's own parse() call re-derives the same fields from the recovered
  // bytes — mirroring UartEncodeDecodeRoundTrip above, the established pattern for driving this
  // path, extended with the trailer-specific assertions. Same field values as
  // proto_frame_test.cpp's AddControllerMacTrailerRoundTrip / tests/corpus/captures/
  // reference_1w_vectors/oneway_add_controller_kat.yaml.
  IoFrame frame{};
  init_frame(frame, /*is_2w=*/false, /*start=*/true, /*end=*/true, /*low_power=*/false);
  const uint8_t src[NODE_ID_SIZE] = {0xAB, 0xCD, 0xEF};
  const uint8_t dst[NODE_ID_SIZE] = {0x00, 0x00, 0x3F};
  set_src(frame, src);
  set_dst(frame, dst);
  const uint8_t payload[20] = {0x7E, 0x60, 0x49, 0x1F, 0x97, 0x6A, 0xDF, 0x65, 0x3D, 0xB0,
                               0xED, 0x78, 0x5E, 0x49, 0xA2, 0x01, 0x02, 0x01, 0x12, 0x34};
  ASSERT_TRUE(set_cmd(frame, CMD_ONEWAY_ADD_CONTROLLER, payload, sizeof(payload)));
  frame.has_mac = true;
  const uint8_t mac[HMAC_SIZE] = {0x19, 0xE8, 0x1E, 0xC4, 0x3D, 0x5E};
  memcpy(frame.mac, mac, HMAC_SIZE);

  uint8_t body[FRAME_MAX_WIRE_SIZE] = {0};
  const uint8_t body_len = serialize(frame, body, sizeof(body));
  ASSERT_EQ(body_len, 35) << "declared 29 bytes plus the 6-byte MAC trailer";

  uint16_t crc = crc_ccitt(body, body_len);
  uint8_t frame_with_crc[FRAME_MAX_WIRE_SIZE];
  memcpy(frame_with_crc, body, body_len);
  frame_with_crc[body_len] = crc & 0xFF;
  frame_with_crc[body_len + 1] = (crc >> 8) & 0xFF;

  uint8_t encoded[64] = {0};
  uint8_t encoded_len =
      uart_encode_packet(frame_with_crc, static_cast<uint8_t>(body_len + 2), encoded, sizeof(encoded));
  ASSERT_GT(encoded_len, 0u);

  UartProbeResult probe = find_uart_probe(encoded, encoded_len);
  ASSERT_TRUE(probe.valid) << "CRC-valid MAC-trailer frame should be accepted by find_uart_probe";
  EXPECT_EQ(probe.frame_len, body_len)
      << "recovered frame length should include the trailer, not just the declared bytes";

  IoFrame parsed{};
  ASSERT_TRUE(parse(probe.decoded + probe.frame_start, probe.frame_len, parsed))
      << "recovered bytes should re-parse as the same MAC-trailer frame";
  EXPECT_TRUE(parsed.has_mac) << "classification should see the frame as MAC-trailer-bearing after recovery";
  EXPECT_EQ(memcmp(parsed.mac, mac, HMAC_SIZE), 0) << "recovered MAC should survive the CRC search + decode path";
  EXPECT_EQ(parsed.cmd, CMD_ONEWAY_ADD_CONTROLLER);
}

TEST(RadioSX1262, UartProbeAcceptsEveryKnownIoCommand) {
  // Regression for a real hardware failure (2026-08-16): find_uart_probe() only accepts a
  // CRC-valid candidate whose cmd passes is_known_io_command() (radio_soft_phy.cpp/.h).
  // CMD_GET_GENERAL_INFO3_RESP (0x59) was missing from that switch, so a real, CRC-valid probe
  // reply on real hardware was silently dropped on the SX1262/LR1121 software PHY and reported
  // as a timeout; the same audit also found CMD_IDENTIFY and
  // CMD_WRITE_PRIVATE/CMD_WRITE_PRIVATE_ACK missing, affecting the shipped identify_device()
  // action (and climate writes). This test does not hand-maintain its own opcode list -- an
  // earlier version did, and that duplicate list was itself a third hand-maintained table that
  // could (and did) drift from is_known_io_command(). Instead it iterates every possible command
  // byte and calls is_known_io_command() directly, so the two can never disagree by construction;
  // what it verifies is that find_uart_probe() actually honors that answer for every byte value,
  // not a fixed sample of them.
  for (int cmd_int = 0; cmd_int <= 0xFF; cmd_int++) {
    const auto cmd = static_cast<uint8_t>(cmd_int);
    if (!is_known_io_command(cmd))
      continue;
    SCOPED_TRACE(::testing::Message() << "cmd=0x" << std::hex << static_cast<int>(cmd));
    const uint8_t frame[] = {0xCE, 0x00, 0xC0, 0xFF, 0xEE, 0xAA, 0xBB, 0xCC, cmd, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
    const uint8_t frame_len = sizeof(frame);

    const uint16_t crc = crc_ccitt(frame, frame_len);
    uint8_t frame_with_crc[32];
    memcpy(frame_with_crc, frame, frame_len);
    frame_with_crc[frame_len] = crc & 0xFF;
    frame_with_crc[frame_len + 1] = (crc >> 8) & 0xFF;

    uint8_t encoded[64] = {0};
    const uint8_t encoded_len = uart_encode_packet(frame_with_crc, frame_len + 2, encoded, sizeof(encoded));
    ASSERT_GT(encoded_len, 0u);

    const UartProbeResult probe = find_uart_probe(encoded, encoded_len);
    ASSERT_TRUE(probe.valid) << "find_uart_probe() rejected a cmd that is_known_io_command() accepts: 0x" << std::hex
                             << static_cast<int>(cmd);
    EXPECT_EQ(probe.frame_len, frame_len);
    EXPECT_EQ(memcmp(probe.decoded + probe.frame_start, frame, frame_len), 0);
  }
}

TEST(RadioSX1262, NamedCommandsAreEitherKnownOrDocumentedNeverReceived) {
  // UartProbeAcceptsEveryKnownIoCommand above closes the class of bug where find_uart_probe()
  // disagrees with is_known_io_command() -- but it cannot catch the original bug itself: an
  // opcode that proto_constants.h names (so this codebase knows about it and gives it a symbol)
  // yet is_known_io_command() has never been taught to accept, because that opcode is genuinely
  // never expected to arrive on the soft-PHY receive path this codebase controls. Every such
  // opcode must be *this specific, checked, and reasoned-about list* -- not silence. An opcode
  // that's missing from both this list and is_known_io_command() fails this test, so a newly
  // named constant forces a deliberate choice (teach is_known_io_command() about it, or document
  // here why it's exempt) instead of silently falling through a soft-PHY reception gap.
  //
  // Reasoned exclusions, checked against actual usage in this codebase (not proto_constants.h's
  // doxygen alone) as of 2026-08-16:
  //   - CMD_ACTIVATE_MODE: only decoded by decode_1w_frame() (proto_codecs.cpp), and every 1W
  //     frame carries CTRL0_PROTOCOL_1W, which is_plausible_uart_frame() (radio_soft_phy.cpp)
  //     already accepts unconditionally -- is_known_io_command() is never consulted for it.
  //   - CMD_ONEWAY_ADD_CONTROLLER / CMD_ONEWAY_REMOVE: same 1W bypass as CMD_ACTIVATE_MODE above,
  //     not a gap -- both are actively received and handled (hub_oneway_key_adoption.cpp's
  //     try_adopt_oneway_key_(), pairing_advisor.cpp), just never through is_known_io_command().
  //   - CMD_SET_SENSOR / CMD_SET_SENSOR_ACK: zero references anywhere outside proto_constants.*.
  //   - CMD_DISCOVER_ALT_REQ: this codebase can transmit it (proto_commands.cpp, as an optional
  //     `pairing_discovery_commands` entry), but the reply PairingEngine actually waits for is the
  //     generic CMD_DISCOVER_RESP (0x29, already known) regardless of which discovery variant was
  //     sent -- and hub_key_extraction.cpp's try_handle_key_extraction_frame_() only recognizes
  //     CMD_DISCOVER_REQ (0x28), not this variant, so there is no live dispatch path that needs
  //     to receive 0x2E itself.
  //   - CMD_DISCOVER_ALT_RESP: captured once (velux_kux100/discover_alt_addressed_challenge_
  //     response.yaml) but "not otherwise used anywhere in this codebase" per its own doxygen --
  //     nothing dispatches on it, corpus replay doesn't go through this soft-PHY gate at all.
  //   - CMD_ADDRESS_RESP: captured once (velux_kux100/pairing_full.yaml) but we only ever
  //     transmit it (create_address_resp_device_role(), the key-extraction responder's answer to
  //     an inbound 0x36) -- we never wait for a reply carrying this opcode. CMD_ADDRESS_REQ is
  //     deliberately NOT in this list any more: the key-extraction responder now expects to
  //     receive it, so it belongs in is_known_io_command() instead (see radio_soft_phy.cpp).
  //   - CMD_LAUNCH_KEY_TRANSFER: never observed in corpus or field log; not sent or handled
  //     anywhere; used only to construct a hypothetical IV in proto_crypto_test.cpp.
  //   - CMD_UNKNOWN4A_REQ: deliberately excluded, see ADR 0024 -- nothing this codebase sends
  //     would ever draw a reply carrying this opcode.
  //   - CMD_GET_INFO1 / CMD_GET_INFO1_RESP: unimplemented, never sent, so never a reply to wait
  //     for; this codebase only uses CMD_GET_INFO2/CMD_GET_GENERAL_INFO3 (both already known).
  //   - CMD_SEND_RAW_MESSAGE / CMD_READ_GROUPS / CMD_REBOOT / CMD_SERVICE_STATUS_ACK: never
  //     observed in corpus or field log, not sent or handled anywhere in this codebase -- named
  //     purely so a future capture of one of them has a symbol to log against.
  static constexpr uint8_t NEVER_RECEIVED_ALLOWLIST[] = {
      CMD_ACTIVATE_MODE,
      CMD_ONEWAY_ADD_CONTROLLER,
      CMD_ONEWAY_REMOVE,
      CMD_SET_SENSOR,
      CMD_SET_SENSOR_ACK,
      CMD_DISCOVER_ALT_REQ,
      CMD_DISCOVER_ALT_RESP,
      CMD_ADDRESS_RESP,
      CMD_LAUNCH_KEY_TRANSFER,
      CMD_UNKNOWN4A_REQ,
      CMD_GET_INFO1,
      CMD_GET_INFO1_RESP,
      CMD_SEND_RAW_MESSAGE,
      CMD_READ_GROUPS,
      CMD_REBOOT,
      CMD_SERVICE_STATUS_ACK,
  };

  for (int cmd_int = 0; cmd_int <= 0xFF; cmd_int++) {
    const auto cmd = static_cast<uint8_t>(cmd_int);
    if (std::strcmp(command_name(cmd), "UNKNOWN_CMD") == 0)
      continue;  // Not a named opcode at all -- outside this test's scope.
    if (is_known_io_command(cmd))
      continue;
    const bool allowlisted = std::find(std::begin(NEVER_RECEIVED_ALLOWLIST), std::end(NEVER_RECEIVED_ALLOWLIST), cmd) !=
                             std::end(NEVER_RECEIVED_ALLOWLIST);
    EXPECT_TRUE(allowlisted) << "cmd=0x" << std::hex << static_cast<int>(cmd) << " (" << command_name(cmd)
                             << ") is named by command_name() but missing from both "
                                "is_known_io_command() and this test's documented allowlist -- decide "
                                "which one it belongs in";
  }
}

TEST(RadioSX1262, UartDecodeFixedStride) {
  // Verify decode_uart_probe correctly decodes a known UART-encoded byte.
  // UART frame for byte 0xA5: start(0), 1,0,1,0,0,1,0,1, stop(1) = 10 bits
  // LSB first: bit0=1, bit1=0, bit2=1, bit3=0, bit4=0, bit5=1, bit6=0, bit7=1
  // Bitstream MSB-first in bytes: 0_10100101_1 = 0b0101001011... packed into bytes
  // Let's just use uart_encode_packet for a single byte and verify decode.
  uint8_t input[] = {0xA5};
  uint8_t encoded[2] = {0};
  uint8_t elen = uart_encode_packet(input, 1, encoded, sizeof(encoded));
  ASSERT_GT(elen, 0u);

  uint8_t decoded[4] = {0};
  uint8_t dlen = decode_uart_probe(encoded, elen, 0, decoded, sizeof(decoded));
  ASSERT_GE(dlen, 1u);
  EXPECT_EQ(decoded[0], 0xA5);
}

// ============================================================================
// Preamble unit regression: SetPacketParams' PreambleLength field is bit-denominated on this
// chip, but every caller passes a byte count (LONG_PREAMBLE, SHORT_PREAMBLE, response_preamble()).
// set_packet_params_() must convert.
// ============================================================================

TEST(RadioSX1262, SendPacketSetsPacketParamsPreambleInBits) {
  ScriptedSpi spi;
  MockPin rst, dio1, busy(false);
  TestableRadioSX1262 radio(&spi, &rst, &dio1, &busy, 0, 0);

  const uint8_t frame[] = {0xC8, 0x00, 0xAA, 0xBB, 0xCC, 0xC0, 0xFF, 0xEE, 0x31};

  RadioTxConfig cfg;
  cfg.freq_hz = FREQ_CH2;
  cfg.preamble_len = LONG_PREAMBLE;
  // As in the LR1121 TX test: send_packet reliably times out waiting for TX_DONE in a
  // synchronous host test, but SetPacketParams/WriteBuffer are both issued before that wait.
  radio.send_packet(frame, sizeof(frame), cfg);

  int write_buffer_idx = -1;
  for (size_t i = 0; i < spi.transactions().size(); i++) {
    if (!spi.transactions()[i].empty() && spi.transactions()[i][0] == SX1262_WRITE_BUFFER) {
      write_buffer_idx = static_cast<int>(i);
      break;
    }
  }
  ASSERT_GE(write_buffer_idx, 0);

  int packet_params_idx = -1;
  for (int i = write_buffer_idx - 1; i >= 0; i--) {
    if (!spi.transactions()[i].empty() && spi.transactions()[i][0] == SX1262_SET_PACKET_PARAMS) {
      packet_params_idx = i;
      break;
    }
  }
  ASSERT_GE(packet_params_idx, 0) << "SetPacketParams must be issued before WriteBuffer for TX";

  const auto &pp = spi.transactions()[packet_params_idx];
  ASSERT_EQ(pp.size(), 10u) << "opcode(1) + 9 packet-param bytes";
  const uint16_t preamble_field = (static_cast<uint16_t>(pp[1]) << 8) | pp[2];
  EXPECT_EQ(preamble_field, static_cast<uint16_t>(LONG_PREAMBLE * 8))
      << "SetPacketParams' PreambleLength is in bits: LONG_PREAMBLE (" << LONG_PREAMBLE
      << " bytes) must reach the chip as " << (LONG_PREAMBLE * 8) << " bits, not " << LONG_PREAMBLE
      << " bits (an 8x-too-short on-air preamble)";

  // Pin every one of the nine payload bytes so a zeroed or transposed field in the shared
  // build_gfsk_packet_params_ fails here instead of silently reaching the radio. pp[0] is the
  // opcode; pp[1..9] are packet-param bytes 0..8.
  EXPECT_EQ(pp[1], static_cast<uint8_t>((LONG_PREAMBLE * 8) >> 8)) << "byte 0: preamble length MSB (bits)";
  EXPECT_EQ(pp[2], static_cast<uint8_t>(LONG_PREAMBLE * 8)) << "byte 1: preamble length LSB (bits)";
  EXPECT_EQ(pp[3], 0x04) << "byte 2: preamble detector = 8 bits (SX1262 constant)";
  EXPECT_EQ(pp[4], 0x18) << "byte 3: sync-word length = 24 bits (SX1262_SYNC_WORD_PARAM_24_BITS)";
  EXPECT_EQ(pp[5], 0x00) << "byte 4: address comparison off";
  EXPECT_EQ(pp[6], SX1262_GFSK_PACKET_TYPE_KNOWN_LENGTH) << "byte 5: known-length packet type";
  EXPECT_EQ(pp[7], sx1262_encoded_frame_len(frame, sizeof(frame)))
      << "byte 6: payload length field must equal the UART-encoded length";
  EXPECT_EQ(pp[8], SX1262_GFSK_CRC_OFF) << "byte 7: CRC mode (off for TX)";
  EXPECT_EQ(pp[9], 0x00) << "byte 8: whitening off";
}

// ============================================================================
// TX modulation-quality erratum (datasheet §15.1): bit 2 of SX1262_REG_TX_MODULATION must be set
// for every (G)FSK transmission. The chip transmits perfectly happily without it, just with
// degraded modulation quality, so nothing but this test catches a regression.
// ============================================================================

TEST(RadioSX1262, SendPacketAppliesTxModulationWorkaroundBeforeSetTx) {
  ScriptedSpi spi;
  MockPin rst, dio1, busy(false);
  TestableRadioSX1262 radio(&spi, &rst, &dio1, &busy, 0, 0);

  const uint8_t frame[] = {0xC8, 0x00, 0xAA, 0xBB, 0xCC, 0xC0, 0xFF, 0xEE, 0x31};

  RadioTxConfig cfg;
  cfg.freq_hz = FREQ_CH2;
  cfg.preamble_len = SHORT_PREAMBLE;
  // As in SendPacketSetsPacketParamsPreambleInBits: send_packet times out waiting for TX_DONE in a
  // synchronous host test, but everything up to and including SetTx has already been issued.
  radio.send_packet(frame, sizeof(frame), cfg);

  int set_tx_idx = -1;
  for (size_t i = 0; i < spi.transactions().size(); i++) {
    if (!spi.transactions()[i].empty() && spi.transactions()[i][0] == SX1262_SET_TX) {
      set_tx_idx = static_cast<int>(i);
      break;
    }
  }
  ASSERT_GE(set_tx_idx, 0);

  int workaround_idx = -1;
  for (int i = set_tx_idx - 1; i >= 0; i--) {
    const auto &tx = spi.transactions()[i];
    if (tx.size() == 4 && tx[0] == SX1262_WRITE_REGISTER &&
        tx[1] == static_cast<uint8_t>(SX1262_REG_TX_MODULATION >> 8) &&
        tx[2] == static_cast<uint8_t>(SX1262_REG_TX_MODULATION)) {
      workaround_idx = i;
      break;
    }
  }
  ASSERT_GE(workaround_idx, 0) << "SX1262_REG_TX_MODULATION must be written before every SetTx "
                                  "(datasheet §15.1 TX modulation-quality erratum)";
  EXPECT_NE(spi.transactions()[workaround_idx][3] & SX1262_TX_MODULATION_GFSK_BIT, 0)
      << "the (G)FSK-correct value for bit 2 of SX1262_REG_TX_MODULATION is 1, not 0";
}

// ============================================================================
// Length-driven receive: RX runs in fixed-length mode at SOFT_PHY_RX_PROBE_PACKET_LEN, so
// RX_DONE lands a fixed ~10 ms after the sync word regardless of how short the frame was. A
// frame's own length is knowable from its first decoded byte, so the shared flow reads it out on
// air time instead. CRC validation is the gate; anything less falls back to the RX_DONE path.
// ============================================================================

namespace {

// Build the raw bytes the chip's data buffer holds mid-reception: the frame, its CRC-CCITT
// trailer, and the whole lot UART-packed exactly as it arrived off air.
std::vector<uint8_t> uart_packed_on_air_bytes(const std::vector<uint8_t> &frame) {
  std::vector<uint8_t> with_crc = frame;
  const uint16_t crc = crc_ccitt(frame.data(), static_cast<uint8_t>(frame.size()));
  with_crc.push_back(static_cast<uint8_t>(crc & 0xFF));
  with_crc.push_back(static_cast<uint8_t>((crc >> 8) & 0xFF));

  uint8_t encoded[RADIO_PACKET_BUFFER_SIZE] = {0};
  const uint8_t encoded_len =
      uart_encode_packet(with_crc.data(), static_cast<uint8_t>(with_crc.size()), encoded, sizeof(encoded));
  return std::vector<uint8_t>(encoded, encoded + encoded_len);
}

// A real 15-byte RS100 challenge (0x3C) — the exact frame the hub must turn around fastest, from
// tests/corpus/captures/issues/field_rs100_pairing_key_transfer_timeout.yaml.
const std::vector<uint8_t> kRs100Challenge = {0x0E, 0x00, 0xD1, 0xD4, 0xFF, 0x8C, 0x08, 0x3C,
                                              0x3C, 0x45, 0x51, 0x6F, 0xFE, 0x59, 0x80};

}  // namespace

// Exposes the protected early_rx_read_offset() so EarlyCompletionIsOptInPerChip can assert
// SX1262's own opt-in value. The full scriptable early-RX fixture now lives in the shared harness
// (tests/stubs/soft_phy_test_driver.h, test::EarlyRxSoftPhy) and drives the typed suite.
class OffsetProbeRadioSX1262 : public RadioSX1262 {
 public:
  using RadioSX1262::early_rx_read_offset;
  using RadioSX1262::RadioSX1262;
};

TEST(SoftPhy, RawBytesForFrameCoversFrameAndCrcCells) {
  // 15 protocol bytes + 2 CRC = 17 UART cells = 170 bits = 22 raw bytes.
  EXPECT_EQ(soft_phy_raw_bytes_for_frame(15), 22);
  EXPECT_EQ(soft_phy_raw_bytes_for_frame(FRAME_MIN_SIZE), 14);
  // Even the longest possible frame stays well inside the raw scratch buffer.
  EXPECT_EQ(soft_phy_raw_bytes_for_frame(FRAME_MAX_SIZE), 43);
  EXPECT_LE(soft_phy_raw_bytes_for_frame(FRAME_MAX_SIZE), RADIO_PACKET_BUFFER_SIZE);
}

TEST(SoftPhy, PeekFrameLengthRecoversLengthFromFirstUartCell) {
  const std::vector<uint8_t> raw = uart_packed_on_air_bytes(kRs100Challenge);
  ASSERT_GE(raw.size(), SOFT_PHY_EARLY_HEADER_RAW_BYTES);

  // Three raw bytes — a quarter of a millisecond of air time — is enough to learn the whole
  // frame's length, because CTRL0 bits [4:0] carry it.
  EXPECT_EQ(soft_phy_peek_frame_length(raw.data(), SOFT_PHY_EARLY_HEADER_RAW_BYTES), kRs100Challenge.size());
}

TEST(SoftPhy, PeekFrameLengthRejectsNoise) {
  const uint8_t zeros[SOFT_PHY_EARLY_HEADER_RAW_BYTES] = {0x00, 0x00, 0x00};
  EXPECT_EQ(soft_phy_peek_frame_length(zeros, sizeof(zeros)), 0) << "no stop bit — not a UART cell at any offset";

  const uint8_t ones[SOFT_PHY_EARLY_HEADER_RAW_BYTES] = {0xFF, 0xFF, 0xFF};
  EXPECT_EQ(soft_phy_peek_frame_length(ones, sizeof(ones)), 0) << "no start bit — not a UART cell at any offset";
}

TEST(SoftPhy, AirTimeIsRoundedUpToWholeBytes) {
  // 38400 bps: one byte is 208.33 µs, and the helper must never round below that.
  EXPECT_GE(soft_phy_air_time_us(1), 208u);
  EXPECT_GE(soft_phy_air_time_us(48), 10000u) << "the fixed-length RX window costs a full 10 ms";
  EXPECT_LT(soft_phy_air_time_us(22 + SOFT_PHY_EARLY_READ_MARGIN_BYTES), soft_phy_air_time_us(48))
      << "a 15-byte frame must complete well before the fixed-length window would";
}

TEST(RadioSX1262, EarlyCompletionIsOptInPerChip) {
  // The base class default keeps a chip on the RX_DONE path until it declares its buffer
  // readable mid-reception; SX1262 opts in with the RX base it already programs.
  MockSpi spi;
  MockPin rst, dio1, busy(false);
  OffsetProbeRadioSX1262 radio(&spi, &rst, &dio1, &busy, 0, 0);
  EXPECT_EQ(radio.early_rx_read_offset(), SX1262_RX_BUFFER_BASE);
}

// ============================================================================
// Idle-path hop holdoff (issue #81): check_for_packet()'s sync-without-RX_DONE branch must arm
// reception_in_progress() so ExchangeEngine::maybe_hop() does not retune under a frame that is
// still arriving. The drop-the-holdoff-again-on-completion half is now covered for both chips by
// SoftPhyDriver.ResetRxStateClearsHopHoldoff in tests/radio_soft_phy_shared_test.cpp.
// ============================================================================

TEST(RadioSX1262, CheckForPacketSyncWithoutRxDoneArmsHopHoldoff) {
  MockSpi spi;
  MockPin rst, dio1, busy(false);
  TestableRadioSX1262 radio(&spi, &rst, &dio1, &busy, 0, 0);
  radio.mark_dio_fired_from_isr();
  radio.set_irq_sequence({SX1262_IRQ_SYNC_WORD_VALID});

  RadioRxPacket packet{};
  bool ok = radio.check_for_packet(packet);

  EXPECT_FALSE(ok) << "SYNC_WORD_VALID without RX_DONE is not a complete packet yet";
  EXPECT_TRUE(radio.reception_in_progress())
      << "the sync-without-RX_DONE branch must record the reception so the idle-path hop holds off";
}

TEST(SoftPhy, EncodePadsTrailingIdleBitsHigh) {
  // 3 bytes = 30 bits = 4 encoded bytes (32 bits), so 2 bits of the last byte are line-idle time
  // rather than data. They must go out as idle-high, not as a spurious start bit.
  const uint8_t data[3] = {0x00, 0xFF, 0x5A};
  uint8_t encoded[RADIO_PACKET_BUFFER_SIZE] = {0};
  const uint8_t encoded_len = uart_encode_packet(data, sizeof(data), encoded, sizeof(encoded));
  ASSERT_EQ(encoded_len, 4);

  const uint16_t data_bits = sizeof(data) * UART_CELL_BITS;
  for (uint16_t pos = data_bits; pos < static_cast<uint16_t>(encoded_len) * 8; pos++) {
    const uint8_t bit = (encoded[pos / 8] >> (7 - (pos % 8))) & 0x01;
    EXPECT_EQ(bit, 1) << "trailing bit " << pos << " must idle high";
  }

  // Padding must not disturb the cells themselves: the stream still round-trips.
  uint8_t decoded[RADIO_PACKET_BUFFER_SIZE] = {0};
  ASSERT_EQ(decode_uart_probe(encoded, encoded_len, 0, decoded, sizeof(decoded)), sizeof(data));
  EXPECT_EQ(memcmp(decoded, data, sizeof(data)), 0);
}

TEST(SoftPhy, EncodeLeavesNoPaddingWhenCellsFillWholeBytes) {
  // 4 bytes = 40 bits = exactly 5 encoded bytes, so there is nothing to pad and the last bit
  // written is the frame's own stop bit.
  const uint8_t data[4] = {0xC8, 0x00, 0x12, 0x34};
  uint8_t encoded[RADIO_PACKET_BUFFER_SIZE] = {0};
  ASSERT_EQ(uart_encode_packet(data, sizeof(data), encoded, sizeof(encoded)), 5);

  uint8_t decoded[RADIO_PACKET_BUFFER_SIZE] = {0};
  ASSERT_EQ(decode_uart_probe(encoded, 5, 0, decoded, sizeof(decoded)), sizeof(data));
  EXPECT_EQ(memcmp(decoded, data, sizeof(data)), 0);
}

// ============================================================================
// Post-TX re-arm: the peer can answer within a millisecond or two of our carrier dropping, so the
// path back into RX carries only what it must. SetStandby is redundant (SetRxTxFallbackMode puts
// the chip in standby the instant TxDone fires) and the buffer base has not moved since init.
// ============================================================================

// Raises the TxDone interrupt the moment SetTx is issued. send_packet() clears the DIO latch
// immediately before arming, so a flag set beforehand is discarded — the interrupt has to arrive
// after. This is the only way to reach send_packet()'s success path in a synchronous host test,
// and the post-TX re-arm runs nowhere else.
class TxCompletingRadioSX1262 : public TestableRadioSX1262 {
 public:
  using TestableRadioSX1262::TestableRadioSX1262;

 protected:
  void start_tx() override {
    TestableRadioSX1262::start_tx();
    this->mark_dio_fired_from_isr();
  }
};

TEST(RadioSX1262, PostTxRearmSkipsRedundantStandbyAndBufferBase) {
  ScriptedSpi spi;
  MockPin rst, dio1, busy(false);
  TxCompletingRadioSX1262 radio(&spi, &rst, &dio1, &busy, 0, 0);
  radio.set_irq_sequence({SX1262_IRQ_TX_DONE});

  const uint8_t frame[] = {0xC8, 0x00, 0xAA, 0xBB, 0xCC, 0xC0, 0xFF, 0xEE, 0x31};
  RadioTxConfig cfg;
  cfg.freq_hz = FREQ_CH2;
  cfg.preamble_len = SHORT_PREAMBLE;
  ASSERT_TRUE(radio.send_packet(frame, sizeof(frame), cfg)) << "TxDone was driven, so this must succeed";

  int set_tx_idx = -1;
  for (size_t i = 0; i < spi.transactions().size(); i++) {
    if (!spi.transactions()[i].empty() && spi.transactions()[i][0] == SX1262_SET_TX) {
      set_tx_idx = static_cast<int>(i);
      break;
    }
  }
  ASSERT_GE(set_tx_idx, 0);

  // Everything after SetTx is the re-arm path.
  int standby = 0, buffer_base = 0, packet_params = 0, set_rx = 0;
  for (size_t i = static_cast<size_t>(set_tx_idx) + 1; i < spi.transactions().size(); i++) {
    const auto &tx = spi.transactions()[i];
    if (tx.empty())
      continue;
    if (tx[0] == SX1262_SET_STANDBY)
      standby++;
    else if (tx[0] == SX1262_SET_BUFFER_BASE_ADDRESS)
      buffer_base++;
    else if (tx[0] == SX1262_SET_PACKET_PARAMS)
      packet_params++;
    else if (tx[0] == SX1262_SET_RX)
      set_rx++;
  }

  EXPECT_EQ(standby, 0) << "the chip is already in standby via SetRxTxFallbackMode when TxDone fires";
  EXPECT_EQ(buffer_base, 0) << "the RX buffer base was written at init and has not moved";
  EXPECT_EQ(packet_params, 1) << "RX packet params must be restored — the transmission overwrote them";
  EXPECT_EQ(set_rx, 1) << "and the radio must actually re-enter RX";
}

// ============================================================================
// init()/configure_radio_() register-programming tests.
//
// Until now, nothing exercised RadioSX1262::init() end to end (unlike RadioLR1121, which has
// GetVersionTransactionBytes/InitSequenceOrder/etc.) — the 2026-08-22 preamble-unmask fix
// (SX1262_IRQ_ACTIVITY_MASK / irqMask 0x004B -> 0x004F) landed with unit coverage for the
// *consuming* logic (poll_until_activity_ treating a bare preamble as non-terminal) but nothing
// pinning that configure_radio_() actually programs the chip that way. These tests close that gap.
// ============================================================================

TEST(RadioSX1262, InitSucceeds) {
  ScriptedSpi spi;
  MockPin rst, dio1, busy(false);
  TestableRadioSX1262 radio(&spi, &rst, &dio1, &busy, 0, 0);

  EXPECT_TRUE(radio.init());
  EXPECT_FALSE(radio.is_failed());
}

TEST(RadioSX1262, InitFailsOnBusyTimeout) {
  // BUSY pin held permanently high simulates a dead/unresponsive chip: init()'s very first SPI
  // transaction blocks in wait_busy_() until SX1262_BUSY_TIMEOUT_MS elapses, which is the only way
  // init() itself can return false.
  ScriptedSpi spi;
  MockPin rst, dio1, busy(true);
  TestableRadioSX1262 radio(&spi, &rst, &dio1, &busy, 0, 0);

  EXPECT_FALSE(radio.init());
  EXPECT_TRUE(radio.is_failed());
}

TEST(RadioSX1262, InitProgramsIrqMaskWithPreambleDetectedButNotDio1) {
  // PreambleDetected (bit 2, 0x0004) must be unmasked in irqMask so is_preamble_detected() can
  // ever see it on real hardware, but left OUT of dio1Mask so the ISR still only wakes on a
  // terminal event, not on every preamble.
  ScriptedSpi spi;
  MockPin rst, dio1, busy(false);
  TestableRadioSX1262 radio(&spi, &rst, &dio1, &busy, 0, 0);

  ASSERT_TRUE(radio.init());

  int irq_params_idx = -1;
  for (size_t i = 0; i < spi.transactions().size(); i++) {
    if (!spi.transactions()[i].empty() && spi.transactions()[i][0] == SX1262_SET_DIO_IRQ_PARAMS) {
      irq_params_idx = static_cast<int>(i);
      break;
    }
  }
  ASSERT_GE(irq_params_idx, 0) << "SetDioIrqParams must be issued during init()";

  const auto &tx = spi.transactions()[irq_params_idx];
  ASSERT_EQ(tx.size(), 9u) << "opcode(1) + irqMask(2) + dio1Mask(2) + dio2Mask(2) + dio3Mask(2)";
  const uint16_t irq_mask = (static_cast<uint16_t>(tx[1]) << 8) | tx[2];
  const uint16_t dio1_mask = (static_cast<uint16_t>(tx[3]) << 8) | tx[4];

  EXPECT_EQ(irq_mask & 0x0004, 0x0004u) << "irqMask must unmask PreambleDetected (bit 2)";
  EXPECT_EQ(irq_mask, 0x004F) << "irqMask: TxDone|RxDone|PreambleDetected|SyncWordValid|CrcErr";
  EXPECT_EQ(dio1_mask, 0x004B) << "dio1Mask must stay narrower than irqMask — PreambleDetected must not wake the ISR";
}

TEST(RadioSX1262, InitWritesExpectedSyncWordRegister) {
  ScriptedSpi spi;
  MockPin rst, dio1, busy(false);
  TestableRadioSX1262 radio(&spi, &rst, &dio1, &busy, 0, 0);

  ASSERT_TRUE(radio.init());

  int sync_word_idx = -1;
  for (size_t i = 0; i < spi.transactions().size(); i++) {
    const auto &tx = spi.transactions()[i];
    if (tx.size() >= 3 && tx[0] == SX1262_WRITE_REGISTER && tx[1] == ((SX1262_REG_SYNC_WORD >> 8) & 0xFF) &&
        tx[2] == (SX1262_REG_SYNC_WORD & 0xFF)) {
      sync_word_idx = static_cast<int>(i);
      break;
    }
  }
  ASSERT_GE(sync_word_idx, 0) << "the sync-word register must be written during init()";

  // Payload starts after opcode(1) + address(2); see SyncWordDerivation
  // (tests/radio_soft_phy_test.cpp) for where this specific value comes from.
  const auto &tx = spi.transactions()[sync_word_idx];
  ASSERT_GE(tx.size(), 6u);
  EXPECT_EQ(tx[3], 0x57);
  EXPECT_EQ(tx[4], 0xFD);
  EXPECT_EQ(tx[5], 0x99);
}
