/// @file pairing_responder.cpp
/// @brief Pure decision logic for the device-role key-extraction responder.
/// @ingroup hioc_hub

#include "pairing_responder.h"

#include "proto_commands.h"

#include <cstring>

namespace esphome {
namespace home_io_control {
namespace pairing_responder {

const char *responder_stage_name(ResponderState state) {
  switch (state) {
    case ResponderState::DISARMED:
      return "disarmed";
    case ResponderState::ARMED_IDLE:
      return "armed_idle";
    case ResponderState::SENT_DISCOVER_RESP:
      return "sent_discover_resp";
    case ResponderState::SENT_CONFIRM_ACK:
      return "sent_confirm_ack";
    case ResponderState::SENT_CHALLENGE:
      return "sent_challenge";
    case ResponderState::EXTRACTED:
      return "extracted";
    case ResponderState::SENT_ADDRESS_RESP:
      return "sent_address_resp";
  }
  return "disarmed";
}

bool on_discover_request(ResponderContext &ctx, const uint8_t hub_node_id[NODE_ID_SIZE]) {
  if (ctx.state == ResponderState::ARMED_IDLE || ctx.state == ResponderState::SENT_DISCOVER_RESP) {
    ctx.state = ResponderState::SENT_DISCOVER_RESP;
    return true;
  }
  // EXTRACTED/SENT_ADDRESS_RESP are a *completed* attempt, not an in-flight one — unlike
  // SENT_CONFIRM_ACK above, a fresh 0x28 arriving here cannot be the same hub's redundant
  // mid-exchange rebroadcast (the only documented real-hub case for staying silent; see the
  // doxygen), so it is treated like ARMED_IDLE: start a new attempt. But ONLY for the hub this
  // responder actually extracted a key from — 0x28 is a broadcast handled before the throwaway-ID
  // dst filter, so accepting it unconditionally here would let any unrelated hub's ordinary 0x28
  // traffic knock a live post-extraction address-verification round with the real hub back to
  // SENT_DISCOVER_RESP. A different hub's 0x28 is silently ignored instead, exactly as it would
  // have been before this widening.
  if (ctx.state == ResponderState::EXTRACTED || ctx.state == ResponderState::SENT_ADDRESS_RESP) {
    if (memcmp(hub_node_id, ctx.hub_node_id, NODE_ID_SIZE) != 0)
      return false;
    ctx.state = ResponderState::SENT_DISCOVER_RESP;
    return true;
  }
  return false;
}

bool on_discover_confirm(ResponderContext &ctx) {
  if (ctx.state != ResponderState::SENT_DISCOVER_RESP && ctx.state != ResponderState::SENT_CONFIRM_ACK)
    return false;
  ctx.state = ResponderState::SENT_CONFIRM_ACK;
  return true;
}

bool on_key_init(ResponderContext &ctx, const uint8_t challenge[HMAC_SIZE], const uint8_t hub_node_id[NODE_ID_SIZE]) {
  // Both pre-key-exchange states are accepted: a hub that insists on our 0x2D arrives here from
  // SENT_CONFIRM_ACK, one that gives up waiting for it arrives straight from SENT_DISCOVER_RESP.
  if (ctx.state == ResponderState::SENT_DISCOVER_RESP || ctx.state == ResponderState::SENT_CONFIRM_ACK) {
    memcpy(ctx.challenge, challenge, HMAC_SIZE);
    memcpy(ctx.hub_node_id, hub_node_id, NODE_ID_SIZE);
    ctx.state = ResponderState::SENT_CHALLENGE;
    return true;
  }
  // Retry: the hub already got past this phase once — keep the previously-stored challenge and
  // hub node ID, just resend the same 0x3C (see doxygen for why regenerating would be wrong).
  return ctx.state == ResponderState::SENT_CHALLENGE;
}

bool on_key_transfer(ResponderContext &ctx, const uint8_t transfer_payload[AES_KEY_SIZE]) {
  if (ctx.state != ResponderState::SENT_CHALLENGE)
    return false;
  if (!recover_system_key_from_transfer(transfer_payload, ctx.challenge, ctx.recovered_key))
    return false;
  ctx.state = ResponderState::EXTRACTED;
  return true;
}

bool on_address_req(ResponderContext &ctx) {
  if (ctx.state != ResponderState::EXTRACTED && ctx.state != ResponderState::SENT_ADDRESS_RESP)
    return false;
  ctx.state = ResponderState::SENT_ADDRESS_RESP;
  return true;
}

bool on_address_challenge(const ResponderContext &ctx) { return ctx.state == ResponderState::SENT_ADDRESS_RESP; }

}  // namespace pairing_responder
}  // namespace home_io_control
}  // namespace esphome
