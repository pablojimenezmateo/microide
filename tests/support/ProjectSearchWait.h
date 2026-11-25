#pragma once

#include <chrono>

#include "TestSupport.h"
#include "workspace/shell/WorkspaceShell.h"
#include "workspace/shell/WorkspaceShellTestAccess.h"

namespace microide::tests {

inline bool WaitForProjectSearchCompletion(workspace::WorkspaceShell& shell,
                                           std::chrono::milliseconds timeout) {
  using Access = workspace::WorkspaceShell::TestAccess;
  return WaitUntil([&shell] { return !Access::ProjectSearchRunning(shell); }, timeout,
                   std::chrono::milliseconds(5),
                   [&shell] { Access::ConsumeProjectSearchUpdates(shell); });
}

inline bool WaitForFileIndexSize(workspace::WorkspaceShell& shell,
                          std::size_t minimum_entries,
                          std::chrono::milliseconds timeout) {
  return WaitUntil(
      [&shell, minimum_entries]() {
        return workspace::WorkspaceShell::TestAccess::FileIndexSize(shell) >= minimum_entries &&
               !workspace::WorkspaceShell::TestAccess::FileIndexScanIncomplete(shell);
      },
      timeout, std::chrono::milliseconds(10));
}

inline bool WaitForProjectSearchResults(workspace::WorkspaceShell& shell,
                                 std::uint64_t revision_before,
                                 std::chrono::milliseconds timeout) {
  return WaitUntil(
      [&shell, revision_before]() {
        return !workspace::WorkspaceShell::TestAccess::ProjectSearchRunning(shell) &&
               workspace::WorkspaceShell::TestAccess::ProjectSearchResultsRevision(shell) != revision_before;
      },
      timeout, std::chrono::milliseconds(5),
      [&shell]() { workspace::WorkspaceShell::TestAccess::ConsumeProjectSearchUpdates(shell); });
}

inline void WaitForProjectSearch(workspace::WorkspaceShell& shell) {
  Expect(WaitForProjectSearchCompletion(shell, std::chrono::seconds(2)),
         "workspace project search should finish");
}

}
