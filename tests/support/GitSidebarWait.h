#pragma once

#include <chrono>
#include <cstddef>

#include "TestSupport.h"
#include "workspace/shell/WorkspaceShell.h"
#include "workspace/shell/WorkspaceShellTestAccess.h"

namespace microide::tests {

// Waiting on the git sidebar's asynchronous refresh. Nine hand-rolled
// deadline/sleep loops and two differently-written copies of the entry-count wait
// had accreted across the shell test files (the WaitUntil consolidation,
// known-tech-debt item 088, never reached them). The timeout only bounds a hang --
// every caller asserts state, never elapsed time -- so it is generous enough for a
// loaded machine.
inline constexpr std::chrono::milliseconds kGitSidebarWaitTimeout{5000};

// Drain refresh completions until the sidebar leaves the refreshing state.
inline bool SettleGitSidebarRefresh(workspace::WorkspaceShell& shell,
                                    std::chrono::milliseconds timeout = kGitSidebarWaitTimeout) {
  using Access = workspace::WorkspaceShell::TestAccess;
  return WaitUntil([&shell] { return !Access::GitSidebarRefreshing(shell); }, timeout,
                   std::chrono::milliseconds(10),
                   [&shell] { Access::ConsumeGitSidebarRefresh(shell); });
}

// ... and until it lists exactly `expected_count` entries.
inline bool WaitForGitSidebarEntryCount(workspace::WorkspaceShell& shell,
                                        std::size_t expected_count,
                                        std::chrono::milliseconds timeout = kGitSidebarWaitTimeout) {
  using Access = workspace::WorkspaceShell::TestAccess;
  return WaitUntil(
      [&shell, expected_count] {
        return Access::GitSidebarEntries(shell).size() == expected_count &&
               !Access::GitSidebarRefreshing(shell);
      },
      timeout, std::chrono::milliseconds(10),
      [&shell] { Access::ConsumeGitSidebarRefresh(shell); });
}

}  // namespace microide::tests
