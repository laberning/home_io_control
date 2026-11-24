#pragma once

/// @file sim_bridge.h
/// @brief A simulated co-processor as a Bridle peer for host tests: a Bridle simulator in a child process,
///        a lockstep adapter that makes its time follow the test's manual clock, and the control channel
///        to its simulated air (Bridle spec §11.1).
/// @ingroup hioc_radio
///
/// The simulator is not a byte-stream server. It takes the host's Bridle bytes on its stdin, and it
/// answers on a control channel: `advance US stop` runs it until the next message it writes, and every
/// message comes back as a decoded `frame` line. This class owns that: it spawns the process, keeps the
/// simulator's clock equal to the manual clock (`FakeByteStream` advances the manual clock on every empty
/// poll, and every poll runs the pump), frames the messages the simulator wrote back into a byte stream
/// for the driver, and tells an air handler about every frame the simulated radio put on the air, at the
/// moment its transmission is over, so that a device model can answer on the air in time.
///
/// Both clocks start at the same instant (the manual clock when the bridge is made is the simulator's
/// time zero), so a run is deterministic: no wall clock is read.

#include <sys/types.h>

#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>

#include "fake_byte_stream.h"
#include "testable_radio_bridle.h"

class SimBridge {
 public:
  /// A frame the simulated radio sent, as `air_log` lists it.
  struct AirTx {
    uint32_t freq_hz{0};
    uint64_t start_us{0};  ///< First preamble bit, simulator time.
    uint64_t sync_us{0};   ///< End of the SFD.
    uint64_t end_us{0};    ///< Last bit.
    uint8_t sfd{0};
    std::vector<uint8_t> psdu;  ///< As on air, FCS included.
  };

  /// Told about the frames the radio sent since the last call: `log` is every frame so far, `first_new` the
  /// index of the first one the handler has not seen.
  using AirHandler = std::function<void(const std::vector<AirTx> &log, size_t first_new)>;

  /// Starts the simulator at @p sim_path. Needs an active ManualClock. Throws std::runtime_error when the
  /// process cannot be started.
  explicit SimBridge(const std::string &sim_path);
  ~SimBridge();
  SimBridge(const SimBridge &) = delete;
  SimBridge &operator=(const SimBridge &) = delete;

  /// The byte stream the driver is handed.
  FakeByteStream &stream() { return stream_; }
  /// The reset line the driver pulses; releasing it resets the simulated co-processor.
  FakeResetLine &reset_line() { return reset_; }

  /// Install the handler of the radio's transmissions.
  void set_air_handler(AirHandler handler) { air_handler_ = std::move(handler); }
  /// Put a 802.15.4 frame on the air: @p psdu without FCS, starting at simulator time @p start_us (not in the
  /// past), at @p level_dbm. Throws std::runtime_error when the simulator refuses it.
  void inject_air(uint32_t freq_hz, uint64_t start_us, const std::vector<uint8_t> &psdu, int level_dbm = -60);
  /// Hold back every byte the host writes from now on by @p delay_us of simulator time (0: no delay), in
  /// order. It models a host link that is slow at that moment, e.g. a PC between the hub and the board.
  void set_uplink_delay_us(uint64_t delay_us) { uplink_delay_us_ = delay_us; }

  /// Kills the simulator process, for a test of what the bridge does when it is gone: the next use of the link
  /// throws std::runtime_error.
  void terminate_simulator();

  /// The simulator's time now.
  [[nodiscard]] uint64_t sim_now_us() const { return sim_now_us_; }
  /// Brings the simulator up to the manual clock. Called by every poll of the stream and every write.
  void pump();
  /// Every frame the radio has sent so far, from the simulator.
  [[nodiscard]] std::vector<AirTx> air_log();

 private:
  /// Host bytes held back by the uplink delay until the simulator's time reaches `due_us`.
  struct HeldWrite {
    uint64_t due_us;
    std::vector<uint8_t> bytes;
  };

  /// Sends one control request and returns the data lines of its reply; throws on `error ...` or when the simulator
  /// ends.
  std::vector<std::string> request_(const std::string &line);
  /// Takes in a reply's lines: the simulator's new time, and each message it wrote framed for the driver's stream.
  void take_(const std::vector<std::string> &lines);
  /// Runs the simulator @p us µs, stopping early at the first message it writes (the air handler must see it at once).
  void advance_(uint64_t us);
  /// Writes Bridle bytes to the simulator's stdin.
  void write_to_sim_(const std::vector<uint8_t> &bytes);
  /// The stream's write handler: brings the simulator to the test's present, then passes the bytes on (or holds them).
  void on_host_write_(const uint8_t *data, size_t n);
  /// Passes on the held writes that are due.
  void flush_due_writes_();
  /// Reads the radio's transmissions and tells the air handler about the new ones.
  void refresh_air_();

  FakeByteStream stream_;  ///< The driver's side of the link.
  FakeResetLine reset_;
  pid_t pid_{-1};
  int control_w_{-1};  ///< Requests to the simulator.
  int control_r_{-1};  ///< Its replies.
  int link0_w_{-1};    ///< The Bridle bytes to its stdin.
  std::string reply_buffer_;
  uint64_t origin_us_{0};  ///< The manual clock at simulator time zero.
  uint64_t sim_now_us_{0};
  uint64_t uplink_delay_us_{0};
  std::deque<HeldWrite> held_;
  size_t air_seen_{0};
  bool pumping_{false};
  AirHandler air_handler_;
  void (*previous_sigpipe_)(int){nullptr};  ///< SIGPIPE's handler before the bridge ignored it.
};
