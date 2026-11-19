#pragma once

/// @file transfer_reader.h
/// @brief Queued, non-blocking reads of device data objects over the data transport.
/// @ingroup hioc_hub
///
/// TransferReader reads one object at a time from a device, one exchange per step, without ever blocking
/// loop(): a read of a few blocks takes several seconds once the TRANSFER_STEP_GAP_MS gaps are counted,
/// and done in one go it would stall the API, the covers and the other devices. Every step is a named
/// timeout that does one exchange and arms the next, so the loop runs in between. The steps come from a
/// TransferSession, the same one the `read_object` probe drives in a blocking loop (ADR 0099).
///
/// The reader gives way, because a device serves one transfer at a time and keeps the handle of an opened
/// one until it is closed:
///   - while the hub holds the radio (TransferReaderHub::background_radio_allowed) a step retries a little
///     later, like a background poll;
///   - a read starts only after TRANSFER_FOREIGN_QUIET_MS without a data-transport frame of another
///     controller to or from that device, and one that appears mid-read closes the transfer and ends the read
///     ABANDONED. A read of a held device waits alone: the first queued read whose device is quiet runs first,
///     reads of quiet devices stay first in, first out, and a started read is never reordered (ADR 0108);
///   - a device removed from the registry mid-read ends the read ABANDONED without a close: a transfer it
///     accepted is left to the device to drop (ADR 0108);
///   - while a read of a device is in progress (transfer_open_to()), the hub keeps that device's queued
///     background operations waiting.
///
/// Read-only by construction: the requests come from create_transfer_open() and create_transfer_read(), which
/// refuse anything outside TRANSFER_RECIPES, and nothing here builds the write twin of the block.
///
/// The class reaches the hub through TransferReaderHub, four injected calls, and includes no hub header
/// (ADR 0083, ADR 0084). The hub queues the reads of its data-object readings (DataObjectSource).

#include "device_registry.h"
#include "hub_hooks.h"
#include "proto_constants.h"
#include "proto_frame.h"
#include "transfer_session.h"

#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace esphome {
namespace home_io_control {

/// Called once, from the loop, when a queued read ends: how it ended and the bytes read (all of them for
/// READ, the part read for INCOMPLETE or ABANDONED, none otherwise). The buffer is only valid during the call.
using TransferReadDone = std::function<void(TransferOutcome outcome, const std::vector<uint8_t> &data)>;

/// @brief The hub capabilities a TransferReader needs, injected by the hub in its initializer list.
struct TransferReaderHub {
  NamedTimeoutFn set_timeout;                      ///< The hub's set_timeout(), which is protected.
  std::function<bool()> background_radio_allowed;  ///< True when background radio work may run now.
  std::function<const IoDevice *(const std::string &device_id)> find_device;  ///< Registry lookup.
  RecordedExchangeFn exchange;  ///< One recorded exchange; see RecordedExchangeFn. Its RecordedAnswer decides NO_DATA.
};

/// @brief Reads data objects of devices one at a time without blocking the loop.
///
/// Non-copyable: its timeout callbacks capture `this`.
/// @ingroup hioc_hub
class TransferReader {
 public:
  /// Waiting reads allowed behind the one in progress. A caller that queues faster than reads complete
  /// must not eat the heap; a read beyond the limit ends at once as ABANDONED.
  static constexpr size_t QUEUE_MAX = 80;

  /// @param node_id Controller 3-byte node ID, owned by the hub.
  /// @param recipes The recipes a read may name, and the allowlist of its opens; the hub passes TRANSFER_RECIPES, a
  ///        test its own table. Must outlive the reader.
  /// @param hub The injected hub calls.
  TransferReader(const uint8_t *node_id, std::span<const TransferRecipe> recipes, TransferReaderHub hub)
      : node_id_(node_id), recipes_(recipes), hub_(std::move(hub)) {}

  /// @brief Replace the recipe table: the allowlist of the opens and the rows a read names.
  /// @details For a test that runs the hub against a recipe table of its own, since the production table has no row
  ///          for a scheduled read until a device needs one. Call it with no read queued; reads already queued name
  ///          rows of the old table.
  /// @param recipes The new table; must outlive the reader.
  void set_recipes(std::span<const TransferRecipe> recipes) { this->recipes_ = recipes; }

  TransferReader(const TransferReader &) = delete;
  TransferReader &operator=(const TransferReader &) = delete;

  /// @brief Queue a read of one object of a device.
  ///
  /// A read that cannot be queued ends at once, inside this call: REFUSED for an unknown recipe, an offset the
  /// recipe does not allow or an unregistered device, ABANDONED for a full queue. Otherwise the callback runs
  /// from the loop when the read ends, never inside this call.
  /// @param device_id Hexadecimal node ID string of a registered device.
  /// @param recipe_index Row of the recipe table passed to the constructor.
  /// @param offset One of the recipe's offsets (0 when it lists none).
  /// @param done Called once when the read ends.
  void read(const std::string &device_id, size_t recipe_index, uint16_t offset, TransferReadDone done);

  /// @brief Note a received or overheard frame: a data-transport frame (CMD_TRANSFER_OPEN to CMD_UNKNOWN4A_RESP)
  /// that is not addressed to this hub belongs to another controller, and holds the reads of each registered
  /// device it names (as source or destination) for TRANSFER_FOREIGN_QUIET_MS.
  /// @param frame The parsed frame.
  /// @param now millis() when it was heard.
  void note_transfer_frame(const IoFrame &frame, uint32_t now);

  /// @brief True while a read of @p device_id is in progress: from its first request until it ends. The hub
  /// keeps that device's queued background operations waiting, so a status poll cannot cut the transfer.
  [[nodiscard]] bool transfer_open_to(const std::string &device_id) const;

 private:
  /// One queued read.
  struct Read {
    std::string device_id;
    size_t recipe_index{0};
    uint16_t offset{0};
    TransferReadDone done;
  };
  /// When another controller last used the data transport to or from a registered device.
  struct ForeignTransfer {
    std::string device_id;
    uint32_t heard_ms{0};
  };

  /// True while @p device_id had a foreign transfer frame less than TRANSFER_FOREIGN_QUIET_MS ago; drops
  /// expired entries.
  [[nodiscard]] bool foreign_active_(const std::string &device_id);
  void drop_expired_foreign_(uint32_t now);
  /// Run @p step after @p delay_ms; while the hub holds the radio, retry it a little later.
  void after_(uint32_t delay_ms, std::function<void()> step);
  /// Start the first queued read whose device is quiet, moved to the front; when every queued device is held, wait
  /// TRANSFER_FOREIGN_RETRY_MS and choose again. Only called with no read in progress.
  void start_next_();
  /// One step of the front read: one exchange, or a wait.
  void step_();
  /// End the front read, report it and start the next one.
  void finish_(TransferOutcome outcome);

  const uint8_t *node_id_;
  std::span<const TransferRecipe> recipes_;
  TransferReaderHub hub_;
  std::deque<Read> reads_;  ///< Waiting reads, oldest first; the front one is in progress while session_.
  std::optional<TransferSession> session_;  ///< The front read's session; empty while idle.
  std::vector<ForeignTransfer> foreign_;
  uint32_t last_exchange_ms_{0};  ///< millis() after the last exchange of any read; valid once exchanged_.
  bool exchanged_{false};
  bool waiting_for_quiet_{false};  ///< Every queued device is held and a choice is pending; no session exists.
};

}  // namespace home_io_control
}  // namespace esphome
