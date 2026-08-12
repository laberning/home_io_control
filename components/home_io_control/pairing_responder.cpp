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
  }
  return "disarmed";
}

bool on_discover_request(ResponderContext &ctx) {
  if (ctx.state != ResponderState::ARMED_IDLE && ctx.state != ResponderState::SENT_DISCOVER_RESP)
    return false;
  ctx.state = ResponderState::SENT_DISCOVER_RESP;
  return true;
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

}  // namespace pairing_responder
}  // namespace home_io_control
}  // namespace esphome
