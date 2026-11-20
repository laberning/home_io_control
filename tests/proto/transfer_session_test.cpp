/// @file transfer_session_test.cpp
/// @brief TransferSession: the order of requests and the meaning of each answer of one data-transport read,
/// driven with answers written by hand, no radio and no clock.

#include "proto_codecs.h"
#include "proto_commands.h"
#include "proto_constants.h"
#include "proto_device_readings.h"
#include "proto_frame.h"
#include "transfer_session.h"

#include "corpus_test_helpers.h"

#include <gtest/gtest.h>

#include <optional>
#include <variant>
#include <vector>

using namespace esphome::home_io_control;

namespace {

const uint8_t OWN[NODE_ID_SIZE] = {0xC0, 0xFF, 0xEE};
const uint8_t DEVICE[NODE_ID_SIZE] = {0xAB, 0xC1, 0x23};

/// A recipe table of the tests' own, as a reading recipe of a device would be: a preparing open that is
/// expected to answer empty, then the data open, at one of three offsets, retried once when it comes back empty.
constexpr TransferOpenStep TWO_STEPS[] = {{.object = {0x0C, 0x1001}, .expect_empty = true}, {.object = {0x0C, 0x1002}}};
constexpr TransferOpenStep ONE_STEP[] = {{.object = {0x00, 0x8003}}};
constexpr uint16_t OFFSETS[] = {0x0010, 0x0020, 0x0030};
constexpr TransferRecipe TABLE[] = {
    {.opens = TWO_STEPS, .offsets = OFFSETS, .max_bytes = 40, .retry_empty_open = true},
    {.opens = ONE_STEP, .offsets = {}, .max_bytes = 40},
};
constexpr const TransferRecipe &READING = TABLE[0];
constexpr const TransferRecipe &PLAIN = TABLE[1];

IoFrame reply(uint8_t cmd, const std::vector<uint8_t> &payload) {
  IoFrame f{};
  init_frame(f, true, false, true, false);
  EXPECT_TRUE(set_cmd(f, cmd, payload.data(), static_cast<uint8_t>(payload.size())));
  return f;
}

IoFrame open_answer(uint8_t handle, uint8_t status, uint16_t length) {
  return reply(CMD_TRANSFER_OPEN_RESP,
               {handle, status, static_cast<uint8_t>(length >> 8), static_cast<uint8_t>(length & 0xFF)});
}

/// The block `first, first+1, ...` of a transfer, echoing handle and chunk.
IoFrame block(uint8_t handle, uint16_t chunk, uint8_t first, uint8_t count) {
  std::vector<uint8_t> payload = {handle, static_cast<uint8_t>(chunk >> 8), static_cast<uint8_t>(chunk & 0xFF)};
  for (uint8_t i = 0; i < count; i++)
    payload.push_back(static_cast<uint8_t>(first + i));
  return reply(CMD_UNKNOWN4A_RESP, payload);
}

/// Next request of the session as a payload (command byte first), or an empty vector when the read is over.
std::vector<uint8_t> next(TransferSession &session) {
  IoFrame request{};
  if (!session.next_request(OWN, DEVICE, false, request))
    return {};
  std::vector<uint8_t> out = {request.cmd};
  out.insert(out.end(), request.data, request.data + request.data_len);
  return out;
}

/// Run `next()` then answer it.
std::vector<uint8_t> next_answer(TransferSession &session, const IoFrame &answer) {
  const auto request = next(session);
  session.on_answer(&answer);
  return request;
}

/// Run `next()` then report a request the device challenged and left unanswered.
std::vector<uint8_t> next_accepted_silence(TransferSession &session) {
  const auto request = next(session);
  session.on_answer(nullptr, true);
  return request;
}

/// Run `next()` then report silence.
std::vector<uint8_t> next_silence(TransferSession &session) {
  const auto request = next(session);
  session.on_answer(nullptr);
  return request;
}

}  // namespace

TEST(TransferSession, OpensPullsBlocksCountingDownAndClosesInOrder) {
  TransferSession session(PLAIN, 0, TABLE);
  // 40 bytes = 3 blocks (18 + 18 + 4).
  EXPECT_EQ(next_answer(session, open_answer(0x10, 0, 40)),
            (std::vector<uint8_t>{CMD_TRANSFER_OPEN, 0x08, 0x00, 0x00, 0x80, 0x03, 0x00, 0x00, 0x00, 0x00}));
  EXPECT_EQ(next_answer(session, block(0x10, 3, 0, 18)), (std::vector<uint8_t>{CMD_UNKNOWN4A_REQ, 0x10, 0x00, 0x03}));
  EXPECT_EQ(next_answer(session, block(0x10, 2, 18, 18)), (std::vector<uint8_t>{CMD_UNKNOWN4A_REQ, 0x10, 0x00, 0x02}));
  EXPECT_EQ(next_answer(session, block(0x10, 1, 36, 4)), (std::vector<uint8_t>{CMD_UNKNOWN4A_REQ, 0x10, 0x00, 0x01}));
  EXPECT_FALSE(session.done()) << "the close is still to send";
  EXPECT_EQ(next_answer(session, block(0x10, 0, 0, 0)), (std::vector<uint8_t>{CMD_UNKNOWN4A_REQ, 0x10, 0x00, 0x00}));
  ASSERT_TRUE(session.done());
  EXPECT_EQ(session.outcome(), TransferOutcome::READ);
  ASSERT_EQ(session.data().size(), 40u);
  for (size_t i = 0; i < 40; i++)
    EXPECT_EQ(session.data()[i], i) << "the data runs forward across blocks";
  EXPECT_TRUE(next(session).empty());
}

TEST(TransferSession, AnAdaptedOpenIsReadLikeAnAcceptedOne) {
  TransferSession session(PLAIN, 0, TABLE);
  next_answer(session, open_answer(0x11, TRANSFER_STATUS_ADAPTED, 4));
  next_answer(session, block(0x11, 1, 7, 4));
  next_answer(session, block(0x11, 0, 0, 0));
  ASSERT_TRUE(session.done());
  EXPECT_EQ(session.outcome(), TransferOutcome::READ);
  EXPECT_EQ(session.open().status, TRANSFER_STATUS_ADAPTED);
  EXPECT_EQ(session.data(), (std::vector<uint8_t>{7, 8, 9, 10}));
}

TEST(TransferSession, NothingToReadAndAZeroLengthOpenEndWithoutAnyClose) {
  {
    TransferSession session(PLAIN, 0, TABLE);
    next_answer(session, open_answer(0x10, TRANSFER_STATUS_NOTHING_TO_READ, 0));
    ASSERT_TRUE(session.done());
    EXPECT_EQ(session.outcome(), TransferOutcome::EMPTY);
    EXPECT_EQ(session.open().status, TRANSFER_STATUS_NOTHING_TO_READ);
    EXPECT_TRUE(next(session).empty()) << "nothing was opened, so nothing is closed";
  }
  {
    TransferSession session(PLAIN, 0, TABLE);
    next_answer(session, open_answer(0x10, TRANSFER_STATUS_ACCEPTED, 0));
    ASSERT_TRUE(session.done());
    EXPECT_EQ(session.outcome(), TransferOutcome::EMPTY);
    EXPECT_TRUE(session.data().empty());
  }
}

TEST(TransferSession, AnErrorReplyAndStatus3MeanNoSuchObjectAndOtherRefusalsAreDeclined) {
  {
    TransferSession session(PLAIN, 0, TABLE);
    next_answer(session, open_answer(0x10, 3, 0));
    EXPECT_TRUE(session.done());
    EXPECT_EQ(session.outcome(), TransferOutcome::NO_SUCH_OBJECT);
    EXPECT_TRUE(session.open_answered());
  }
  {
    // Busy / another transfer open: the object may exist, so it stays a declined open.
    TransferSession session(PLAIN, 0, TABLE);
    next_answer(session, open_answer(0x10, 6, 0));
    EXPECT_EQ(session.outcome(), TransferOutcome::DECLINED);
  }
  {
    // A refusal that announces a length is still a refusal: the status says no.
    TransferSession session(PLAIN, 0, TABLE);
    next_answer(session, open_answer(0x10, 2, 40));
    EXPECT_EQ(session.outcome(), TransferOutcome::DECLINED);
  }
  {
    TransferSession session(PLAIN, 0, TABLE);
    next_answer(session, reply(CMD_ERROR_RESP, {RESULT_COMMAND_UNKNOWN}));
    EXPECT_TRUE(session.done());
    EXPECT_EQ(session.outcome(), TransferOutcome::NO_SUCH_OBJECT);
    EXPECT_FALSE(session.open_answered());
  }
}

TEST(TransferSession, AnAnswerThatIsNoOpenAnswerIsMalformed) {
  {
    TransferSession session(PLAIN, 0, TABLE);
    next_answer(session, reply(CMD_TRANSFER_OPEN_RESP, {0x10, 0x00, 0x02}));  // one byte short
    EXPECT_TRUE(session.done());
    EXPECT_EQ(session.outcome(), TransferOutcome::MALFORMED);
    EXPECT_FALSE(session.open_answered());
  }
  {
    TransferSession session(PLAIN, 0, TABLE);
    next_answer(session, reply(CMD_PRIVATE_RESP, {0x00, 0x00, 0x00, 0x00}));
    EXPECT_EQ(session.outcome(), TransferOutcome::MALFORMED);
  }
}

TEST(TransferSession, SilenceToTheOpenEndsAsNoAnswer) {
  TransferSession session(PLAIN, 0, TABLE);
  next_silence(session);
  ASSERT_TRUE(session.done());
  EXPECT_EQ(session.outcome(), TransferOutcome::NO_ANSWER);
  EXPECT_TRUE(next(session).empty());
}

TEST(TransferSession, AnOpenTheDeviceAcceptedButLeftUnansweredEndsAsNoDataNotNoAnswer) {
  TransferSession session(PLAIN, 0, TABLE);
  next_accepted_silence(session);
  ASSERT_TRUE(session.done());
  EXPECT_EQ(session.outcome(), TransferOutcome::NO_DATA);
  EXPECT_TRUE(session.ended_on_accepted_silence());
  EXPECT_FALSE(session.open_answered());
}

TEST(TransferSession, TheAcceptedFlagMeansNothingWhenTheDeviceAnswered) {
  TransferSession session(PLAIN, 0, TABLE);
  next(session);
  const IoFrame answer = open_answer(0x10, 3, 0);
  session.on_answer(&answer, true);  // a reply always wins over the flag
  ASSERT_TRUE(session.done());
  EXPECT_EQ(session.outcome(), TransferOutcome::NO_SUCH_OBJECT);
  EXPECT_FALSE(session.ended_on_accepted_silence());
}

TEST(TransferSession, ABlockTheDeviceAcceptedButLeftUnansweredEndsIncompleteAndSaysSo) {
  TransferSession session(PLAIN, 0, TABLE);
  next_answer(session, open_answer(0x10, 0, 40));
  next_accepted_silence(session);  // block 3
  next_silence(session);           // the close
  ASSERT_TRUE(session.done());
  EXPECT_EQ(session.outcome(), TransferOutcome::INCOMPLETE);
  EXPECT_TRUE(session.ended_on_accepted_silence()) << "a silent close does not erase what ended the read";
}

TEST(TransferSession, AnObjectLongerThanTheRecipeAllowsIsClosedUnread) {
  TransferSession session(PLAIN, 0, TABLE);  // max_bytes 40
  next_answer(session, open_answer(0x10, 0, 41));
  EXPECT_FALSE(session.done());
  EXPECT_EQ(next_answer(session, block(0x10, 0, 0, 0)), (std::vector<uint8_t>{CMD_UNKNOWN4A_REQ, 0x10, 0x00, 0x00}))
      << "the open was accepted, so the next request is the close, not a block";
  ASSERT_TRUE(session.done());
  EXPECT_EQ(session.outcome(), TransferOutcome::TOO_LONG);
  EXPECT_TRUE(session.data().empty());
}

TEST(TransferSession, AnObjectExactlyAtTheCapIsRead) {
  TransferSession session(PLAIN, 0, TABLE);
  next_answer(session, open_answer(0x10, 0, 40));
  next_answer(session, block(0x10, 3, 0, 18));
  next_answer(session, block(0x10, 2, 18, 18));
  next_answer(session, block(0x10, 1, 36, 4));
  next_answer(session, block(0x10, 0, 0, 0));
  EXPECT_EQ(session.outcome(), TransferOutcome::READ);
}

TEST(TransferSession, BlocksThatOverflowTheCapEndIncompleteAfterAClose) {
  TransferSession session(PLAIN, 0, TABLE);  // max_bytes 40
  next_answer(session, open_answer(0x10, 0, 40));
  next_answer(session, block(0x10, 3, 0, 18));
  next_answer(session, block(0x10, 2, 18, 18));
  // The last block is meant to hold 4 bytes; the device sends 18, which would pass the cap of 40.
  next_answer(session, block(0x10, 1, 36, 18));
  EXPECT_FALSE(session.done());
  EXPECT_EQ(next_silence(session), (std::vector<uint8_t>{CMD_UNKNOWN4A_REQ, 0x10, 0x00, 0x00}));
  ASSERT_TRUE(session.done());
  EXPECT_EQ(session.outcome(), TransferOutcome::INCOMPLETE);
  EXPECT_EQ(session.data().size(), 36u) << "the overflowing block is not kept";
}

TEST(TransferSession, BlocksThatAddUpToLessOrMoreThanTheAnnouncedLengthEndIncomplete) {
  // The open announces 20 bytes (two blocks); the cap of 40 leaves room for a last block that is too long.
  for (const uint8_t last_block_bytes : {uint8_t{1}, uint8_t{5}}) {
    TransferSession session(PLAIN, 0, TABLE);
    next_answer(session, open_answer(0x10, 0, 20));
    next_answer(session, block(0x10, 2, 0, 18));
    next_answer(session, block(0x10, 1, 18, last_block_bytes));  // the announced 20 leaves 2 for this one
    EXPECT_EQ(next_silence(session), (std::vector<uint8_t>{CMD_UNKNOWN4A_REQ, 0x10, 0x00, 0x00}))
        << "the transfer is closed all the same";
    ASSERT_TRUE(session.done());
    EXPECT_EQ(session.outcome(), TransferOutcome::INCOMPLETE) << static_cast<int>(last_block_bytes);
    EXPECT_EQ(session.data().size(), 18u + last_block_bytes) << "the data read is kept";
  }
}

TEST(TransferSession, ABlockThatEchoesAnotherChunkEndsIncompleteWithTheCloseAsLastRequest) {
  TransferSession session(PLAIN, 0, TABLE);
  next_answer(session, open_answer(0x10, 0, 40));
  next_answer(session, block(0x10, 3, 0, 18));
  // The block for chunk 2 echoes chunk 7: a late answer to something else, not spliced in.
  next_answer(session, block(0x10, 7, 99, 18));
  EXPECT_FALSE(session.done());
  EXPECT_EQ(next_silence(session), (std::vector<uint8_t>{CMD_UNKNOWN4A_REQ, 0x10, 0x00, 0x00}));
  ASSERT_TRUE(session.done());
  EXPECT_EQ(session.outcome(), TransferOutcome::INCOMPLETE);
  EXPECT_EQ(session.data().size(), 18u) << "only the block that fitted is kept";
}

TEST(TransferSession, ASilentBlockEndsIncompleteAndStillCloses) {
  TransferSession session(PLAIN, 0, TABLE);
  next_answer(session, open_answer(0x10, 0, 40));
  next_silence(session);  // block 3 unanswered
  EXPECT_EQ(next_silence(session), (std::vector<uint8_t>{CMD_UNKNOWN4A_REQ, 0x10, 0x00, 0x00}));
  ASSERT_TRUE(session.done());
  EXPECT_EQ(session.outcome(), TransferOutcome::INCOMPLETE);
  EXPECT_TRUE(session.data().empty());
}

TEST(TransferSession, ASilentCloseStillEndsTheReadWithItsDecidedOutcome) {
  TransferSession session(PLAIN, 0, TABLE);
  next_answer(session, open_answer(0x10, 0, 4));
  next_answer(session, block(0x10, 1, 0, 4));
  next_silence(session);  // the close
  ASSERT_TRUE(session.done());
  EXPECT_EQ(session.outcome(), TransferOutcome::READ);
}

TEST(TransferSession, ARecipeWithAPrepareStepOpensItFirstAndThenTheDataObjectAtTheOffset) {
  TransferSession session(READING, 0x0020, TABLE);
  // Prepare open: class 0x0C, id 0x1001, offset 0x0020 in payload bytes 5-6; answered empty.
  EXPECT_EQ(next_answer(session, open_answer(0x00, 0, 0)),
            (std::vector<uint8_t>{CMD_TRANSFER_OPEN, 0x08, 0x0C, 0x00, 0x10, 0x01, 0x00, 0x20, 0x00, 0x00}));
  EXPECT_FALSE(session.done()) << "an empty prepare answer moves on";
  EXPECT_EQ(next_answer(session, open_answer(0x21, 0, 3)),
            (std::vector<uint8_t>{CMD_TRANSFER_OPEN, 0x08, 0x0C, 0x00, 0x10, 0x02, 0x00, 0x20, 0x00, 0x00}));
  EXPECT_EQ(next_answer(session, block(0x21, 1, 0x80, 3)), (std::vector<uint8_t>{CMD_UNKNOWN4A_REQ, 0x21, 0x00, 0x01}));
  next_answer(session, block(0x21, 0, 0, 0));
  ASSERT_TRUE(session.done());
  EXPECT_EQ(session.outcome(), TransferOutcome::READ);
  EXPECT_EQ(session.data(), (std::vector<uint8_t>{0x80, 0x81, 0x82}));
}

TEST(TransferSession, APrepareStepMovesOnWhateverItAnsweredAndTakesDataIfItCameBackWithSome) {
  {
    // Silence and a refusal both move on to the data open.
    TransferSession session(READING, 0x0010, TABLE);
    next_silence(session);
    EXPECT_FALSE(session.done());
    TransferSession refused(READING, 0x0010, TABLE);
    next_answer(refused, open_answer(0x00, 6, 0));
    EXPECT_FALSE(refused.done());
  }
  {
    // A prepare open that already carries the data is read from there: the next request is a block.
    TransferSession session(READING, 0x0010, TABLE);
    next_answer(session, open_answer(0x33, 0, 2));
    EXPECT_EQ(next(session), (std::vector<uint8_t>{CMD_UNKNOWN4A_REQ, 0x33, 0x00, 0x01}));
  }
}

TEST(TransferSession, AnEmptyDataOpenIsTriedOnceMoreFromTheFirstStepWhenTheRecipeAsksForIt) {
  TransferSession session(READING, 0x0010, TABLE);
  next_answer(session, open_answer(0x00, 0, 0));  // prepare, empty
  next_answer(session, open_answer(0x00, 0, 0));  // data, empty: retried
  ASSERT_FALSE(session.done());
  const auto again = next_answer(session, open_answer(0x00, 0, 0));  // the pair starts over at the prepare open
  EXPECT_EQ(again[4], 0x10);
  EXPECT_EQ(again[5], 0x01) << "back at the prepare step (id 0x1001)";
  EXPECT_EQ(next_answer(session, open_answer(0x41, 0, 2))[5], 0x02);
  next_answer(session, block(0x41, 1, 0, 2));
  next_answer(session, block(0x41, 0, 0, 0));
  EXPECT_EQ(session.outcome(), TransferOutcome::READ);
}

TEST(TransferSession, TheSecondEmptyAnswerEndsAsEmptyAndANonEmptyFailureIsNeverRetried) {
  {
    TransferSession session(READING, 0x0010, TABLE);
    for (int i = 0; i < 4; i++)  // prepare + data, twice
      next_answer(session, open_answer(0x00, 0, 0));
    ASSERT_TRUE(session.done());
    EXPECT_EQ(session.outcome(), TransferOutcome::EMPTY);
  }
  {
    TransferSession session(READING, 0x0010, TABLE);
    next_silence(session);                          // prepare
    next_answer(session, open_answer(0x00, 6, 0));  // data refused
    ASSERT_TRUE(session.done());
    EXPECT_EQ(session.outcome(), TransferOutcome::DECLINED);
  }
}

TEST(TransferSession, ARecipeWithoutRetryEndsAtTheFirstEmptyDataOpen) {
  static constexpr TransferRecipe NO_RETRY[] = {{.opens = TWO_STEPS, .offsets = OFFSETS}};
  TransferSession session(NO_RETRY[0], 0x0010, NO_RETRY);
  next_answer(session, open_answer(0x00, 0, 0));
  next_answer(session, open_answer(0x00, 0, 0));
  ASSERT_TRUE(session.done());
  EXPECT_EQ(session.outcome(), TransferOutcome::EMPTY);
}

TEST(TransferSession, AbandonAfterAnAcceptedOpenClosesBeforeEnding) {
  TransferSession session(PLAIN, 0, TABLE);
  next_answer(session, open_answer(0x10, 0, 40));
  next_answer(session, block(0x10, 3, 0, 18));
  session.abandon();
  EXPECT_FALSE(session.done());
  EXPECT_EQ(next_answer(session, block(0x10, 0, 0, 0)), (std::vector<uint8_t>{CMD_UNKNOWN4A_REQ, 0x10, 0x00, 0x00}));
  ASSERT_TRUE(session.done());
  EXPECT_EQ(session.outcome(), TransferOutcome::ABANDONED);
}

TEST(TransferSession, AbandonWhenOnlyTheCloseIsLeftKeepsAFullReadAsRead) {
  TransferSession session(PLAIN, 0, TABLE);
  next_answer(session, open_answer(0x10, 0, 4));
  next_answer(session, block(0x10, 1, 0, 4));
  session.abandon();
  EXPECT_EQ(next_answer(session, block(0x10, 0, 0, 0)), (std::vector<uint8_t>{CMD_UNKNOWN4A_REQ, 0x10, 0x00, 0x00}))
      << "the close is still sent";
  ASSERT_TRUE(session.done());
  EXPECT_EQ(session.outcome(), TransferOutcome::READ) << "every block was read before the other controller appeared";
  EXPECT_EQ(session.data().size(), 4u);
}

TEST(TransferSession, AbandonBeforeAnythingIsOpenEndsAtOnceAndAfterTheEndChangesNothing) {
  TransferSession session(READING, 0x0010, TABLE);
  EXPECT_FALSE(session.started());
  session.abandon();
  EXPECT_TRUE(session.done());
  EXPECT_EQ(session.outcome(), TransferOutcome::ABANDONED);
  EXPECT_TRUE(next(session).empty());

  TransferSession finished(PLAIN, 0, TABLE);
  next_silence(finished);
  finished.abandon();
  EXPECT_EQ(finished.outcome(), TransferOutcome::NO_ANSWER) << "an ended read keeps its outcome";
}

TEST(TransferSession, IsStartedFromTheFirstRequestUntilItEnds) {
  TransferSession session(PLAIN, 0, TABLE);
  EXPECT_FALSE(session.started());
  next_answer(session, open_answer(0x10, 0, 4));
  EXPECT_TRUE(session.started());
  next_answer(session, block(0x10, 1, 0, 4));
  next_answer(session, block(0x10, 0, 0, 0));
  EXPECT_TRUE(session.done());
  EXPECT_FALSE(session.started());
}

TEST(TransferSession, AnOpenTheAllowlistRefusesEndsRefusedWithoutSendingAnything) {
  // The recipe is not in the table the session is checked against, and its offset is not listed.
  TransferSession outside(READING, 0x0010, std::span<const TransferRecipe>(&TABLE[1], 1));
  IoFrame request{};
  EXPECT_FALSE(outside.next_request(OWN, DEVICE, false, request));
  EXPECT_TRUE(outside.done());
  EXPECT_EQ(outside.outcome(), TransferOutcome::REFUSED);

  TransferSession bad_offset(READING, 0x0011, TABLE);
  EXPECT_FALSE(bad_offset.next_request(OWN, DEVICE, false, request));
  EXPECT_EQ(bad_offset.outcome(), TransferOutcome::REFUSED);
}

TEST(TransferSession, ADefaultSessionFollowsTheProbeRecipesAtOffsetZero) {
  TransferSession session(TRANSFER_RECIPES[2], 0);
  IoFrame request{};
  ASSERT_TRUE(session.next_request(OWN, DEVICE, false, request));
  EXPECT_EQ(request.cmd, CMD_TRANSFER_OPEN);
  TransferSession off(TRANSFER_RECIPES[2], 1);
  EXPECT_FALSE(off.next_request(OWN, DEVICE, false, request)) << "no probe recipe reads at another offset";
}

TEST(TransferSession, ReadsTheCapturedRs100OpenAnswerInTwentyOneBlocks) {
  // The RS100 Solar io's answer to the open of object 0xA601: handle 0x10, 366 bytes, 21 chunks.
  const corpus::CorpusCapture *capture = corpus_test::capture_by_id("somfy_rs100_probe_read_object_a601_sx1262");
  ASSERT_NE(capture, nullptr);
  const IoFrame answer = corpus_test::parse_capture_frame(capture->frames[0]);
  // TRANSFER_RECIPES row 13 is object 0xA601 of class 2.
  TransferSession session(TRANSFER_RECIPES[13], 0);
  EXPECT_EQ(next_answer(session, answer)[1], 0x08);
  EXPECT_FALSE(session.done());
  EXPECT_EQ(next(session), (std::vector<uint8_t>{CMD_UNKNOWN4A_REQ, 0x10, 0x00, 21})) << "21 chunks, counting down";
}

namespace {

/// Replays the capture @p id through a session of the Atlantic heat pump recipe at @p offset: every request the session
/// builds is the captured one (command and payload). Returns the data read.
std::vector<uint8_t> replay_atlantic_capture(const char *id, uint16_t offset) {
  const corpus::CorpusCapture *capture = corpus_test::capture_by_id(id);
  EXPECT_NE(capture, nullptr) << id;
  if (capture == nullptr)
    return {};
  TransferSession session(TRANSFER_RECIPES[TRANSFER_RECIPE_ATLANTIC_HEAT_PUMP], offset);
  for (size_t i = 0; i < capture->frame_count; i++) {
    SCOPED_TRACE(i);
    const corpus::CorpusFrame &frame = capture->frames[i];
    const IoFrame parsed = corpus_test::parse_capture_frame(frame);
    if (frame.tx) {
      std::vector<uint8_t> captured = {parsed.cmd};
      captured.insert(captured.end(), parsed.data, parsed.data + parsed.data_len);
      EXPECT_EQ(next(session), captured);
    } else {
      session.on_answer(&parsed);
    }
  }
  EXPECT_TRUE(session.done());
  EXPECT_EQ(session.outcome(), TransferOutcome::READ);
  return session.data();
}

/// The value the production row of @p reading gives @p data.
std::optional<float> decode_with_row(DeviceReading reading, const std::vector<uint8_t> &data) {
  const DeviceReadingRow *row = find_device_reading(reading);
  EXPECT_NE(row, nullptr);
  if (row == nullptr)
    return std::nullopt;
  return decode_data_object_reading(std::get<DataObjectSource>(row->source), data);
}

}  // namespace

TEST(TransferSession, ReplaysTheCapturedAtlanticHeatPumpCounterRead) {
  // Issue #244: the heating counter of an Atlantic heat pump, read with the recipe at offset 0x0023.
  const std::vector<uint8_t> data =
      replay_atlantic_capture("atlantic_alfea_probe_read_object_heating_energy_sx1276", 0x0023);
  ASSERT_EQ(data, (std::vector<uint8_t>{0x80, 0x2C, 0xC7}));
  EXPECT_EQ(decode_with_row(DeviceReading::HEAT_PUMP_HEATING_ENERGY, data), 11463.0F);
}

TEST(TransferSession, ReplaysTheCapturedAtlanticHeatPumpOutdoorTemperatureRead) {
  // Issue #244: the outdoor temperature entry, offset 0x019B.
  const std::vector<uint8_t> data = replay_atlantic_capture("atlantic_alfea_probe_outdoor_temperature_sx1276", 0x019B);
  ASSERT_EQ(data, (std::vector<uint8_t>{0x80, 0x00, 0x77}));
  const std::optional<float> value = decode_with_row(DeviceReading::HEAT_PUMP_OUTDOOR_TEMPERATURE, data);
  ASSERT_TRUE(value.has_value());
  EXPECT_FLOAT_EQ(*value, 11.9F);
}

TEST(TransferSession, OutcomesHaveNames) {
  for (const TransferOutcome outcome :
       {TransferOutcome::READ, TransferOutcome::EMPTY, TransferOutcome::DECLINED, TransferOutcome::NO_SUCH_OBJECT,
        TransferOutcome::REFUSED, TransferOutcome::MALFORMED, TransferOutcome::TOO_LONG, TransferOutcome::INCOMPLETE,
        TransferOutcome::NO_ANSWER, TransferOutcome::NO_DATA, TransferOutcome::ABANDONED})
    EXPECT_STRNE(transfer_outcome_name(outcome), "unknown");
}
