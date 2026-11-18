/// @file transfer_reader_test.cpp
/// @brief TransferReader without a hub: the queue, the step gaps on a named timeout, giving way to the hub's
/// radio work and to another controller's transfers, abandoning with a close, and the contract of its calls.
///
/// The hub's calls are fakes: a timer list that the test runs on a manual clock, a device model that answers
/// the reader's requests, and a registry. A test needs no hub object.

#include "device_registry.h"
#include "proto_codecs.h"
#include "proto_constants.h"
#include "proto_frame.h"
#include "proto_timing.h"
#include "transfer_reader.h"

#include "esphome/core/hal.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <deque>
#include <optional>
#include <string>
#include <vector>

using namespace esphome::home_io_control;
using esphome::millis;
using esphome::test_clock::ManualClock;

namespace {

constexpr TransferOpenStep PREPARE_AND_DATA[] = {{.object = {0x0C, 0x1001}, .expect_empty = true},
                                                 {.object = {0x0C, 0x1002}}};
constexpr TransferOpenStep PLAIN_STEP[] = {{.object = {0x00, 0x8003}}};
constexpr uint16_t OFFSETS[] = {0x0010, 0x0020};
/// Row 0: a plain one-step recipe; row 1: prepare and data open at listed offsets.
constexpr TransferRecipe RECIPES[] = {
    {.opens = PLAIN_STEP, .offsets = {}, .max_bytes = 40},
    {.opens = PREPARE_AND_DATA, .offsets = OFFSETS, .max_bytes = 40, .retry_empty_open = true},
};

const uint8_t NODE_ID[NODE_ID_SIZE] = {0xC0, 0xFF, 0xEE};
const uint8_t OTHER_CONTROLLER[NODE_ID_SIZE] = {0x12, 0x34, 0x56};

IoFrame make_frame(uint8_t cmd, const uint8_t src[NODE_ID_SIZE], const uint8_t dst[NODE_ID_SIZE],
                   const std::vector<uint8_t> &payload = {}) {
  IoFrame f{};
  init_frame(f, true, false, true, false);
  set_src(f, src);
  set_dst(f, dst);
  EXPECT_TRUE(set_cmd(f, cmd, payload.data(), static_cast<uint8_t>(payload.size())));
  return f;
}

/// The hub side of a TransferReader: a timer list, a device model and a registry with two devices.
class Rig {
 public:
  /// One request the reader sent, and when.
  struct Sent {
    std::string device_id;
    uint8_t cmd;
    std::vector<uint8_t> payload;
    uint32_t at_ms;
  };
  /// One ended read as the callback saw it.
  struct Ended {
    TransferOutcome outcome;
    std::vector<uint8_t> data;
  };

  explicit Rig(uint32_t start_ms = 100000) : clock(start_ms), reader(NODE_ID, RECIPES, make_hub()) {
    registry.add("ABC123");
    registry.add("DEF456");
  }
  Rig(const Rig &) = delete;
  Rig &operator=(const Rig &) = delete;

  /// Queue a read that records its ending in `ended`.
  void read(const std::string &device_id, size_t recipe, uint16_t offset = 0) {
    reader.read(device_id, recipe, offset, [this](TransferOutcome outcome, const std::vector<uint8_t> &data) {
      ended.push_back({outcome, data});
    });
  }

  /// Run the timers for `ms` of manual-clock time, firing each when it is due.
  void run_for(uint32_t ms) {
    const uint32_t end = millis() + ms;
    for (;;) {
      auto next = std::min_element(timers.begin(), timers.end(),
                                   [](const Timer &a, const Timer &b) { return a.due_ms - 0 < b.due_ms; });
      if (next == timers.end() || static_cast<int32_t>(next->due_ms - end) > 0)
        break;
      const uint32_t due = next->due_ms;
      std::function<void()> callback = std::move(next->callback);
      timers.erase(next);
      if (static_cast<int32_t>(due - millis()) > 0)
        esphome::test_clock::advance_ms(due - millis());
      callback();
    }
    if (static_cast<int32_t>(end - millis()) > 0)
      esphome::test_clock::advance_ms(end - millis());
  }

  /// Another controller's data-transport request to `device_id`, heard now.
  void foreign_request_to(const std::string &device_id) {
    const IoDevice *dev = registry.get(device_id);
    ASSERT_NE(dev, nullptr);
    reader.note_transfer_frame(make_frame(CMD_TRANSFER_OPEN, OTHER_CONTROLLER, dev->node_id), millis());
  }

  std::vector<Sent> sent_to(const std::string &device_id) const {
    std::vector<Sent> out;
    for (const Sent &s : sent)
      if (s.device_id == device_id)
        out.push_back(s);
    return out;
  }

  ManualClock clock;
  DeviceRegistry registry;
  TransferReader reader;
  bool radio_allowed{true};
  bool hide_devices{false};    ///< find_device() reports every device as removed.
  uint16_t object_length{40};  ///< Length the modelled device announces for an accepted open.
  bool answer_data_open_empty{false};
  bool accept_without_answer{false};  ///< The device authenticates every request and sends nothing back.
  std::vector<Sent> sent;
  std::vector<Ended> ended;
  std::vector<std::string> timer_names;  ///< Name of every timeout the reader armed.

 private:
  struct Timer {
    std::string name;
    uint32_t due_ms;
    std::function<void()> callback;
  };

  /// The device model: answers an open with the configured length, a block with bytes counting up from the
  /// chunk's place, the close with its echo. A prepare open (id 0x1001) is answered empty.
  std::optional<IoFrame> answer(const IoDevice &dev, const IoFrame &request) {
    if (request.cmd == CMD_TRANSFER_OPEN) {
      const uint16_t id = static_cast<uint16_t>((request.data[3] << 8) | request.data[4]);
      const bool empty = id == 0x1001 || answer_data_open_empty;
      const uint16_t length = empty ? 0 : object_length;
      return make_frame(CMD_TRANSFER_OPEN_RESP, dev.node_id, NODE_ID,
                        {0x10, 0x00, static_cast<uint8_t>(length >> 8), static_cast<uint8_t>(length & 0xFF)});
    }
    const uint16_t chunk = static_cast<uint16_t>((request.data[1] << 8) | request.data[2]);
    std::vector<uint8_t> payload = {request.data[0], request.data[1], request.data[2]};
    const uint16_t chunks = transfer_chunk_count(object_length);
    if (chunk != 0) {
      const uint16_t index = static_cast<uint16_t>(chunks - chunk);
      const uint16_t first = static_cast<uint16_t>(index * TRANSFER_BLOCK_SIZE);
      const uint16_t count = std::min<uint16_t>(TRANSFER_BLOCK_SIZE, static_cast<uint16_t>(object_length - first));
      for (uint16_t i = 0; i < count; i++)
        payload.push_back(static_cast<uint8_t>(first + i));
    }
    return make_frame(CMD_UNKNOWN4A_RESP, dev.node_id, NODE_ID, payload);
  }

  TransferReaderHub make_hub() {
    TransferReaderHub hub;
    hub.set_timeout = [this](const char *name, uint32_t delay_ms, std::function<void()> callback) {
      timer_names.push_back(name);
      timers.erase(std::remove_if(timers.begin(), timers.end(), [&](const Timer &t) { return t.name == name; }),
                   timers.end());
      timers.push_back({name, millis() + delay_ms, std::move(callback)});
    };
    hub.background_radio_allowed = [this]() { return radio_allowed; };
    hub.find_device = [this](const std::string &device_id) -> const IoDevice * {
      return hide_devices ? nullptr : registry.get(device_id);
    };
    hub.exchange = [this](const std::string &device_id, const IoFrame &request, IoFrame &response) {
      sent.push_back(
          {device_id, request.cmd, std::vector<uint8_t>(request.data, request.data + request.data_len), millis()});
      if (accept_without_answer)
        return RecordedAnswer::ACCEPTED_NO_REPLY;
      const auto reply = answer(*registry.get(device_id), request);
      if (!reply.has_value())
        return RecordedAnswer::SILENT;
      response = *reply;
      return RecordedAnswer::REPLY;
    };
    return hub;
  }

  std::vector<Timer> timers;
};

}  // namespace

TEST(TransferReader, ReadsOneObjectStepByStepWithTheGapOnANamedTimeout) {
  Rig rig;
  rig.read("ABC123", 0);
  EXPECT_TRUE(rig.sent.empty()) << "the first request goes out on the next loop pass, not inside read()";
  rig.run_for(10000);

  // Open, three blocks counting down, close.
  ASSERT_EQ(rig.sent.size(), 5u);
  EXPECT_EQ(rig.sent[0].cmd, CMD_TRANSFER_OPEN);
  for (size_t i = 1; i < 5; i++) {
    EXPECT_EQ(rig.sent[i].cmd, CMD_UNKNOWN4A_REQ);
    EXPECT_EQ(rig.sent[i].at_ms - rig.sent[i - 1].at_ms, TRANSFER_STEP_GAP_MS) << "one gap after each answer";
  }
  EXPECT_EQ(rig.sent[1].payload[2], 3);
  EXPECT_EQ(rig.sent[4].payload[2], 0);
  ASSERT_EQ(rig.ended.size(), 1u);
  EXPECT_EQ(rig.ended[0].outcome, TransferOutcome::READ);
  ASSERT_EQ(rig.ended[0].data.size(), 40u);
  EXPECT_EQ(rig.ended[0].data[39], 39);
  for (const std::string &name : rig.timer_names)
    EXPECT_EQ(name, "transfer_read") << "a single chain on one name";
}

TEST(TransferReader, ReadsQueuedBehindEachOtherOneAtATime) {
  Rig rig;
  rig.read("ABC123", 0);
  rig.read("DEF456", 0);
  rig.run_for(30000);

  ASSERT_EQ(rig.ended.size(), 2u);
  const auto first = rig.sent_to("ABC123");
  const auto second = rig.sent_to("DEF456");
  ASSERT_EQ(first.size(), 5u);
  ASSERT_EQ(second.size(), 5u);
  EXPECT_LT(first.back().at_ms, second.front().at_ms) << "the second read starts after the first ended";
}

TEST(TransferReader, ACallbackMayQueueTheNextRead) {
  Rig rig;
  int ends = 0;
  rig.reader.read("ABC123", 0, 0, [&](TransferOutcome, const std::vector<uint8_t> &) {
    ends++;
    rig.read("DEF456", 0);
  });
  rig.run_for(30000);

  EXPECT_EQ(ends, 1);
  ASSERT_EQ(rig.ended.size(), 1u);
  EXPECT_EQ(rig.ended[0].outcome, TransferOutcome::READ);
  EXPECT_EQ(rig.sent_to("DEF456").size(), 5u);
}

TEST(TransferReader, ReportsEachReadOnceAndNamesTheOutcomeOfAFailure) {
  Rig rig;
  rig.answer_data_open_empty = true;
  rig.read("ABC123", 0);
  rig.run_for(30000);

  ASSERT_EQ(rig.ended.size(), 1u);
  EXPECT_EQ(rig.ended[0].outcome, TransferOutcome::EMPTY);
  EXPECT_TRUE(rig.ended[0].data.empty());
  EXPECT_EQ(rig.sent.size(), 1u) << "nothing was opened, so nothing is closed";
}

TEST(TransferReader, ARecipeWithAPrepareStepAndAnOffsetRunsBothOpens) {
  Rig rig;
  rig.read("ABC123", 1, 0x0020);
  rig.run_for(30000);

  ASSERT_EQ(rig.ended.size(), 1u);
  EXPECT_EQ(rig.ended[0].outcome, TransferOutcome::READ);
  ASSERT_GE(rig.sent.size(), 2u);
  EXPECT_EQ(rig.sent[0].payload[4], 0x01) << "the prepare open (id 0x1001) first";
  EXPECT_EQ(rig.sent[1].payload[4], 0x02) << "then the data open (id 0x1002)";
  EXPECT_EQ(rig.sent[1].payload[5], 0x00);
  EXPECT_EQ(rig.sent[1].payload[6], 0x20) << "the offset in payload bytes 5-6";
  EXPECT_EQ(rig.sent[1].at_ms - rig.sent[0].at_ms, TRANSFER_STEP_GAP_MS);
}

TEST(TransferReader, WaitsWhileTheHubHoldsTheRadioAndRetriesEveryHundredMilliseconds) {
  Rig rig;
  rig.radio_allowed = false;
  rig.read("ABC123", 0);
  rig.run_for(2000);
  EXPECT_TRUE(rig.sent.empty()) << "no exchange while background radio work is not allowed";
  // The waiting step re-arms itself on the retry delay: about 20 checks in two seconds.
  EXPECT_GE(rig.timer_names.size(), 15u);

  rig.radio_allowed = true;
  const uint32_t released = millis();
  rig.run_for(TRANSFER_RADIO_HELD_RETRY_MS);
  ASSERT_FALSE(rig.sent.empty());
  EXPECT_LE(rig.sent[0].at_ms - released, TRANSFER_RADIO_HELD_RETRY_MS);
}

TEST(TransferReader, TheRadioBeingHeldBetweenTwoStepsDelaysOnlyTheNextStep) {
  Rig rig;
  rig.read("ABC123", 0);
  rig.run_for(1);
  ASSERT_EQ(rig.sent.size(), 1u);
  rig.radio_allowed = false;
  rig.run_for(3000);
  EXPECT_EQ(rig.sent.size(), 1u);
  rig.radio_allowed = true;
  rig.run_for(10000);
  EXPECT_EQ(rig.ended.size(), 1u);
  EXPECT_EQ(rig.ended[0].outcome, TransferOutcome::READ);
}

TEST(TransferReader, AReadWaitsForTheQuietTimeAfterAnotherControllersTransferToThatDevice) {
  Rig rig;
  rig.foreign_request_to("ABC123");
  rig.read("ABC123", 0);
  rig.run_for(TRANSFER_FOREIGN_QUIET_MS - 1000);
  EXPECT_TRUE(rig.sent.empty());
  rig.run_for(2000);
  EXPECT_FALSE(rig.sent.empty()) << "the first request goes out once the quiet time has passed";
  EXPECT_GE(rig.sent[0].at_ms, 100000u + TRANSFER_FOREIGN_QUIET_MS);
}

TEST(TransferReader, AnotherControllersTrafficToADifferentDeviceIsNoReasonToWait) {
  Rig rig;
  rig.foreign_request_to("DEF456");
  rig.read("ABC123", 0);
  rig.run_for(100);
  EXPECT_FALSE(rig.sent_to("ABC123").empty());
}

TEST(TransferReader, ANewForeignFrameRestartsTheQuietTime) {
  Rig rig;
  rig.foreign_request_to("ABC123");
  rig.run_for(10000);
  rig.foreign_request_to("ABC123");  // heard again 10 s in: the quiet time starts over
  rig.read("ABC123", 0);
  rig.run_for(TRANSFER_FOREIGN_QUIET_MS - 1000);
  EXPECT_TRUE(rig.sent.empty());
  rig.run_for(2000);
  EXPECT_FALSE(rig.sent.empty());
}

TEST(TransferReader, ADevicesOwnAnswerToAnotherControllerCountsAsItsTransfer) {
  Rig rig;
  const IoDevice *dev = rig.registry.get("ABC123");
  ASSERT_NE(dev, nullptr);
  // The device answering the other controller: source is the device, destination the other controller.
  rig.reader.note_transfer_frame(make_frame(CMD_UNKNOWN4A_RESP, dev->node_id, OTHER_CONTROLLER, {0x10, 0, 1}),
                                 millis());
  rig.read("ABC123", 0);
  rig.run_for(5000);
  EXPECT_TRUE(rig.sent.empty());
}

TEST(TransferReader, FramesAddressedToUsOrOutsideTheTransportOrOfUnknownNodesDoNotHoldReads) {
  Rig rig;
  const IoDevice *dev = rig.registry.get("ABC123");
  ASSERT_NE(dev, nullptr);
  // A late answer to our own request.
  rig.reader.note_transfer_frame(make_frame(CMD_UNKNOWN4A_RESP, dev->node_id, NODE_ID, {0x10, 0, 1}), millis());
  // Another command of another controller.
  rig.reader.note_transfer_frame(make_frame(CMD_PRIVATE, OTHER_CONTROLLER, dev->node_id), millis());
  // Transport frames between two nodes that are not ours.
  const uint8_t stranger[NODE_ID_SIZE] = {0x77, 0x88, 0x99};
  rig.reader.note_transfer_frame(make_frame(CMD_TRANSFER_OPEN, OTHER_CONTROLLER, stranger), millis());
  rig.read("ABC123", 0);
  rig.run_for(100);
  EXPECT_FALSE(rig.sent.empty());
}

TEST(TransferReader, EveryTransportCommandCounts) {
  for (uint8_t cmd = CMD_TRANSFER_OPEN; cmd <= CMD_UNKNOWN4A_RESP; cmd++) {
    SCOPED_TRACE(static_cast<unsigned>(cmd));
    Rig rig;
    const IoDevice *dev = rig.registry.get("ABC123");
    ASSERT_NE(dev, nullptr);
    rig.reader.note_transfer_frame(make_frame(cmd, OTHER_CONTROLLER, dev->node_id), millis());
    rig.read("ABC123", 0);
    rig.run_for(100);
    EXPECT_TRUE(rig.sent.empty());
  }
}

TEST(TransferReader, TheQuietTimeIsRightAcrossAMillisWrap) {
  Rig rig(0xFFFFFFFFu - 4000u);
  rig.foreign_request_to("ABC123");
  rig.read("ABC123", 0);
  rig.run_for(TRANSFER_FOREIGN_QUIET_MS - 1000);  // the clock wraps to 0 on the way
  EXPECT_TRUE(rig.sent.empty());
  rig.run_for(2000);
  EXPECT_FALSE(rig.sent.empty());
}

TEST(TransferReader, AnotherControllersTransferMidReadClosesTheTransferAndAbandonsTheRead) {
  Rig rig;
  rig.read("ABC123", 0);
  rig.run_for(TRANSFER_STEP_GAP_MS + 10);  // open and the first block request are out
  ASSERT_EQ(rig.sent.size(), 2u);
  rig.foreign_request_to("ABC123");
  rig.run_for(5000);

  ASSERT_EQ(rig.ended.size(), 1u);
  EXPECT_EQ(rig.ended[0].outcome, TransferOutcome::ABANDONED);
  EXPECT_EQ(rig.ended[0].data.size(), TRANSFER_BLOCK_SIZE) << "the block read before it is reported";
  ASSERT_EQ(rig.sent.size(), 3u) << "no further block request";
  EXPECT_EQ(rig.sent.back().cmd, CMD_UNKNOWN4A_REQ);
  EXPECT_EQ(rig.sent.back().payload[2], 0) << "the last request is the close (chunk 0)";
}

TEST(TransferReader, ARemovedDeviceDropsItsReadAndTheQueueGoesOn) {
  Rig rig;
  rig.read("ABC123", 0);
  rig.read("DEF456", 0);
  rig.hide_devices = true;
  rig.run_for(100);
  rig.hide_devices = false;
  rig.run_for(30000);

  ASSERT_EQ(rig.ended.size(), 2u);
  EXPECT_EQ(rig.ended[0].outcome, TransferOutcome::ABANDONED);
  EXPECT_EQ(rig.ended[1].outcome, TransferOutcome::ABANDONED) << "every step of the hidden registry drops its read";
}

TEST(TransferReader, ADeviceRemovedAfterTheFirstReadStartedOnlyDropsTheRemainingReads) {
  Rig rig;
  rig.read("ABC123", 0);
  rig.run_for(30000);
  ASSERT_EQ(rig.ended.size(), 1u);
  rig.read("DEF456", 0);
  rig.hide_devices = true;
  rig.run_for(1000);
  ASSERT_EQ(rig.ended.size(), 2u);
  EXPECT_EQ(rig.ended[0].outcome, TransferOutcome::READ);
  EXPECT_EQ(rig.ended[1].outcome, TransferOutcome::ABANDONED);
}

TEST(TransferReader, TheTransferIsOpenToTheDeviceFromItsFirstRequestUntilItEnds) {
  Rig rig;
  EXPECT_FALSE(rig.reader.transfer_open_to("ABC123"));
  rig.read("ABC123", 0);
  EXPECT_FALSE(rig.reader.transfer_open_to("ABC123")) << "queued, nothing sent yet";
  rig.run_for(1);
  EXPECT_TRUE(rig.reader.transfer_open_to("ABC123"));
  EXPECT_FALSE(rig.reader.transfer_open_to("DEF456")) << "only the device being read";
  rig.run_for(TRANSFER_STEP_GAP_MS * 2);
  EXPECT_TRUE(rig.reader.transfer_open_to("ABC123"));
  rig.run_for(30000);
  EXPECT_FALSE(rig.reader.transfer_open_to("ABC123"));
}

TEST(TransferReader, ARequestItCannotQueueEndsAtOnceInsideTheCall) {
  Rig rig;
  rig.read("NOTADEVICE", 0);
  rig.read("ABC123", 7);        // no such recipe
  rig.read("ABC123", 0, 0x10);  // offset the recipe does not allow
  rig.read("ABC123", 1, 0x0030);
  ASSERT_EQ(rig.ended.size(), 4u);
  for (const auto &ended : rig.ended)
    EXPECT_EQ(ended.outcome, TransferOutcome::REFUSED);
  EXPECT_FALSE(rig.reader.transfer_open_to("ABC123"));
  rig.run_for(1000);
  EXPECT_TRUE(rig.sent.empty());
}

TEST(TransferReader, AFullQueueAbandonsTheNewReadAndKeepsTheOnesAhead) {
  Rig rig;
  for (size_t i = 0; i < TransferReader::QUEUE_MAX + 1; i++)
    rig.read("ABC123", 0);
  EXPECT_TRUE(rig.ended.empty());
  rig.read("ABC123", 0);
  ASSERT_EQ(rig.ended.size(), 1u);
  EXPECT_EQ(rig.ended[0].outcome, TransferOutcome::ABANDONED);
}

TEST(TransferReader, ADeviceThatAcceptsTheOpenButSendsNothingEndsAsNoDataNotNoAnswer) {
  Rig rig;
  rig.accept_without_answer = true;
  rig.read("ABC123", 0);
  rig.run_for(10000);
  ASSERT_EQ(rig.ended.size(), 1u);
  EXPECT_EQ(rig.ended[0].outcome, TransferOutcome::NO_DATA);
}
