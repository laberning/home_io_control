/// @file heater_climate_write_test.cpp
/// @brief HeaterController: planned climate writes, the temperature in force after a write and the question a target
/// asks the heater first (HeaterControllerFixture).

#include "heater_controller.h"
#include "payload_frame.h"
#include "reading_store.h"
#include "water_heater_values.h"
#include "decisions_status.h"
#include "proto_codecs.h"
#include "proto_commands.h"
#include "proto_device_readings.h"
#include "proto_dhw.h"
#include "proto_frame.h"
#include "proto_heating.h"
#include "proto_heating_profiles.h"
#include "proto_private_registers.h"

#include "heater_controller_fixture.h"
#include "heater_controller_helpers.h"

#include <array>
#include <cmath>
#include <functional>
#include <vector>

using namespace esphome::home_io_control;
using namespace heater_controller_test;

// === Planned climate writes ===

TEST(HeaterController, APlannedRegisterWriteSendsItsBytesAppliesTheHookThenReadsBack) {
  HeaterControllerFixture fx;
  make_heater(fx);
  fx.queue_register_ack({0x0C, 0x61, 0x01, 0x03, 0xCD, 0x00});
  fx.queue_register_ack({0x0C, 0x60, 0x01, 0x03, 0xCD, 0x00});
  size_t frames_when_accepted = 0;
  int hook_calls = 0;

  const bool accepted =
      fx.controller.execute_heating_plan("ABC123", register_plan(DeviceRegister::COMFORT_SETPOINT, 20.5F), [&] {
        hook_calls++;
        frames_when_accepted = fx.radio.get_sent_data().size();
      });

  EXPECT_TRUE(accepted);
  EXPECT_EQ(hook_calls, 1);
  EXPECT_EQ(frames_when_accepted, 1u) << "the hook runs after the write and before the read-back";
  ASSERT_EQ(fx.radio.get_sent_data().size(), 2u);
  EXPECT_EQ(sent_payload(fx.radio.get_sent_data()[0], 6), (std::vector<uint8_t>{0x0C, 0x61, 0x01, 0x03, 0xCD, 0x00}));
  EXPECT_EQ(fx.radio.get_sent_data()[1][10], 0x60) << "the read-back is a get of the same register";
  EXPECT_EQ(sent_register(fx.radio.get_sent_data()[1]), 0x03);
}

TEST(HeaterController, AReadBackAfterAnAcknowledgedWriteKeepsTheFullRetryBudget) {
  HeaterControllerFixture fx;
  make_heater(fx);
  fx.queue_register_ack({0x0C, 0x61, 0x01, 0x03, 0xCD, 0x00});  // the write is acknowledged, the read-back is not

  EXPECT_TRUE(
      fx.controller.execute_heating_plan("ABC123", register_plan(DeviceRegister::COMFORT_SETPOINT, 20.5F), nullptr));

  const auto &sent = fx.radio.get_sent_data();
  ASSERT_GT(sent.size(), 1u);
  EXPECT_EQ(sent[0][10], 0x61) << "the write goes first";
  size_t read_tries = 0;
  for (size_t i = 1; i < sent.size(); i++) {
    EXPECT_EQ(sent[i][10], 0x60) << "after the write only the read-back is sent";
    read_tries++;
  }
  EXPECT_EQ(read_tries, static_cast<size_t>(EXCHANGE_RETRY_COUNT))
      << "the user waits on it: the full budget, not a poll's " << static_cast<int>(SCHEDULED_POLL_MAX_TRIES);
}

TEST(HeaterControllerHeatingPlan, ARegisterPlanWithoutARegisterSendsNothingAndIsNotAccepted) {
  HeaterControllerFixture fx;
  make_heater(fx);
  HeatingWritePlan plan;
  plan.kind = HeatingPlanKind::REGISTER;
  plan.register_value = 1.0F;
  plan.read_back = true;
  int hook_calls = 0;

  EXPECT_FALSE(fx.controller.execute_heating_plan("ABC123", plan, [&] { hook_calls++; }));

  EXPECT_TRUE(fx.radio.get_sent_data().empty());
  EXPECT_EQ(hook_calls, 0);
}

namespace {

/// A towel dryer in its programme, comfort 19.0 C, holding @p in_force_c (a number marks it reported), whose
/// status read reported @p level.
IoDevice *make_towel_dryer_in_programme(HeaterControllerFixture &fx, float in_force_c, uint8_t level) {
  IoDevice *dev = make_towel_dryer(fx);
  fx.status_reply_level = level;  // the level the next status read answers
  test::store_reading(*dev, DeviceReading::TOWEL_MODE, static_cast<int16_t>(TowelDryerMode::PROG));
  test::store_reading(*dev, DeviceReading::TEMPORARY_STATE, static_cast<int16_t>(HeatingTemporaryState::NONE));
  test::store_reading(*dev, DeviceReading::SETPOINT, 19.0F);
  test::store_reading(*dev, DeviceReading::HEATING_LEVEL, level);
  if (!std::isnan(in_force_c))
    test::store_reading(*dev, DeviceReading::TOWEL_TEMPERATURE_IN_FORCE, in_force_c);
  if (!std::isnan(in_force_c))
    test::set_in_force_support(*dev, InForceSupport::REPORTED);
  return dev;
}

/// A radiator running its programme in @p mode, comfort 19.0 C, holding @p in_force_c (a number marks it
/// reported).
IoDevice *make_radiator_in_programme(HeaterControllerFixture &fx, HeatingMode mode, float in_force_c) {
  IoDevice *dev = make_heater(fx);
  test::store_reading(*dev, DeviceReading::MODE, static_cast<int16_t>(mode));
  test::store_reading(*dev, DeviceReading::SETPOINT, 19.0F);
  if (!std::isnan(in_force_c))
    test::store_reading(*dev, DeviceReading::TEMPERATURE_IN_FORCE, in_force_c);
  if (!std::isnan(in_force_c))
    test::set_in_force_support(*dev, InForceSupport::REPORTED);
  return dev;
}

/// An override of @p value written to the override register with its read-back: a plan, not a decision, so the
/// tests of execute_heating_plan() do not depend on any stored reading.
HeatingWritePlan override_plan(float value) {
  HeatingWritePlan plan = register_plan(DeviceRegister::DEROGATED_SETPOINT, value);
  plan.temporary_override = true;
  return plan;
}
/// The registers of the frames sent since @p before frames.
std::vector<uint8_t> sent_registers_since(const HeaterControllerFixture &fx, size_t before) {
  std::vector<uint8_t> registers = sent_registers(fx);
  registers.erase(registers.begin(), registers.begin() + static_cast<std::ptrdiff_t>(before));
  return registers;
}

/// Whether the frame is a write (`0x61`) and not a read.
bool is_write_frame(const std::vector<uint8_t> &tx) { return tx.at(10) == 0x61; }

/// The writes a fixture's radio carried, as register numbers.
std::vector<uint8_t> written_registers(const HeaterControllerFixture &fx) {
  std::vector<uint8_t> registers;
  for (const auto &frame : fx.radio.get_sent_data()) {
    if (is_write_frame(frame))
      registers.push_back(sent_register(frame));
  }
  return registers;
}

/// What the climate entity does for a target: ask the heater what it runs, plan from the record, and send.
struct TargetOutcome {
  bool state_answered{false};  ///< The heater answered the mode (and the timed run where it matters).
  HeatingWritePlan plan;       ///< What the target became.
  bool sent{false};            ///< The write was sent and acknowledged.
};

TargetOutcome set_target(HeaterControllerFixture &fx, float target_c) {
  TargetOutcome outcome;
  outcome.state_answered = fx.controller.read_programme_state("ABC123");
  const IoDevice *dev = fx.registry.get("ABC123");
  outcome.plan =
      plan_heating_write(*find_heating_profile(*dev), {.kind = HeatingWriteKind::TARGET, .target_c = target_c},
                         heating_device_facts(dev, outcome.state_answered));
  outcome.sent = outcome.plan.is_write() && fx.controller.execute_heating_plan("ABC123", outcome.plan, nullptr);
  return outcome;
}

}  // namespace

TEST(HeaterController, AnOverrideInAnEcoOrAComfortPeriodSendsTheOverrideRegisterAndShowsTheNewTargetAtTheReadBack) {
  for (const uint8_t level : {heating_level::ECO, heating_level::COMFORT}) {
    HeaterControllerFixture fx;
    IoDevice *dev = make_towel_dryer_in_programme(fx, level == heating_level::ECO ? 15.5F : 19.0F, level);
    const HeatingProfile &profile = *find_heating_profile(*dev);
    float seen = NAN;
    fx.registry.subscribe([&seen](const std::string &, const IoDevice &d) {
      seen = test::number_read(d, DeviceReading::TOWEL_TEMPERATURE_IN_FORCE);
    });
    queue_register_write_and_read_back(fx, 0x1D, 0xAF, 0xAF);

    ASSERT_TRUE(fx.controller.execute_heating_plan("ABC123", override_plan(17.5F), nullptr)) << "level " << +level;

    ASSERT_EQ(fx.radio.get_sent_data().size(), 2u);
    EXPECT_EQ(sent_payload(fx.radio.get_sent_data()[0], 6), (std::vector<uint8_t>{0x0C, 0x61, 0x01, 0x1D, 0xAF, 0x00}));
    EXPECT_EQ(sent_register(fx.radio.get_sent_data()[1]), 0x1D) << "the read-back is of the override register";
    for (const auto &frame : fx.radio.get_sent_data())
      EXPECT_NE(sent_register(frame), 0x03) << "the comfort setpoint is never written";
    EXPECT_FLOAT_EQ(seen, 17.5F) << "the card shows the new target before the next poll";
    EXPECT_FLOAT_EQ(test::number_read(*dev, DeviceReading::SETPOINT), 19.0F) << "the comfort setpoint is untouched";

    // The next poll's 0x37 replaces the shown value.
    fx.queue_register_ack({0x0C, 0x60, 0x01, 0x37, 0xAA, 0x00});
    ASSERT_TRUE(fx.controller.read_heating_register("ABC123", profile.programme_period->in_force_fn, nullptr,
                                                    EXCHANGE_RETRY_COUNT));
    EXPECT_FLOAT_EQ(test::number_read(*dev, DeviceReading::TOWEL_TEMPERATURE_IN_FORCE), 17.0F)
        << "what the dryer reports replaces the shown value";
  }
}

TEST(HeaterController, AnOverrideReadBackNotifiesOnceWithTheTemperatureInForceStored) {
  HeaterControllerFixture fx;
  make_towel_dryer_in_programme(fx, 15.5F, heating_level::ECO);
  std::vector<float> shown;
  fx.registry.subscribe([&shown](const std::string &, const IoDevice &d) {
    shown.push_back(test::number_read(d, DeviceReading::TOWEL_TEMPERATURE_IN_FORCE));
  });
  queue_register_write_and_read_back(fx, 0x1D, 0xAF, 0xAF);

  ASSERT_TRUE(fx.controller.execute_heating_plan("ABC123", override_plan(17.5F), nullptr));

  ASSERT_EQ(shown.size(), 2u) << "one notification for the write, one for the read-back";
  EXPECT_FLOAT_EQ(shown.back(), 17.5F) << "the read-back notifies after the temperature in force is stored";
}

// === The temperature in force is read once after a write that starts the programme (display only) ===

namespace {

/// Whether a device reports its temperature in force, as one of the stored readings of a heater's record.
/// The temperature in force the heater's profile reads from, whichever register that is.
float stored_in_force(const IoDevice &dev) {
  return test::number_read(dev, find_heating_profile(dev)->programme_period->in_force_fn);
}

}  // namespace

TEST(HeaterController, AModeWriteIntoTheProgrammeIsReadBackThenTheTemperatureInForceIsReadOnce) {
  struct Case {
    bool towel;
    uint8_t mode_register;
    uint8_t in_force_register;
    uint8_t from_wire;
    uint8_t to_wire;
  };
  for (const Case &c :
       {Case{true, DRYER_MODE, DRYER_IN_FORCE, DRYER_HEAT, DRYER_PROGRAMME},
        Case{false, RADIATOR_MODE, RADIATOR_IN_FORCE, RADIATOR_HEAT, static_cast<uint8_t>(HeatingMode::AUTO)}}) {
    HeaterControllerFixture fx;
    IoDevice *dev = c.towel ? make_towel_dryer_in_programme(fx, NAN, heating_level::COMFORT)
                            : make_radiator_in_programme(fx, HeatingMode::MANUAL, NAN);
    if (c.towel)
      test::store_reading(*dev, DeviceReading::TOWEL_MODE, c.from_wire);
    const DeviceRegister mode_fn = *find_heating_profile(*dev)->mode_fn;
    fx.queue_register_ack({0x0C, 0x61, 0x01, c.mode_register, c.to_wire});
    fx.queue_register_ack({0x0C, 0x60, 0x01, c.mode_register, c.to_wire});
    queue_read_reply(fx, c.in_force_register, {0xAF, 0x00});  // 17.5 C
    ASSERT_EQ(test::in_force_of(*dev), InForceSupport::UNKNOWN);

    ASSERT_TRUE(fx.controller.execute_heating_plan("ABC123", register_plan(mode_fn, c.to_wire), nullptr));

    EXPECT_EQ(sent_registers(fx), (std::vector<uint8_t>{c.mode_register, c.mode_register, c.in_force_register}))
        << "write, read-back, one read of the temperature in force, towel " << c.towel;
    EXPECT_FLOAT_EQ(stored_in_force(*dev), 17.5F);
    EXPECT_EQ(test::in_force_of(*dev), InForceSupport::REPORTED);
  }
}

TEST(HeaterController, AWriteThatDoesNotStartTheProgrammeReadsNoTemperatureInForce) {
  // A mode write into heat, a boost started on the dryer: the read-back shows no programme running.
  {
    HeaterControllerFixture fx;
    IoDevice *dev = make_radiator_in_programme(fx, HeatingMode::PROG, 17.0F);
    queue_register_write_and_read_back(fx, RADIATOR_MODE, RADIATOR_HEAT, RADIATOR_HEAT);
    ASSERT_TRUE(fx.controller.execute_heating_plan(
        "ABC123", register_plan(*find_heating_profile(*dev)->mode_fn, RADIATOR_HEAT), nullptr));
    EXPECT_EQ(sent_registers(fx), (std::vector<uint8_t>{RADIATOR_MODE, RADIATOR_MODE}));
  }
  {
    HeaterControllerFixture fx;
    make_towel_dryer_in_programme(fx, NAN, heating_level::COMFORT);
    queue_register_write_and_read_back(fx, DRYER_MODE, DRYER_HEAT, DRYER_HEAT);
    ASSERT_TRUE(
        fx.controller.execute_heating_plan("ABC123", register_plan(DeviceRegister::TOWEL_MODE, DRYER_HEAT), nullptr));
    EXPECT_EQ(sent_registers(fx), (std::vector<uint8_t>{DRYER_MODE, DRYER_MODE}));
  }
  {
    HeaterControllerFixture fx;
    make_towel_dryer_in_programme(fx, NAN, heating_level::COMFORT);
    queue_register_write_and_read_back(fx, DRYER_TIMED_RUN, TIMED_BOOST, TIMED_BOOST);
    ASSERT_TRUE(fx.controller.execute_heating_plan(
        "ABC123", register_plan(DeviceRegister::TEMPORARY_STATE, TIMED_BOOST), nullptr));
    EXPECT_EQ(sent_registers(fx), (std::vector<uint8_t>{DRYER_TIMED_RUN, DRYER_TIMED_RUN}))
        << "a boost on top of the programme is not the programme running";
  }
}

TEST(HeaterController, ADryerBoostEndedWhileTheModeShowsTheProgrammeReadsTheTemperatureInForce) {
  HeaterControllerFixture fx;
  IoDevice *dev = make_towel_dryer_in_programme(fx, NAN, heating_level::COMFORT);
  test::store_reading(*dev, DeviceReading::TEMPORARY_STATE, TIMED_BOOST);
  queue_register_write_and_read_back(fx, DRYER_TIMED_RUN, NO_TIMED_RUN, NO_TIMED_RUN);
  queue_read_reply(fx, DRYER_IN_FORCE, {0xAF, 0x00});

  ASSERT_TRUE(fx.controller.execute_heating_plan("ABC123", register_plan(DeviceRegister::TEMPORARY_STATE, NO_TIMED_RUN),
                                                 nullptr));

  EXPECT_EQ(sent_registers(fx), (std::vector<uint8_t>{DRYER_TIMED_RUN, DRYER_TIMED_RUN, DRYER_IN_FORCE}));
  EXPECT_FLOAT_EQ(stored_in_force(*dev), 17.5F);
}

TEST(HeaterController, ASilentModeReadBackReadsNoTemperatureInForce) {
  // The stored mode already shows the programme, so only the missing read-back keeps the read away.
  HeaterControllerFixture fx;
  IoDevice *dev = make_radiator_in_programme(fx, HeatingMode::PROG, NAN);
  fx.queue_register_ack({0x0C, 0x61, 0x01, RADIATOR_MODE, static_cast<uint8_t>(HeatingMode::AUTO)});  // no read-back

  ASSERT_TRUE(fx.controller.execute_heating_plan(
      "ABC123", register_plan(*find_heating_profile(*dev)->mode_fn, static_cast<float>(HeatingMode::AUTO)), nullptr));

  for (const uint8_t reg : sent_registers(fx))
    EXPECT_EQ(reg, RADIATOR_MODE) << "the write and its read-back attempts only";
}

TEST(HeaterController, AHeaterWithoutATemperatureInForceIsNotAskedAfterAModeWrite) {
  HeaterControllerFixture fx;
  IoDevice *dev = make_radiator_in_programme(fx, HeatingMode::MANUAL, NAN);
  test::set_in_force_support(*dev, InForceSupport::ABSENT);
  const uint8_t auto_wire = static_cast<uint8_t>(HeatingMode::AUTO);
  queue_register_write_and_read_back(fx, RADIATOR_MODE, auto_wire, auto_wire);

  ASSERT_TRUE(fx.controller.execute_heating_plan(
      "ABC123", register_plan(*find_heating_profile(*dev)->mode_fn, auto_wire), nullptr));

  EXPECT_EQ(sent_registers(fx), (std::vector<uint8_t>{RADIATOR_MODE, RADIATOR_MODE}));
}

TEST(HeaterController, AProfileWithoutAProgrammeIsNotAskedAfterAWrite) {
  // A heating-level heater has no programme period: its plan has no register read-back and nothing is read.
  HeaterControllerFixture fx;
  ASSERT_NE(fx.add_device_of_kind(DeviceType::ELECTRICAL_HEATER, 0), nullptr);
  ASSERT_EQ(find_heating_profile(*fx.registry.get("ABC123"))->programme_period, nullptr);
  HeatingWritePlan plan;
  plan.kind = HeatingPlanKind::LEVEL;
  plan.level = heating_level::ECO;

  EXPECT_TRUE(fx.controller.execute_heating_plan("ABC123", plan, nullptr));

  EXPECT_TRUE(fx.radio.get_sent_data().empty()) << "the level is an execute, and no register is read";
}

TEST(HeaterController, ASilentTemperatureInForceReadAfterAModeWriteChangesNothingElse) {
  HeaterControllerFixture fx;
  IoDevice *dev = make_radiator_in_programme(fx, HeatingMode::MANUAL, NAN);
  const uint8_t auto_wire = static_cast<uint8_t>(HeatingMode::AUTO);
  queue_register_write_and_read_back(fx, RADIATOR_MODE, auto_wire, auto_wire);  // the in-force read gets no reply

  ASSERT_TRUE(fx.controller.execute_heating_plan(
      "ABC123", register_plan(*find_heating_profile(*dev)->mode_fn, auto_wire), nullptr));

  const std::vector<uint8_t> registers = sent_registers(fx);
  ASSERT_GE(registers.size(), 3u);
  EXPECT_EQ(registers[2], RADIATOR_IN_FORCE);
  EXPECT_EQ(test::in_force_of(*dev), InForceSupport::UNKNOWN) << "a missed read says nothing";
  EXPECT_TRUE(std::isnan(stored_in_force(*dev)));
  EXPECT_EQ(test::wire_read(*dev, DeviceReading::MODE), auto_wire) << "the mode the read-back stored stays";
  EXPECT_TRUE(written_registers(fx) == std::vector<uint8_t>{RADIATOR_MODE}) << "nothing else is written";
}

TEST(HeaterController, AnEmptyTemperatureInForceAfterAWriteLeavesTheTargetsOwnReadToDecide) {
  // The display read goes by the stored readings, and on a dryer one of the two registers that say "the programme
  // runs" was not read with the write: the heater may run something else and answer the register empty. Only the
  // target's own read, made on fresh readings, may mark the heater as having no temperature in force.
  struct Case {
    const char *what;
    uint8_t written_register;
    uint8_t written_wire;
    uint8_t stored_mode;
    uint8_t stored_timed_run;
  };
  for (const Case &c :
       {Case{"mode written, stored timed run stale", DRYER_MODE, DRYER_PROGRAMME, DRYER_HEAT, NO_TIMED_RUN},
        Case{"boost ended, stored mode stale", DRYER_TIMED_RUN, NO_TIMED_RUN, DRYER_PROGRAMME, TIMED_BOOST}}) {
    HeaterControllerFixture fx;
    IoDevice *dev = make_towel_dryer_in_programme(fx, NAN, heating_level::ECO);
    test::store_reading(*dev, DeviceReading::TOWEL_MODE, c.stored_mode);
    test::store_reading(*dev, DeviceReading::TEMPORARY_STATE, c.stored_timed_run);
    const DeviceRegister fn =
        c.written_register == DRYER_MODE ? DeviceRegister::TOWEL_MODE : DeviceRegister::TEMPORARY_STATE;
    queue_register_write_and_read_back(fx, c.written_register, c.written_wire, c.written_wire);
    queue_read_reply(fx, DRYER_IN_FORCE, {});  // answered, no value

    ASSERT_TRUE(fx.controller.execute_heating_plan("ABC123", register_plan(fn, c.written_wire), nullptr)) << c.what;
    ASSERT_EQ(sent_registers(fx).back(), DRYER_IN_FORCE) << c.what;
    EXPECT_EQ(test::in_force_of(*dev), InForceSupport::UNKNOWN) << c.what;

    // The next target asks the heater, which now runs its programme and reports the temperature in force.
    const size_t before = fx.radio.get_sent_data().size();
    queue_read_reply(fx, DRYER_MODE, {DRYER_PROGRAMME});
    queue_read_reply(fx, DRYER_TIMED_RUN, {NO_TIMED_RUN});
    queue_read_reply(fx, DRYER_IN_FORCE, {0xAF, 0x00});
    queue_register_write_and_read_back(fx, OVERRIDE_REGISTER, 0xD2, 0xD2);

    EXPECT_TRUE(set_target(fx, 21.0F).sent) << c.what;

    EXPECT_EQ(sent_registers_since(fx, before),
              (std::vector<uint8_t>{DRYER_MODE, DRYER_TIMED_RUN, DRYER_IN_FORCE, OVERRIDE_REGISTER, OVERRIDE_REGISTER}))
        << c.what;
    EXPECT_EQ(test::in_force_of(*dev), InForceSupport::REPORTED) << c.what;
  }
}

TEST(HeaterController, AnOverrideWriteReadsNoTemperatureInForceOfItsOwn) {
  // Its read-back already stores the value in force.
  HeaterControllerFixture fx;
  IoDevice *dev = make_radiator_in_programme(fx, HeatingMode::PROG, 17.0F);
  queue_register_write_and_read_back(fx, OVERRIDE_REGISTER, 0xAF, 0xAF);

  ASSERT_TRUE(fx.controller.execute_heating_plan("ABC123", override_plan(17.5F), nullptr));

  EXPECT_EQ(sent_registers(fx), (std::vector<uint8_t>{OVERRIDE_REGISTER, OVERRIDE_REGISTER}));
  EXPECT_FLOAT_EQ(stored_in_force(*dev), 17.5F);
}

// === A target asks the heater what it runs first ===

TEST(HeaterController, ATargetOnAProgrammeHeaterSendsTheSameExchangesEveryTimeWhateverTheStoredReadingsSay) {
  // Radiator (mode 0x00, temperature in force 0x2A) and towel dryer (mode 0x36, timed run 0x3E, temperature in
  // force 0x37). R = read, W = write.
  struct Case {
    const char *what;
    bool towel;
    InForceSupport support;
    std::vector<std::vector<uint8_t>> replies;  // to the reads, in order: register, value bytes
    std::vector<uint8_t> sequence;              // registers, in the order they are sent
    std::vector<uint8_t> writes;                // the registers written
  };
  const std::vector<uint8_t> in_force = {0xAF, 0x00};  // 17.5 C
  const std::vector<Case> cases = {
      {"radiator in heat: R mode, W+R comfort",
       false,
       InForceSupport::UNKNOWN,
       {{RADIATOR_MODE, RADIATOR_HEAT}},
       {RADIATOR_MODE, COMFORT_REGISTER, COMFORT_REGISTER},
       {COMFORT_REGISTER}},
      {"radiator in the programme, reported: R mode, W+R override",
       false,
       InForceSupport::REPORTED,
       {{RADIATOR_MODE, RADIATOR_PROGRAMME}},
       {RADIATOR_MODE, OVERRIDE_REGISTER, OVERRIDE_REGISTER},
       {OVERRIDE_REGISTER}},
      {"radiator in the programme, unknown: R mode, R in force, W+R override",
       false,
       InForceSupport::UNKNOWN,
       {{RADIATOR_MODE, RADIATOR_PROGRAMME}, {RADIATOR_IN_FORCE, 0xAF, 0x00}},
       {RADIATOR_MODE, RADIATOR_IN_FORCE, OVERRIDE_REGISTER, OVERRIDE_REGISTER},
       {OVERRIDE_REGISTER}},
      {"radiator in the programme, absent: R mode, W+R comfort",
       false,
       InForceSupport::ABSENT,
       {{RADIATOR_MODE, RADIATOR_PROGRAMME}},
       {RADIATOR_MODE, COMFORT_REGISTER, COMFORT_REGISTER},
       {COMFORT_REGISTER}},
      {"dryer in heat: R mode, W+R comfort",
       true,
       InForceSupport::UNKNOWN,
       {{DRYER_MODE, DRYER_HEAT}},
       {DRYER_MODE, COMFORT_REGISTER, COMFORT_REGISTER},
       {COMFORT_REGISTER}},
      {"dryer in the programme, reported: R mode, R timed run, W+R override",
       true,
       InForceSupport::REPORTED,
       {{DRYER_MODE, DRYER_PROGRAMME}, {DRYER_TIMED_RUN, NO_TIMED_RUN}},
       {DRYER_MODE, DRYER_TIMED_RUN, OVERRIDE_REGISTER, OVERRIDE_REGISTER},
       {OVERRIDE_REGISTER}},
      {"dryer in the programme, unknown: R mode, R timed run, R in force, W+R override",
       true,
       InForceSupport::UNKNOWN,
       {{DRYER_MODE, DRYER_PROGRAMME}, {DRYER_TIMED_RUN, NO_TIMED_RUN}, {DRYER_IN_FORCE, 0xAF, 0x00}},
       {DRYER_MODE, DRYER_TIMED_RUN, DRYER_IN_FORCE, OVERRIDE_REGISTER, OVERRIDE_REGISTER},
       {OVERRIDE_REGISTER}},
      {"dryer in the programme, absent: R mode, R timed run, W+R comfort",
       true,
       InForceSupport::ABSENT,
       {{DRYER_MODE, DRYER_PROGRAMME}, {DRYER_TIMED_RUN, NO_TIMED_RUN}},
       {DRYER_MODE, DRYER_TIMED_RUN, COMFORT_REGISTER, COMFORT_REGISTER},
       {COMFORT_REGISTER}},
      {"dryer in the programme with a boost on top: R mode, R timed run, W+R comfort",
       true,
       InForceSupport::REPORTED,
       {{DRYER_MODE, DRYER_PROGRAMME}, {DRYER_TIMED_RUN, TIMED_BOOST}},
       {DRYER_MODE, DRYER_TIMED_RUN, COMFORT_REGISTER, COMFORT_REGISTER},
       {COMFORT_REGISTER}},
      {"dryer in the programme with a boost on top, unknown: R mode, R timed run, W+R comfort (no R in force)",
       true,
       InForceSupport::UNKNOWN,
       {{DRYER_MODE, DRYER_PROGRAMME}, {DRYER_TIMED_RUN, TIMED_BOOST}},
       {DRYER_MODE, DRYER_TIMED_RUN, COMFORT_REGISTER, COMFORT_REGISTER},
       {COMFORT_REGISTER}},
  };
  for (const Case &c : cases) {
    // Whatever the hub stored (heat, or the programme with a boost on a dryer), the heater's answer decides.
    for (const bool stored_in_heat : {false, true}) {
      HeaterControllerFixture fx;
      IoDevice *dev = c.towel ? make_towel_dryer_in_programme(fx, NAN, heating_level::ECO)
                              : make_radiator_in_programme(fx, HeatingMode::MANUAL, NAN);
      test::set_in_force_support(*dev, c.support);
      if (c.support == InForceSupport::REPORTED)
        test::store_reading(*dev, DeviceReading::TOWEL_TEMPERATURE_IN_FORCE, 17.0F);
      if (c.towel) {
        test::store_reading(*dev, DeviceReading::TOWEL_MODE, stored_in_heat ? DRYER_HEAT : DRYER_PROGRAMME);
        test::store_reading(*dev, DeviceReading::TEMPORARY_STATE, stored_in_heat ? TIMED_BOOST : NO_TIMED_RUN);
      } else {
        test::store_reading(*dev, DeviceReading::MODE, stored_in_heat ? RADIATOR_HEAT : RADIATOR_PROGRAMME);
      }
      for (const auto &reply : c.replies)
        queue_read_reply(fx, reply[0], std::vector<uint8_t>(reply.begin() + 1, reply.end()));
      fx.queue_register_ack({0x0C, 0x61, 0x01, c.writes.front(), 0xD2, 0x00});
      fx.queue_register_ack({0x0C, 0x60, 0x01, c.writes.front(), 0xD2, 0x00});

      const TargetOutcome outcome = set_target(fx, 21.0F);

      EXPECT_TRUE(outcome.sent) << c.what << ", stored " << stored_in_heat;
      EXPECT_EQ(sent_registers(fx), c.sequence) << c.what << ", stored " << stored_in_heat;
      EXPECT_EQ(written_registers(fx), c.writes) << c.what;
    }
  }
}

TEST(HeaterController, AHeaterNowInTheProgrammeTakesTheTargetAsAnOverrideWhateverTheHubStoredBefore) {
  // Stored: heat. The heater runs its programme (set on its panel since the last poll).
  for (const bool towel : {false, true}) {
    HeaterControllerFixture fx;
    IoDevice *dev = towel ? make_towel_dryer_in_programme(fx, 17.0F, heating_level::ECO)
                          : make_radiator_in_programme(fx, HeatingMode::PROG, 17.0F);
    if (towel)
      test::store_reading(*dev, DeviceReading::TOWEL_MODE, DRYER_HEAT);
    else
      test::store_reading(*dev, DeviceReading::MODE, RADIATOR_HEAT);
    queue_read_reply(fx, towel ? DRYER_MODE : RADIATOR_MODE, {towel ? DRYER_PROGRAMME : RADIATOR_PROGRAMME});
    if (towel)
      queue_read_reply(fx, DRYER_TIMED_RUN, {NO_TIMED_RUN});
    queue_register_write_and_read_back(fx, OVERRIDE_REGISTER, 0xD2, 0xD2);

    const TargetOutcome outcome = set_target(fx, 21.0F);

    EXPECT_TRUE(outcome.sent) << "towel " << towel;
    EXPECT_TRUE(outcome.plan.temporary_override);
    EXPECT_EQ(written_registers(fx), std::vector<uint8_t>{OVERRIDE_REGISTER}) << "the comfort setpoint is not written";
    EXPECT_FLOAT_EQ(test::number_read(*dev, DeviceReading::SETPOINT), 19.0F);
  }
}

TEST(HeaterController, AHeaterNowInHeatTakesTheTargetAsTheComfortSetpointWhateverTheHubStoredBefore) {
  // Stored: the programme, with a reported temperature in force. The heater was set to heat.
  for (const bool towel : {false, true}) {
    HeaterControllerFixture fx;
    IoDevice *dev = towel ? make_towel_dryer_in_programme(fx, 17.0F, heating_level::COMFORT)
                          : make_radiator_in_programme(fx, HeatingMode::PROG, 17.0F);
    queue_read_reply(fx, towel ? DRYER_MODE : RADIATOR_MODE, {towel ? DRYER_HEAT : RADIATOR_HEAT});
    queue_register_write_and_read_back(fx, COMFORT_REGISTER, 0xD2, 0xD2);

    const TargetOutcome outcome = set_target(fx, 21.0F);

    EXPECT_TRUE(outcome.sent) << "towel " << towel;
    EXPECT_FALSE(outcome.plan.temporary_override);
    EXPECT_EQ(written_registers(fx), std::vector<uint8_t>{COMFORT_REGISTER});
    EXPECT_FLOAT_EQ(test::number_read(*dev, DeviceReading::SETPOINT), 21.0F);
  }
}

TEST(HeaterController, ADryerThatStoredABoostButAnswersNoTimedRunTakesTheTargetAsAnOverride) {
  HeaterControllerFixture fx;
  IoDevice *dev = make_towel_dryer_in_programme(fx, 17.0F, heating_level::ECO);
  test::store_reading(*dev, DeviceReading::TEMPORARY_STATE, static_cast<int16_t>(HeatingTemporaryState::BOOST));
  queue_read_reply(fx, DRYER_MODE, {DRYER_PROGRAMME});
  queue_read_reply(fx, DRYER_TIMED_RUN, {NO_TIMED_RUN});
  queue_register_write_and_read_back(fx, OVERRIDE_REGISTER, 0xD2, 0xD2);

  EXPECT_TRUE(set_target(fx, 21.0F).sent);

  EXPECT_EQ(written_registers(fx), std::vector<uint8_t>{OVERRIDE_REGISTER});
  EXPECT_EQ(test::wire_read(*dev, DeviceRegister::TEMPORARY_STATE), static_cast<int16_t>(HeatingTemporaryState::NONE));
}

TEST(HeaterController, ADryerThatStoredTheProgrammeButAnswersABoostTakesTheTargetAsTheComfortSetpoint) {
  HeaterControllerFixture fx;
  make_towel_dryer_in_programme(fx, 17.0F, heating_level::COMFORT);
  queue_read_reply(fx, DRYER_MODE, {DRYER_PROGRAMME});
  queue_read_reply(fx, DRYER_TIMED_RUN, {TIMED_BOOST});
  queue_register_write_and_read_back(fx, COMFORT_REGISTER, 0xD2, 0xD2);

  EXPECT_TRUE(set_target(fx, 21.0F).sent);

  EXPECT_EQ(written_registers(fx), std::vector<uint8_t>{COMFORT_REGISTER});
}

TEST(HeaterController, ATargetOnAHeaterNeverReadInThisBootAsksTheHeaterAndFollowsItsAnswer) {
  for (const bool towel : {false, true}) {
    HeaterControllerFixture fx;
    IoDevice *dev = towel ? make_towel_dryer(fx) : make_heater(fx);
    fx.status_reply_level = heating_level::ECO;
    ASSERT_EQ(test::wire_read(*dev, towel ? DeviceRegister::TOWEL_MODE : DeviceRegister::OPERATING_MODE), -1);
    queue_read_reply(fx, towel ? DRYER_MODE : RADIATOR_MODE, {towel ? DRYER_PROGRAMME : RADIATOR_PROGRAMME});
    if (towel)
      queue_read_reply(fx, DRYER_TIMED_RUN, {NO_TIMED_RUN});
    queue_read_reply(fx, towel ? DRYER_IN_FORCE : RADIATOR_IN_FORCE, {0xAF, 0x00});
    queue_register_write_and_read_back(fx, OVERRIDE_REGISTER, 0xD2, 0xD2);

    EXPECT_TRUE(set_target(fx, 21.0F).sent) << "towel " << towel;

    EXPECT_EQ(written_registers(fx), std::vector<uint8_t>{OVERRIDE_REGISTER});
  }
}

TEST(HeaterController, AModeWriteWhoseAcknowledgementWasLostIsFollowedByATargetThatAsksTheHeater) {
  // The write reached the heater (it runs its programme now) but its acknowledgement did not arrive.
  HeaterControllerFixture fx;
  IoDevice *dev = make_radiator_in_programme(fx, HeatingMode::MANUAL, 17.0F);
  EXPECT_FALSE(fx.controller.execute_heating_plan(
      "ABC123", register_plan(DeviceRegister::OPERATING_MODE, static_cast<float>(HeatingMode::AUTO)), nullptr));
  const size_t before = fx.radio.get_sent_data().size();
  queue_read_reply(fx, RADIATOR_MODE, {static_cast<uint8_t>(HeatingMode::AUTO)});
  queue_register_write_and_read_back(fx, OVERRIDE_REGISTER, 0xD2, 0xD2);

  EXPECT_TRUE(set_target(fx, 21.0F).sent);

  EXPECT_EQ(sent_registers_since(fx, before),
            (std::vector<uint8_t>{RADIATOR_MODE, OVERRIDE_REGISTER, OVERRIDE_REGISTER}));
  EXPECT_FLOAT_EQ(test::number_read(*dev, DeviceReading::SETPOINT), 19.0F) << "the comfort setpoint is not written";
}

TEST(HeaterController, AModeSetWithTheActionIsFollowedByATargetThatAsksTheHeater) {
  // The heating_control action writes the mode and reads nothing back.
  HeaterControllerFixture fx;
  IoDevice *dev = make_radiator_in_programme(fx, HeatingMode::MANUAL, 17.0F);
  fx.queue_register_ack({0x0C, 0x61, 0x01, 0x00, static_cast<uint8_t>(HeatingMode::PROG)});
  ASSERT_TRUE(fx.controller.send_heating_command("ABC123", DeviceRegister::OPERATING_MODE,
                                                 static_cast<float>(HeatingMode::PROG)));
  ASSERT_EQ(fx.radio.get_sent_data().size(), 1u) << "the write alone";
  const size_t before = fx.radio.get_sent_data().size();
  queue_read_reply(fx, RADIATOR_MODE, {RADIATOR_PROGRAMME});
  queue_register_write_and_read_back(fx, OVERRIDE_REGISTER, 0xD2, 0xD2);

  EXPECT_TRUE(set_target(fx, 21.0F).sent);

  EXPECT_EQ(sent_registers_since(fx, before),
            (std::vector<uint8_t>{RADIATOR_MODE, OVERRIDE_REGISTER, OVERRIDE_REGISTER}));
  EXPECT_FLOAT_EQ(test::number_read(*dev, DeviceReading::SETPOINT), 19.0F);
}

TEST(HeaterController, AModeAndATargetInOneCallReadTheTemperatureInForceOnceAndTheModeAgainBeforeTheTarget) {
  // The mode write's read-back, the display read of the temperature in force, then the target's own read of the
  // mode. The target's rule reads the temperature in force only while it is UNKNOWN: the display read answered
  // it, so it is not sent twice.
  HeaterControllerFixture fx;
  make_radiator_in_programme(fx, HeatingMode::MANUAL, NAN);
  const uint8_t auto_wire = static_cast<uint8_t>(HeatingMode::AUTO);
  fx.queue_register_ack({0x0C, 0x61, 0x01, RADIATOR_MODE, auto_wire});
  fx.queue_register_ack({0x0C, 0x60, 0x01, RADIATOR_MODE, auto_wire});
  queue_read_reply(fx, RADIATOR_IN_FORCE, {0xAF, 0x00});
  queue_read_reply(fx, RADIATOR_MODE, {auto_wire});
  queue_register_write_and_read_back(fx, OVERRIDE_REGISTER, 0xD2, 0xD2);

  ASSERT_TRUE(fx.controller.execute_heating_plan(
      "ABC123", register_plan(DeviceRegister::OPERATING_MODE, static_cast<float>(HeatingMode::AUTO)), nullptr));
  EXPECT_TRUE(set_target(fx, 21.0F).sent);

  EXPECT_EQ(sent_registers(fx), (std::vector<uint8_t>{RADIATOR_MODE, RADIATOR_MODE, RADIATOR_IN_FORCE, RADIATOR_MODE,
                                                      OVERRIDE_REGISTER, OVERRIDE_REGISTER}));
}

TEST(HeaterController, AHeaterThatDoesNotAnswerTheModeReadGetsNothingWrittenAndNothingChanges) {
  for (const bool towel : {false, true}) {
    HeaterControllerFixture fx;
    IoDevice *dev = towel ? make_towel_dryer_in_programme(fx, 17.0F, heating_level::COMFORT)
                          : make_radiator_in_programme(fx, HeatingMode::PROG, 17.0F);
    const IoDevice before = *dev;

    const TargetOutcome outcome = set_target(fx, 21.0F);  // no reply

    EXPECT_FALSE(outcome.state_answered) << "towel " << towel;
    EXPECT_EQ(outcome.plan.refusal, HeatingRefusal::PROGRAMME_STATE_UNANSWERED);
    EXPECT_FALSE(outcome.sent);
    EXPECT_FALSE(sent_registers(fx).empty());
    for (const uint8_t reg : sent_registers(fx))
      EXPECT_EQ(reg, towel ? DRYER_MODE : RADIATOR_MODE) << "only the mode is asked, and nothing is written";
    EXPECT_TRUE(written_registers(fx).empty());
    EXPECT_EQ(test::wire_read(*dev, DeviceReading::MODE), test::wire_read(before, DeviceReading::MODE))
        << "the stored reading stays as it was";
    EXPECT_EQ(test::wire_read(*dev, DeviceReading::TOWEL_MODE), test::wire_read(before, DeviceReading::TOWEL_MODE));
    EXPECT_EQ(test::in_force_of(*dev), test::in_force_of(before));
    EXPECT_FLOAT_EQ(stored_in_force(*dev), stored_in_force(before));
  }
}

TEST(HeaterController, ADryerInItsProgrammeWhoseTimedRunIsNotAnsweredGetsNothingWritten) {
  HeaterControllerFixture fx;
  make_towel_dryer_in_programme(fx, 17.0F, heating_level::COMFORT);
  queue_read_reply(fx, DRYER_MODE, {DRYER_PROGRAMME});  // the timed-run read gets no reply

  const TargetOutcome outcome = set_target(fx, 21.0F);

  EXPECT_FALSE(outcome.state_answered);
  EXPECT_EQ(outcome.plan.refusal, HeatingRefusal::PROGRAMME_STATE_UNANSWERED)
      << "a boost or drying run could be on top: neither register is known to be right";
  const std::vector<uint8_t> reads = sent_registers(fx);
  ASSERT_GE(reads.size(), 2u);
  EXPECT_EQ(reads.front(), DRYER_MODE);
  for (size_t i = 1; i < reads.size(); i++)
    EXPECT_EQ(reads[i], DRYER_TIMED_RUN) << "nothing is asked after a silent read";
  EXPECT_TRUE(written_registers(fx).empty());
}

TEST(HeaterController, AModeReadAnsweredWithoutAValueGetsNothingWrittenAndIsNotAskedAgain) {
  HeaterControllerFixture fx;
  make_radiator_in_programme(fx, HeatingMode::PROG, 17.0F);
  fx.queue_register_ack({0x0C, 0x60, 0x01, RADIATOR_MODE});  // the mode register echoed without a value

  const TargetOutcome outcome = set_target(fx, 21.0F);

  EXPECT_FALSE(outcome.state_answered);
  EXPECT_EQ(outcome.plan.refusal, HeatingRefusal::PROGRAMME_STATE_UNANSWERED);
  EXPECT_EQ(sent_registers(fx), std::vector<uint8_t>{RADIATOR_MODE}) << "one answered read, not asked again";
}

TEST(HeaterController, AHeaterInTheProgrammeThatDoesNotAnswerTheTemperatureInForceGetsNothingWritten) {
  HeaterControllerFixture fx;
  IoDevice *dev = make_radiator_in_programme(fx, HeatingMode::PROG, NAN);
  queue_read_reply(fx, RADIATOR_MODE, {RADIATOR_PROGRAMME});  // the read of 0x012A gets no reply

  const TargetOutcome outcome = set_target(fx, 21.0F);

  EXPECT_TRUE(outcome.state_answered);
  EXPECT_EQ(outcome.plan.refusal, HeatingRefusal::IN_FORCE_UNKNOWN);
  EXPECT_FALSE(outcome.sent);
  EXPECT_TRUE(written_registers(fx).empty());
  EXPECT_EQ(test::in_force_of(*dev), InForceSupport::UNKNOWN);
}

TEST(HeaterController, AHeaterThatAnswersTheTemperatureInForceEmptyTakesTheTargetAsTheComfortSetpoint) {
  HeaterControllerFixture fx;
  IoDevice *dev = make_radiator_in_programme(fx, HeatingMode::MANUAL, NAN);
  queue_read_reply(fx, RADIATOR_MODE, {RADIATOR_PROGRAMME});
  queue_read_reply(fx, RADIATOR_IN_FORCE, {});  // answered, no value
  queue_register_write_and_read_back(fx, COMFORT_REGISTER, 0xD2, 0xD2);

  EXPECT_TRUE(set_target(fx, 21.0F).sent);

  EXPECT_EQ(test::in_force_of(*dev), InForceSupport::ABSENT);
  EXPECT_EQ(written_registers(fx), std::vector<uint8_t>{COMFORT_REGISTER});
}

TEST(HeaterController, ADryerTargetInAComfortPeriodAsksForTheLevelThenWritesTheComfortSetpoint) {
  HeaterControllerFixture fx;
  IoDevice *dev = make_towel_dryer_in_programme(fx, 19.0F, heating_level::COMFORT);
  queue_read_reply(fx, DRYER_MODE, {DRYER_PROGRAMME});
  queue_read_reply(fx, DRYER_TIMED_RUN, {NO_TIMED_RUN});
  queue_register_write_and_read_back(fx, COMFORT_REGISTER, 0xD2, 0xD2);

  const TargetOutcome outcome = set_target(fx, 21.0F);

  EXPECT_TRUE(outcome.sent);
  EXPECT_FALSE(outcome.plan.temporary_override);
  EXPECT_EQ(sent_registers(fx),
            (std::vector<uint8_t>{DRYER_MODE, DRYER_TIMED_RUN, COMFORT_REGISTER, COMFORT_REGISTER}));
  EXPECT_EQ(written_registers(fx), std::vector<uint8_t>{COMFORT_REGISTER}) << "the override is not written";
  ASSERT_EQ(fx.status_reads.size(), 1u) << "one status read, after the registers and before the write";
  EXPECT_EQ(fx.status_reads[0].max_tries, EXCHANGE_RETRY_COUNT);
  EXPECT_FLOAT_EQ(test::number_read(*dev, DeviceReading::SETPOINT), 21.0F);
}

TEST(HeaterController, ADryerTargetInAnEcoPeriodAsksForTheLevelThenWritesTheOverride) {
  HeaterControllerFixture fx;
  make_towel_dryer_in_programme(fx, 16.5F, heating_level::ECO);
  queue_read_reply(fx, DRYER_MODE, {DRYER_PROGRAMME});
  queue_read_reply(fx, DRYER_TIMED_RUN, {NO_TIMED_RUN});
  queue_register_write_and_read_back(fx, OVERRIDE_REGISTER, 0xD2, 0xD2);

  const TargetOutcome outcome = set_target(fx, 21.0F);

  EXPECT_TRUE(outcome.sent);
  EXPECT_TRUE(outcome.plan.temporary_override);
  EXPECT_EQ(written_registers(fx), std::vector<uint8_t>{OVERRIDE_REGISTER});
  EXPECT_EQ(fx.status_reads.size(), 1u);
}

TEST(HeaterController, ADryerWhoseStoredLevelIsOldTakesTheLevelOfTheReadMadeForTheTarget) {
  // The poll stored eco; the period has changed since. The stored level is not used.
  HeaterControllerFixture fx;
  IoDevice *dev = make_towel_dryer_in_programme(fx, 16.5F, heating_level::ECO);
  fx.status_reply_level = heating_level::COMFORT;
  queue_read_reply(fx, DRYER_MODE, {DRYER_PROGRAMME});
  queue_read_reply(fx, DRYER_TIMED_RUN, {NO_TIMED_RUN});
  queue_register_write_and_read_back(fx, COMFORT_REGISTER, 0xD2, 0xD2);

  EXPECT_TRUE(set_target(fx, 21.0F).sent);

  EXPECT_EQ(written_registers(fx), std::vector<uint8_t>{COMFORT_REGISTER});
  EXPECT_EQ(test::wire_read(*dev, DeviceReading::HEATING_LEVEL), heating_level::COMFORT);
}

TEST(HeaterController, ADryerThatDoesNotAnswerTheLevelReadGetsNothingWritten) {
  HeaterControllerFixture fx;
  IoDevice *dev = make_towel_dryer_in_programme(fx, 16.5F, heating_level::ECO);
  fx.status_reply_level = -1;  // the status read brings no level; the stored eco is old
  queue_read_reply(fx, DRYER_MODE, {DRYER_PROGRAMME});
  queue_read_reply(fx, DRYER_TIMED_RUN, {NO_TIMED_RUN});

  const TargetOutcome outcome = set_target(fx, 21.0F);

  EXPECT_TRUE(outcome.state_answered) << "the registers answered";
  EXPECT_EQ(outcome.plan.refusal, HeatingRefusal::PERIOD_UNKNOWN);
  EXPECT_FALSE(outcome.sent);
  EXPECT_TRUE(written_registers(fx).empty());
  EXPECT_EQ(fx.status_reads.size(), 1u);
  EXPECT_EQ(test::wire_read(*dev, DeviceReading::HEATING_LEVEL), -1) << "the old level is not kept as if it were fresh";
}

TEST(HeaterController, ATargetThatNeedsNoPeriodMakesNoStatusRead) {
  // heat and a boost on top: the comfort setpoint without a period; a radiator: no such rule.
  {
    HeaterControllerFixture fx;
    make_towel_dryer_in_programme(fx, 17.0F, heating_level::ECO);
    queue_read_reply(fx, DRYER_MODE, {DRYER_HEAT});
    queue_register_write_and_read_back(fx, COMFORT_REGISTER, 0xD2, 0xD2);
    EXPECT_TRUE(set_target(fx, 21.0F).sent);
    EXPECT_TRUE(fx.status_reads.empty()) << "heat";
  }
  {
    HeaterControllerFixture fx;
    make_towel_dryer_in_programme(fx, 17.0F, heating_level::ECO);
    queue_read_reply(fx, DRYER_MODE, {DRYER_PROGRAMME});
    queue_read_reply(fx, DRYER_TIMED_RUN, {TIMED_BOOST});
    queue_register_write_and_read_back(fx, COMFORT_REGISTER, 0xD2, 0xD2);
    EXPECT_TRUE(set_target(fx, 21.0F).sent);
    EXPECT_TRUE(fx.status_reads.empty()) << "a boost on top";
  }
  {
    HeaterControllerFixture fx;
    make_radiator_in_programme(fx, HeatingMode::PROG, 17.0F);
    queue_read_reply(fx, RADIATOR_MODE, {RADIATOR_PROGRAMME});
    queue_register_write_and_read_back(fx, OVERRIDE_REGISTER, 0xD2, 0xD2);
    EXPECT_TRUE(set_target(fx, 21.0F).sent);
    EXPECT_TRUE(fx.status_reads.empty()) << "a radiator keeps the override in every period";
  }
}

TEST(HeaterController, AHeaterWithoutAProgrammeIsNotReadBeforeATarget) {
  HeaterControllerFixture fx;
  IoDevice *plain = fx.add_device_of_kind(DeviceType::ELECTRICAL_HEATER, 0);
  ASSERT_NE(plain, nullptr);

  EXPECT_FALSE(fx.controller.read_programme_state("ABC123"));
  EXPECT_TRUE(fx.radio.get_sent_data().empty()) << "a profile without a programme period has nothing to read";
}

TEST(HeaterController, ARefusedOverrideWritesNothingElseAndTheCardShowsWhatTheHeaterHolds) {
  HeaterControllerFixture fx;
  IoDevice *dev = make_radiator_in_programme(fx, HeatingMode::PROG, 17.0F);
  // The heater accepts the write but holds 0: no override runs.
  queue_register_write_and_read_back(fx, 0x1D, 0xC8, 0x00);

  EXPECT_TRUE(fx.controller.execute_heating_plan("ABC123", override_plan(20.0F), nullptr))
      << "the write was acknowledged";

  ASSERT_EQ(fx.radio.get_sent_data().size(), 2u) << "no fallback write of the comfort setpoint";
  for (const auto &frame : fx.radio.get_sent_data())
    EXPECT_NE(sent_register(frame), 0x03);
  EXPECT_FLOAT_EQ(test::number_read(*dev, DeviceReading::SETPOINT), 19.0F);
  EXPECT_FLOAT_EQ(stored_in_force(*dev), 17.0F) << "what the heater held stays shown until the poll";
  EXPECT_FLOAT_EQ(test::number_read(*dev, DeviceReading::DEROGATED_SETPOINT), 0.0F);
}

TEST(HeaterController, AnOverrideTheHeaterClampsShowsTheValueItHolds) {
  HeaterControllerFixture fx;
  IoDevice *dev = make_radiator_in_programme(fx, HeatingMode::PROG, 17.0F);
  queue_register_write_and_read_back(fx, 0x1D, 0xC8, 0xBE);  // asked 20.0, holds 19.0

  EXPECT_TRUE(fx.controller.execute_heating_plan("ABC123", override_plan(20.0F), nullptr));

  EXPECT_FLOAT_EQ(stored_in_force(*dev), 19.0F);
  EXPECT_FLOAT_EQ(test::number_read(*dev, DeviceReading::SETPOINT), 19.0F);
}

TEST(HeaterController, AnOverrideReadBackOfZeroLeavesTheTemperatureInForceAlone) {
  HeaterControllerFixture fx;
  IoDevice *dev = make_towel_dryer_in_programme(fx, 15.5F, heating_level::ECO);
  fx.queue_register_ack({0x0C, 0x61, 0x01, 0x1D, 0x00, 0x00});
  fx.queue_register_ack({0x0C, 0x60, 0x01, 0x1D, 0x00, 0x00});

  ASSERT_TRUE(
      fx.controller.execute_heating_plan("ABC123", register_plan(DeviceRegister::DEROGATED_SETPOINT, 0.0F), nullptr));

  EXPECT_FLOAT_EQ(stored_in_force(*dev), 15.5F) << "a dryer that kept no override shows what it held";
}

TEST(HeaterController, AnOverrideReadBackDoesNotStoreTheTemperatureInForceOfAHeaterThatHasNone) {
  HeaterControllerFixture fx;
  IoDevice *dev = make_towel_dryer_in_programme(fx, NAN, heating_level::ECO);
  test::set_in_force_support(*dev, InForceSupport::ABSENT);
  fx.queue_register_ack({0x0C, 0x61, 0x01, 0x1D, 0xD2, 0x00});
  fx.queue_register_ack({0x0C, 0x60, 0x01, 0x1D, 0xD2, 0x00});

  ASSERT_TRUE(
      fx.controller.execute_heating_plan("ABC123", register_plan(DeviceRegister::DEROGATED_SETPOINT, 21.0F), nullptr));

  EXPECT_EQ(test::in_force_of(*dev), InForceSupport::ABSENT) << "a store would lift the heater's absent mark";
  EXPECT_TRUE(std::isnan(stored_in_force(*dev)));
}

TEST(HeaterController, AComfortReadOutsideAWriteDoesNotMoveTheTemperatureInForce) {
  HeaterControllerFixture fx;
  const IoDevice *dev = make_towel_dryer_in_programme(fx, 19.0F, heating_level::COMFORT);
  fx.queue_register_ack({0x0C, 0x60, 0x01, 0x03, 0xC8, 0x00});

  ASSERT_TRUE(
      fx.controller.read_heating_register("ABC123", DeviceRegister::COMFORT_SETPOINT, nullptr, EXCHANGE_RETRY_COUNT));

  EXPECT_FLOAT_EQ(test::number_read(*dev, DeviceReading::SETPOINT), 20.0F);
  EXPECT_FLOAT_EQ(stored_in_force(*dev), 19.0F) << "only the poll's own read of 0x37 changes it";
}

TEST(HeaterController, APlannedRegisterWriteWithoutReadBackSendsOnlyTheWrite) {
  HeaterControllerFixture fx;
  make_heater(fx);
  fx.queue_register_ack({0x0C, 0x61, 0x01, 0x03, 0xCD, 0x00});
  HeatingWritePlan plan = register_plan(DeviceRegister::COMFORT_SETPOINT, 20.5F);
  plan.read_back = false;

  EXPECT_TRUE(fx.controller.execute_heating_plan("ABC123", plan, nullptr));
  EXPECT_EQ(fx.radio.get_sent_data().size(), 1u);
}

TEST(HeaterController, AnUnacknowledgedPlannedWriteRunsNoHookAndSendsNoReadBack) {
  HeaterControllerFixture fx;
  make_heater(fx);
  int hook_calls = 0;

  EXPECT_FALSE(fx.controller.execute_heating_plan("ABC123", register_plan(DeviceRegister::COMFORT_SETPOINT, 20.5F),
                                                  [&] { hook_calls++; }));

  EXPECT_EQ(hook_calls, 0);
  for (const auto &tx : fx.radio.get_sent_data())
    EXPECT_EQ(tx[10], 0x61) << "only write attempts, no read-back";
}

TEST(HeaterController, ARefusedPlanSendsNothing) {
  HeaterControllerFixture fx;
  make_heater(fx);
  HeatingWritePlan refused;
  refused.refusal = HeatingRefusal::NO_MATCH;
  int hook_calls = 0;

  EXPECT_FALSE(fx.controller.execute_heating_plan("ABC123", refused, [&] { hook_calls++; }));

  EXPECT_EQ(hook_calls, 0);
  EXPECT_TRUE(fx.radio.get_sent_data().empty());
  EXPECT_TRUE(fx.executes.empty());
}

TEST(HeaterController, APlannedLevelWriteIsTheLevelExecuteAndIsNotReadBack) {
  HeaterControllerFixture fx;
  ASSERT_NE(fx.add_device_of_kind(DeviceType::ELECTRICAL_HEATER, 0), nullptr);
  HeatingWritePlan plan;
  plan.kind = HeatingPlanKind::LEVEL;
  plan.level = heating_level::ECO;
  int hook_calls = 0;

  EXPECT_TRUE(fx.controller.execute_heating_plan("ABC123", plan, [&] { hook_calls++; }));

  EXPECT_EQ(hook_calls, 1);
  ASSERT_EQ(fx.executes.size(), 1u);
  EXPECT_EQ(fx.executes[0].action, "heating level");
  ASSERT_TRUE(fx.executes[0].built);
  IoFrame expected;
  const IoDevice *dev = fx.registry.get("ABC123");
  ASSERT_TRUE(create_execute_heating_level(expected, fx.node_id, dev->node_id, dev->low_power, heating_level::ECO));
  uint8_t expected_raw[64];
  uint8_t got_raw[64];
  const uint8_t expected_len = serialize(expected, expected_raw, sizeof(expected_raw));
  const uint8_t got_len = serialize(fx.executes[0].request, got_raw, sizeof(got_raw));
  EXPECT_EQ(std::vector<uint8_t>(got_raw, got_raw + got_len),
            std::vector<uint8_t>(expected_raw, expected_raw + expected_len));
  EXPECT_TRUE(fx.radio.get_sent_data().empty()) << "no register read-back after a level write";
}

TEST(HeaterController, APlannedTemperatureWriteCarriesItsMainParameterAndFunctionalParameter) {
  HeaterControllerFixture fx;
  fx.add_device_of_kind(DeviceType::HEAT_PUMP);
  HeatingWritePlan plan;
  plan.kind = HeatingPlanKind::TEMPERATURE;
  plan.main_parameter = 0xD80F;
  plan.fp = HeatingFp{HeatingFpKind::FP16, heating_fp16::ON};

  EXPECT_TRUE(fx.controller.execute_heating_plan("ABC123", plan, nullptr));

  ASSERT_EQ(fx.executes.size(), 1u);
  EXPECT_EQ(fx.executes[0].action, "heating temperature");
  ASSERT_TRUE(fx.executes[0].built);
  IoFrame expected;
  const IoDevice *dev = fx.registry.get("ABC123");
  ASSERT_TRUE(
      create_execute_heating_with_fp16(expected, fx.node_id, dev->node_id, dev->low_power, 0xD80F, heating_fp16::ON));
  uint8_t expected_raw[64];
  uint8_t got_raw[64];
  const uint8_t expected_len = serialize(expected, expected_raw, sizeof(expected_raw));
  const uint8_t got_len = serialize(fx.executes[0].request, got_raw, sizeof(got_raw));
  EXPECT_EQ(std::vector<uint8_t>(got_raw, got_raw + got_len),
            std::vector<uint8_t>(expected_raw, expected_raw + expected_len));
  EXPECT_TRUE(fx.radio.get_sent_data().empty());
}

TEST(HeaterController, SetDeviceTemperatureRangeStoresTheRangeOnTheDevice) {
  HeaterControllerFixture fx;
  const IoDevice *dev = make_heater(fx);

  fx.controller.set_device_temperature_range("ABC123", 7.0F, 28.0F);
  fx.controller.set_device_temperature_range("DEF456", 1.0F, 2.0F);  // unknown: no effect, no crash

  EXPECT_FLOAT_EQ(dev->temperature_range_min_c, 7.0F);
  EXPECT_FLOAT_EQ(dev->temperature_range_max_c, 28.0F);
}
