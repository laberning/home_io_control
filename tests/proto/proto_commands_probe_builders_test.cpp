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
#include <iterator>

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
  ASSERT_TRUE(create_get_status(via_status, test::OWN_ID, test::DST_ID, /*low_power=*/true));
  ASSERT_TRUE(create_private_function(via_function, test::OWN_ID, test::DST_ID, /*low_power=*/true, 0x03));

  uint8_t status_bytes[64] = {0}, function_bytes[64] = {0};
  uint8_t status_len = serialize(via_status, status_bytes, sizeof(status_bytes));
  uint8_t function_len = serialize(via_function, function_bytes, sizeof(function_bytes));
  ASSERT_EQ(status_len, function_len);
  EXPECT_EQ(0, std::memcmp(status_bytes, function_bytes, status_len));
}

TEST(ProtoCommandsProbeBuilders, GetStatusExtendedMatchesFixture11SelectorN0) {
  expect_builder_matches_capture("multi_somfy_probe_extended_private_both_selectors", 0,
                                 [](IoFrame &f, const uint8_t *own, const uint8_t *dst) {
                                   return create_get_status_extended(f, own, dst, /*low_power=*/true, 0x80, 0x00);
                                 });
}

TEST(ProtoCommandsProbeBuilders, GetStatusExtendedMatchesFixture11SelectorN1) {
  expect_builder_matches_capture("multi_somfy_probe_extended_private_both_selectors", 1,
                                 [](IoFrame &f, const uint8_t *own, const uint8_t *dst) {
                                   return create_get_status_extended(f, own, dst, /*low_power=*/true, 0x80, 0x01);
                                 });
}

TEST(ProtoCommandsProbeBuilders, Private2ReadMatchesFixture9LongForm) {
  // Fixture 9's request carries CTRL1_LOW_POWER set (target: a solar shutter).
  expect_builder_matches_capture("multi_somfy_probe_private2_long_form", 0,
                                 [](IoFrame &f, const uint8_t *own, const uint8_t *dst) {
                                   return create_private2_read(f, own, dst, 0x06, true, /*low_power=*/true);
                                 });
}

TEST(ProtoCommandsProbeBuilders, Private2ReadMatchesFixture10ShortFormModifier09) {
  // Fixture 10's requests carry CTRL1_LOW_POWER clear (target: 586E37, a mains switch) —
  // deliberately the opposite of fixture 9, so these two tests together would catch a
  // low_power/long_form mixup (see create_private2_read()'s doxygen for why they are not the
  // same parameter).
  expect_builder_matches_capture("multi_somfy_probe_private2_short_form", 0,
                                 [](IoFrame &f, const uint8_t *own, const uint8_t *dst) {
                                   return create_private2_read(f, own, dst, 0x09, false, /*low_power=*/false);
                                 });
}

TEST(ProtoCommandsProbeBuilders, Private2ReadMatchesFixture10ShortFormModifier03) {
  expect_builder_matches_capture("multi_somfy_probe_private2_short_form", 1,
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

TEST(ProtoCommandsProbeBuilders, GetInfo1NoPayload) {
  IoFrame frame{};
  ASSERT_TRUE(create_get_info1(frame, test::OWN_ID, test::DST_ID, /*low_power=*/true));
  EXPECT_EQ(frame.cmd, CMD_GET_INFO1);
  EXPECT_EQ(frame.data_len, 0);
  EXPECT_TRUE((frame.ctrl1 & CTRL1_LOW_POWER) != 0);

  IoFrame mains{};
  ASSERT_TRUE(create_get_info1(mains, test::OWN_ID, test::DST_ID, /*low_power=*/false));
  EXPECT_TRUE((mains.ctrl1 & CTRL1_LOW_POWER) == 0);
}

TEST(ProtoCommandsProbeBuilders, GetInfo2NoPayload) {
  IoFrame frame{};
  ASSERT_TRUE(create_get_info2(frame, test::OWN_ID, test::DST_ID, /*low_power=*/true));
  EXPECT_EQ(frame.cmd, CMD_GET_INFO2);
  EXPECT_EQ(frame.data_len, 0);
  EXPECT_TRUE((frame.ctrl1 & CTRL1_LOW_POWER) != 0);

  IoFrame mains{};
  ASSERT_TRUE(create_get_info2(mains, test::OWN_ID, test::DST_ID, /*low_power=*/false));
  EXPECT_TRUE((mains.ctrl1 & CTRL1_LOW_POWER) == 0);
}

TEST(ProtoCommandsProbeBuilders, NoPayloadRequestsShareOneShape) {
  // Pins the shared no-payload builder: create_get_name / create_general_info3 / create_get_info1
  // / create_get_info2 all route through one helper, so with the same arguments their serialized
  // frames differ *only* in the command byte -- and that byte is the one each builder names.
  struct NoPayloadBuilder {
    const char *name;
    uint8_t cmd;
    std::function<bool(IoFrame &)> build;
  };
  const NoPayloadBuilder builders[] = {
      {"get_name", CMD_GET_NAME, [](IoFrame &f) { return create_get_name(f, test::OWN_ID, test::DST_ID, true); }},
      {"general_info3", CMD_GET_GENERAL_INFO3,
       [](IoFrame &f) { return create_general_info3(f, test::OWN_ID, test::DST_ID, true); }},
      {"get_info1", CMD_GET_INFO1, [](IoFrame &f) { return create_get_info1(f, test::OWN_ID, test::DST_ID, true); }},
      {"get_info2", CMD_GET_INFO2, [](IoFrame &f) { return create_get_info2(f, test::OWN_ID, test::DST_ID, true); }},
  };

  uint8_t ref_bytes[64] = {0};
  uint8_t ref_len = 0;
  for (size_t i = 0; i < std::size(builders); i++) {
    SCOPED_TRACE(builders[i].name);
    IoFrame f{};
    ASSERT_TRUE(builders[i].build(f));
    uint8_t bytes[64] = {0};
    const uint8_t len = serialize(f, bytes, sizeof(bytes));
    ASSERT_GT(len, 0u);
    if (i == 0) {
      ref_len = len;
      std::memcpy(ref_bytes, bytes, len);
      continue;
    }
    ASSERT_EQ(len, ref_len) << "wire length differs from get_name";
    // Exactly one byte may differ from the reference frame, and it must be the command byte this
    // builder claims to send -- a same-length frame differing somewhere else would pass a bare
    // "one byte differs" count.
    int diffs = 0;
    int diff_offset = -1;
    for (uint8_t b = 0; b < len; b++) {
      if (bytes[b] != ref_bytes[b]) {
        diffs++;
        diff_offset = b;
      }
    }
    ASSERT_EQ(diffs, 1) << "differs from get_name in more than just the command byte";
    EXPECT_EQ(bytes[diff_offset], builders[i].cmd) << "the differing byte is not this builder's command byte";
    EXPECT_EQ(ref_bytes[diff_offset], CMD_GET_NAME) << "the differing byte is not get_name's command byte";
  }
}

TEST(ProtoCommandsProbeBuilders, GeneralInfo3MatchesFixture12CapturedRequest) {
  // Frame 2 of velux_kig300_probe_capability_burst: "48 00 58 6E 36 CA 0A 18 58", a real captured
  // CMD_GET_GENERAL_INFO3 request with CTRL1_LOW_POWER clear.
  expect_builder_matches_capture("velux_kig300_probe_capability_burst", 2,
                                 [](IoFrame &f, const uint8_t *own, const uint8_t *dst) {
                                   return create_general_info3(f, own, dst, /*low_power=*/false);
                                 });
}

// The community fixtures above pin the builders against a capture of *another* hub's output.
// The own-hardware captures below pin them against a capture of *our own* builders' output
// through the probe_device() action -- the difference between "the builder can reproduce a real
// hub's bytes" and "the builder reproduces what we actually put on air".

TEST(ProtoCommandsProbeBuilders, PrivateFunctionMatchesOwnHardwareFn06Probe) {
  expect_builder_matches_capture("somfy_awning_probe_private_fn_lr1121", 0,
                                 [](IoFrame &f, const uint8_t *own, const uint8_t *dst) {
                                   return create_private_function(f, own, dst, /*low_power=*/true, 0x06);
                                 });
}

TEST(ProtoCommandsProbeBuilders, PrivateFunctionMatchesOwnHardwareFn09Probe) {
  expect_builder_matches_capture("somfy_izymo_dimmer_probe_private_fn_lr1121", 2,
                                 [](IoFrame &f, const uint8_t *own, const uint8_t *dst) {
                                   return create_private_function(f, own, dst, /*low_power=*/true, 0x09);
                                 });
}

TEST(ProtoCommandsProbeBuilders, GetStatusExtendedMatchesOwnHardwareBlock00Probe) {
  expect_builder_matches_capture("somfy_awning_probe_status_ext_lr1121", 0,
                                 [](IoFrame &f, const uint8_t *own, const uint8_t *dst) {
                                   return create_get_status_extended(f, own, dst, /*low_power=*/true, 0x80, 0x00);
                                 });
}

TEST(ProtoCommandsProbeBuilders, GetStatusExtendedMatchesOwnHardwareBlock01Probe) {
  expect_builder_matches_capture("somfy_awning_probe_status_ext_lr1121", 2,
                                 [](IoFrame &f, const uint8_t *own, const uint8_t *dst) {
                                   return create_get_status_extended(f, own, dst, /*low_power=*/true, 0x80, 0x01);
                                 });
}

TEST(ProtoCommandsProbeBuilders, GeneralInfo3MatchesOwnHardwareProbe) {
  // low_power=true, unlike GeneralInfo3MatchesFixture12CapturedRequest above -- every probe
  // builder call sends low_power=true (see build_probe_general_info3() in management_actions.cpp).
  expect_builder_matches_capture("somfy_awning_probe_general_info3_lr1121", 0,
                                 [](IoFrame &f, const uint8_t *own, const uint8_t *dst) {
                                   return create_general_info3(f, own, dst, /*low_power=*/true);
                                 });
}

TEST(ProtoCommandsProbeBuilders, Private2ReadMatchesOwnHardwareFavoriteProbe) {
  expect_builder_matches_capture("somfy_awning_probe_private2_favorite_lr1121", 0,
                                 [](IoFrame &f, const uint8_t *own, const uint8_t *dst) {
                                   return create_private2_read(f, own, dst, 0x00, /*long_form=*/true,
                                                               /*low_power=*/true);
                                 });
}

TEST(ProtoCommandsProbeBuilders, Private2ReadMatchesOwnHardwareVentProbe) {
  expect_builder_matches_capture("somfy_awning_probe_private2_vent_lr1121", 0,
                                 [](IoFrame &f, const uint8_t *own, const uint8_t *dst) {
                                   return create_private2_read(f, own, dst, POS_VENT_MODIFIER, /*long_form=*/true,
                                                               /*low_power=*/true);
                                 });
}

// --- Trailing DEFAULTED parameters on the two CMD_PRIVATE builders ---------------------------
// The point of the default is that every existing call site and corpus fixture stays
// byte-identical. The fixture replays above (GetStatusExtendedMatchesFixture11SelectorN0/N1,
// GetStatusExtendedMatchesOwnHardwareBlock00/01Probe, PrivateFunctionMatchesOwnHardwareFn06/09Probe)
// are the stronger half of that guarantee; these two pin the defaulted-vs-explicit equivalence
// directly.

TEST(ProtoCommandsProbeBuilders, GetStatusExtendedDefaultFunctionIdIsByteIdenticalToPreChange) {
  IoFrame implicit_default{}, explicit_default{};
  ASSERT_TRUE(create_get_status_extended(implicit_default, test::OWN_ID, test::DST_ID, /*low_power=*/true, 0x80, 0x01));
  ASSERT_TRUE(create_get_status_extended(explicit_default, test::OWN_ID, test::DST_ID, /*low_power=*/true, 0x80, 0x01,
                                         PRIVATE_GET_POSITION_STATUS));

  uint8_t a[64] = {0}, b[64] = {0};
  uint8_t a_len = serialize(implicit_default, a, sizeof(a));
  uint8_t b_len = serialize(explicit_default, b, sizeof(b));
  ASSERT_EQ(a_len, b_len);
  EXPECT_EQ(0, std::memcmp(a, b, a_len));
}

TEST(ProtoCommandsProbeBuilders, PrivateFunctionDefaultSubIndexIsByteIdenticalToPreChange) {
  IoFrame implicit_default{}, explicit_default{};
  ASSERT_TRUE(create_private_function(implicit_default, test::OWN_ID, test::DST_ID, /*low_power=*/true, 0x09));
  ASSERT_TRUE(create_private_function(explicit_default, test::OWN_ID, test::DST_ID, /*low_power=*/true, 0x09, 0x00));

  uint8_t a[64] = {0}, b[64] = {0};
  uint8_t a_len = serialize(implicit_default, a, sizeof(a));
  uint8_t b_len = serialize(explicit_default, b, sizeof(b));
  ASSERT_EQ(a_len, b_len);
  EXPECT_EQ(0, std::memcmp(a, b, a_len));
}

TEST(ProtoCommandsProbeBuilders, GetStatusExtendedCarriesChosenFunctionId) {
  IoFrame f{};
  ASSERT_TRUE(create_get_status_extended(f, test::OWN_ID, test::DST_ID, /*low_power=*/true, 0x80, 0x00, 0x09));
  EXPECT_EQ(f.cmd, CMD_PRIVATE);
  EXPECT_EQ(f.data_len, 4);
  const uint8_t want_09_00[4] = {0x09, 0x80, 0x00, 0x00};
  EXPECT_EQ(0, std::memcmp(f.data, want_09_00, 4));
  EXPECT_TRUE((f.ctrl1 & CTRL1_LOW_POWER) != 0);

  IoFrame f2{};
  ASSERT_TRUE(create_get_status_extended(f2, test::OWN_ID, test::DST_ID, /*low_power=*/false, 0x80, 0x01, 0x09));
  const uint8_t want_09_01[4] = {0x09, 0x80, 0x01, 0x00};
  EXPECT_EQ(0, std::memcmp(f2.data, want_09_01, 4));
  EXPECT_TRUE((f2.ctrl1 & CTRL1_LOW_POWER) == 0);

  IoFrame f3{};
  ASSERT_TRUE(create_get_status_extended(f3, test::OWN_ID, test::DST_ID, /*low_power=*/true, 0x80, 0x00, 0x06));
  const uint8_t want_06_00[4] = {0x06, 0x80, 0x00, 0x00};
  EXPECT_EQ(0, std::memcmp(f3.data, want_06_00, 4));

  IoFrame f4{};
  ASSERT_TRUE(create_get_status_extended(f4, test::OWN_ID, test::DST_ID, /*low_power=*/true, 0x80, 0x80, 0x06));
  const uint8_t want_06_80[4] = {0x06, 0x80, 0x80, 0x00};
  EXPECT_EQ(0, std::memcmp(f4.data, want_06_80, 4));
}

TEST(ProtoCommandsProbeBuilders, PrivateFunctionCarriesChosenSubIndex) {
  IoFrame f{};
  ASSERT_TRUE(create_private_function(f, test::OWN_ID, test::DST_ID, /*low_power=*/true, 0x09, 0x01));
  EXPECT_EQ(f.cmd, CMD_PRIVATE);
  EXPECT_EQ(f.data_len, 3);
  const uint8_t want[3] = {0x09, 0x01, 0x00};
  EXPECT_EQ(0, std::memcmp(f.data, want, 3));
}
