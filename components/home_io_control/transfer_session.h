#pragma once

/// @file transfer_session.h
/// @brief One read of a device object over the data transport, as a pure request/answer state machine.
/// @ingroup hioc_hub
///
/// A TransferSession walks one TransferRecipe at one offset: the open steps, the block reads counting
/// down, and the close. It sends nothing and waits for nothing. A runner asks it for the next request,
/// performs the exchange its own way (the `read_object` probe in a blocking loop, TransferReader one
/// step per timeout) and hands the answer back, so the order of requests and the meaning of each answer
/// exist once (ADR 0099). Once a device accepted an open, the session's last request is always the close,
/// whatever went wrong in between.
///
/// The session includes no hub header and no radio header: it is tested with answers written by hand.

#include "proto_codecs.h"
#include "proto_constants.h"
#include "proto_frame.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace esphome {
namespace home_io_control {

/// @brief How a transfer read ended.
enum class TransferOutcome : uint8_t {
  READ,            ///< Every announced block was read; the data is complete.
  EMPTY,           ///< The device answered the open and has nothing to read (length 0, or status "nothing to read").
  DECLINED,        ///< The device answered the open with a status other than accepted, adapted or "no such object"
                   ///< (busy, another transfer open, ...): the object may exist, so asking again later can succeed.
  NO_SUCH_OBJECT,  ///< The device does not have the object: it answered the open with an error reply or with
                   ///< TRANSFER_STATUS_NO_SUCH_OBJECT.
  REFUSED,         ///< The hub itself refused to open: an unknown recipe, an offset the recipe does not list or an
                   ///< unregistered device. Nothing was sent.
  MALFORMED,       ///< The answer to an open was not a well-formed open answer.
  TOO_LONG,        ///< The device announced more than the recipe accepts; the transfer was closed unread.
  INCOMPLETE,      ///< A block went unanswered, came back wrong or overflowed the cap; the data is what was read.
  NO_ANSWER,       ///< The device never answered the open.
  NO_DATA,         ///< The device accepted the open (it challenged the request) but sent no answer to it.
  ABANDONED,       ///< The runner gave the transfer up (another controller started one); closed if it was open.
};

/// @brief Short lower-case name of an outcome, for logs.
/// @param outcome Outcome to name.
[[nodiscard]] const char *transfer_outcome_name(TransferOutcome outcome);

/// @brief The state of one read of one recipe at one offset.
///
/// Usage: build, then repeat `next_request()`, perform the exchange, `on_answer()` until `done()`. A runner
/// spaces the requests TRANSFER_STEP_GAP_MS apart (proto_timing.h), none before the first. The recipe must
/// outlive the session.
/// @ingroup hioc_hub
class TransferSession {
 public:
  /// @param recipe The recipe to follow; its opens must not be empty.
  /// @param offset The offset to open every step at; the open builder refuses one the recipe does not allow.
  /// @param allowlist What create_transfer_open() accepts. Production code leaves the default, TRANSFER_RECIPES; a
  ///        test passes the table its recipe is in.
  TransferSession(const TransferRecipe &recipe, uint16_t offset,
                  std::span<const TransferRecipe> allowlist = TRANSFER_RECIPES)
      : recipe_(&recipe), offset_(offset), allowlist_(allowlist) {}

  /// @brief Build the next request of the read.
  ///
  /// Call again only after on_answer() for the previous request.
  /// @param own Controller's 3-byte node ID.
  /// @param dst Target device's 3-byte node ID.
  /// @param low_power True if the target is a low-power device (sets CTRL1_LOW_POWER).
  /// @param[out] request The request to send.
  /// @return true when @p request holds a request to send; false when the read is over (done()), also when
  ///         the open itself cannot be built, which ends it as REFUSED without sending anything.
  bool next_request(const uint8_t *own, const uint8_t *dst, bool low_power, IoFrame &request);

  /// @brief Take the device's answer to the request next_request() just built.
  /// @param reply The answer, or nullptr when the device did not answer. The answer to the close is ignored.
  /// @param request_accepted Only meaningful without a reply: the device challenged the request, so it heard it,
  ///        and still sent no data. An open that ends so is NO_DATA instead of NO_ANSWER.
  void on_answer(const IoFrame *reply, bool request_accepted = false);

  /// @brief Give the read up, for instance because another controller started a transfer to the same device.
  ///
  /// A transfer the device opened is closed first: the next request is the close, and the read ends
  /// ABANDONED after it. A read that opened nothing ends at once. No effect once the close is next (the read
  /// keeps the ending it already has, READ included) or once done().
  void abandon();

  /// True once the read ended and no request is left; outcome() and data() are final.
  [[nodiscard]] bool done() const { return phase_ == Phase::DONE; }
  /// True from the first request until done(): the device may be holding a handle or a half-open exchange.
  [[nodiscard]] bool started() const { return started_ && phase_ != Phase::DONE; }
  /// How the read ended; meaningful once done().
  [[nodiscard]] TransferOutcome outcome() const { return outcome_; }
  /// True once the device gave a well-formed answer to an open step, whether it accepted the open or not.
  [[nodiscard]] bool open_answered() const { return open_answered_; }
  /// The last well-formed open answer (handle, status, length); all zero until open_answered().
  [[nodiscard]] const TransferOpenReply &open() const { return open_; }
  /// True when the request that ended the read (an open or a block) was accepted by the device and got no answer;
  /// meaningful once done(). Lets a runner say "accepted but silent" for an INCOMPLETE read too.
  [[nodiscard]] bool ended_on_accepted_silence() const { return ended_on_accepted_silence_; }
  /// The bytes read so far: all of them for READ, the part read for INCOMPLETE, empty otherwise.
  [[nodiscard]] const std::vector<uint8_t> &data() const { return data_; }

 private:
  /// Where the read stands.
  enum class Phase : uint8_t {
    OPEN,    ///< Sending the open steps.
    BLOCKS,  ///< The device accepted an open; reading blocks counting down.
    CLOSE,   ///< The close is next; the outcome is already decided.
    DONE,    ///< Nothing is left to send.
  };

  void on_open_answer_(const IoFrame *reply, bool request_accepted);
  void on_block_answer_(const IoFrame *reply, bool request_accepted);
  /// Decide the ending of an open that gave no data, or start over from the first step (retry_empty_open).
  void end_open_without_data_(const IoFrame *reply, bool well_formed);
  /// The transfer is open at the device: the next request is the close, and the read ends as @p outcome.
  void close_then_(TransferOutcome outcome);
  void finish_(TransferOutcome outcome);

  const TransferRecipe *recipe_;
  uint16_t offset_;
  std::span<const TransferRecipe> allowlist_;
  Phase phase_{Phase::OPEN};
  bool started_{false};
  size_t step_{0};       ///< Index of the open step to send next.
  bool retried_{false};  ///< The empty-open retry was used.
  bool open_answered_{false};
  bool ended_on_accepted_silence_{false};                ///< The deciding request was accepted and unanswered.
  TransferOpenReply open_{};                             ///< The last well-formed open answer.
  uint16_t chunk_{0};                                    ///< Block to request next, counting down to 1.
  TransferOutcome outcome_{TransferOutcome::NO_ANSWER};  ///< Final once done(); the pending one in CLOSE.
  std::vector<uint8_t> data_;
};

}  // namespace home_io_control
}  // namespace esphome
