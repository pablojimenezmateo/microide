# 2026-06-11 deep correctness / tech-debt audit (§18–§24)

- Date: 2026-06-11
- Area: editor, platform, project, plugin, workspace, compare, persistence
- Source: §18–§24; a fan-out correctness/tech-debt audit across subsystems

## Summary

A multi-subsystem fan-out audit found and fixed a batch of real correctness defects (persisted-state
OOM, focus-stranding, git parsing, SIGPIPE, compare whitespace, Lua stack leaks), then closed every
deferred item it surfaced across follow-up passes (Lua longjmp safety, symlink-loop guard,
subprocess/fd hardening, overlay focus round 2, a correctness batch, and hit-test geometry dedup).

## Resolution

### §18 Fixed in the initial pass (with regression coverage)

- **Persisted-state OOM on corrupt input**: `DecodeSplitNode` and `PrimitiveReader::ReadVector`
  reserved an attacker-controlled `count` (up to 2^32-1) before reading any element; a corrupt
  session file could force a multi-GB allocation. Both now clamp the reservation to remaining input.
  `src/workspace/WorkspacePersistenceBinaryInternal.h`, `src/persistence/PersistedRecord.h`; test
  `PersistedStateRecord/RejectsAdversarialLengthWithoutOom`.
- **Keyboard-focus stranding ("dead input")**: confirming an external-URL prompt, Escape from
  BufferReplace/ProjectSearch overlays, and renaming a file under an open commit picker all left
  `surface.focus == Overlay` on a hidden surface. Centralized via `PrimarySurfaceFocus`/`HideOverlay`
  in `WorkspaceProjectState.h`; Escape paths route through canonical `DismissOverlay`. New hard
  invariant `CheckOverlayDismissalIsCentralized` forbids bare `overlay.visible = false` outside
  `WorkspaceShellOverlay.cpp` / `WorkspacePersistenceCoordinatorSession.cpp`.
- **Git branch-diff parser corrupted spaced paths / renames**: `CollectGitBranchOutgoingFiles`
  whitespace-split `--name-status` (no `-z`), truncating paths with spaces and mis-handling renames.
  Rewritten to `-z` NUL parsing via testable `ParseGitBranchDiffNameStatusZ`.
  `src/project/GitCompareService.cpp`; test `Git/BranchDiffNameStatusZParser`.
- **SIGPIPE crash**: no handler existed, so a `write()` to a subprocess/PTY/LSP pipe whose reader
  died could terminate the editor. `platform::IgnoreBrokenPipeSignal()` installed at startup.
  `src/platform/HostPlatform.cpp`; test `Subprocess/IgnoreBrokenPipeSignalPreventsCrash`.
- **Compare `ignore_whitespace` showed wrong right-column text**: the all-equal fast path in
  `BuildCompareModel` copied the left line into `right_text`. `src/compare/CompareModel.cpp`; test
  `Compare/IgnoreWhitespacePreservesRightText`.
- **Plugin Lua stack leak (hot paths)**: `QueryCompletions`/`QueryCodeActions` left pushed function +
  args on the Lua stack when `find_plugin_by_state` returned null, accumulating toward overflow. They
  now `lua_settop` back to the captured base. `src/plugin/PluginProviderQueryInterop.cpp`.

The §18 deferred items (cold-path Lua leak, snippet desync, blame cache, hit-test dedup, settings
focus trap, divergent git status tables) were all closed in §19–§24 below.

### §19 Plugin Lua-error longjmp safety pass

Raising a Lua error is a C `longjmp` (the project links the C build of Lua, `liblua5.4`); it unwinds
the native stack back to the enclosing protected call **without running any C++ destructor** — UB and
a leak whenever a `std::string`/`std::vector`/`std::filesystem::path` is alive on an intervening
frame. ASAN proved the leak was in both the interop locals and the thin `PluginHostLuaApi.inc`
wrapper null-host fallbacks (`host ? host->member : T{}` materialized a temporary the `longjmp`
skipped).

Fixed comprehensively: new `src/plugin/LuaError.h` (`lua_error_util::PushMessage` copies the message
into Lua memory; `kPendingError` sentinel). Delegating TU functions are longjmp-free (validate with
`lua_type`/`lua_isstring`, `PushMessage` + return `kPendingError`). `.inc` wrappers raise (`lua_error`)
only after their own locals destruct and bind null-host fallbacks by reference via
`EmptyProjectRoot()`/`EmptyCallbacks()`. New hard invariant
`CheckPluginLuaErrorDoesNotLongjmpOverCppLocals` bans `luaL_error` in `src/plugin` (entry-only
`luaL_check*` stays allowed). Validated under ASAN with `MICROIDE_HAS_LUA_PLUGINS=1` — always validate
plugin work with Lua enabled (the plugin code is compiled out when Lua dev headers are absent).

### §20 Recursive-scanner symlink-loop guard

`ProjectFileScanner::CollectFiles` and `DirectoryTree::AppendDirectory` recursed into child dirs; a
directory symlink whose real target is an ancestor turned the tree into a cycle (stack-overflow/hang
on open). Since a real directory can never be its own ancestor, every cycle crosses a directory
*symlink*. New header-only `src/project/SymlinkLoopGuard.h` records canonical targets of symlinks
followed on the current descent branch and refuses to descend when a symlink resolves to a target
already on the branch; non-symlink dirs pay no `canonical()` cost; RAII `Scope` lets sibling branches
follow the same real dir once each. Tests `ProjectFileScanner/TerminatesOnSymlinkLoop`,
`DirectoryTree/StopsExpandingSymlinkCycle` (regular + ASAN).

### §21 Subprocess deadlock + terminal fd-lifecycle hardening

- **Dead code removed**: `src/platform/ProcessBackend.{cpp,h}` (`RunSubprocessWithBackend`,
  `AsyncProcessBackend`, `PosixAsyncProcessBackend`, `CreateAsyncProcessBackend`) had zero consumers
  and duplicated ~190 lines from live `Subprocess.cpp`; deleted (resolves the `PosixAsyncProcessBackend`
  fd-race and the dead twin of the stdin deadlock). Live path: `Subprocess.cpp::RunSubprocess`.
- **Live stdin deadlock fixed**: `RunSubprocess` wrote all stdin synchronously before draining
  stdout/stderr; a child filling its stdout pipe before consuming stdin deadlocked. Replaced with a
  single non-blocking `poll()` pump (`PumpChildIo`) feeding stdin (`POLLOUT`) while draining
  stdout/stderr (`POLLIN`), all ends `O_NONBLOCK`, `DrainReadyPipe` treats `EAGAIN` as drained. Test
  `Subprocess/LargeStdinDoesNotDeadlock` (4 MiB through `cat`).
- **Terminal fd race fixed**: `PosixTerminalBackend::Stop()` closed `master_fd` while the reader was
  polling it (data race + fd reuse). Added a self-pipe: reader polls `master_fd` + wake fd; `Stop()`
  writes one byte, reaps child, joins reader, then closes `master_fd`. `master_fd_` became
  `std::atomic<int>`; the reader's 100 ms poll timeout was dropped (a small low-CPU win). Clean under
  regular, ASAN, and TSAN.

### §22 Overlay focus & dismissal correctness (round 2)

- **Commit picker stayed painted over the comparison it opened**: `ActivateOverlaySelection`'s
  `CommitPicker` case opened the compare then `return true` with no `DismissOverlay`. Fix:
  `DismissOverlay(true)` after the open. Test `WorkspaceShell/CommitPickerDismissesAfterOpeningCompare`.
- **Settings/Help overlay did not trap keyboard focus**: the overlay (a `SettingsOverlayService`, not
  part of `current_project_state.overlay`) set no `surface.focus` and consumed only Escape, so other
  keys edited the buffer underneath and editor chords could fire. Fix: a dedicated modal trap at the
  top of `KeyInputCoordinator::HandleKeyDown` — while `settings_overlay_visible()`, Escape closes and
  every other key returns handled. Test `WorkspaceShell/SettingsOverlayTrapsKeyboardInput`.
- **Disproved (do not re-investigate)**: "Compare/Merge Escape closes the tab before dismissing an
  open overlay." Not a bug — when any `state_.overlay` is visible, focus stays `Overlay`, so
  `HandleOverlayKeyDown` runs before the compare/merge editor handlers; the tab-close Escape only runs
  with no overlay open, which is correct.

### §23 Correctness batch: snippet mirror, git status priority, blame cache, Lua cold paths

- **Snippet linked-placeholder column desync** (`src/editor/SnippetEngine.cpp`): `SnippetTryInsertText`
  bumped only the edited range's `end.column`; same-line siblings to the right kept stale columns. Now
  advances both `start.column` and `end.column` of every sibling range at/after the insertion. Test
  `EditorSnippet/MultiOccurrenceLinkedTabMultiKeystroke`.
- **Divergent git status-priority tables** (`src/project/GitStatusService.cpp`): `BuildGitStatusMap`
  ranked `Added == Untracked` while `GitPorcelainParser::GitStatusPriority` ranks `Added > Untracked`.
  `BuildGitStatusMap` now delegates aggregation to `GitPorcelainParser::RecordGitStatus`
  (single-sourced). Test `Git/BuildStatusMapFolderPriorityIsSingleSourced`.
- **Blame cache eligible-but-empty after self-eviction** (`src/project/GitBlameService.cpp`): the
  re-validation block used `file_caches[key]` (operator[]) which could resurrect a just-evicted key as
  an empty-but-eligible entry. Now `find()`s and re-validates only if it survived. No deterministic
  regression test (needs a >16,000-line blame fixture or a test-only budget setter); deferred as a
  small follow-up.
- **Plugin Lua stack leak (cold paths)** (`PluginProviderQueryInterop.cpp`,
  `PluginSidebarHoverInterop.cpp`): the remaining provider/test/scm/auth/mcp/command/save-participant/
  sidebar/hover functions shared the §18 hot-path bug. Introduced header-only RAII
  `lua_interop::StackResetGuard` (`PluginLuaInterop.h`) restoring stack height on every scope exit;
  declared right after acquiring provider state. File dropped to 793 lines, back under the 800-line
  cap. Validated under ASAN.

### §24 Hit-test geometry dedup (+ a real compare hit-test bug)

Already centralized (audit overstated — untouched): per-mode sidebar header buttons; tab-strip
overflow/tab/close walk.

Fixed (three real clusters):
- **Compare collapsed-context action buttons — render/hit-test divergence (real bug)**: the renderer
  drew buttons inside a `block_rect` excluding the scrollbar reserve and inset 4px/side, right-aligned;
  the three hit-test paths used the full-width `row_rect`. The clickable region sat
  `scrollbar_reserve + 4px` right of the painted buttons (up to 16px), leaving "Show next" largely
  unclickable. New shared `CompareCollapsedContextBlockRect(...)` in `CompareMergeRender.{h,cpp}`;
  render + all three hit-test paths feed it to `BuildCollapsedContextActionRects`. Test
  `WorkspaceShared/CompareCollapsedContextBlockRect`.
- **Empty tab-strip "Welcome" placeholder rect**: the same `MakeRect(...)` was copy-pasted into
  render/click/cursor paths. Extracted `EmptyTabStripPlaceholderRect` into `WorkspaceLayout.{h,cpp}`.
  Test `WorkspaceShared/EmptyTabStripPlaceholderRect`.
- **Bottom-panel output line-at-point**: floor-vs-truncate divergence between
  `WorkspacePanelMouseCoordinator` and `WorkspaceShellCursor`. Promoted to shared
  `BottomPanelLineIndexAtY(...)` in `WorkspaceLayout.{h,cpp}`. Test `WorkspaceShared/BottomPanelLineIndexAtY`.
  (The terminal-grid positioners resolve to terminal cells, not output lines, so they stayed separate.)

Validated: full `ctest` green (incl. ArchitectureInvariants) under regular and ASAN presets.
