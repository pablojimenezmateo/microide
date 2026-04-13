# Tech Debt Audit 2026-04-13

This pass focused on duplicated code and adjacent structural debt that still has a high maintenance
cost after the recent workspace-shell cleanup work.

Scope:

- Read the current `src/workspace`, `src/project`, `src/compare`, and `src/terminal` code.
- Compared the current code against the existing tech-debt notes in `docs/`.
- Looked for duplication in parser loops, view-model builders, helper utilities, and async service
  lifecycle code.

## Findings

### 1. `WorkspaceShell` is still the God class

Priority: High

Evidence:

- `src/workspace/WorkspaceShell.h:172-779` still stores compare, merge, editor, terminal, menu,
  overlay, prompt, drag, project, and persistence-related state under one owner.
- `src/workspace/WorkspaceShell.h:820-1416` still declares one very broad API surface for actions,
  project catalog changes, tab management, compare or merge workflows, terminal control,
  persistence, input routing, mouse handling, and chrome rendering.
- The implementation remains concentrated in very large companion files:
  - `src/workspace/WorkspaceShell.cpp` at 2652 lines
  - `src/workspace/WorkspaceShellShared.cpp` at 2467 lines
  - `src/workspace/WorkspaceShellMouse.cpp` at 2008 lines
  - `src/workspace/WorkspaceShellRender.cpp` at 1883 lines
  - `src/workspace/WorkspaceShell.h` at 1482 lines

Impact:

- Almost every feature change in the workspace still has a wide blast radius.
- Invariants are implicit because state and behavior for unrelated subsystems share one boundary.
- Recent extractions improved internals, but the public ownership model is still effectively
  "everything lives in `WorkspaceShell`".

Recommended split target:

1. `ProjectCatalogController`
   Own open, switch, close, restore, and rollback for project tabs.
2. `EditorWorkspaceController`
   Own editor tabs, split panes, buffer search or replace, and active editor synchronization.
3. `CompareMergeController`
   Own compare tabs, merge tabs, conflict tracking, compare or merge scrolling, and persistence
   adapters for those tab types.
4. `TerminalPanelController`
   Own terminal tabs, panel scroll state, selection, clipboard bridging, and panel-local commands.
5. `WorkspaceChromeController`
   Own menu bar, prompt surfaces, overlay visibility, cursor routing, and drag state.
6. `WorkspacePersistenceController`
   Own user config, project config, session serialization, and file-path policy.

Pragmatic first extraction steps:

1. Move the tab-strip models and builders out of `WorkspaceShell`.
2. Move compare or merge tab state plus helpers behind one controller boundary.
3. Move project catalog transactions and persistence checkpoints behind one controller boundary.
4. Leave `WorkspaceShell` as the event-loop orchestrator and top-level layout coordinator only.

### 2. `GitStatusService` parses the same porcelain stream twice

Priority: Medium

Evidence:

- `src/project/GitStatusService.cpp:84-118` in `ParseGitPorcelainStatus(...)`
- `src/project/GitStatusService.cpp:121-171` in `ParseGitWorkingTreeEntries(...)`

Both functions walk the same NUL-delimited `git status --porcelain=v1 -z` output, repeat the same
entry framing logic, repeat the same rename or copy target-path handling, and then project the
result into two different shapes.

Impact:

- Any parser fix for rename handling, malformed entries, or future porcelain quirks must be applied
  in two places.
- The status and working-tree views can drift if one parser path changes and the other does not.

Recommendation:

- Parse porcelain output once into a small normalized record such as
  `ParsedGitStatusEntry { code, path, target_path }`.
- Derive the directory-status map and working-tree list from that normalized vector.

### 3. `SerializeLines(...)` is duplicated verbatim inside the workspace shell

Priority: Medium

Evidence:

- `src/workspace/WorkspaceShell.cpp:130-147`
- `src/workspace/WorkspaceShellCompare.cpp:31-48`

The helper that joins lines with `LF`, `CRLF`, or `CR` exists in two copies with the same logic.

Impact:

- Line-ending behavior for compare or merge save paths can drift from the main workspace path.
- The duplication is small, but it is the kind of low-signal duplication that accumulates in a
  God-class codebase and makes helpers harder to discover.

Recommendation:

- Move this helper to `WorkspaceShellShared.cpp` or an editor or text utility shared by compare,
  merge, and generic save flows.

### 4. Chrome tab-strip view models are triplicated

Priority: Medium

Evidence:

- `src/workspace/WorkspaceShell.h:346-382` defines three near-identical structs:
  `VisibleTab`, `VisibleProjectTab`, and `VisibleTerminalTab`.
- `src/workspace/WorkspaceShellChrome.cpp:50-98`,
  `src/workspace/WorkspaceShellChrome.cpp:131-178`, and
  `src/workspace/WorkspaceShellChrome.cpp:180-236` follow the same pipeline:
  build widths and labels, compute visible strip layouts, call `BuildChromeTabRenderItems(...)`,
  then copy the result into a concrete visible-tab type.

Impact:

- Tab-strip changes still require edits in three code paths.
- The project, editor, and terminal strips already share most of the same geometry and view-model
  plumbing, but the duplication hides that shared behavior.

Recommendation:

- Replace the three visible-tab structs with one shared strip item model.
- Keep only the source-specific policies separate: label generation, width policy, close-button
  availability, and reserved-right-space policy.

### 5. Compare and merge still maintain parallel diff-building infrastructure

Priority: Medium

Evidence:

- `src/compare/CompareModel.cpp:20-29` and `src/compare/MergeModel.cpp:14-23` both define local
  `DiffOpKind` and `DiffOp` primitives.
- `src/compare/CompareModel.cpp:63-76` and `src/compare/MergeModel.cpp:41-63` both define local
  line-splitting helpers, but with different newline semantics.

Impact:

- Diff behavior is harder to reason about consistently across compare and merge features.
- The duplicate text-splitting logic already differs: merge handles `CRLF` and `CR`, while compare
  uses a simpler `'\n'` split path.
- Future diff-core fixes are likely to be applied unevenly because the shared concepts do not live
  in a shared utility layer.

Recommendation:

- Introduce a small shared diff-core utility layer for normalized line splitting and basic diff
  operations.
- Keep compare-specific token similarity and merge-specific hunk grouping above that shared layer.

### 6. Async service lifecycle and SDL wake-event plumbing are implemented repeatedly

Priority: Medium

Evidence:

- `src/project/ProjectSearchService.cpp:216-250` and `418-428`
- `src/project/GitBlameService.cpp:595-613` and `846-856`
- `src/terminal/TerminalSession.cpp:405-432` and `1877-1889`

Each subsystem manages its own thread lifetime, stop or join semantics, and SDL wake-event push
logic. The exact behavior is not identical, but the infrastructure pattern is repeated.

Impact:

- Concurrency fixes have to be rediscovered in multiple classes.
- Testing async behavior is harder because there is no single runtime or worker abstraction to
  instrument.
- Shutdown edge cases will keep recurring as one-off fixes.

Recommendation:

- Introduce a narrow shared runtime for background workers that need SDL wake events.
- A small `AsyncWakeWorker` style abstraction would be enough if it owned thread start or stop,
  wake-event dispatch, and common shutdown behavior while leaving domain logic in each service.

## Prioritized Cleanup Order

1. Split `WorkspaceShell` by controller boundary, starting with tab strips and compare or merge.
2. Deduplicate `GitStatusService` porcelain parsing into a single normalized parse pass.
3. Consolidate the repeated chrome tab-strip builders behind one shared model.
4. Move the duplicate line-serialization and line-splitting helpers into shared text utilities.
5. Standardize async service lifecycle handling after the shell split reduces call-site coupling.

## Summary

The recent cleanup work removed several previous hotspots, but the current debt is still dominated
by one large pattern: too much behavior is routed through `WorkspaceShell`, and that shape keeps
producing small local duplications around it. The best next step is not a broad rewrite. It is to
extract a few narrow controllers and shared helpers so the remaining duplication has somewhere sane
to live.
