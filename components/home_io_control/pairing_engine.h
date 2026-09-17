#pragma once

/// @file pairing_engine.h
/// @brief Device discovery and key-exchange engine for IO-Homecontrol pairing.
/// @ingroup hioc_hub
///
/// PairingEngine encapsulates all three phases of the IO-Homecontrol pairing flow:
///
/// Phase 1 — Discovery (0x28 → 0x29 → 0x2C → 0x2D):
///   Controller broadcasts a discovery packet. A device in pairing mode responds
///   with its node ID and type/subtype metadata. The controller then sends a discover-confirm
///   (0x2C) directly to that device and, when tuning allows, waits up to
///   `PAIRING_DISCOVER_CONFIRM_TRIES` × `PAIRING_DISCOVER_CONFIRM_ACK_TIMEOUT_MS` for its 0x2D —
///   every real controller in this project's corpus does this before proceeding; see
///   `pairing_discover_confirm` (tuning_config.h) for the mode knob and `run_discover_confirm_step_()`
///   for why the step never fails a pairing attempt.
///
/// Phase 2 — Authenticated Key Exchange (0x31 → 0x3C → 0x32 → 0x33):
///   The controller sends CMD_KEY_INIT (0x31). The device challenges with 0x3C.
///   The controller proves knowledge of the system key and simultaneously
///   transfers the encrypted system key (0x32). The device confirms with 0x33.
///
/// Phase 3 — Configuration (0x6F):
///   The controller sends SetConfig1 to enable automatic device status updates.
///
/// The engine is constructed once by IOHomeControlComponent. It holds double-pointer
/// indirection for the radio driver so test assignments (`comp.radio_ = &mock`) propagate
/// without calling setup(). All radio operations go through ExchangeEngine so that
/// LBT, preamble selection, and frequency hopping are centralised.
///
/// PairingEngine is non-copyable and non-movable because it stores pointer and reference
/// addresses that would dangle in a copy.

#include "proto_frame.h"
#include "proto_codecs.h"
#include "hub_pairing.h"
#include "hub_decisions.h"
#include "exchange_engine.h"
#include "device_registry.h"
#include "pairing_advisor.h"
#include "pairing_telemetry.h"
#include "radio_interface.h"
#include "tuning_config.h"

#include <cstdint>
#include <string>

namespace esphome {
namespace home_io_control {

/// @name Pairing timing constants
/// Timeouts and retry limits for the pairing flow's blocking waits.
///@{
inline constexpr uint32_t PAIRING_DISCOVERY_RESPONSE_TIMEOUT_MS = 2000;  ///< Discovery wait window after sending 0x28.
inline constexpr uint8_t PAIRING_DISCOVERY_MAX_ATTEMPTS = 3;             ///< Retry discovery TX up to this many times.
inline constexpr uint32_t PAIRING_KEY_CHALLENGE_TIMEOUT_MS = 500;  ///< Wait window for the device's 0x3C challenge.
inline constexpr uint32_t PAIRING_KEY_CONFIRM_TIMEOUT_MS = 500;    ///< Wait for 0x33 key confirm after sending 0x32.
inline constexpr uint8_t PAIRING_DISCOVER_CONFIRM_TRIES = 3;       ///< Max tries for the discover-confirm (0x2C) step.
inline constexpr uint32_t PAIRING_DISCOVER_CONFIRM_ACK_TIMEOUT_MS = 1500;  ///< Wait window per discover-confirm try.
/// How recent a RecentOneWayPairingSighting has to be, relative to discover_and_pair() starting,
/// to still count as evidence for this attempt. Generous relative to the doc's "a few seconds"
/// PROG-then-press guidance: real field reports (issue #27) show gaps up to ~4-7 s between the
/// PROG gesture and pressing "Discover & Pair" in the app, so a tight window would reintroduce
/// the same miss it's meant to fix. Not tied to ONEWAY_QUIET_PERIOD_MS (status_poll_policy.h,
/// 700 ms) — that constant is about collapsing one remote's repeat burst, a different, much
/// shorter timescale than "how long ago did the user press PROG."
inline constexpr uint32_t PAIRING_RECENT_ONE_WAY_SIGHTING_WINDOW_MS = 15000;
///@}

/// Owns and drives all three phases of the IO-Homecontrol device pairing flow.
///
/// Constructed once by IOHomeControlComponent; collaborators (radio, exchange engine,
/// device registry) are injected as pointers/references so the engine never outlives them.
/// @ingroup hioc_hub
class PairingEngine {
 public:
  /// Construct the engine with all required collaborators.
  ///
  /// @param radio_ptr  Double pointer into the hub's `radio_` member — survives driver replacement in tests.
  /// @param node_id    Controller 3-byte node ID buffer, owned by the hub.
  /// @param system_key 16-byte AES system key buffer, owned by the hub.
  /// @param tuning     Tuning configuration, owned by the hub.
  /// @param engine     Shared exchange engine for transmit/receive operations.
  /// @param registry   Device registry where paired devices are permanently registered.
  /// @param telemetry  Per-attempt telemetry recorder, owned by the hub.
  /// @param recent_oneway_sighting Most recent 1W pairing-gesture sighting from the hub's normal
  ///        passive RX path, owned by the hub; see RecentOneWayPairingSighting.
  PairingEngine(RadioDriver **radio_ptr, const uint8_t *node_id, const uint8_t *system_key, const TuningConfig *tuning,
                ExchangeEngine &engine, DeviceRegistry &registry, PairingTelemetry &telemetry,
                const RecentOneWayPairingSighting &recent_oneway_sighting);

  /// Non-copyable — stores double-pointer and references into hub member addresses.
  PairingEngine(const PairingEngine &) = delete;
  PairingEngine &operator=(const PairingEngine &) = delete;

  /// Discover and pair a device currently in pairing mode (three-phase orchestrator).
  /// @return true if all three phases completed successfully; false otherwise.
  bool discover_and_pair();

  /// Extract node ID, device type, and subtype from a CMD_DISCOVER_RESP frame.
  /// @return The decoded extended discovery fields (manufacturer / Multi Information Byte / length
  ///         flags), so a caller can read the self-reported power class without decoding twice.
  static DiscoveryResponseInfo parse_device_from_discovery(const IoFrame &frame, IoDevice &device,
                                                           std::string &device_id);

 protected:
  // --- Phase helpers (protected; exposed to tests via TestablePairingEngine in test_helpers.h) ---

  /// Phase 1: broadcast discovery command(s) and wait for a device response (0x29).
  /// @param context Pairing context updated on success.
  /// @return ACCEPT on success; NO_RESPONSE or INVALID otherwise.
  decisions::PairingDiscoveryDisposition run_discovery_phase_(pairing::PairingContext &context);

  /// @brief Discover-confirm step (0x2C → 0x2D): sent directly to the just-discovered device,
  /// once per discover_and_pair() attempt, between discovery and the key-exchange retry loop.
  ///
  /// Every real controller in this project's corpus sends 0x2C here. The step **never fails a
  /// pairing attempt**: a refusal, timeout, or `skip` tuning mode all still let the caller proceed
  /// to `run_key_exchange_phase_()` — only the result and log line differ. Applies the
  /// `pairing_key_init_delay_ms` pause afterward for every outcome except `SKIPPED` (see
  /// `tuning_config.h`'s `pairing_discover_confirm`/`pairing_key_init_delay_ms` doc for the modes
  /// and defaults).
  /// @param context Pairing context populated by run_discovery_phase_(); `context.req`/`context.rx`
  ///        are reused as scratch space for the 0x2C/0x2D exchange, same as the other phases.
  /// @return What the step actually observed — see @ref pairing::DiscoverConfirmResult. Not stored
  ///         in `context`: nothing downstream reads it.
  pairing::DiscoverConfirmResult run_discover_confirm_step_(pairing::PairingContext &context);

  /// Phase 2: authenticated key exchange (0x31 → 0x3C → 0x32 → 0x33).
  /// @param context Pairing context populated by run_discovery_phase_().
  /// @return true if key exchange completes; false on any failure.
  bool run_key_exchange_phase_(pairing::PairingContext &context);

  /// Phase 3: send SetConfig1 (0x6F) to enable automatic status updates; best-effort.
  /// Pairing always proceeds regardless of the outcome — the return value is informational
  /// only, used to distinguish PairingOutcome::PAIRED from PairingOutcome::CONFIG_FAILED in
  /// telemetry; it never causes discover_and_pair() to report failure.
  /// @param context Pairing context with device information from phases 1 and 2.
  /// @return true if the SetConfig1 exchange completed; false if it was skipped or failed.
  bool finalize_pairing_configuration_(pairing::PairingContext &context);

  /// Wait for a discovery response (0x29) within timeout_ms with per-chip frequency hopping.
  /// @param timeout_ms     Maximum wait window.
  /// @param packet         Output: raw RadioRxPacket of the accepted discovery frame.
  /// @param response_frame Output: parsed IoFrame of the accepted discovery frame.
  /// @return ACCEPT on success; NO_RESPONSE (no traffic) or INVALID (wrong frames) otherwise.
  decisions::PairingDiscoveryDisposition wait_for_discovery_response_(uint32_t timeout_ms, RadioRxPacket &packet,
                                                                      IoFrame &response_frame);

  /// Wait for a key-challenge (0x3C) or direct key-confirm (0x33) from the target device.
  /// @param timeout_ms        Maximum wait window.
  /// @param packet            Output: raw RadioRxPacket of the accepted frame.
  /// @param challenge_frame   Output: parsed IoFrame.
  /// @param device_node_id    Expected source node ID (devices paired to).
  /// @return true if a valid challenge or confirm was received; false on timeout.
  bool wait_for_key_challenge_(uint32_t timeout_ms, RadioRxPacket &packet, IoFrame &challenge_frame,
                               const uint8_t device_node_id[NODE_ID_SIZE]);

  /// Wait for one discover-confirm (0x2C) try's answer: a matching 0x2D, a matching
  /// CMD_ERROR_RESP, or nothing recognisable before the window closes.
  ///
  /// Listen policy alternates by try: `discover_confirm_try_rotates(try_index)` selects
  /// `ListenPolicy::ROTATE_ALL_CHANNELS` for the one try that hedges against an off-channel 0x2D,
  /// and `ListenPolicy::HOLD_REQUEST_CHANNEL` (matching every other unicast pairing wait) for the
  /// rest — see that decision's doc for why only one try rotates.
  /// @param try_index 1-based try number, used only to pick the listen policy.
  /// @param context   Pairing context; `context.req` is the 0x2C just transmitted, `context.rx`/
  ///        `context.packet` receive the candidate reply.
  /// @return ACK or ERROR for a matching reply; IGNORE only once the window closes with nothing
  ///         recognised — an unrelated frame arriving mid-window does not end the try, it keeps
  ///         listening.
  decisions::PairingDiscoverConfirmDisposition wait_for_discover_confirm_ack_(uint8_t try_index,
                                                                              pairing::PairingContext &context);

  /// Transmit the 0x32 key transfer and wait for the 0x33 key confirm with retry.
  ///
  /// A device that challenges the key transfer (slow-turnaround chips answering with a fresh 0x3C
  /// instead of confirming directly) is handled inline via `listen_for_key_confirm_()` and
  /// `ExchangeEngine::answer_challenge()` — see their docs. An explicit refusal (REFUSE: a wrong
  /// reply shape, or CMD_ERROR_RESP) must not spend the remaining retries; a challenge is not a
  /// refusal, so it does not return false either — the try only ends without confirming.
  bool wait_for_key_confirm_(pairing::PairingContext &context);

  /// Build CMD_KEY_TRANSFER against the current challenge and wait for the 0x33 confirm; see
  /// run_key_exchange_phase_()'s doc comment for why this is a separate, replayable step.
  /// @param context Pairing context; `context.rx.data` supplies the challenge bytes, `context.req`
  ///        is filled with the outbound 0x32, `context.resp` with the inbound 0x33 on success.
  /// @return true if the device confirmed the key.
  bool transfer_key_and_wait_confirm_(pairing::PairingContext &context);

 private:
  /// Preamble for a directed pairing start frame (0x2C, 0x31, 0x6F) to the device discovery just
  /// found: the shorter of ExchangeEngine::request_preamble_for()'s rule and the preamble the
  /// discovery request went out with (`pairing_discovery_preamble`).
  ///
  /// The device answered a discovery request carrying that preamble moments ago, so it is
  /// listening and demonstrably hears it; pairing follows within seconds. The long wake-up
  /// preamble the rule would otherwise pick for a low-power target (and that 0x31/0x6F always
  /// carry) is not just unnecessary then: some VELUX receivers never detect a frame behind a
  /// 1024-byte preamble at all (ADR 0029). With the default discovery preamble (LONG_PREAMBLE)
  /// the result equals the rule, so default setups transmit exactly what they did before.
  /// @param frame The start frame about to be sent.
  /// @return Preamble length in bytes.
  [[nodiscard]] uint16_t pairing_start_preamble_(const IoFrame &frame) const;

  /// One `ListenPolicy::HOLD_REQUEST_CHANNEL` listen for the key-transfer confirm wait, called up
  /// to twice per try in `wait_for_key_confirm_()` — once for the initial reply, once more (a
  /// fresh window) after answering a device challenge — so the listen spec, logging, and telemetry
  /// have exactly one owner instead of two copies of the same lambda.
  /// @param context     Pairing context; `context.req` is the outbound 0x32 (the challenge-response
  ///        transcript), `context.resp` receives the candidate reply.
  /// @param try_number  1-based try number, for the timeout log line only.
  /// @param after_challenge True if this is the second, post-challenge listen within the try
  ///        (labelled in the timeout log line so it isn't mistaken for the first).
  /// @return CONFIRM/CHALLENGE/REFUSE for a matching reply; IGNORE on a timeout. A frame from the
  ///         wrong endpoints does not end the listen — it is ignored and the wait continues.
  decisions::PairingKeyConfirmDisposition listen_for_key_confirm_(pairing::PairingContext &context, uint8_t try_number,
                                                                  bool after_challenge);

  /// Convenience accessor returning the current radio driver (dereferences double pointer).
  [[nodiscard]] RadioDriver *radio_() const { return *radio_ptr_; }

  /// Record the final outcome, detach telemetry from the exchange engine, and log the
  /// end-of-attempt summary. Called once at every discover_and_pair() exit point.
  /// @param outcome Final disposition of this attempt.
  void finish_pairing_attempt_(PairingOutcome outcome);

  /// Record an RX or RX_REJECT telemetry event for a discovery-response candidate frame.
  /// Factored out of wait_for_discovery_response_() purely to keep that function's cognitive
  /// complexity under the clang-tidy threshold — no behavior beyond the telemetry call.
  /// @param frame Parsed candidate frame.
  /// @param accepted true if the frame was classified as a valid discovery response.
  /// @param rssi RSSI of the captured frame.
  void record_discovery_rx_telemetry_(const IoFrame &frame, bool accepted, int16_t rssi);

  RadioDriver **radio_ptr_;
  const uint8_t *node_id_;
  const uint8_t *system_key_;
  const TuningConfig *tuning_;
  ExchangeEngine &engine_;
  DeviceRegistry &registry_;
  PairingTelemetry &telemetry_;
  const RecentOneWayPairingSighting &recent_oneway_sighting_;
};

}  // namespace home_io_control
}  // namespace esphome
