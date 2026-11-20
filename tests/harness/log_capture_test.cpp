/// @file
/// @brief Tests of the host log stub's recorder and test::LogCapture.

#include <gtest/gtest.h>

#include "esphome/core/log.h"
#include "log_capture.h"

// ============================================================================
// LogCapture test suite
// ============================================================================
// tests/include/esphome/core/log.h hands each line's level, tag and format string to the installed
// recorder and still never evaluates the arguments; test::LogCapture installs itself for its
// lifetime and restores the recorder it replaced.
// ============================================================================

namespace {

constexpr const char *TAG = "log_capture_test";

}  // namespace

TEST(LogCapture, RecordsEachMacroWithItsLevelTagAndFormat) {
  test::LogCapture log;
  ESP_LOGE(TAG, "e");
  ESP_LOGW(TAG, "w %d", 1);
  ESP_LOGI(TAG, "i");
  ESP_LOGD(TAG, "d");
  ESP_LOGV(TAG, "v");
  ESP_LOGCONFIG(TAG, "c");

  ASSERT_EQ(log.lines().size(), 6u);
  const char expected_levels[] = {'E', 'W', 'I', 'D', 'V', 'C'};
  for (size_t i = 0; i < 6; ++i) {
    EXPECT_EQ(log.lines()[i].level, expected_levels[i]);
    EXPECT_EQ(log.lines()[i].tag, TAG);
  }
  EXPECT_EQ(log.lines()[1].format, "w %d");
  EXPECT_EQ(log.count('W'), 1u);
}

TEST(LogCapture, RecordingLeavesTheArgumentsUnevaluated) {
  test::LogCapture log;
  int calls = 0;
  const auto count_call = [&calls] { return ++calls; };
  ESP_LOGW(TAG, "%d", count_call());
  EXPECT_EQ(calls, 0);
  EXPECT_EQ(log.count('W'), 1u);
}

TEST(LogCapture, ClearForgetsEarlierLines) {
  test::LogCapture log;
  ESP_LOGW(TAG, "before");
  log.clear();
  ESP_LOGW(TAG, "after");
  ASSERT_EQ(log.lines().size(), 1u);
  EXPECT_EQ(log.lines()[0].format, "after");
}

TEST(LogCapture, ANestedCaptureRestoresTheOuterOne) {
  test::LogCapture outer;
  {
    test::LogCapture inner;
    ESP_LOGW(TAG, "inner");
    EXPECT_EQ(inner.count('W'), 1u);
  }
  ESP_LOGW(TAG, "outer");
  ASSERT_EQ(outer.lines().size(), 1u);
  EXPECT_EQ(outer.lines()[0].format, "outer");
}

TEST(LogCapture, NothingIsRecordedOnceTheCaptureEnds) {
  {
    test::LogCapture log;
  }
  EXPECT_EQ(esphome::host_log_stub::recorder(), nullptr);
  ESP_LOGW(TAG, "unrecorded");  // must not reach a destroyed capture
}
