#pragma once

/// @file fullstack_rig.h
/// @brief The full-stack rig: the hub's exchange engine over the real Bridle driver over the real
///        co-processor's core in a Bridle simulator, with an emulated 2.4 GHz blind answering on the simulated air.
/// @ingroup hioc_hub
///
/// Nothing in the chain is scripted: the engine and the driver are production code, the co-processor
/// is a Bridle simulator (any program that speaks the simulator control channel of Bridle spec §11.1 and
/// has an 802.15.4 radio) on a simulated radio and air, and the only model is the blind (io_blind_emulator.h).
/// Time is the manual clock, which the simulator follows in lockstep (sim_bridge.h), so every run is the
/// same and a timing assertion is about the exchange, not the machine.

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "exchange_engine.h"
#include "io_blind_emulator.h"
#include "proto_commands.h"
#include "radio_io2g4_phy.h"
#include "sim_bridge.h"
#include "test_helpers.h"
#include "testable_radio_bridle.h"

namespace fullstack {

/// The path of the simulator binary: `BRIDLE_SIM`, set by `make fullstack`.
inline std::string sim_path() {
  const char *path = std::getenv("BRIDLE_SIM");
  return path != nullptr ? path : "";
}

/// The hub's side of the chain, and the blind.
class Rig {
 public:
  using HeardLog = std::vector<HeardFrame>;

  explicit Rig(const IoBlindEmulator::Config &blind_config = default_blind_config())
      : bridge_(sim_path()),
        blind_(blind_config),
        radio_(&bridge_.stream(), esphome::home_io_control::bridle::io2g4_phy(), &bridge_.reset_line(), std::nullopt),
        engine_(&driver_, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning_) {
    bridge_.set_air_handler(
        [this](const std::vector<SimBridge::AirTx> &log, size_t first_new) { on_air_(log, first_new); });
    initialised_ = radio_.init();
  }

  [[nodiscard]] bool initialised() const { return initialised_; }
  esphome::home_io_control::ExchangeEngine &engine() { return engine_; }
  esphome::home_io_control::TuningConfig &tuning() { return tuning_; }
  IoBlindEmulator &blind() { return blind_; }
  SimBridge &bridge() { return bridge_; }
  TestableRadioBridle &radio() { return radio_; }
  /// The hub's frames the blind was told about, in order.
  [[nodiscard]] const HeardLog &heard() const { return heard_; }
  /// What the blind did, with its own time stamps (simulator µs).
  [[nodiscard]] const std::vector<EmulatorEvent> &events() const { return blind_.events(); }

  /// Simulator time as the blind sees it, for an assertion about a frame's position in an exchange.
  [[nodiscard]] uint64_t sim_now_us() const { return bridge_.sim_now_us(); }

  /// Called with every frame of the hub the blind hears of, before the blind answers it.
  void set_on_hub_frame(std::function<void(const HeardFrame &)> hook) { on_hub_frame_ = std::move(hook); }

 private:
  void on_air_(const std::vector<SimBridge::AirTx> &log, size_t first_new) {
    namespace bridle = esphome::home_io_control::bridle;
    for (size_t i = first_new; i < log.size(); i++) {
      const SimBridge::AirTx &tx = log[i];
      if (tx.psdu.size() == sizeof(bridle::IO2G4_STROBE_TEMPLATE) + bridle::IO2G4_FCS_SIZE) {
        if (wake_frames_++ == 0)  // a wake-train frame: `00 <counter>` and its FCS
          first_wake_start_us_ = tx.start_us;
        continue;
      }
      if (tx.psdu.size() < bridle::IO2G4_PSDU_HEADER_SIZE + bridle::IO2G4_FCS_SIZE)
        continue;
      HeardFrame frame;
      frame.start_us = tx.start_us;
      frame.end_us = tx.end_us;
      frame.freq_hz = tx.freq_hz;
      frame.io.assign(tx.psdu.begin() + bridle::IO2G4_PSDU_HEADER_SIZE, tx.psdu.end() - bridle::IO2G4_FCS_SIZE);
      frame.wake_frames = wake_frames_;
      frame.first_wake_start_us = wake_frames_ > 0 ? first_wake_start_us_ : 0;
      wake_frames_ = 0;
      heard_.push_back(frame);
      if (on_hub_frame_)
        on_hub_frame_(frame);
      for (const EmulatedReply &reply : blind_.hear(frame)) {
        std::vector<uint8_t> psdu(bridle::IO2G4_PSDU_HEADER,
                                  bridle::IO2G4_PSDU_HEADER + bridle::IO2G4_PSDU_HEADER_SIZE);
        psdu.insert(psdu.end(), reply.io.begin(), reply.io.end());
        bridge_.inject_air(reply.freq_hz, reply.start_us, psdu);
      }
    }
  }

  SimBridge bridge_;
  IoBlindEmulator blind_;
  TestableRadioBridle radio_;
  esphome::home_io_control::RadioDriver *driver_{&radio_};
  esphome::home_io_control::TuningConfig tuning_;
  esphome::home_io_control::ExchangeEngine engine_;
  HeardLog heard_;
  std::function<void(const HeardFrame &)> on_hub_frame_;
  uint32_t wake_frames_{0};
  uint64_t first_wake_start_us_{0};
  bool initialised_{false};
};

}  // namespace fullstack
