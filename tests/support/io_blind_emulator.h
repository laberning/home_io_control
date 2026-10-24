#pragma once

/// @file io_blind_emulator.h
/// @brief A 2.4 GHz io blind as a pure model: the frames the hub puts on the air in, the frames the
///        blind answers with (and when) out. No radio, no simulator, no clock of its own.
/// @ingroup hioc_hub
///
/// The model answers the way a real blind does on the bench:
///  - a status read (`0x03`) draws a `0x04` in the 14-byte layout of the blind (flags, a constant,
///    target, current, remaining travel in seconds, last commander, `01 00 00`), resting or on its
///    way, with the position advancing and the remaining seconds falling in time;
///  - an EXECUTE (`0x00`) draws a challenge (`0x3C`); a right `0x3D` that starts within the answer
///    window of the challenge's end draws a `0x04` and moves the blind, a wrong or late one draws
///    silence;
///  - a START frame behind a short wake train is heard only while the blind is awake (it travels, or it
///    heard a frame addressed to it recently); behind a long train it is always heard. A frame without the
///    START flag is heard whatever the blind's state; it draws a reply only when it answers an open exchange
///    (anything else is recorded as UNEXPECTED);
///  - the answer to a challenge is accepted with CTRL1 LOW_POWER set or clear. That is a decision of this
///    model: whether a real blind accepts it is untested;
///  - an EXECUTE whose main parameter is neither a position (0 to 0xC800) nor STOP is not understood: the
///    blind does not move and does not confirm it.
///
/// Time is the caller's: every frame comes with its own start and end, in whatever µs clock the caller
/// keeps, and every reply comes back with a start on that clock. The answer window is the edge measured
/// on one blind under bench conditions, not a property of every blind.

#include <cstdint>
#include <optional>
#include <vector>

namespace fullstack {

/// A frame of the hub on the air, as the emulator is told of it.
struct HeardFrame {
  uint64_t start_us{0};             ///< First preamble bit of the main frame.
  uint64_t end_us{0};               ///< Last bit of the main frame.
  uint32_t freq_hz{0};              ///< The carrier it was on.
  std::vector<uint8_t> io;          ///< CTRL0 ... data: the io frame, without the PSDU header and the FCS.
  uint32_t wake_frames{0};          ///< Wake-train frames that came right before it on this carrier.
  uint64_t first_wake_start_us{0};  ///< Start of the first of them; 0 when there were none.
};

/// A frame the blind puts on the air.
struct EmulatedReply {
  uint64_t start_us{0};     ///< First preamble bit.
  uint32_t freq_hz{0};      ///< The carrier.
  std::vector<uint8_t> io;  ///< CTRL0 ... data.
  /// Last bit of the frame, from its length (802.15.4 at 250 kb/s: 160 µs of preamble and SFD, then the
  /// PHR, the PSDU header, the io frame and the FCS at 32 µs a byte).
  [[nodiscard]] uint64_t end_us() const;
};

/// What the model did with a frame, for a test to read back.
struct EmulatorEvent {
  /// What happened.
  enum class Kind : uint8_t {
    STATUS_ANSWERED,     ///< A status read was heard and answered.
    SLEPT_THROUGH,       ///< A START frame behind too short a train while the blind was asleep: not heard.
    LOST,                ///< A frame lost on the air (lose_next_frames()): not heard.
    EXECUTE_CHALLENGED,  ///< An EXECUTE was heard and challenged.
    ANSWER_ACCEPTED,     ///< A right 0x3D within the window: the blind answered and moved.
    ANSWER_TOO_LATE,     ///< A 0x3D that started after the window: ignored.
    ANSWER_WRONG,        ///< A 0x3D with a wrong authenticator: ignored.
    UNEXPECTED,          ///< A frame the model has no use for (no open exchange, an unknown command).
  };
  Kind kind{Kind::UNEXPECTED};
  uint64_t at_us{0};        ///< The frame's start.
  uint32_t wake_frames{0};  ///< The frame's wake train.
  uint64_t gap_us{0};       ///< ANSWER_*: from the end of the challenge to the start of the 0x3D.
  uint64_t reply_gap_us{
      0};  ///< STATUS_ANSWERED, EXECUTE_CHALLENGED, ANSWER_ACCEPTED: the request's end to the reply's start.
};

/// The blind.
class IoBlindEmulator {
 public:
  /// How the blind is set up.
  struct Config {
    uint8_t node[3]{0x51, 0x6B, 0x10};  ///< The blind's node id.
    uint8_t key[16]{};                  ///< The system key the blind authenticates under.
    uint8_t commander[3]{0x4D, 0x84,
                         0xA6};  ///< Who it reports as the last to command it, until an EXECUTE says otherwise.
    double position_pct{50.0};   ///< Where it rests at the start, io scale: 0 open, 100 closed.
    double travel_s{72.0};       ///< A full travel, open to closed.
    /// A 0x3D that starts later than this after the end of its challenge is ignored (46 ms accepted and 47 ms
    /// ignored on the blind this was measured on).
    uint64_t answer_window_us{46000};
    /// How long after a frame addressed to it the blind stays awake and hears a short train. Not measured: the
    /// traces cannot tell "travelling" from "addressed recently", so this is a guess.
    uint64_t awake_window_us{5000000};
    /// A start frame behind at least this many wake frames wakes the blind whatever its state.
    uint32_t long_train_min_frames{400};
    /// From the end of a request to the end of the reply's SFD, as measured on a real blind (median 2.8 ms).
    uint64_t reply_sfd_after_request_us{2800};
    /// When not 0, the blind hears only frames on this carrier (a channel the others fade on): a frame
    /// on any other carrier is LOST.
    uint32_t only_freq_hz{0};
    /// Seed of the challenges: they are deterministic so that a run is the same every time.
    uint8_t challenge_seed{0x11};
  };

  explicit IoBlindEmulator(const Config &config);

  /// The blind hears @p frame. @return the frames it answers with, in order (none for silence).
  [[nodiscard]] std::vector<EmulatedReply> hear(const HeardFrame &frame);

  /// Where the blind is at @p now_us, raw (0xC800 = closed).
  [[nodiscard]] uint16_t current_raw(uint64_t now_us) const;
  /// Whether the blind is travelling at @p now_us.
  [[nodiscard]] bool moving(uint64_t now_us) const;
  /// The data of the 0x04 a status read draws at @p now_us.
  [[nodiscard]] std::vector<uint8_t> status_data(uint64_t now_us) const;
  /// A remote sends the blind off for @p target_raw (raw, 0xC800 = closed) at @p now_us.
  void set_off(uint16_t target_raw, uint64_t now_us);
  /// The next @p count frames addressed to the blind are lost on the air: it does not hear them.
  void lose_next_frames(uint32_t count) { to_lose_ = count; }
  /// What the model did, in order.
  [[nodiscard]] const std::vector<EmulatorEvent> &events() const { return events_; }
  /// The configuration the blind was made with.
  [[nodiscard]] const Config &config() const { return config_; }
  /// The number of EXECUTEs the blind accepted (a right 0x3D in time).
  [[nodiscard]] size_t accepted_executes() const { return accepted_; }

 private:
  /// A journey that is under way: where it set off from, where it is going, and when.
  struct Journey {
    uint16_t from_raw{0};
    uint16_t target_raw{0};
    uint64_t since_us{0};
  };
  /// An EXECUTE that has been challenged and waits for its 0x3D.
  struct OpenExchange {
    std::vector<uint8_t> request;  ///< The EXECUTE's io frame.
    uint8_t challenge[6]{};
    uint64_t challenge_end_us{0};
  };

  /// Whether the blind is awake when @p frame starts: it travels, or heard a frame addressed to it within the window.
  [[nodiscard]] bool awake_(const HeardFrame &frame) const;
  /// The reply to @p to: @p cmd and @p data, addressed to the sender, timed from the end of @p to.
  [[nodiscard]] EmulatedReply reply_(const HeardFrame &to, uint8_t cmd, const std::vector<uint8_t> &data,
                                     bool end) const;
  /// Makes a journey that has reached its target a rest, so that `rest_raw_` is the position again.
  void settle_(uint64_t now_us);
  /// Carries out an accepted EXECUTE's main parameter: a position starts a journey, STOP freezes the blind where it is.
  /// @return false for a parameter the blind does not understand (nothing happens).
  bool begin_move_(uint16_t main, uint64_t now_us);

  Config config_;                              ///< How the blind was set up.
  uint16_t rest_raw_;                          ///< Where it rests when no journey is under way.
  std::optional<Journey> journey_;             ///< The journey under way, if any.
  uint8_t commander_[3];                       ///< Who commanded it last, as its status replies say.
  std::optional<uint64_t> last_addressed_us_;  ///< When a frame addressed to it was last heard.
  std::optional<OpenExchange> open_;           ///< The challenged EXECUTE waiting for its answer.
  uint8_t next_challenge_;                     ///< The next challenge byte: they are deterministic.
  std::vector<EmulatorEvent> events_;          ///< What the model did.
  size_t accepted_{0};                         ///< EXECUTEs accepted.
  uint32_t to_lose_{0};
};

/// The blind's default setup for a test: node `test::DST_ID`, the corpus system key `test::TEST_SYSTEM_KEY`, and
/// the rest of IoBlindEmulator::Config as it is.
IoBlindEmulator::Config default_blind_config();

}  // namespace fullstack
