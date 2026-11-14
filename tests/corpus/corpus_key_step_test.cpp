/// @file corpus_key_step_test.cpp
/// @brief The key-step and broadcast-reply classifiers applied to real frames from the corpus.
///
/// The synthetic cases in decisions_pairing_test.cpp and decisions_exchange_test.cpp cover every
/// branch; these pin the same classifiers on captured frames, so a frame shape the classifiers
/// misjudge fails here. No capture holds a device's unmasked 0x32 answer to a key pull, so the key
/// case uses a captured 0x32 of the same shape (command byte plus a 16-byte encrypted key),
/// re-addressed from the device to the controller.

#include "corpus_test_helpers.h"
#include "decisions_exchange.h"
#include "decisions_pairing.h"
#include "proto_constants.h"
#include "proto_frame.h"

#include <gtest/gtest.h>

using namespace esphome::home_io_control;

namespace {

/// Izymo dimmer pairing: 0x32 key transfer (frame 7), 0x33 key confirm (frame 8) and a 0xFE with
/// result code 0x28 (frame 12), between the dimmer 389618 and the controller C0FFEE.
constexpr const char *IZYMO_PAIRING = "somfy_izymo_dimmer_pairing_full_sx1276";
constexpr uint8_t IZYMO_KEY_TRANSFER_FRAME = 7;
constexpr uint8_t IZYMO_KEY_CONFIRM_FRAME = 8;
constexpr uint8_t IZYMO_ERROR_FRAME = 12;
constexpr uint8_t IZYMO_DEVICE[NODE_ID_SIZE] = {0x38, 0x96, 0x18};
constexpr uint8_t IZYMO_CONTROLLER[NODE_ID_SIZE] = {0xC0, 0xFF, 0xEE};

/// Awning name read: the 0x51 answer after the challenge (frame 3), from 30E1F2 to C0FFEE.
constexpr const char *AWNING_GET_NAME = "somfy_awning_exchange_get_name_sx1276";
constexpr uint8_t AWNING_NAME_FRAME = 3;

/// AGILIA pairing: the heater's 0x2B answer to our roll-call (frame 6), addressed to 60F823.
constexpr const char *AGILIA_ROLLCALL = "atlantic_agilia_pairing_key_pull_heater_rollcall";
constexpr uint8_t AGILIA_ROLLCALL_REPLY_FRAME = 6;
constexpr uint8_t AGILIA_CONTROLLER[NODE_ID_SIZE] = {0x60, 0xF8, 0x23};

/// Frame @p index of capture @p id, parsed as captured.
IoFrame captured(const char *id, uint8_t index) {
  const corpus::CorpusCapture *capture = corpus_test::capture_by_id(id);
  EXPECT_NE(capture, nullptr) << id << " is missing from the corpus";
  if (capture == nullptr || index >= capture->frame_count)
    return IoFrame{};
  return corpus_test::parse_capture_frame(capture->frames[index]);
}

}  // namespace

TEST(CorpusKeyStep, CapturedKeyFrameFromTheDeviceIsTheKey) {
  const IoFrame key =
      corpus_test::readdressed_capture_frame(IZYMO_PAIRING, IZYMO_KEY_TRANSFER_FRAME, IZYMO_DEVICE, IZYMO_CONTROLLER);
  ASSERT_EQ(key.cmd, CMD_KEY_TRANSFER);

  EXPECT_EQ(decisions::classify_key_pull_reply(key, IZYMO_DEVICE, IZYMO_CONTROLLER),
            decisions::PairingKeyPullReplyDisposition::KEY);
}

TEST(CorpusKeyStep, CapturedErrorFromTheDeviceRefusesAKeyPullAndAKeyVerify) {
  const IoFrame error = captured(IZYMO_PAIRING, IZYMO_ERROR_FRAME);
  ASSERT_EQ(error.cmd, CMD_ERROR_RESP);

  EXPECT_EQ(decisions::classify_key_pull_reply(error, IZYMO_DEVICE, IZYMO_CONTROLLER),
            decisions::PairingKeyPullReplyDisposition::REFUSE);
  EXPECT_EQ(decisions::classify_key_verify_reply(true, true, error), decisions::PairingKeyVerifyDisposition::REFUSED);
}

TEST(CorpusKeyStep, CapturedKeyConfirmIsADirectKeyConfirm) {
  const IoFrame confirm = captured(IZYMO_PAIRING, IZYMO_KEY_CONFIRM_FRAME);

  EXPECT_TRUE(decisions::is_direct_key_confirm(confirm, IZYMO_DEVICE, IZYMO_CONTROLLER));
  EXPECT_EQ(decisions::classify_key_pull_reply(confirm, IZYMO_DEVICE, IZYMO_CONTROLLER),
            decisions::PairingKeyPullReplyDisposition::REJECT)
      << "a 0x33 is no key";
}

TEST(CorpusKeyStep, CapturedNameAnswerAfterAChallengeMeansTheKeyIsHeld) {
  const IoFrame name = captured(AWNING_GET_NAME, AWNING_NAME_FRAME);
  ASSERT_EQ(name.cmd, CMD_GET_NAME_RESP);

  EXPECT_EQ(decisions::classify_key_verify_reply(true, true, name), decisions::PairingKeyVerifyDisposition::HELD);
}

TEST(CorpusKeyStep, CapturedRollCallReplyIsCollectedOnlyByItsController) {
  const IoFrame reply = captured(AGILIA_ROLLCALL, AGILIA_ROLLCALL_REPLY_FRAME);

  EXPECT_TRUE(decisions::is_broadcast_reply_for(reply, CMD_DISCOVER_SPE_RESP, AGILIA_CONTROLLER));
  EXPECT_FALSE(decisions::is_broadcast_reply_for(reply, CMD_DISCOVER_SPE_RESP, IZYMO_CONTROLLER))
      << "addressed to another controller";
  EXPECT_FALSE(decisions::is_broadcast_reply_for(reply, CMD_DISCOVER_RESP, AGILIA_CONTROLLER))
      << "not the command the broadcast's replies carry";
}
