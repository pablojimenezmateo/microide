#pragma once

#include <atomic>
#include <cstdint>

namespace microide::editor {

// One buffer-scoped slot for work that runs OFF the shell thread and then wants
// to apply its result back into the buffer it was posted against.
//
// Three things can happen between posting and completing, and all three are
// ordinary: the tab is closed, the tab is retargeted at another file, or the
// user types. A completion that ignores any of them applies its result to the
// wrong buffer, or silently undoes an edit the user made while it ran. So the
// slot holds two things:
//
//   * the operation's id — process-unique within its slot kind and never
//     reused, stored ON the tab, which is what lets a completion FIND its tab,
//     and find none if the tab is gone;
//   * the buffer's `content_revision` at the moment the work was posted, which
//     is what separates "apply it" from "the user typed; drop it".
//
// Asynchronous open, format-on-save and the disk-content hash all need exactly
// this, which is why it is one type rather than three hand-rolled pairs of
// fields that each remember a different half of the rule.
class AsyncBufferWork {
 public:
  enum class Claim {
    // This slot is not waiting for that id: another tab posted it, or this
    // slot's operation was superseded or cancelled. Keep looking; apply nothing
    // and change nothing here.
    NotMine,
    // The slot's operation, but the buffer changed under it. The slot is now
    // clear and the caller must NOT apply the result.
    Stale,
    // The slot's operation, and the buffer is exactly the one it was posted
    // against. The slot is now clear; apply the result.
    Current,
  };

  // Ids for slots whose work carries no id of its own. Callers that post through
  // a service which already mints a run id (`SaveFormatterService::Begin`) arm
  // with that id instead, so one completion carries one identity end to end.
  //
  // Process-wide and monotonic. Ids only need to be unique among the operations
  // a single slot can be confused between, so one counter for every kind is
  // stricter than required and cheaper than a counter per kind.
  static std::uint64_t NextId() {
    // Constant-initialized, so this costs one relaxed increment and no
    // thread-safe-static guard. Posting is not shell-thread-only in principle
    // (a worker may chain work), which is why it is atomic at all.
    static std::atomic<std::uint64_t> next_id{0};
    return next_id.fetch_add(1, std::memory_order_relaxed) + 1;
  }

  // Arm the slot for a run posted against `content_revision`. Arming over an
  // in-flight operation abandons the earlier one: its completion then claims
  // `NotMine` and is dropped, which is what a second Ctrl+S or a retarget wants.
  // `id` must be non-zero; zero is the disarmed state.
  void Arm(std::uint64_t id, std::uint64_t content_revision) {
    id_ = id;
    posted_revision_ = content_revision;
  }

  // Convenience for the no-service case: mints an id, arms with it, returns it.
  std::uint64_t Post(std::uint64_t content_revision) {
    const std::uint64_t id = NextId();
    Arm(id, content_revision);
    return id;
  }

  // Abandon whatever is in flight without waiting for it. The completion still
  // arrives and still claims `NotMine`.
  void Disarm() {
    id_ = 0;
    posted_revision_ = 0;
  }

  [[nodiscard]] bool armed() const { return id_ != 0; }
  [[nodiscard]] std::uint64_t id() const { return id_; }
  [[nodiscard]] std::uint64_t posted_revision() const { return posted_revision_; }

  // Does this slot hold `id`? A lookup that must not consume the slot — finding
  // the tab a completion belongs to before deciding what to do with it.
  [[nodiscard]] bool Holds(std::uint64_t id) const { return id != 0 && id_ == id; }

  // Resolve a completion against this slot. Consumes the slot on a match, so a
  // duplicate delivery of the same id reads `NotMine` rather than applying
  // twice.
  [[nodiscard]] Claim Resolve(std::uint64_t id, std::uint64_t current_content_revision) {
    if (!Holds(id)) {
      return Claim::NotMine;
    }
    const bool current = posted_revision_ == current_content_revision;
    Disarm();
    return current ? Claim::Current : Claim::Stale;
  }

 private:
  std::uint64_t id_ = 0;
  std::uint64_t posted_revision_ = 0;
};

}  // namespace microide::editor
