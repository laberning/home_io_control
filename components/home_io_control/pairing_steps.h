#pragma once

/// @file pairing_steps.h
/// @brief Steps the pairing flow's parts share: entering a state, the start preamble, noting a refusal.
/// @ingroup hioc_hub
///
/// Push pairing (PairingEngine), the key pull (KeyPull) and the key-holder search (KeyHolderSearch) are
/// separate classes. What more than one of them does the same way lives here, once, as free functions
/// over the collaborators they hold, so no part reaches into another.

#include "exchange_engine.h"
#include "hub_pairing.h"
#include "pairing_telemetry.h"
#include "proto_frame.h"
#include "tuning_config.h"

#include <cstdint>

namespace esphome {
namespace home_io_control {

/// Wait for a device's unicast reply to a unicast key-step frame: the 0x33 after our 0x32, the 0x32 after
/// our 0x38.
inline constexpr uint32_t PAIRING_KEY_REPLY_TIMEOUT_MS = 500;

/// @brief Move the attempt to `state`: set it on the context, record the exchange debug stage and the
/// telemetry phase together.
/// @param engine Exchange engine whose debug snapshot gets the stage name.
/// @param telemetry Recorder that gets the phase.
/// @param context Pairing context whose `state` is set.
/// @param state The state entered.
/// @param try_number Try number for the debug stage (1 for a step that runs once).
/// @param saw_challenge The debug snapshot's `saw_challenge` flag for this stage.
void enter_pairing_state(ExchangeEngine &engine, PairingTelemetry &telemetry, pairing::PairingContext &context,
                         pairing::PairingState state, uint8_t try_number, bool saw_challenge);

/// @brief Preamble for a directed pairing start frame (0x2C, 0x31, 0x38, 0x6F) to the device discovery
/// just found: the shorter of ExchangeEngine::request_preamble_for()'s rule and the preamble the
/// discovery request went out with (`effective_discovery_preamble_ms()`).
///
/// The device answered a discovery request carrying that preamble moments ago, so it is listening and
/// demonstrably hears it; pairing follows within seconds. The long wake-up preamble the rule would
/// otherwise pick for a low-power target is not just unnecessary then: some VELUX receivers never detect
/// a frame behind a 1024-byte preamble at all (ADR 0029). Without a tuned discovery preamble the cap is
/// the start preamble, so a low-power target's frames follow discovery down to it; with a low-power
/// discovery frame (wake-up preamble) the result equals the rule.
/// @param engine Exchange engine whose rule applies.
/// @param tuning Live tuning (the discovery preamble).
/// @param frame The start frame about to be sent.
/// @return Preamble length in bytes.
[[nodiscard]] uint16_t pairing_start_preamble(const ExchangeEngine &engine, const TuningConfig &tuning,
                                              const IoFrame &frame);

/// @brief Open a pairing or key-pull session with @p frame through the engine's channel access on the
/// band's wake-up channels (ADR 0133), and keep the channel in `context.session_freq`.
///
/// Every later frame of the session goes out on `context.session_freq` and every wait for its answers
/// listens there. A step whose channel stays busy ends like a try that got no answer: this returns false
/// and nothing was sent. The next session avoids the channel of the previous one where the set has another.
/// @param engine Exchange engine whose channel access applies.
/// @param context Pairing context; `session_freq` is read as the previous channel and set to the new one.
/// @param frame The frame that opens the session.
/// @param preamble Preamble of the frame in programmed bytes.
/// @return true when the frame went out.
[[nodiscard]] bool open_pairing_session(ExchangeEngine &engine, pairing::PairingContext &context, const IoFrame &frame,
                                        uint16_t preamble);

/// @brief The channel a frame of pairing names where it needs one before a session has opened: the first
/// of the band's wake-up channels. Only the estimate of a transmit time and the channel reported for a frame
/// that cannot be built read it; an opening frame takes its channel from open_pairing_session().
/// @param engine Exchange engine whose band applies.
[[nodiscard]] uint32_t nominal_pairing_freq(const ExchangeEngine &engine);

/// @brief Record a CMD_ERROR_RESP answer to the key step: context flag, telemetry, and a decoded log line.
/// @param telemetry Recorder that gets the refusal code.
/// @param context Pairing context to mark as refused.
/// @param reply The device's CMD_ERROR_RESP frame.
/// @param step Log label for the refused step (e.g. "Key transfer").
void note_key_refusal(PairingTelemetry &telemetry, pairing::PairingContext &context, const IoFrame &reply,
                      const char *step);

}  // namespace home_io_control
}  // namespace esphome
