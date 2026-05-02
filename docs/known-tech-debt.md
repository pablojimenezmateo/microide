# MicroIDE Known Tech Debt

Reviewed on 2026-04-23. Updated 2026-04-29 after comprehensive tech-debt cleanup slices.

This document records the meaningful debt that remains after commit `0aa44cb`
(`Fix shared diff/search paths and active editor state`).

Use this file for deferred work that is real, actionable, and still open.
Use `docs/active-work.md` for current priorities.
The broader architectural review (from 2026-04-20) is archived at `docs/archive/production-tech-debt-review.md`.

## Scope

This list focuses on debt that is:

- still present in the current tree
- likely to affect correctness, latency, or extensibility
- worth preserving as a future work queue

This list intentionally does not repeat issues that were already closed in
`0aa44cb`, including:

- merge using its own quadratic line-diff matrix
- merge result text always serializing with `\n`
- project search rescanning disk on every run instead of consuming an indexed snapshot
- project search allocating a lowercase copy of every candidate line in case-insensitive mode
- `TextViewport::MaxVisualColumns()` always rescanning the entire buffer after ordinary edits
- FIFO text-render cache eviction in `SdlTtfTextBackend`
- split-editor actions and several open-or-navigate paths mutating the stale floating editor copy

## Closed Debt From Comprehensive Cleanup

The following previously tracked debts were closed on 2026-04-29 by
`comprehensive-tech-debt-cleanup`:

- item 1 (`WorkspaceShell` ownership bottleneck): closed
  - `WorkspaceShell.h` now satisfies the architectural size contract
  - legacy `WorkspaceActionContext` file names were removed from the tree
- item 2 (coordinator separation still superficial): closed for this phase
  - architectural lint now hard-fails key boundary regressions
- item 3 (active editor viewport ownership migration): closed
  - stale shell-level viewport alias paths were removed
- item 4 (render and hover shell reach): closed for this phase
  - render-path architectural constraints are enforced by lint
- item 7 (single-line shell text input model): closed
  - shared single-line editor and key-handler model is now shipped
- `WorkspaceLspClient` TSAN race (reported during sanitizer bring-up): closed
  - request/callback ownership synchronization was fixed and verified with TSAN runs in the sanitizer matrix

## 5. Search and Index Integration — Event-Driven File Watch

Status:
- Partially resolved on 2026-05-02 by `deferred-work-and-throughput-pass`.

What was closed:
- `FileIndexWatcher` platform abstraction ships Linux `inotify`, macOS `FSEvents`, Windows
  `ReadDirectoryChangesW`, and a poll-fallback backend.
- `PatternCache` with LRU eviction eliminates repeated PCRE2 compile/JIT on repeated searches.
- `ProjectBackgroundExecutor` isolates per-project git dispatch from the main thread.
- `BackgroundTaskCounter` tracks in-flight background work for adaptive idle rendering.
- PCRE2 JIT is now compiled into the search engine; interpreted fallback emits a one-time log.
- `ProjectSearchService` wires `BackgroundTaskCounter` so the event loop stays awake during search.

What is still open:
- The workspace coordinator does not yet wire `FileIndexWatcher` to project open/close; the
  watcher exists but is not plumbed into the file-finder or project-search call sites (tasks 2.2–2.5).
- File-finder and project-search still fall back to `CollectProjectFiles` directory traversal until
  the watcher wiring lands.
- Git dispatch (`GitOperations::Status`, `Blame`, `Log`) still runs synchronously on the tab/sidebar
  activation path; the `ProjectBackgroundExecutor` exists but migration is deferred (tasks 3.2–3.6).
- `WorkspaceTabCoordinatorShellBridge` and `WorkspaceToolDownloader` still call `RunSubprocess`
  synchronously; tracked separately as formatter and tool-validator follow-ups.

Recommended follow-up:
- Wire `FileIndexWatcher` to project open in the workspace coordinator (task 2.2) and update the
  file-finder and search call sites to consume `ProjectFileIndex::Snapshot()` (tasks 2.4–2.5).
- Migrate git sidebar dispatch through `ProjectBackgroundExecutor` (tasks 3.2–3.4).
- Only move formatter and tool-validator calls async after the git dispatch migration lands and
  profiling confirms they are on a latency-sensitive path.

## 6. Large-File and Performance Validation Still Needs Measurement, Not Assumptions

Impact:
- Medium
- This is process debt with real product consequences

What is still open:
- The recent fixes remove known hotspots, but large-file behavior still needs empirical validation.
- The syntax-highlight jump problem in particular should be measured before and after any future
  checkpoint design.
- Search, merge, blame, and redraw changes should continue to be validated with the startup and
  runtime profiling docs rather than by intuition.

References:
- `docs/startup-tracing.md`
- `docs/runtime-profiling.md`
- `docs/performance-findings.md`

Recommended follow-up:
- Add or extend focused benchmarks where repeated regressions are likely:
  - large-file cursor jump and initial paint
  - merge-model build for large, partially similar inputs
  - project search across large trees with smart-case and regex modes
  - syntax cache invalidation after plugin reload

## 7. Single-Line Shell Text Input Model

Status:
- Addressed on 2026-04-29 by introducing shared `SingleLineEditor` and `SingleLineKeyHandler` for migrated single-line surfaces

Impact:
- Closed in this change.

What remains:
- No further migration is required for task closure.
- Chat composer remains multiline by design and is tracked separately as feature work, not debt.

Relevant code:
- `src/editor/SingleLineEditor.h`
- `src/editor/SingleLineKeyHandler.h`
- `tests/SingleLineEditorTests.cpp`

## 8. `WorkspaceReviewComments` Does Linear Scans Per Frame When Review Comments Are Active

Status:
- Addressed on 2026-04-23 by indexing review comments and threads per URI and line inside
  `WorkspaceReviewComments`

Impact:
- High when the review-comments feature is populated; zero cost when there are no comments

Current state:
- `GetThreads(uri)` and `GetComments(uri, line_index)` now read from indexed URI/line buckets
- The render path uses `HasThreads(uri, line)` and `HasComments(uri, line)` for marker checks
- Add, remove, and clear operations invalidate the affected indexes before the next lookup

What remains:
- No known follow-up for this item unless profiling shows review marker drawing itself is still hot

Relevant code:
- `src/workspace/WorkspaceReviewComments.cpp` — `GetThreads`, `GetComments`
- `src/workspace/WorkspaceShellRenderFrame.cpp` — `draw_review_comment_markers` lambda

See also `docs/performance-findings.md` — Second Performance Pass, New finding 1.

## 9. `ComputeEditorPaneLayouts` Called Twice Per Render Frame

Status:
- Addressed on 2026-04-23 by computing editor pane layouts once in `RenderActiveWorkspaceSurface`

Impact:
- Medium; redundant geometry computation on every frame

Current state:
- `WorkspaceShellRenderFrame.cpp` computes the pane layout once near the top of the active
  workspace-surface render path and reuses it for the main editor render pass and scrollbar pass

What remains:
- No known follow-up for this item unless profiling shows pane layout computation is still hot

Relevant code:
- `src/workspace/WorkspaceShellRenderFrame.cpp`

See also `docs/performance-findings.md` — Second Performance Pass, New finding 2.

## 10. Terminal Cursor State Acquired Under Three Separate Mutex Locks Per Frame

Status:
- Addressed on 2026-04-23 by adding `TerminalSession::CursorSnapshot()`

Impact:
- Medium; three mutex round-trips on the render thread every frame when terminal is visible

Current state:
- Terminal render, caret invalidation, and pending-input capture use `CursorSnapshot()` to read
  row, column, and visibility under one mutex acquisition

What remains:
- The legacy scalar accessors still exist for tests and non-hot callers; remove them only if a
  later cleanup proves they are unused

Relevant code:
- `src/terminal/TerminalSession.h` — `TerminalCursorSnapshot`, `CursorSnapshot()`
- `src/workspace/WorkspaceShellRenderBottomPanel.cpp` — terminal cursor render path

See also `docs/performance-findings.md` — Second Performance Pass, New finding 3.

## 11. `std::find` on `marked_lines` Vector in Review-Comment Marker Rendering

Status:
- Addressed on 2026-04-23 by removing `marked_lines` from render marker drawing

Impact:
- Medium; O(visible_lines × marked_lines) per frame when review markers are present

Current state:
- `draw_review_comment_markers` performs direct indexed thread/comment checks per visible line
- There is no per-frame marked-line vector allocation and no per-line `std::find`

What remains:
- No known follow-up for this item unless review-marker rendering becomes a measured hotspot again

Relevant code:
- `src/workspace/WorkspaceShellRenderFrame.cpp` — `draw_review_comment_markers` lambda

See also `docs/performance-findings.md` — Second Performance Pass, New finding 4.

## Open Follow-Ups After The 2026-04-29 Cleanup

The cleanup change closed items 1–4 and 7 above and shipped the durable contracts in
`openspec/specs/workspace-architecture/spec.md`,
`openspec/specs/persisted-state-format/spec.md`, and
`openspec/specs/shared-edit-primitives/spec.md`. These follow-ups are still worth tracking and
are good candidates for the next openspec tech-debt pass:

1. `WorkspaceShellTestAccess.h` trim follow-up:
   - Closed in the comprehensive tech-debt and perf-harness pass.
   - The top-level header is now a small scoped aggregator with a hard architectural size gate,
     and category-(a) wrappers were migrated to direct shell APIs / event helpers.
   - Keep the remaining scoped methods focused on genuinely test-only seams.
2. The `WorkspaceShell*.cpp` companion files (~70 translation units defined against
   `WorkspaceShellMembers.inc`) keep behavior on the shell namespace even though the header was
   slimmed. Any new behavior should land on a service, not a new `WorkspaceShell*.cpp` companion.
3. `WorkspacePersistenceLegacyFormat.cpp` and the surrounding one-shot importer should be deleted
   in the scheduled `legacy-persistence-cleanup` follow-up (release +2). Do not extend the legacy
   parser; only the structured format gets new fields.
4. Architectural-lint coverage gap:
   - Closed in this change: discovered render-unit scanning is active, plugin/coordinator size
     checks are hard-fail, and the shell test-access header now has an explicit cap check.
5. Oversized coordinator translation units:
   - Closed in this change: coordinator units were decomposed and the coordinator TU-size rule is
     hard-fail.
6. Project-content and indexing architecture (item 5) now has an event-driven watcher layer and
   background executor, but workspace wiring (file-finder, search, git dispatch) is deferred to the
   next pass. Revisit when the `deferred-work-and-throughput-pass` wiring tasks (2.2–2.5, 3.2–3.6)
   are scoped into an openspec change.

## 8–12 Status Update (2026-05-01)

The debt items tracked as 8–12 in this document are now resolved or explicitly narrowed:

- item 8 (review-comment linear scans): resolved; indexed lookup path remains in place
- item 9 (double layout computation per frame): resolved; layout is prepared once and reused
- item 10 (terminal cursor multi-lock reads): resolved; cursor snapshots are single-lock
- item 11 (`std::find` over `marked_lines` in render path): resolved; vector scan removed
- item 12 (sanitizer/fuzz triage tracking): reduced to environment/process follow-up only

Remaining debt focus stays on items 5 and 6 unless new profiling demonstrates regressions.

## 12. 2026-04-29 Sanitizer/Fuzz Triage Snapshot

Status:
- In progress for this change; current non-blocking findings below are triaged with reproduction
  notes and severity.

### 12.1 TSAN Linux Prerequisite

Impact:
- Medium process risk (false negatives if skipped)

Reproduction:
- Run TSAN tests without setting Linux mmap randomization to the expected value.
- Command sequence:
  - `sudo sysctl vm.mmap_rnd_bits=28`
  - `cmake --preset microide-tsan`
  - `cmake --build build/microide-tsan`
  - `ctest --test-dir build/microide-tsan --output-on-failure`

Notes:
- This is an environment prerequisite, not an app bug.
- Documented in `docs/runtime-profiling.md`, `guidelines/testing.md`, `AGENTS.md`, and
  `CLAUDE.md`.

### 12.2 UBSAN Intermittent FileWatcher Assertion Under Heavy Mixed Runs

Impact:
- Low to medium (intermittent in stressy mixed runs, not consistently reproducible in focused reruns)

Reproduction:
- Run broader sanitizer slices in quick succession; one run observed a transient FileWatcher
  assertion failure.
- Focused reruns for the affected area passed.

Notes:
- Keep as watchlist until it reproduces deterministically with a minimized command.
- If it reproduces again, capture exact command and stack and promote to a dedicated debt item.

### 12.3 Fuzz Harness Results (PR-style Short Runs)

Impact:
- No memory-safety findings observed in current short runs.

Reproduction:
- `cmake -S . -B build/microide-fuzz -DMICROIDE_FUZZ=ON -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++`
- `cmake --build build/microide-fuzz`
- `./build/microide-fuzz/microide/PersistedRecordReaderFuzz -max_total_time=10 tests/fuzz/corpora/PersistedRecordReaderFuzz`
- `./build/microide-fuzz/microide/LegacyImporterFuzz -max_total_time=10 tests/fuzz/corpora/LegacyImporterFuzz`
- `./build/microide-fuzz/microide/SearchRegexFuzz -max_total_time=10 tests/fuzz/corpora/SearchRegexFuzz`
- `./build/microide-fuzz/microide/GitBlameParserFuzz -max_total_time=10 tests/fuzz/corpora/GitBlameParserFuzz`

Notes:
- Initial clang/fuzz build surfaced integration defects (sized-delete portability in tests and
  missing object linkage in `LegacyImporterFuzz`), both fixed in-tree.
- No additional deferred fuzz finding is open from this triage pass.
