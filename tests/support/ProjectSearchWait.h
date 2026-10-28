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

// Waiting on one named path is NOT a wait for a complete index: the background
// scan walks the directory in filesystem order, not creation order, so the path a
// test happens to name can be indexed first and the wait returns with the rest of
// the tree still missing. Tests whose assertion depends on the *whole* fixture
// being indexed must wait on the count instead.
//
// The count alone is also not enough: the initial scan can return a prefix and
// report itself incomplete, and a search dispatched against that prefix silently
// works from a short candidate set. Require both.
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

// A project search is dispatched asynchronously, so `!running` is also true in the
// window BEFORE the worker picks it up — a plain wait-for-not-running can return on
// its first poll and leave the caller asserting against the previous (empty) result
// set. Pass the results revision sampled before dispatch and this waits for a run
// that actually landed. (WorkspaceShell/ReplaceAllFallsBackWhenResultsTruncated
// flaked exactly this way under TSAN's slower scheduling: the search had not
// started, so nothing was truncated yet.)
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

}  // namespace microide::tests
