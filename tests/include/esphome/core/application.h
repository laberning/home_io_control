#pragma once

#include "component.h"
#include "hal.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

namespace esphome {

/// The pending-timer registry backing every `set_timeout`/`set_interval`/`cancel_*` entry point:
/// `Component`'s own methods (component.h, out-of-line in stubs.cpp) and the two `App.scheduler`
/// entry points production code calls directly (`Scheduler` below). Unlike the single
/// "most-recent-call" stub this replaced, every call keeps its own pending item until it fires,
/// is replaced, or is cancelled — so a test can have two devices' polls pending at once, assert on
/// deadline order, and drive time forward with `run_due()`/`run_until()` instead of hand-firing a
/// captured callback.
namespace test_scheduler {

/// Which of the three key shapes a pending item was scheduled with. A static name and a numeric id
/// are always scoped to the owning `Component` (two components may each have their own "foo" timer
/// without colliding); a self key has no owning `Component` at all — see `Key::for_self()`.
enum class KeyKind : uint8_t { kStaticName, kNumericId, kSelfKey };

/// One scheduled item's identity: enough to find, replace, or cancel it, mirroring real ESPHome
/// closely enough to reproduce its bugs. The `const char *` overload stores the caller's pointer
/// rather than a copy (see `Component::set_timeout`'s doc comment) and a later call with the same
/// (component, name) still counts as "the same timer" even from a *different* string-literal
/// instance with equal contents, so matching falls back to `strcmp` when the pointers differ.
/// The three key kinds never match each other, regardless of what `name`/`id`/`self` happen to be.
struct Key {
  KeyKind kind;
  const Component *component{nullptr};  // owner for kStaticName/kNumericId; unused (nullptr) for kSelfKey
  const char *name{nullptr};            // kStaticName only
  uint32_t id{0};                       // kNumericId only
  const void *self{nullptr};            // kSelfKey only

  static Key for_name(const Component *c, const char *n) { return Key{KeyKind::kStaticName, c, n, 0, nullptr}; }
  static Key for_id(const Component *c, uint32_t i) { return Key{KeyKind::kNumericId, c, nullptr, i, nullptr}; }
  static Key for_self(const void *s) { return Key{KeyKind::kSelfKey, nullptr, nullptr, 0, s}; }
};

inline bool keys_match(const Key &a, const Key &b) {
  if (a.kind != b.kind)
    return false;
  switch (a.kind) {
    case KeyKind::kStaticName:
      return a.component == b.component &&
             (a.name == b.name || (a.name != nullptr && b.name != nullptr && std::strcmp(a.name, b.name) == 0));
    case KeyKind::kNumericId:
      return a.component == b.component && a.id == b.id;
    case KeyKind::kSelfKey:
      return a.self == b.self;
  }
  return false;
}

enum class ItemKind : uint8_t { kTimeout, kInterval };

struct Item {
  ItemKind kind;
  Key key;
  uint32_t deadline_ms;
  uint32_t interval_ms;  ///< kInterval only; re-arm period.
  uint64_t seq;          ///< Insertion order, and this scheduling's stable identity (see fire_by_seq_()).
  std::function<void()> callback;
};

/// A heap-leaked function-local singleton (never destroyed), so a `Component` destroyed during
/// static teardown can never touch a destroyed registry — ASan treats an intentional, permanent
/// function-local `new` like this as reachable, not a leak.
///
/// Not modelled: deadlines are plain `uint32_t` compared with `<=`, so a clock moved with
/// `test_clock::set_ms()` to near `UINT32_MAX` while items are pending would see them all appear
/// due at once when `millis()` wraps, rather than surviving the wrap the way a real device's
/// scheduler does. No test does this (`test_clock_test.cpp`'s own wraparound coverage is for the
/// raw clock, not for anything scheduled through this registry) — if one ever needs to, this is
/// the place that would need widening to unwrapped 64-bit time.
class Registry {
 public:
  static Registry &instance() {
    static Registry *r = new Registry();
    return *r;
  }

  /// Replaces any existing item with the same key — cancels it first, so the new item goes to the
  /// back of insertion order, mirroring the real scheduler's "same (component, name) re-adds"
  /// behaviour — then adds the new one with deadline `peek_ms() + delay_ms`.
  void schedule(ItemKind kind, const Key &key, uint32_t delay_ms, std::function<void()> &&cb) {
    this->cancel(key);
    const uint32_t interval_ms = kind == ItemKind::kInterval ? delay_ms : 0;
    items_.push_back(Item{kind, key, test_clock::peek_ms() + delay_ms, interval_ms, next_seq_++, std::move(cb)});
  }

  bool cancel(const Key &key) {
    for (size_t i = 0; i < items_.size(); i++) {
      if (keys_match(items_[i].key, key)) {
        items_.erase(items_.begin() + static_cast<std::ptrdiff_t>(i));
        return true;
      }
    }
    return false;
  }

  /// Purges every item owned by, or self-keyed to, `component`. A self key can be any pointer,
  /// including a Component's own `this` (lr1121_firmware_update_controller.cpp's
  /// arm_flash_confirmation_() self-keys on the hub, which is itself a Component) — both checks are
  /// needed or a destroyed component can leave a dangling self-keyed callback behind.
  void purge_owner(const Component *component) {
    const void *self = static_cast<const void *>(component);
    for (size_t i = 0; i < items_.size();) {
      const Key &k = items_[i].key;
      const bool owned =
          (k.kind != KeyKind::kSelfKey && k.component == component) || (k.kind == KeyKind::kSelfKey && k.self == self);
      if (owned) {
        items_.erase(items_.begin() + static_cast<std::ptrdiff_t>(i));
      } else {
        i++;
      }
    }
  }

  /// Fires every item due at `peek_ms()`, in (deadline, insertion-order) order. A Component-keyed
  /// item whose component `is_failed()` is skipped — left pending, unfired — while a self-keyed
  /// item has no Component to check and is never skipped (the whole reason
  /// arm_flash_confirmation_() uses the self-keyed overload: the auto-disarm must still fire on a
  /// hub that failed its own re-init). An item a fired callback adds is never fired by this same
  /// call: the due set is snapshotted by sequence number before any callback runs, and a new item
  /// always gets a larger sequence number than everything in that snapshot.
  size_t run_due() {
    const uint32_t now = test_clock::peek_ms();
    std::vector<std::pair<uint32_t, uint64_t>> due;  // (deadline_ms, seq); lexicographic sort below
    for (const Item &item : items_) {
      if (item.deadline_ms <= now)
        due.emplace_back(item.deadline_ms, item.seq);
    }
    std::sort(due.begin(), due.end());

    size_t fired = 0;
    for (const auto &entry : due) {
      if (this->fire_by_seq_(entry.second, /*apply_failed_skip=*/true))
        fired++;
    }
    return fired;
  }

  /// Manual mode only: advances the clock in deadline order up to (and including) `target_ms`,
  /// running due items at each stop, so a callback sees the same time a real device's scheduler
  /// would have handed it rather than jumping straight to `target_ms` and firing everything at
  /// once. Always leaves the clock at exactly `target_ms`.
  size_t run_until(uint32_t target_ms) {
    size_t fired = 0;
    uint32_t floor_ms = test_clock::peek_ms();
    for (;;) {
      const std::optional<uint32_t> next = this->earliest_deadline_at_or_after_(floor_ms);
      if (!next.has_value() || *next > target_ms)
        break;
      test_clock::set_ms(*next);
      // Keep firing at this same instant until nothing more is due here, not just once: a
      // callback that schedules (or an interval that re-arms) a zero-delay follow-up must not
      // have to wait for the next *distinct* deadline to be seen — a real device's loop() would
      // pick it straight back up on its very next iteration, still at the same millis() value.
      // This terminates because run_due() itself always eventually returns 0: a permanently-failed
      // owner's item stops it here the same way it stops a single run_due() call.
      for (size_t fired_here = this->run_due(); fired_here > 0; fired_here = this->run_due())
        fired += fired_here;
      // Forward progress even if everything due at *next was skipped (failed component) —
      // otherwise a permanently-failed owner's item would be "the earliest deadline" forever.
      floor_ms = *next + 1;
    }
    test_clock::set_ms(target_ms);
    return fired;
  }

  /// Looks up a still-pending item by key, or nullptr. The one lookup pending_by_key() (and, for a
  /// natural re-arm's "did the callback already claim this key itself" check, fire_by_seq_()) is
  /// built on.
  const Item *find(const Key &key) const {
    for (const Item &item : items_) {
      if (keys_match(item.key, key))
        return &item;
    }
    return nullptr;
  }

  /// Fires a specific still-pending item on request (test_scheduler::PendingView::fire()),
  /// unconditionally — unlike run_due(), this never applies the failed-component skip, since a
  /// test asking for this exact item by key has already decided it should fire.
  bool fire_by_seq(uint64_t seq) { return this->fire_by_seq_(seq, /*apply_failed_skip=*/false); }

  size_t pending_count() const { return items_.size(); }
  void clear() { items_.clear(); }

 private:
  Item *find_by_seq_(uint64_t seq) {
    for (Item &item : items_) {
      if (item.seq == seq)
        return &item;
    }
    return nullptr;
  }

  std::optional<uint32_t> earliest_deadline_at_or_after_(uint32_t floor_ms) const {
    std::optional<uint32_t> best;
    for (const Item &item : items_) {
      if (item.deadline_ms < floor_ms)
        continue;
      if (!best.has_value() || item.deadline_ms < *best)
        best = item.deadline_ms;
    }
    return best;
  }

  bool fire_by_seq_(uint64_t seq, bool apply_failed_skip) {
    Item *item = this->find_by_seq_(seq);
    if (item == nullptr)
      return false;  // already cancelled, or fired earlier within this same run_due() call
    if (apply_failed_skip && item->key.kind != KeyKind::kSelfKey && item->key.component != nullptr &&
        item->key.component->is_failed())
      return false;  // left pending; see run_due()'s doc comment

    // Copied out before cb() runs, not read from *item afterwards: cb() can freely mutate items_
    // (schedule/cancel anything, including this exact key), which may reallocate the vector and
    // would leave `item` dangling.
    const ItemKind kind = item->kind;
    const Key key = item->key;
    const uint32_t interval_ms = item->interval_ms;
    std::function<void()> cb = item->callback;

    cb();  // may itself cancel or re-register `key` -- see the comment below for what that means

    // If the callback already disposed of this exact scheduling itself -- cancelling it, or
    // re-registering the same key (set_*() cancels-then-adds, so the original seq is gone either
    // way) -- respect that and stop here. Re-arming on top would silently undo a self-cancel, and
    // adding a second item on top of a self-re-register would leave a duplicate, uncancellable
    // zombie at the same key. Otherwise the callback left this item untouched: remove it (a
    // one-shot timeout is consumed exactly once; a stale interval occurrence must not be found
    // "due" again) and, for an interval, re-arm it the natural way, from execution time — so a
    // slow-to-fire callback doesn't get a shortened next interval, and at most once per call()
    // regardless, since a natural re-arm's fresh sequence number is always greater than every
    // entry in run_due()'s due snapshot.
    if (this->find_by_seq_(seq) == nullptr)
      return true;

    this->cancel(key);
    if (kind == ItemKind::kInterval) {
      items_.push_back(
          Item{ItemKind::kInterval, key, test_clock::peek_ms() + interval_ms, interval_ms, next_seq_++, cb});
    }
    return true;
  }

  std::vector<Item> items_;
  uint64_t next_seq_{0};
};

/// A handle onto one still-pending item, from pending()/pending_self() below. `deadline_ms` is a
/// snapshot taken at query time; `fire()` looks the item up again by its scheduling sequence number
/// (stable across other items being added or removed) and, if it's still there, fires it through
/// the same shared logic run_due() uses per-item — so a manually fired interval re-arms exactly the
/// way a naturally due one would. Returns false if the item is no longer pending (already fired or
/// cancelled since this view was obtained) — fire() never applies run_due()'s failed-component
/// skip, since asking for this exact item by key is already an explicit decision that it should fire.
class PendingView {
 public:
  uint32_t deadline_ms;

  bool fire() const { return Registry::instance().fire_by_seq(seq_); }

 private:
  friend std::optional<PendingView> pending_by_key(const Key &key);
  PendingView(uint32_t deadline, uint64_t seq) : deadline_ms(deadline), seq_(seq) {}
  uint64_t seq_;
};

inline std::optional<PendingView> pending_by_key(const Key &key) {
  const Item *item = Registry::instance().find(key);
  if (item == nullptr)
    return std::nullopt;
  return PendingView(item->deadline_ms, item->seq);
}

inline std::optional<PendingView> pending(const Component *component, const char *name) {
  return pending_by_key(Key::for_name(component, name));
}
inline std::optional<PendingView> pending(const Component *component, uint32_t id) {
  return pending_by_key(Key::for_id(component, id));
}
inline std::optional<PendingView> pending_self(const void *self) { return pending_by_key(Key::for_self(self)); }

inline size_t run_due() { return Registry::instance().run_due(); }
inline size_t run_until(uint32_t target_ms) { return Registry::instance().run_until(target_ms); }
inline size_t pending_count() { return Registry::instance().pending_count(); }
inline void clear() { Registry::instance().clear(); }
inline void purge_owner(const Component *component) { Registry::instance().purge_owner(component); }

}  // namespace test_scheduler

/// Minimal host stub of ESPHome's Scheduler, backing Component::set_timeout/set_interval/cancel_*
/// (via test_scheduler::Registry above) plus the two entry points production code reaches directly
/// through `App.scheduler`: the Component-keyed overload (platform_entity_base.h, for a mixin that
/// isn't itself a Component and so can't call the protected Component::set_timeout) and the
/// self-keyed overload (lr1121_firmware_update_controller.cpp, for a timer that must survive its
/// owning component being marked failed).
class Scheduler {
 public:
  void set_timeout(Component *component, const char *name, uint32_t timeout, std::function<void()> &&func) {
    const test_scheduler::Key key = test_scheduler::Key::for_name(component, name);
    this->record_last_(component, test_scheduler::ItemKind::kTimeout, key, timeout, func);
    test_scheduler::Registry::instance().schedule(test_scheduler::ItemKind::kTimeout, key, timeout, std::move(func));
  }

  void set_timeout(Component *component, uint32_t id, uint32_t timeout, std::function<void()> &&func) {
    const test_scheduler::Key key = test_scheduler::Key::for_id(component, id);
    this->record_last_(component, test_scheduler::ItemKind::kTimeout, key, timeout, func);
    test_scheduler::Registry::instance().schedule(test_scheduler::ItemKind::kTimeout, key, timeout, std::move(func));
  }

  void set_interval(Component *component, const char *name, uint32_t interval, std::function<void()> &&func) {
    const test_scheduler::Key key = test_scheduler::Key::for_name(component, name);
    this->record_last_(component, test_scheduler::ItemKind::kInterval, key, interval, func);
    test_scheduler::Registry::instance().schedule(test_scheduler::ItemKind::kInterval, key, interval, std::move(func));
  }

  bool cancel_timeout(Component *component, const char *name) {
    return this->cancel_(component, test_scheduler::ItemKind::kTimeout, test_scheduler::Key::for_name(component, name));
  }

  bool cancel_timeout(Component *component, uint32_t id) {
    return this->cancel_(component, test_scheduler::ItemKind::kTimeout, test_scheduler::Key::for_id(component, id));
  }

  bool cancel_interval(Component *component, const char *name) {
    return this->cancel_(component, test_scheduler::ItemKind::kInterval,
                         test_scheduler::Key::for_name(component, name));
  }

  // Self-keyed overload, mirroring the real Scheduler::set_timeout(const void *self, ...): no
  // Component is recorded, so (unlike the overload above) this is not subject to a failed
  // component being skipped. There is no Component to store the callback on, so it is recorded
  // here on the Scheduler stub instead, keyed by `self` for cancel_timeout()/inspection.
  void set_timeout(const void *self, uint32_t timeout, std::function<void()> &&func) {
    last_self_timeout_self_ = self;
    last_self_timeout_ms_ = timeout;
    last_self_timeout_callback_ = func;
    test_scheduler::Registry::instance().schedule(test_scheduler::ItemKind::kTimeout,
                                                  test_scheduler::Key::for_self(self), timeout, std::move(func));
  }
  // Cancels from the registry regardless of whether `self` is the *most recently* self-keyed
  // timeout — only the compat fields (which, by construction, can only ever mirror one call) are
  // conditioned on that; the registry itself may hold several self-keyed items at once, and an
  // older one must still be cancellable.
  bool cancel_timeout(const void *self) {
    if (self == nullptr)
      return false;
    const bool cancelled = test_scheduler::Registry::instance().cancel(test_scheduler::Key::for_self(self));
    if (cancelled && self == last_self_timeout_self_) {
      last_self_timeout_self_ = nullptr;
      last_self_timeout_callback_ = nullptr;
    }
    return cancelled;
  }

  // Test helpers for verifying self-keyed set_timeout() calls.
  const void *last_self_timeout_self_{nullptr};
  uint32_t last_self_timeout_ms_{0};
  std::function<void()> last_self_timeout_callback_;

 private:
  // The last_timeout_*/last_interval_* compat fields mirror the most recent call for their own
  // kind (component.h documents why they're kept: several tests read, clear, or copy them by
  // hand). Shared by all three set_*() overloads above instead of each repeating it.
  void record_last_(Component *component, test_scheduler::ItemKind kind, const test_scheduler::Key &key,
                    uint32_t timeout, const std::function<void()> &cb) {
    if (component == nullptr)
      return;
    if (kind == test_scheduler::ItemKind::kInterval) {
      component->last_interval_name_ = key.name != nullptr ? key.name : "";
      component->last_interval_ms_ = timeout;
      component->last_interval_callback_ = cb;
      return;
    }
    if (key.kind == test_scheduler::KeyKind::kStaticName)
      component->last_timeout_name_ = key.name != nullptr ? key.name : "";
    else
      component->last_timeout_id_ = key.id;
    component->last_timeout_ms_ = timeout;
    component->last_timeout_callback_ = cb;
  }

  // Shared by all three cancel_*() overloads above. The compat fields are only cleared when the
  // cancelled key is the one they currently reflect — an unrelated cancel (a different name/id
  // than the most recent call) must leave "mirrors the most recent set_*() call" true.
  bool cancel_(Component *component, test_scheduler::ItemKind kind, const test_scheduler::Key &key) {
    const bool cancelled = test_scheduler::Registry::instance().cancel(key);
    if (!cancelled || component == nullptr)
      return cancelled;
    if (kind == test_scheduler::ItemKind::kInterval) {
      if (component->last_interval_name_ == (key.name != nullptr ? key.name : "")) {
        component->last_interval_name_.clear();
        component->last_interval_callback_ = nullptr;
      }
      return cancelled;
    }
    if (key.kind == test_scheduler::KeyKind::kStaticName) {
      if (component->last_timeout_name_ == (key.name != nullptr ? key.name : "")) {
        component->last_timeout_name_.clear();
        component->last_timeout_callback_ = nullptr;
      }
    } else if (component->last_timeout_id_ == key.id) {
      component->last_timeout_id_ = 0;
      component->last_timeout_callback_ = nullptr;
    }
    return cancelled;
  }
};

class Application {
 public:
  Scheduler scheduler;
  // Counts calls rather than being a pure no-op so tests can assert that a long blocking wait
  // actually feeds the watchdog, not just that it eventually returns. Global and monotonic —
  // callers snapshot the count before their wait and assert on the delta, since unrelated tests
  // may also drive code paths that feed.
  uint32_t feed_wdt_calls{0};
  void feed_wdt() { this->feed_wdt_calls++; }

  // Counts calls instead of actually restarting the process, so tests can assert a code path
  // reaches the safe_reboot() invariant (see the LR1121 firmware-update ground rules: every
  // bootloader excursion must end in either radio_->init() or App.safe_reboot()) without tearing
  // down the test binary.
  uint32_t safe_reboot_calls{0};
  void safe_reboot() { this->safe_reboot_calls++; }
};

extern Application App;

}  // namespace esphome
