#include "oneway_key_adoption.h"

#include "hub_internal.h"
#include "proto_codecs.h"

/// @file oneway_key_adoption.cpp
/// @brief Opt-in, receive-only adoption of a 1W installation's controller key.
/// @ingroup hioc_hub
///
/// A 1W device broadcasts CMD_ONEWAY_ADD_CONTROLLER (0x30) while its key-copy gesture is active,
/// handing its network's system key to whichever controller is listening. The payload is wrapped
/// with the public TRANSFER_KEY under an IV derived only from the sender's own node address, both
/// of which are available to anyone in radio range — so overhearing that one frame is enough to
/// recover the key. This file turns that into a deliberate, time-boxed, user-armed action.
///
/// **This is a property of io-homecontrol, not something this project introduces.** The same
/// framing applies as to 2W key extraction (key_extraction_responder.cpp): the protocol offers no
/// confidentiality for the key-copy gesture, so the honest response is to make the capability
/// explicit, opt-in and loud rather than to pretend it is unavailable.
///
/// Differences from its 2W sibling, both deliberate:
/// - **Receive-only.** Nothing here transmits. 2W extraction impersonates an unpaired device to
///   make a foreign hub pair *to* it; this only listens for a frame a device sends of its own
///   accord, so it cannot disturb an existing installation at all.
/// - **One adoption per arm.** The window closes as soon as a key is recovered, bounding the
///   period in which a key-bearing frame is captured and matching the single physical gesture
///   the user performs.
///
/// The recovered key is never persisted — not to NVS, not to a Home Assistant event. It is
/// reported once for the user to paste into their own YAML/secrets, which is ADR 0018's
/// paste-and-reflash shape, exactly as the 2W feature does.

namespace esphome {
namespace home_io_control {

namespace {

/// Arm window. Deliberately the same 10 minutes as the 2W responder's KEY_EXTRACTION_AUTO_OFF_MS
/// (key_extraction_responder.cpp): both windows exist for the same reason — long enough to walk to the
/// device and perform a physical gesture, short enough that forgetting to disarm is not a
/// standing exposure — and a user arming both should not have to reason about two numbers.
constexpr uint32_t ONEWAY_KEY_ADOPTION_AUTO_OFF_MS = 10 * 60 * 1000;
constexpr const char *ONEWAY_KEY_ADOPTION_TIMEOUT_NAME = "oneway_key_adoption_auto_off";

}  // namespace

void OnewayKeyAdoption::set_armed(bool armed) {
  if (!armed) {
    if (!this->armed_)
      return;
    // No cancel_timeout() here: a pending auto-off callback is harmless because it re-checks the
    // armed flag before acting (see the guard below), and set_timeout() replaces a callback of
    // the same name on re-arm. Same idiom as the 2W responder in key_extraction_responder.cpp.
    this->armed_ = false;
    ESP_LOGI(detail::TAG, "1W key adoption: disarmed");
    if (this->armed_callback_)
      this->armed_callback_(false);
    return;
  }

  this->armed_ = true;
  // Drop any class observed during an earlier window so a stale sender's type can never prefill
  // this one's report.
  this->observed_class_ = ObservedClass{};
  ESP_LOGW(detail::TAG,
           "1W key adoption: ARMED for 10 minutes. Trigger the key-copy gesture on your existing 1W remote now "
           "(the remote-to-remote copy mode described in its manual). Receive-only — nothing is transmitted.");

  this->schedule_auto_off_(ONEWAY_KEY_ADOPTION_TIMEOUT_NAME, ONEWAY_KEY_ADOPTION_AUTO_OFF_MS, [this]() {
    // Guards against a stale timeout firing after a manual disarm/re-arm already ran; set_timeout()
    // replaces a pending callback of the same name, but the check documents the intent either way.
    if (!this->armed_)
      return;
    ESP_LOGW(detail::TAG, "1W key adoption: window expired, no add-controller frame seen. Disarming.");
    this->set_armed(false);
  });

  if (this->armed_callback_)
    this->armed_callback_(true);
}

void OnewayKeyAdoption::record_observed_class(const OneWayFrameInfo &info) {
  if (!this->armed_)
    return;
  // A typed broadcast names a device class; the add-controller frame itself targets "all" and
  // decodes to UNKNOWN, so skipping UNKNOWN keeps the 0x30 from clobbering a genuine earlier
  // observation from the same sender.
  if (info.target_type == DeviceType::UNKNOWN)
    return;
  memcpy(this->observed_class_.node, info.src, NODE_ID_SIZE);
  this->observed_class_.type = info.target_type;
  this->observed_class_.valid = true;
}

void OnewayKeyAdoption::try_adopt(const IoFrame &frame) {
  if (!this->armed_)
    return;
  if (frame.cmd != CMD_ONEWAY_ADD_CONTROLLER)
    return;

  OneWayAdoptedKey adopted{};
  const OneWayAddControllerDecodeError error = decode_1w_add_controller(frame, adopted);
  if (error != OneWayAddControllerDecodeError::NONE) {
    // Stay armed: a malformed 0x30 is far more likely to be a corrupted capture than the user's
    // real gesture, and disarming here would make them re-arm and repeat the gesture for nothing.
    ESP_LOGW(detail::TAG, "1W key adoption: heard an add-controller frame from %s but could not decode it (error %u)",
             node_id_to_string(frame.src).c_str(), static_cast<unsigned>(error));
    return;
  }

  // Prefill io_device_type from this sender's other 1W traffic, if any was overheard during this
  // window. The 0x30 itself broadcasts to "all" and carries no class, so without this the user
  // would have to guess which class their device answers to.
  const bool observed_known =
      this->observed_class_.valid && memcmp(this->observed_class_.node, adopted.sender_node, NODE_ID_SIZE) == 0;
  const DeviceType observed_type = observed_known ? this->observed_class_.type : DeviceType::UNKNOWN;

  // The single intentional emission of the recovered key — a deliberate, narrow exception to
  // redaction.h's masking, exactly as KeyExtractionResponder::log_result_() is for the 2W path. The key
  // must not reach any other log path, and the generic frame-log helpers keep masking 0x30.
  //
  // The report is logged line-by-line via log_multiline_result(), not as one ESP_LOGW("%s", ...)
  // call: a single call silently truncates at ESPHome's 512-byte log buffer, and this report is
  // long enough to do exactly that — cutting off before the recovered key ever appears, which
  // defeats the entire feature with no error and no indication anything was lost. See
  // log_multiline_result()'s doxygen (hub_internal.h) for the root cause.
  ESP_LOGW(detail::TAG, "========================================");
  ESP_LOGW(detail::TAG, "1W CONTROLLER KEY ADOPTED FROM %s -- DO NOT SHARE THIS KEY",
           node_id_to_string(adopted.sender_node).c_str());
  detail::log_multiline_result(detail::TAG, /*is_warning=*/true, /*prefix=*/"",
                               detail::build_oneway_adoption_report(adopted, observed_known, observed_type));
  ESP_LOGW(detail::TAG, "========================================");

  // One adoption per arm.
  this->set_armed(false);
}

}  // namespace home_io_control
}  // namespace esphome
