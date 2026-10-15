## Context

The remote project shipped across `openspec/changes/archive/2026-10-13-remote-projects-phase-2b` (proposal and tasks only) and the Phase 3 commits of 2026-10-12..15; its behaviour is described in `dev-docs/design/remote-projects.md` and pinned by `tests/RemoteHostServiceTests.cpp`, the kernel's `MirrorSyncEngine`/`RemoteProject` suites and `tests/parity/`. The 2026-10-16 verification drove a real host headlessly through the control channel (open and sync, save, host edits under clean and dirty tabs, offline save, a killed ssh master, a host terminal, clangd on the host, eight simulated agents churning the tree while saving, a relaunch on the mirror) and found the contract holds; what it could not do was answer the banner or read readiness.

## Goals / Non-Goals

**Goals:**
- One spec that states the user-visible contract of a remote project so the next change edits requirements, not the design essay.
- A way to remove a mirror from the editor.
- Remote state on the control channel.
- The verification as a rerunnable script.

**Non-Goals:**
- Any protocol or server change. `Remote: Resync (full manifest)` stays unbuilt (measured unnecessary, design § 13). `Remote: Reinstall Server` is covered by the lifecycle's install-beside-and-restart. Phase 4's on-demand tier.
- A `ui` query or redraw verifier — those are the `gui-qa-harness` change.

## Decisions

- **The spec records what was verified, not the design's full intent.** Requirements whose behaviour the host run or a suite exercised are stated with that scenario; design ideas that never shipped (the resync threshold, the read-only git pool) are not requirements. Alternative: transcribe § 6–7 of the design wholesale — rejected, half of it is reasoning and some of it was decided against.
- **Remove Mirror lives on `RemoteHostService`** next to the close/disconnect path, and deletes through `MirrorStore` (which owns the directory layout) rather than a filesystem call in the service, so a future layout change has one owner. The dirty prompt and the unsent-push check run before any deletion; the recents entry is dropped through `RecentsService`, which already reads the mirror record. Alternative: a plain `rm -rf` of the root from the welcome page — rejected, the mirror has state outside its tree.
- **Control state is read from the service, not a new cache.** `RemoteHostService::ProjectStatusOf` already composes the welcome page's status block; the `status` query serializes the same struct plus the engine's pending counts. Alternative: new fields on `ProjectWorkspaceState` — rejected, they would duplicate `remote_status`.
- **The remote scenario script reuses `tools/qa/microide_qa.py` unchanged** (`App.launch(keep_home=True)`, `MICROIDE_SERVER_BINARY`), adds `MQA_REMOTE_HOST` and `MQA_REMOTE_ROOT`, resets the host repository to a pinned commit before and after, and records PASS/FAIL per scenario with the measured latencies. It is on-demand, never in a lane: it needs a host.

## What pins each requirement (task 1.1)

| requirement | unit / parity | host suite (`tools/qa/remote.py`) |
| --- | --- | --- |
| The mirror is the host's content set | `MirrorSyncEngine/InitialSyncAndHostChanges`, `FollowsTheHostWatch`, `CarriesNonUtf8Names`, `PullsLargeFilesLast`, `PullsAFileOverSixtyFourMegabytes`, `RemoteProject/OpensAMirrorOverSsh`, `RemoteHostService/OpenFolderOnHostEditsTheHostTree` | A, C |
| A save lands locally and is pushed under compare-and-swap | `MirrorSyncEngine/PushesLocalWritesUnderCompareAndSwap`, `JournalSurvivesARestart`, `WriteGateReachesTheHost`, `TreeOpsMadeOfflineReplay`, `Parity/SaveReachesTheTree`, `Parity/FileOperationsReachTheTree` | B, E, F |
| A host change under an open tab follows the external-change contract | `WorkspaceShell/BannerActionCommandPressesTheActiveTabsBanner`, `WorkspaceShell/DirtyPromptSaveRefusesToClobberAnExternalChange`, `RemoteHostService/OpenFolderOnHostEditsTheHostTree` (the aggregated row) | C, D, D2 |
| A pushed version the host changed meanwhile is a parked conflict | `MirrorSyncEngine/ParksConflictsAndNeverOverwritesLocalEdits`, `ResolvesConflictsTheUsersWay`, `MergesAConflictThreeWays`, `RemoteHostService/ConflictMergesThreeWaysToTheHost`, `OpenFolderOnHostEditsTheHostTree` (Merge / Keep Mine / Take Host's) | — (needs a disconnect-edit-reconnect race; covered by the units) |
| A mass delete is held unless the host moved HEAD | `MirrorSyncEngine/HoldsAMassDelete`, `HoldsAMassDeleteTheRepositoryDidNotExplain`, `FollowsABranchSwitchThatDeletesMostFiles`, `RemoteHostService/HeldMassDeleteIsAppliedFromItsRow` | I (branch switch) |
| Changed on Host lists what the host changed | `MirrorSyncEngine/RecordsChangedOnHost`, `RemoteHostService/UnfetchedFilesAreListedAndOpenWhenFetched` | C (the row) |
| Reopening the mirror reconnects it | `RemoteHostService/OpenFolderOnHostEditsTheHostTree` (close, reopen, a copy opens as a local folder) | L |
| Host processes run on the host | `Parity/TerminalStartsInTheHostTree`, `LanguageServerSeesTheHostTree`, `PluginToolsFollowTheProject`, `FormatOnSaveReachesTheTree`, `GitSidebarShowsTheWorkingTree`, `RemoteHostSession/HostProcessSurvivesAReconnectAndEndsOnDisconnect`, `RemoteProcess/KeptProcessIsReleasedAfterTheKeepGrace` | G, H |
| A mirror can be removed from the editor | `RemoteHostService/RemoveMirrorDeletesAClosedMirrorAndKeepsUnsentSaves` | — |
| The control channel reports remote state | `RemoteHostService/OpenFolderOnHostEditsTheHostTree` (`status.remote`, `projects`) | A, E (the suite reads it) |
| Entry points beside their local twins | `WorkspaceMenuRegistry/ExpandedMenusExposeExpectedEntries`, `RenderViewModelBuilder/WelcomeView…`, `ComputeWelcomeLayoutProducesHitRegions` | — |
| Banner actions from the control channel | `WorkspaceShell/BannerActionCommandPressesTheActiveTabsBanner` | D, D2 |
| A kept process is released after the keep grace | `RemoteProcess/KeptProcessIsReleasedAfterTheKeepGrace` | — (observed: no clangd left on the host after a session) |

## Risks / Trade-offs

- [Remove Mirror on a project with an unsent journal loses edits] → the command asks, naming the count, and the dirty prompt runs first.
- [A driver treats `ready` as "every byte pulled"] → `ready` means the manifest is applied and no pull is pending, which is the same point the notification row is withdrawn; the field names say what they count.
- [The host suite depends on a reachable `worky`] → it is opt-in by `MQA_REMOTE_HOST` and skips with a message, never fails, when unset.
