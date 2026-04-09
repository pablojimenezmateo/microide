• Findings

  1. High: in-flight blame invalidation is racy and can resurrect stale results. src/project/GitBlameService.cpp:604, src/project/GitBlameService.cpp:615, src/project/
     GitBlameService.cpp:743. InvalidatePath() and Clear() erase cache state, but they do not cancel the currently running worker or mark its result obsolete. A request already in
     ProcessRequest() will recreate file_caches[file_key] afterward. That means save/rename/project-switch can clear blame, then an older blame result repopulates it and suppresses a refresh
     until the validation window expires.
  2. Medium: Discard all does not fully reconcile open editor tabs with what git actually removed or restored. src/workspace/WorkspaceShellSidebar.cpp:453, src/workspace/
     WorkspaceShellSidebar.cpp:469, src/workspace/WorkspaceShellPrompts.cpp:750. The bulk path only remembers files that existed before the discard and only reloads those. Deleted tracked
     files restored from HEAD were absent from that list, and untracked files removed by git clean are never closed. The result can be stale open tabs after a successful “Discarded all git
     changes”, even though there is already close-by-path machinery available.
  3. Medium: the new blame cache is being invalidated much more aggressively than the design intends. src/workspace/WorkspaceShellSidebar.cpp:144, src/workspace/
     WorkspaceShellSidebar.cpp:427, src/workspace/WorkspaceShellSidebar.cpp:474. RefreshGitSidebar() unconditionally calls ClearEditorBlame(), so stage/unstage/discard flows flush the entire
     blame service, not just the affected paths. The feature still works, but this undercuts the whole viewport-scoped cache investment and is likely to show up as avoidable re-blame churn.
  4. Medium: terminal “last command + output” still depends on a brittle wrap heuristic. src/workspace/WorkspaceShellTerminal.cpp:62, src/workspace/WorkspaceShellTerminal.cpp:73. The capture
     logic walks backward while the previous row is full-width and treats that as a wrapped invocation. That is a shortcut, not a real continuation signal, so a previous output line that
     happened to fill the terminal width can be pulled into the copied command transcript.
  5. Low: git shell helper logic is now duplicated across services. src/project/GitStatusService.cpp:18, src/project/GitCompareService.cpp:17, src/project/GitBlameService.cpp:88.
     EscapeShellArg, command execution, and git-root detection now exist in three variants. It is not breaking today, but it is cleanup debt and will make future git behavior fixes easy to
     miss in one path.

  I did not find obvious dead code in today’s work. The main debt is stale-state handling, over-broad invalidation, and a couple of heuristics that are good enough for now but not robust.

  I also ran cmake --build build, ctest --test-dir build --output-on-failure, and git diff --check; they all passed. If you want the cleanup pass, I’d fix 1 and 2 first, then narrow 3 into
  path-level blame invalidation.
