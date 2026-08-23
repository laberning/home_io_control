#pragma once

/// @file pairing_responder.h
/// @brief Pure decision logic for the device-role "Accept Foreign Pairing" (system-key
/// extraction) responder.
/// @ingroup hioc_hub
///
/// Mirrors hub_pairing.h's PairingState enum but for the reverse role: here the hub emulates an
/// *unpaired device* so a user's existing (foreign) hub can pair to it and hand over its
/// node_id/system_key. This lets a user recover credentials from an installation they already
/// own without a separate sniffing tool or a device reset.
///
/// This header holds only the pure state-transition decisions (no radio I/O, no timers), matching
/// hub_decisions.h's split, so it is directly host-testable. The impure orchestration — arming,
/// throwaway node-ID generation, transmitting replies, the auto-off timer, and the security log
/// block — lives on IOHomeControlComponent (hub_key_extraction.cpp), which calls into this header
/// from the 0x28/0x2C/0x31/0x32 branches in process_received_packet_() (hub_status.cpp).

#include "proto_device_model.h"
#include "proto_sizes.h"

#include <cstdint>

namespace esphome {
namespace home_io_control {
namespace pairing_responder {

/// @brief State machine for the device-role key-extraction responder.
enum class ResponderState : uint8_t {
  DISARMED,            ///< Not armed; 0x28/0x2C/0x31/0x32 traffic is ignored.
  ARMED_IDLE,          ///< Armed, listening for a discovery request (0x28).
  SENT_DISCOVER_RESP,  ///< Replied to discovery (0x29); waiting for discovery-confirm (0x2C) or key-init (0x31).
  SENT_CONFIRM_ACK,    ///< Acknowledged discovery-confirm (0x2D); waiting for key-init (0x31).
  SENT_CHALLENGE,      ///< Replied to key-init with our challenge (0x3C); waiting for key-transfer (0x32).
  EXTRACTED,           ///< System key recovered from a valid 0x32. Some hubs (KIG300) need nothing more and
                       ///< the responder disarms after a grace window; others (KLR200) follow up with an
                       ///< address request (0x36) — see SENT_ADDRESS_RESP.
  SENT_ADDRESS_RESP,   ///< Answered a hub's CMD_ADDRESS_REQ (0x36) with our CMD_ADDRESS_RESP (0x37);
                       ///< waiting for the hub's own CMD_CHALLENGE_REQ (0x3C) challenging it, or a
                       ///< retry of 0x36 itself. Appended at the end of the enum rather than in
                       ///< sequence order after EXTRACTED: nothing serializes or compares this
                       ///< uint8_t-backed enum ordinally (every guard below is an explicit == set),
                       ///< so ordering is cosmetic, and appending keeps the diff minimal.
};

/// @brief Get a short, log/telemetry-friendly name for a responder state.
/// @param state Responder state.
/// @return Null-terminated lowercase string such as "sent_challenge".
const char *responder_stage_name(ResponderState state);

/// @brief Context for one key-extraction arm cycle.
///
/// Owned directly by the hub (like PairingTelemetry), not by PairingEngine/ExchangeEngine — this
/// is the reverse (device) role and does not fit either of those collaborators.
struct ResponderContext {
  ResponderState state{ResponderState::DISARMED};   ///< Current state.
  uint8_t throwaway_id[NODE_ID_SIZE]{};             ///< Random per-arm-cycle node ID we advertise as ourselves.
  DeviceType advertised_type{DeviceType::UNKNOWN};  ///< Device type advertised in our 0x29.
  uint8_t advertised_subtype{0};                    ///< Device subtype advertised in our 0x29.
  uint8_t challenge[HMAC_SIZE]{};                   ///< Our challenge, generated on the first 0x31 of an attempt.
  uint8_t hub_node_id[NODE_ID_SIZE]{};              ///< Foreign hub's real node ID, captured from the 0x31's src.
  uint8_t recovered_key[AES_KEY_SIZE]{};            ///< Recovered system key; valid once state == EXTRACTED.
};

/// @brief Decide how to react to an inbound CMD_DISCOVER_REQ (0x28) while armed.
///
/// Valid from ARMED_IDLE (first response — replies using the throwaway ID/advertised type
/// already stored in @p ctx by the caller at arm time) and SENT_DISCOVER_RESP (a hub retry —
/// resend the same values without regenerating anything). No-op from any later state: a discovery
/// request must never re-arm or restart an exchange already past this phase. That silence is what
/// a real device does too — in tests/corpus/captures/velux_kux100/pairing_full.yaml the hub
/// re-broadcasts 0x28 right after the confirm-ack and the device pointedly does not answer,
/// before the hub moves on to the key exchange. A second, independent hub does the same thing in
/// tests/corpus/captures/issues/issue_45_velux_kig300_key_extraction_success.yaml (re-broadcast
/// omitted from that capture's frame list as redundant, per its own notes).
/// @param ctx Responder context (mutated: state only).
/// @return true if the caller should build and send a CMD_DISCOVER_RESP (0x29) reply.
bool on_discover_request(ResponderContext &ctx);

/// @brief Decide how to react to an inbound CMD_DISCOVER_CONFIRM (0x2C) addressed to our
/// throwaway ID.
///
/// A hub sends 0x2C directly to a device it just discovered and, in general, will not proceed to
/// the key exchange until that device answers with CMD_DISCOVER_CONFIRM_ACK (0x2D) — the step
/// between discovery and key-init in a real pairing
/// (tests/corpus/captures/velux_kux100/pairing_full.yaml, and this project's own responder
/// answering a real hub's 0x2C in
/// tests/corpus/captures/issues/issue_45_velux_kig300_key_extraction_success.yaml — the fixed,
/// successful retest of issue_45_velux_kig300_key_extraction_stall.yaml's stalled session, where
/// that same hub's 0x2C previously went unanswered). Hub strictness varies: some retry 0x2C
/// indefinitely without it, others eventually send 0x31 anyway, which is why on_key_init() also
/// accepts a key-init straight from SENT_DISCOVER_RESP.
///
/// Valid from SENT_DISCOVER_RESP (first confirm) and SENT_CONFIRM_ACK (a hub retry after missing
/// our 0x2D — resend the same bare ack, nothing is regenerated). No-op from any other state: like
/// on_discover_request(), an early-phase frame must never pull an exchange already past this
/// point back down.
/// @param ctx Responder context (mutated: state only).
/// @return true if the caller should build and send a CMD_DISCOVER_CONFIRM_ACK (0x2D) reply.
bool on_discover_confirm(ResponderContext &ctx);

/// @brief Decide how to react to an inbound CMD_KEY_INIT (0x31) addressed to our throwaway ID.
///
/// Valid from SENT_DISCOVER_RESP and SENT_CONFIRM_ACK (first key-init: stores @p challenge and
/// @p hub_node_id, then advances — accepted from both because not every hub waits for our 0x2D,
/// see on_discover_confirm()) and SENT_CHALLENGE (a hub retry after missing our 0x3C — @p challenge is discarded
/// and the previously-stored one is reused instead, since the hub's eventual 0x32 must be
/// decrypted with the exact challenge bytes it actually received; SX1262's slow TX→RX turnaround
/// makes this retry path the expected common case, not an edge case). No-op from any other state.
/// @param ctx Responder context (mutated: state, and on the first call challenge/hub_node_id).
/// @param challenge Caller-generated 6 random bytes (crypto::generate_challenge()); only consumed
///        on the first 0x31 of an attempt.
/// @param hub_node_id The inbound frame's src (the foreign hub's real node ID).
/// @return true if the caller should build and send a CMD_CHALLENGE_REQ (0x3C) reply using
///         ctx.challenge (not @p challenge directly — they may differ on a retry).
bool on_key_init(ResponderContext &ctx, const uint8_t challenge[HMAC_SIZE], const uint8_t hub_node_id[NODE_ID_SIZE]);

/// @brief Decide how to react to an inbound CMD_KEY_TRANSFER (0x32) while armed.
///
/// Valid only from SENT_CHALLENGE. Recovers the system key from @p transfer_payload using
/// ctx.challenge via recover_system_key_from_transfer() (proto_commands.h) and, on success,
/// transitions to EXTRACTED. No-op (state unchanged) from any other state, or if decoding fails.
/// @param ctx Responder context (mutated: state, and on success recovered_key).
/// @param transfer_payload 16-byte CMD_KEY_TRANSFER payload (frame.data).
/// @return true if the key was recovered and the caller should build and send a
///         CMD_KEY_CONFIRM (0x33) reply and report ctx.recovered_key to the user.
bool on_key_transfer(ResponderContext &ctx, const uint8_t transfer_payload[AES_KEY_SIZE]);

/// @brief Decide how to react to an inbound CMD_ADDRESS_REQ (0x36) addressed to our throwaway ID.
///
/// Valid from EXTRACTED (first reply) and SENT_ADDRESS_RESP (a hub retry — resend the same 0x37,
/// nothing regenerated, matching on_discover_request()'s established retry idiom). No-op from any
/// earlier state: a hub that hasn't completed the key exchange yet has no business asking for our
/// address. No-op from DISARMED/ARMED_IDLE etc. too, same as every other on_*() here.
/// @param ctx Responder context (mutated: state only).
/// @return true if the caller should build and send a CMD_ADDRESS_RESP (0x37) reply.
bool on_address_req(ResponderContext &ctx);

/// @brief Decide how to react to an inbound CMD_CHALLENGE_REQ (0x3C) — issued by the hub this
/// time, challenging our own CMD_ADDRESS_RESP (0x37) rather than us challenging the hub.
///
/// Valid only from SENT_ADDRESS_RESP: a hub-issued 0x3C only makes sense after we've told it our
/// address. No new fields to store — the caller has everything it needs (ctx.recovered_key,
/// ctx.throwaway_id, the inbound frame's own challenge bytes) to build 0x3D on the spot.
///
/// Unlike every other on_*() in this file, this function does NOT clear or advance state on
/// success — it stays in SENT_ADDRESS_RESP precisely so a retried 0x3C is answered the same way a
/// retried 0x31 is by on_key_init(). That makes it the only on_*() here that returns true without
/// ever changing @p ctx; that is intentional, not an oversight.
/// @param ctx Responder context (read-only; not mutated).
/// @return true if the caller should build and send a CMD_CHALLENGE_RESP (0x3D) reply.
bool on_address_challenge(const ResponderContext &ctx);

}  // namespace pairing_responder
}  // namespace home_io_control
}  // namespace esphome
