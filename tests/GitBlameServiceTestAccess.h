#pragma once

#include "project/GitBlameService.h"

#ifdef MICROIDE_TESTING

#include <functional>
#include <utility>

namespace microide::tests {

struct GitBlameServiceTestAccess {
  static void SetBeforeCacheApplyHook(microide::project::GitBlameService& service,
                                      std::function<void()> hook) {
    service.SetBeforeCacheApplyHook(std::move(hook));
  }
  static bool HasReadyResults(const microide::project::GitBlameService& service) {
    return service.HasReadyResults();
  }
  static bool WorkerIdle(const microide::project::GitBlameService& service) {
    return service.WorkerIdle();
  }
  static std::size_t TrackedFileCount(const microide::project::GitBlameService& service) {
    return service.TrackedFileCount();
  }
};

}  // namespace microide::tests

#endif
