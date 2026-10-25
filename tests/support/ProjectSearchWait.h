#pragma once

#include <chrono>

#include "TestSupport.h"
#include "workspace/shell/WorkspaceShell.h"
#include "workspace/shell/WorkspaceShellTestAccess.h"

namespace microide::tests {

// Waiting on the asynchronous project search. Three shell test files carried their
// own copy of this loop. Note that `!running` is also true in the window before
// the worker picks a search up: a caller that needs THIS run's results waits on the
// results revision instead (WorkspaceShellProjectTests' revision-aware wait).
inline bool WaitForProjectSearchCompletion(workspace::WorkspaceShell& shell,
                                           std::chrono::milliseconds timeout) {
  using Access = workspace::WorkspaceShell::TestAccess;
  return WaitUntil([&shell] { return !Access::ProjectSearchRunning(shell); }, timeout,
                   std::chrono::milliseconds(5),
                   [&shell] { Access::ConsumeProjectSearchUpdates(shell); });
}

inline void WaitForProjectSearch(workspace::WorkspaceShell& shell) {
  Expect(WaitForProjectSearchCompletion(shell, std::chrono::seconds(2)),
         "workspace project search should finish");
}

}  // namespace microide::tests
