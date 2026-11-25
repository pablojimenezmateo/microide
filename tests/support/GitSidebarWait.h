#pragma once

#include <chrono>
#include <cstddef>

#include "TestSupport.h"
#include "workspace/shell/WorkspaceShell.h"
#include "workspace/shell/WorkspaceShellTestAccess.h"

namespace microide::tests {

inline constexpr std::chrono::milliseconds kGitSidebarWaitTimeout{5000};

inline bool SettleGitSidebarRefresh(workspace::WorkspaceShell& shell,
                                    std::chrono::milliseconds timeout = kGitSidebarWaitTimeout) {
  using Access = workspace::WorkspaceShell::TestAccess;
  return WaitUntil([&shell] { return !Access::GitSidebarRefreshing(shell); }, timeout,
                   std::chrono::milliseconds(10),
                   [&shell] { Access::ConsumeGitSidebarRefresh(shell); });
}

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

}
