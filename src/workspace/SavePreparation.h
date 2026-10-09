#pragma once

#include <cstdint>

namespace microide::workspace {

// What happened when a save's transforms (save participants, then the contributed
// formatter) were prepared for a viewport.
//
// A save used to be one synchronous step, so this was a bool. It is three states now
// because the formatter runs off the shell thread: `Deferred` means the write has NOT
// happened and must not happen yet — the formatter's completion finishes the save.
struct SavePreparation {
  enum class Status {
    // Nothing left to transform, or the transforms already applied. Write now.
    Ready,
    // A formatter run was posted. The caller must not write; the completion re-enters
    // the save with the formatter suppressed exactly once.
    Deferred,
    // A save participant rejected the save. Do not write.
    Failed,
  };

  Status status = Status::Ready;
  // Non-zero only for Deferred: the SaveFormatterService run id whose completion
  // finishes this save. Stored on the tab so the completion can find its way back.
  std::uint64_t deferred_run_id = 0;

  static SavePreparation Ready() { return SavePreparation{Status::Ready, 0}; }
  static SavePreparation Failed() { return SavePreparation{Status::Failed, 0}; }
  static SavePreparation Deferred(std::uint64_t run_id) {
    return SavePreparation{Status::Deferred, run_id};
  }

  bool ok() const { return status != Status::Failed; }
  bool deferred() const { return status == Status::Deferred; }
};

// Whether a save may hand its formatter to the background worker and return before
// the file is written. Interactive saves defer (the point of the whole exercise);
// a save whose caller acts on completion — closing a tab, renaming, quitting —
// blocks, which costs the same wait the inline formatter always did.
enum class SaveMode {
  // Run the formatter and wait for it. For a save whose caller acts on completion:
  // closing a tab, renaming, deleting, quitting.
  Blocking,
  // Hand the formatter to the worker and return. For an interactive save, which is
  // the one the user is sitting in front of.
  Deferred,
  // Run no save transform at all — neither participants nor the formatter. Set
  // for exactly one save: the one a deferred completion re-enters, which already
  // has their answer. (It skipped only the formatter once, so a save with both
  // ran its participants twice: before the formatter, and again on its output.)
  SkipTransforms,
};

}  // namespace microide::workspace
