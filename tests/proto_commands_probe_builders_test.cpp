/// @file proto_commands_probe_builders_test.cpp
/// @brief Byte-exact replay of the Step 2 diagnostic-probe builders against real captured
/// requests — the strongest available check that create_get_status_extended() and
/// create_private2_read() reproduce exactly what a real Somfy hub transmits.

#include "proto_commands.h"
#include "proto_constants.h"
#include "proto_frame.h"

#include "corpus_test_helpers.h"
#include "test_helpers.h"

#include <cstring>
#include <functional>
#include <gtest/gtest.h>

using namespace esphome::home_io_control;

namespace {

const corpus::CorpusCapture *find_capture(const char *id) {
  auto matches =
      corpus_test::captures_where([&](const corpus::CorpusCapture *c) { return std::strcmp(c->id, id) == 0; });
  if (matches.empty())
    return nullptr;
  return matches.front();
}

/// Builds `frame` via `builder`, serializes it, and asserts the result is byte-identical to the
/// captured frame at `capture_id`/`frame_index` (CRC-stripped — all these fixtures are
/// `crc: absent`).
void expect_builder_matches_capture(const char *capture_id, uint8_t frame_index,
                                    const std::function<bool(IoFrame &, const uint8_t *, const uint8_t *)> &builder) {
  const corpus::CorpusCapture *capture = find_capture(capture_id);
  ASSERT_NE(capture, nullptr) << "capture not found: " << capture_id;
  ASSERT_LT(frame_index, capture->frame_count);
  const corpus::CorpusFrame &cf = capture->frames[frame_index];
  IoFrame captured = corpus_test::parse_capture_frame(cf);

  IoFrame built{};
  ASSERT_TRUE(builder(built, captured.src, captured.dst)) << "builder should succeed for " << capture_id;

  uint8_t built_bytes[64] = {0};
  uint8_t built_len = serialize(built, built_bytes, sizeof(built_bytes));
  uint8_t expected_len = corpus_test::wire_len(cf);

  ASSERT_EQ(built_len, expected_len) << "wire length mismatch for " << capture_id;
  EXPECT_EQ(0, std::memcmp(built_bytes, cf.bytes, expected_len)) << "wire bytes mismatch for " << capture_id;
}

}  // namespace

TEST(ProtoCommandsProbeBuilders, PrivateFunctionMatchesGetStatusShape) {
  // create_get_status() is create_private_function() frozen at PRIVATE_GET_POSITION_STATUS —
  // the existing CreateGetStatus test already pins this byte-for-byte; this just confirms the
  // generalized entry point produces the identical frame for the same function_id.
  IoFrame via_status{}, via_function{};
  ASSERT_TRUE(create_get_status(via_status, test::OWN_ID, test::DST_ID));
  ASSERT_TRUE(create_private_function(via_function, test::OWN_ID, test::DST_ID, 0x03));

  uint8_t status_bytes[64] = {0}, function_bytes[64] = {0};
  uint8_t status_len = serialize(via_status, status_bytes, sizeof(status_bytes));
  uint8_t function_len = serialize(via_function, function_bytes, sizeof(function_bytes));
  ASSERT_EQ(status_len, function_len);
  EXPECT_EQ(0, std::memcmp(status_bytes, function_bytes, status_len));
}

TEST(ProtoCommandsProbeBuilders, GetStatusExtendedMatchesFixture11SelectorN0) {
  expect_builder_matches_capture("issue_45_extended_private_both_selectors", 0,
                                 [](IoFrame &f, const uint8_t *own, const uint8_t *dst) {
                                   return create_get_status_extended(f, own, dst, 0x80, 0x00);
                                 });
}

TEST(ProtoCommandsProbeBuilders, GetStatusExtendedMatchesFixture11SelectorN1) {
  expect_builder_matches_capture("issue_45_extended_private_both_selectors", 1,
                                 [](IoFrame &f, const uint8_t *own, const uint8_t *dst) {
                                   return create_get_status_extended(f, own, dst, 0x80, 0x01);
                                 });
}

TEST(ProtoCommandsProbeBuilders, Private2ReadMatchesFixture9LongForm) {
  // Fixture 9's request carries CTRL1_LOW_POWER set (target: a solar shutter).
  expect_builder_matches_capture("issue_45_private2_long_form_request_response", 0,
                                 [](IoFrame &f, const uint8_t *own, const uint8_t *dst) {
                                   return create_private2_read(f, own, dst, 0x06, true, /*low_power=*/true);
                                 });
}

TEST(ProtoCommandsProbeBuilders, Private2ReadMatchesFixture10ShortFormModifier09) {
  // Fixture 10's requests carry CTRL1_LOW_POWER clear (target: 586E37, a mains switch) —
  // deliberately the opposite of fixture 9, so these two tests together would catch a
  // low_power/long_form mixup (see create_private2_read()'s doxygen for why they are not the
  // same parameter).
  expect_builder_matches_capture("issue_45_private2_short_form", 0,
                                 [](IoFrame &f, const uint8_t *own, const uint8_t *dst) {
                                   return create_private2_read(f, own, dst, 0x09, false, /*low_power=*/false);
                                 });
}

TEST(ProtoCommandsProbeBuilders, Private2ReadMatchesFixture10ShortFormModifier03) {
  expect_builder_matches_capture("issue_45_private2_short_form", 1,
                                 [](IoFrame &f, const uint8_t *own, const uint8_t *dst) {
                                   return create_private2_read(f, own, dst, POS_VENT_MODIFIER, false,
                                                               /*low_power=*/false);
                                 });
}

TEST(ProtoCommandsProbeBuilders, Private2ReadLowPowerIsIndependentOfLongForm) {
  // Neither corpus fixture exercises "short form to a solar device" or "long form to a mains
  // device" -- both combinations are unreachable by capture but reachable by probe. This pins
  // that low_power is a real, independent parameter rather than silently following long_form.
  IoFrame short_form_low_power{};
  ASSERT_TRUE(create_private2_read(short_form_low_power, test::OWN_ID, test::DST_ID, 0x06, false, true));
  EXPECT_TRUE((short_form_low_power.ctrl1 & CTRL1_LOW_POWER) != 0);

  IoFrame long_form_mains{};
  ASSERT_TRUE(create_private2_read(long_form_mains, test::OWN_ID, test::DST_ID, 0x06, true, false));
  EXPECT_TRUE((long_form_mains.ctrl1 & CTRL1_LOW_POWER) == 0);
}

TEST(ProtoCommandsProbeBuilders, GeneralInfo3NoPayload) {
  IoFrame frame{};
  ASSERT_TRUE(create_general_info3(frame, test::OWN_ID, test::DST_ID, true));
  EXPECT_EQ(frame.cmd, CMD_GET_GENERAL_INFO3);
  EXPECT_EQ(frame.data_len, 0);
  EXPECT_TRUE((frame.ctrl1 & CTRL1_LOW_POWER) != 0);
}

TEST(ProtoCommandsProbeBuilders, GeneralInfo3RespectsLowPowerFalse) {
  IoFrame frame{};
  ASSERT_TRUE(create_general_info3(frame, test::OWN_ID, test::DST_ID, false));
  EXPECT_TRUE((frame.ctrl1 & CTRL1_LOW_POWER) == 0);
}

TEST(ProtoCommandsProbeBuilders, GeneralInfo3MatchesFixture12CapturedRequest) {
  // Frame 2 of issue_45_capability_probe_burst: "48 00 58 6E 36 CA 0A 18 58", a real captured
  // CMD_GET_GENERAL_INFO3 request with CTRL1_LOW_POWER clear.
  expect_builder_matches_capture("issue_45_capability_probe_burst", 2,
                                 [](IoFrame &f, const uint8_t *own, const uint8_t *dst) {
                                   return create_general_info3(f, own, dst, /*low_power=*/false);
                                 });
}

// The community fixtures above pin the builders against a capture of *another* hub's output.
// The own-hardware captures below pin them against a capture of *our own* builders' output
// through the probe_device() action -- the difference between "the builder can reproduce a real
// hub's bytes" and "the builder reproduces what we actually put on air".

TEST(ProtoCommandsProbeBuilders, PrivateFunctionMatchesOwnHardwareFn06Probe) {
  expect_builder_matches_capture(
      "somfy_awning_private_fn_probe_lr1121", 0,
      [](IoFrame &f, const uint8_t *own, const uint8_t *dst) { return create_private_function(f, own, dst, 0x06); });
}

TEST(ProtoCommandsProbeBuilders, PrivateFunctionMatchesOwnHardwareFn09Probe) {
  expect_builder_matches_capture(
      "somfy_dimmer_private_fn_probe_lr1121", 2,
      [](IoFrame &f, const uint8_t *own, const uint8_t *dst) { return create_private_function(f, own, dst, 0x09); });
}

TEST(ProtoCommandsProbeBuilders, GetStatusExtendedMatchesOwnHardwareBlock00Probe) {
  expect_builder_matches_capture("somfy_awning_status_ext_probe_lr1121", 0,
                                 [](IoFrame &f, const uint8_t *own, const uint8_t *dst) {
                                   return create_get_status_extended(f, own, dst, 0x80, 0x00);
                                 });
}

TEST(ProtoCommandsProbeBuilders, GetStatusExtendedMatchesOwnHardwareBlock01Probe) {
  expect_builder_matches_capture("somfy_awning_status_ext_probe_lr1121", 2,
                                 [](IoFrame &f, const uint8_t *own, const uint8_t *dst) {
                                   return create_get_status_extended(f, own, dst, 0x80, 0x01);
                                 });
}

TEST(ProtoCommandsProbeBuilders, GeneralInfo3MatchesOwnHardwareProbe) {
  // low_power=true, unlike GeneralInfo3MatchesFixture12CapturedRequest above -- every probe
  // builder call sends low_power=true (see build_probe_general_info3() in management_actions.cpp).
  expect_builder_matches_capture("somfy_awning_general_info3_probe_lr1121", 0,
                                 [](IoFrame &f, const uint8_t *own, const uint8_t *dst) {
                                   return create_general_info3(f, own, dst, /*low_power=*/true);
                                 });
}

TEST(ProtoCommandsProbeBuilders, Private2ReadMatchesOwnHardwareFavoriteProbe) {
  expect_builder_matches_capture("somfy_awning_private2_probe_favorite_lr1121", 0,
                                 [](IoFrame &f, const uint8_t *own, const uint8_t *dst) {
                                   return create_private2_read(f, own, dst, 0x00, /*long_form=*/true,
                                                               /*low_power=*/true);
                                 });
}

TEST(ProtoCommandsProbeBuilders, Private2ReadMatchesOwnHardwareVentProbe) {
  expect_builder_matches_capture("somfy_awning_private2_probe_vent_lr1121", 0,
                                 [](IoFrame &f, const uint8_t *own, const uint8_t *dst) {
                                   return create_private2_read(f, own, dst, POS_VENT_MODIFIER, /*long_form=*/true,
                                                               /*low_power=*/true);
                                 });
}
