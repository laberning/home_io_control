/// @file hub_core.cpp
/// @brief Component lifecycle and main-loop scheduling.
/// @ingroup hioc_hub
///
/// The core file owns the parts of IOHomeControlComponent that are primarily about
/// runtime orchestration rather than protocol interpretation:
/// - hardware/radio setup,
/// - main loop scheduling,
/// - device registry and callback fan-out.
///
/// Protocol exchange, pairing, inbound status handling, and outbound operations live
/// in dedicated translation units so this file remains the place to understand how the
/// component is brought up and driven over time.

#include "hub_internal.h"

#include "radio_sx1276.h"
#include "radio_sx1262.h"
#include "radio_lr1121.h"
#include "tuning_config.h"
#include "tuning_registry.h"

#include <cinttypes>
#include <new>
#include <vector>

namespace esphome {
namespace home_io_control {

namespace {

constexpr uint32_t BLOCKING_WARNING_THRESHOLD_MS =
    250;  ///< setup() and exchanges can legitimately block longer than generic ESPHome components.

}  // namespace

static const char *const TAG = detail::TAG;

// === Setup ===

/// Initialize the IO‑Homecontrol component and radio hardware.
///
/// This is the main setup entry point called by ESPHome during startup.
/// The sequence:
///   1. Parse node_id and system_key from hex strings (fails early if malformed).
///   2. Initialize the SPI bus via spi_setup().
///   3. Construct the driver named by the required `radio_type` YAML field
///      ("sx1276", "sx1262", or "lr1121").
///   4. Allocate the appropriate RadioDriver (SX1276 needs DIO0; SX1262/LR1121 need BUSY+DIO1,
///      DIO1 carrying the LR1121's DIO9 IRQ line).
///   5. Call radio_->init() which performs chip reset, calibration, and register configuration.
///   6. Enter normal loop() operation with radio in RX mode.
///
/// @note Blocking operations in setup() temporarily raise the ESPHome WDT threshold
///       to 250 ms (warn_if_blocking_over_) because radio init can
///       exceed the default 30–50 ms budget.
void IOHomeControlComponent::setup() {
  // IO-homecontrol exchanges are intentionally blocking and often take a few hundred
  // milliseconds, so use a higher warning threshold than ESPHome's generic 30-50 ms.
  this->warn_if_blocking_over_ = BLOCKING_WARNING_THRESHOLD_MS;
#ifdef IOHOME_UNSAFE_LOG_KEY_MATERIAL
  // Loud, unconditional, every-boot warning so a build left with this flag on by accident can
  // never stay quiet about it — see log_frame.h::render_frame_hex_redacted() for the full
  // rationale and safe-use rules.
  ESP_LOGE(detail::TAG, "########################################");
  ESP_LOGE(detail::TAG, "IOHOME_UNSAFE_LOG_KEY_MATERIAL IS ENABLED -- FRAME LOGS EXPOSE YOUR SYSTEM KEY");
  ESP_LOGE(detail::TAG, "This build is NOT safe to run normally or share logs from. Rebuild without this");
  ESP_LOGE(detail::TAG, "flag as soon as you are done capturing.");
  ESP_LOGE(detail::TAG, "########################################");
#endif
  ESP_LOGI(detail::TAG, "Initializing...");
  if (!hex_to_bytes(this->node_id_str_, this->node_id_, NODE_ID_SIZE) ||
      !hex_to_bytes(this->system_key_str_, this->system_key_, AES_KEY_SIZE)) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) — ESPHome's own LOG_STR() macro.
    this->mark_failed(LOG_STR("Invalid node_id or system_key configuration"));
    return;
  }

  this->spi_setup();

  const char *chip_name_for_log = nullptr;
  this->radio_ = this->select_and_construct_radio_(&chip_name_for_log);
  if (this->radio_ == nullptr) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) — ESPHome's own LOG_STR() macro.
    this->mark_failed(LOG_STR("Radio driver selection/allocation failed (see earlier log for details)"));
    return;
  }

#ifdef IOHOME_LR1121_FIRMWARE_UPDATE
  // Boot-time bootloader-version excursion — deliberately before
  // init(): nothing has configured the radio yet, so this costs one extra chip reset and needs no
  // reboot afterward, unlike every other bootloader excursion this feature performs.
  this->run_lr1121_boot_time_bootloader_read_();
#endif

  if (!this->radio_->init()) {
    delete this->radio_;
    this->radio_ = nullptr;
#ifdef IOHOME_LR1121_FIRMWARE_UPDATE
    // Cache the verdict even on a failed init() — a null radio_ is exactly the case
    // trigger_lr1121_firmware_update()'s guard 0 still allows an attempt for (reflashing is the
    // recovery), and it needs a cached "installed version unknown" verdict to route through.
    this->cache_lr1121_flash_verdict_();
#endif
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) — ESPHome's own LOG_STR() macro.
    this->mark_failed(LOG_STR("Radio hardware initialization failed (see earlier log for details)"));
    return;
  }

#ifdef IOHOME_LR1121_FIRMWARE_UPDATE
  this->cache_lr1121_flash_verdict_();
#endif

  this->initialized_ = true;
  this->register_management_actions_();
  this->exchange_engine_.reset_hop_timestamp();
  this->apply_tuning_to_radio_();
  if (this->tuning_.active) {
    std::string const snapshot = tuning_config_full_snapshot(this->tuning_);
    ESP_LOGI(detail::TAG, "%s", snapshot.c_str());
  }
  ESP_LOGI(detail::TAG, "Radio initialized (%s), Node ID: %s", chip_name_for_log, this->node_id_str_.c_str());
}

// See the declaration in hub_core.h for the full contract. `radio_type_` is one of "lr1121",
// "sx1262", or "sx1276" — the YAML schema requires the field and validates it against exactly
// those three values, so the fallthrough below is unreachable in a config-driven build and
// exists only to fail loudly rather than guess if this method is ever called some other way.
RadioDriver *IOHomeControlComponent::select_and_construct_radio_(const char **chip_name_out) {
  if (this->radio_type_ == "lr1121") {
    *chip_name_out = "LR1121";
    if (this->busy_pin_ == nullptr || this->dio1_pin_ == nullptr) {
      ESP_LOGE(detail::TAG, "LR1121 requires busy_pin and dio1_pin (dio1_pin carries the chip's DIO9 IRQ line)");
      return nullptr;
    }
    auto *radio = new (std::nothrow)
        RadioLR1121(this, this->rst_pin_, this->dio1_pin_, this->busy_pin_, this->tx_power_, this->tcxo_voltage_);
    if (radio == nullptr)
      ESP_LOGE(detail::TAG, "Failed to allocate LR1121 radio driver");
    return radio;
  }

  if (this->radio_type_ == "sx1262") {
    *chip_name_out = "SX1262";
    if (this->busy_pin_ == nullptr || this->dio1_pin_ == nullptr) {
      ESP_LOGE(detail::TAG, "SX1262 requires busy_pin and dio1_pin");
      return nullptr;
    }
    auto *radio =
        new (std::nothrow) RadioSX1262(this, this->rst_pin_, this->dio1_pin_, this->busy_pin_, this->tx_power_,
                                       this->tcxo_voltage_, this->fem_en_pin_, this->vfem_pin_, this->fem_pa_pin_);
    if (radio == nullptr)
      ESP_LOGE(detail::TAG, "Failed to allocate SX1262 radio driver");
    return radio;
  }

  if (this->radio_type_ == "sx1276") {
    *chip_name_out = "SX1276";
    if (this->dio0_pin_ == nullptr) {
      ESP_LOGE(detail::TAG, "SX1276 requires dio0_pin");
      return nullptr;
    }
    auto *radio = new (std::nothrow)
        RadioSX1276(this, this->rst_pin_, this->dio0_pin_, this->dio4_pin_, this->tx_power_, this->pa_pin_);
    if (radio == nullptr)
      ESP_LOGE(detail::TAG, "Failed to allocate SX1276 radio driver");
    return radio;
  }

  *chip_name_out = "unknown";
  ESP_LOGE(detail::TAG, "Unrecognized radio_type '%s'", this->radio_type_.c_str());
  return nullptr;
}

// === Tuning layer ===

/// Apply the current tuning configuration to the active radio driver.
///
/// Only chip-specific parameters are forwarded; the rest are consumed by the
/// pairing flow and LBT logic. This is called once at the end of setup() and
/// again whenever a UI-driven change modifies a radio parameter.
void IOHomeControlComponent::apply_tuning_to_radio_() {
  if (this->radio_ == nullptr)
    return;
  this->radio_->apply_tuning(this->tuning_);
}

/// Update a numeric tuning parameter from a Home Assistant `number` entity.
///
/// Parses the parameter name and applies the new value to the in-memory tuning
/// configuration. Radio-affecting parameters are forwarded to the active driver
/// immediately; the change is logged in YAML-compatible form so it can be copied
/// back into the configuration file.
void IOHomeControlComponent::update_tuning_number(const std::string &name, float value) {
  const TuningNumberParam *param = find_tuning_number(name);
  if (param == nullptr) {
    ESP_LOGW(detail::TAG, "Unknown tuning number parameter: %s", name.c_str());
    return;
  }
  param->set(this->tuning_, value);
  if (param->applies_to_radio)
    this->apply_tuning_to_radio_();
  ESP_LOGI(detail::TAG, "%s", tuning_update_log_line(name, std::to_string(static_cast<int>(value))).c_str());
}

/// Update a select tuning parameter from a Home Assistant `select` entity.
///
/// Parses the selected option string and applies it to the in-memory tuning
/// configuration. Radio-affecting parameters are forwarded to the active driver
/// immediately; the change is logged in YAML-compatible form.
void IOHomeControlComponent::update_tuning_select(const std::string &name, const std::string &value) {
  const TuningSelectParam *param = find_tuning_select(name);
  if (param == nullptr) {
    ESP_LOGW(detail::TAG, "Unknown tuning select parameter: %s", name.c_str());
    return;
  }
  // Radio-affecting parameters re-apply only when the option string actually parsed; the
  // update is still logged for any known parameter, matching the original dispatch.
  if (param->set(this->tuning_, value) && param->applies_to_radio)
    this->apply_tuning_to_radio_();
  ESP_LOGI(detail::TAG, "%s", tuning_update_log_line(name, value).c_str());
}

/// Return the current value of a numeric tuning parameter.
///
/// Mirror of update_tuning_number(); used by IOHomeTuningNumber::setup() to publish
/// the boot-time value so the Home Assistant slider reflects the active configuration
/// (default or YAML override) without restating any default on the Python side.
float IOHomeControlComponent::get_tuning_number_value(const std::string &name) const {
  const TuningNumberParam *param = find_tuning_number(name);
  if (param == nullptr) {
    ESP_LOGW(detail::TAG, "Unknown tuning number parameter: %s", name.c_str());
    return 0.0F;
  }
  return param->get(this->tuning_);
}

/// Return the current option string of a select tuning parameter.
///
/// Mirror of update_tuning_select(); used by IOHomeTuningSelect::setup() to publish the
/// boot-time option so the Home Assistant dropdown reflects the active configuration. The
/// returned strings match the YAML/UI option labels exactly. The command list is returned
/// as a comma-separated preset string (e.g. "0x28,0x2E") matching the dropdown options.
std::string IOHomeControlComponent::get_tuning_select_value(const std::string &name) const {
  const TuningSelectParam *param = find_tuning_select(name);
  if (param == nullptr) {
    ESP_LOGW(detail::TAG, "Unknown tuning select parameter: %s", name.c_str());
    return "";
  }
  return param->get(this->tuning_);
}

// === Protocol send/receive (thin wrappers delegating to ExchangeEngine) ===

/// Delegate channel hop to ExchangeEngine (which owns last_hop_us_).
void IOHomeControlComponent::hop_frequency_() { this->exchange_engine_.hop_frequency(); }

/// Delegate LBT transmit to ExchangeEngine.
bool IOHomeControlComponent::transmit_frame_(const IoFrame &frame, uint32_t freq, uint16_t preamble) {
  return this->exchange_engine_.transmit_frame(frame, freq, preamble);
}

/// Delegate outbound exchange to ExchangeEngine and manage the busy_ flag.
bool IOHomeControlComponent::send_and_receive_(const IoFrame &request, IoFrame &response, uint32_t freq) {
  this->busy_ = true;
  bool const ok = this->exchange_engine_.send_and_receive(request, response, freq);
  this->busy_ = false;
  return ok;
}

/// Delegate inbound authentication to ExchangeEngine.
bool IOHomeControlComponent::authenticate_request_(const IoFrame &request, uint32_t freq) {
  return this->exchange_engine_.authenticate_request(request, freq);
}

void IOHomeControlComponent::notify_device_update_(const std::string &id) { this->registry_.notify(id); }

// === Device management ===

void IOHomeControlComponent::set_device_status_poll_interval(const std::string &device_id, uint32_t poll_interval_ms) {
  if (this->get_device(device_id) == nullptr)
    return;
  this->poll_policy_.set_interval(device_id, poll_interval_ms);
}

void IOHomeControlComponent::schedule_background_poll_backoff_(const std::string &device_id, bool auth_like) {
  uint32_t const now = millis();
  uint32_t const backoff_ms = this->poll_policy_.on_exchange_failed(device_id, auth_like, now);
  if (backoff_ms > 0) {
    ESP_LOGD(TAG,
             "Background status poll backoff for device %s: delay=%" PRIu32
             " ms auth_like=%s status_failures=%u auth_failures=%u",
             device_id.c_str(), backoff_ms, YESNO(auth_like), this->poll_policy_.get_status_poll_failures(device_id),
             this->poll_policy_.get_auth_poll_failures(device_id));
  }
}

void IOHomeControlComponent::add_device(const std::string &device_id) { this->registry_.add(device_id); }

void IOHomeControlComponent::add_device(const std::string &device_id, const DeviceConfig &cfg) {
  this->registry_.add(device_id, cfg);
}

IoDevice *IOHomeControlComponent::get_device(const std::string &device_id) { return this->registry_.get(device_id); }

void IOHomeControlComponent::set_device_dimmable(const std::string &device_id, bool dimmable) {
  this->registry_.set_dimmable(device_id, dimmable);
}

// === Main loop ===

void IOHomeControlComponent::loop() {
  if (!this->initialized_)
    return;
  if (this->radio_test_mode_)
    return;

  // Check for received packets (non-blocking)
  if (!this->busy_) {
    RadioRxPacket packet{};
    if (this->radio_->check_for_packet(packet))
      this->process_received_packet_(packet);
  }

  // A blocking exchange makes the radio deaf for 1–3 s. When a linked remote's press schedules a
  // status poll, dispatching it while that same remote is still transmitting would blind the hub
  // to the rest of the press — so background polls yield for a moment. Control operations never do.
  if (!this->busy_ && !this->defer_background_poll_())
    this->process_pending_operation_();

  // Frequency hopping — protocol specifies 2.7ms per channel, but ESPHome calls
  // loop() every ~16-30ms. This is acceptable for a controller: we initiate all
  // exchanges with a long preamble (1024 bytes ≈ 330ms airtime) so the device has
  // time to detect us regardless of channel alignment. Precise hopping would only
  // matter for a passive receiver scanning for unsolicited frames.
  if (!this->busy_)
    this->exchange_engine_.maybe_hop();

  // Periodic status polling
  if (!this->busy_) {
    auto due = this->poll_policy_.pop_due_device(millis());
    if (due.has_value())
      this->queue_request_device_status(*due);
  }
}

void IOHomeControlComponent::dump_config() {
  ESP_LOGCONFIG(detail::TAG, "IO-Homecontrol:");
  ESP_LOGCONFIG(detail::TAG, "  Node ID: %s", this->node_id_str_.c_str());
  ESP_LOGCONFIG(detail::TAG, "  Radio: %s", this->radio_type_.c_str());
  ESP_LOGCONFIG(detail::TAG, "  TX Power: %u dBm", this->tx_power_);
  LOG_PIN("  RST Pin: ", this->rst_pin_);
  if (this->dio0_pin_ != nullptr)
    LOG_PIN("  DIO0 Pin: ", this->dio0_pin_);
  if (this->dio1_pin_ != nullptr)
    LOG_PIN("  DIO1 Pin: ", this->dio1_pin_);
  if (this->dio4_pin_ != nullptr)
    LOG_PIN("  DIO4 Pin: ", this->dio4_pin_);
  if (this->busy_pin_ != nullptr)
    LOG_PIN("  BUSY Pin: ", this->busy_pin_);
  ESP_LOGCONFIG(detail::TAG, "  Devices: %zu", this->registry_.size());
  if (this->registry_.linked_remote_count() > 0) {
    ESP_LOGCONFIG(detail::TAG, "  Linked Remotes: %zu", this->registry_.linked_remote_count());
    this->registry_.for_each_linked_remote([](const std::string &remote_id, const std::vector<std::string> &devices) {
      for (const auto &device_id : devices)
        ESP_LOGCONFIG("home_io_control", "    - remote %s -> device %s", remote_id.c_str(), device_id.c_str());
    });
  }

  if (this->radio_ != nullptr)
    this->radio_->dump_debug();

#ifdef IOHOME_LR1121_FIRMWARE_UPDATE
  this->dump_lr1121_firmware_update_debug_();
#endif
}

}  // namespace home_io_control
}  // namespace esphome
