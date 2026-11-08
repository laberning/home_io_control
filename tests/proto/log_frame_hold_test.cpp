/// @file log_frame_hold_test.cpp
/// @brief Tests for FrameLogBuffer (log_frame.h): frame-log lines held while the hub answers a
/// device's challenge, printed in order afterwards (issue #163).

#include "log_frame.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace esphome::home_io_control;

namespace {

/// One line as the sink received it.
struct Printed {
  FrameLogLevel level;
  std::string tag;
  std::string text;
};

std::vector<Printed> g_printed;

void capture_sink(FrameLogLevel level, const char *tag, const char *text) { g_printed.push_back({level, tag, text}); }

class FrameLogHold : public ::testing::Test {
 protected:
  void SetUp() override { g_printed.clear(); }
  FrameLogBuffer buffer_{capture_sink};
};

}  // namespace

TEST_F(FrameLogHold, PrintsAtOnceWhenNotHolding) {
  buffer_.write(FrameLogLevel::INFO, "io_frame", "RX a");
  ASSERT_EQ(g_printed.size(), 1u);
  EXPECT_EQ(g_printed[0].text, "RX a");
  EXPECT_EQ(buffer_.held(), 0u);
}

TEST_F(FrameLogHold, HoldsUntilReleaseThenPrintsInOrderWithLevelAndTag) {
  buffer_.hold();
  buffer_.write(FrameLogLevel::DEBUG, "radio", "Early RX");
  buffer_.write(FrameLogLevel::INFO, "io_frame", "RX challenge");
  buffer_.write(FrameLogLevel::INFO, "io_frame", "TX answer");
  EXPECT_TRUE(g_printed.empty()) << "nothing may print between the challenge and the answer";
  EXPECT_EQ(buffer_.held(), 3u);

  buffer_.release();
  ASSERT_EQ(g_printed.size(), 3u);
  EXPECT_EQ(g_printed[0].level, FrameLogLevel::DEBUG);
  EXPECT_EQ(g_printed[0].tag, "radio");
  EXPECT_EQ(g_printed[0].text, "Early RX");
  EXPECT_EQ(g_printed[1].text, "RX challenge");
  EXPECT_EQ(g_printed[2].text, "TX answer");
  EXPECT_FALSE(buffer_.holding());

  buffer_.write(FrameLogLevel::INFO, "io_frame", "after");
  EXPECT_EQ(g_printed.size(), 4u) << "released: prints at once again";
}

TEST_F(FrameLogHold, FlushPrintsButKeepsHolding) {
  buffer_.hold();
  buffer_.write(FrameLogLevel::INFO, "io_frame", "RX unrelated");
  buffer_.flush();
  ASSERT_EQ(g_printed.size(), 1u);
  EXPECT_TRUE(buffer_.holding());

  buffer_.write(FrameLogLevel::INFO, "io_frame", "RX challenge");
  EXPECT_EQ(g_printed.size(), 1u);
  buffer_.release();
  EXPECT_EQ(g_printed.size(), 2u);
}

TEST_F(FrameLogHold, OverflowIsCountedNotPrintedInsideTheWindow) {
  buffer_.hold();
  for (size_t i = 0; i < FRAME_LOG_HELD_LINES + 2; i++)
    buffer_.write(FrameLogLevel::INFO, "io_frame", "line");
  EXPECT_TRUE(g_printed.empty());

  buffer_.release();
  ASSERT_EQ(g_printed.size(), FRAME_LOG_HELD_LINES + 1);
  EXPECT_EQ(g_printed.back().level, FrameLogLevel::WARN);
  EXPECT_NE(g_printed.back().text.find("2 frame-log lines not printed"), std::string::npos) << g_printed.back().text;
}

TEST_F(FrameLogHold, LongLineIsTruncatedNotOverrun) {
  const std::string long_line(FRAME_LOG_LINE_SIZE + 50, 'x');
  buffer_.hold();
  buffer_.write(FrameLogLevel::INFO, "io_frame", long_line.c_str());
  buffer_.release();
  ASSERT_EQ(g_printed.size(), 1u);
  EXPECT_EQ(g_printed[0].text.size(), FRAME_LOG_LINE_SIZE - 1);
}

TEST_F(FrameLogHold, ReleaseWithoutHoldIsHarmless) {
  buffer_.release();
  buffer_.release();
  EXPECT_TRUE(g_printed.empty());
  EXPECT_FALSE(buffer_.holding());
}
