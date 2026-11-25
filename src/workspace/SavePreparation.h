#pragma once

#include <cstdint>

namespace microide::workspace {

struct SavePreparation {
  enum class Status {
    Ready,
    Deferred,
    Failed,
  };

  Status status = Status::Ready;
  std::uint64_t deferred_run_id = 0;

  static SavePreparation Ready() { return SavePreparation{Status::Ready, 0}; }
  static SavePreparation Failed() { return SavePreparation{Status::Failed, 0}; }
  static SavePreparation Deferred(std::uint64_t run_id) {
    return SavePreparation{Status::Deferred, run_id};
  }

  bool ok() const { return status != Status::Failed; }
  bool deferred() const { return status == Status::Deferred; }
};

enum class SaveMode {
  Blocking,
  Deferred,
  SkipTransforms,
  Autosave,
};

}
