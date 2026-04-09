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
};

}  // namespace microide::tests

#endif
