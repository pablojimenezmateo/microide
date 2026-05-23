## Context

After `comprehensive-tech-debt-cleanup` (archived 2026-04-29) and the throughput passes that followed, the workspace layer is in good shape: `WorkspaceShell` is thin, persistence routes through `PersistenceService`, viewport ownership is collapsed onto editor tabs, and a discovery-based render lint enforces view-model-only access. What remains are mechanical leftovers:

- The 870-LOC one-shot legacy persistence importer is still compiled. Six of its public functions (`SerializeUserConfig`, `SerializeProjectConfig`, `SerializeProjectSession`, `SerializeWorkspaceSession`, `EncodeSessionNodePath`, `DecodeSessionNodePath`) have zero callers. The `persisted-state-format` spec already schedules its deletion in the release-after-next.
- `WorkspaceShellRenderSidebar.cpp:120-121` allocates `std::string` per frame to materialize a search/replace fallback string from a `std::string_view`. Two heap allocations every frame the search panel is visible — exactly the pattern the render lint was meant to catch, but the lint only checks for shell calls and geometry access.
- `TextViewport::TextForRange` (`src/editor/TextViewport.cpp:754-760`) builds a multi-line clipboard string with `+=` and `push_back('\n')` without reserving capacity. On large multi-line copies this incurs O(log N) reallocations that are easy to remove.
- `TextViewport::ReplaceAllCaseInsensitive` (line 479) copies the entire `document_->lines` vector before mutating, then the change runs per-line. Range-based undo is already shipped for ordinary edits per `dev-docs/project/active-work.md`; this call site is a remnant.
- `WorkspaceTabCoordinatorShellBridge::PrepareEditorViewportForSave` (line 128-135) calls `platform::RunSubprocess(formatter->command, ...)` synchronously on the save path. `WorkspaceToolDownloader::ComputeSha256` (lines 35,41) calls `RunSubprocess` synchronously on tool installation. Both are in `src/workspace/`, both block a foreground thread, and both are flagged in tech-debt item 5 as "deferred". A `ProjectBackgroundExecutor` already exists; the migration just needs to land.
- `FileIndexWatcher::PollFallback` (`src/platform/FileIndexWatcher.cpp:546-575`) walks the snapshot map twice — once for create/modify, once for delete. A merge-walk over both maps in one pass produces the same change set with half the lookups.
- The architectural-lint test (`tests/ArchitectureInvariantsTests.cpp`, 896 LOC, ~14 hard-fail rules) does not cover any of the patterns above. Without new rules, every future contributor who reverts one of the cleanups above goes through code review only.

The user explicitly asked for new invariants so that the patterns we delete cannot be reintroduced.

## Goals / Non-Goals

**Goals:**
- Delete the legacy persistence importer module and its unit-test fixtures in one commit, gated by a green harness run on `cold_startup_*`.
- Remove the four concrete perf paper-cuts above without changing any user-visible behavior.
- Close the formatter and tool-validator follow-ups under tech-debt item 5 by routing both through `ProjectBackgroundExecutor`.
- Extend `ArchitectureInvariantsTests` with four new hard-fail rules covering each removed pattern.
- Update durable docs (`AGENTS.md`, `CLAUDE.md`'s "Hard Architectural Invariants" mirror, `dev-docs/project/known-tech-debt.md`) so the new rules are discoverable.

**Non-Goals:**
- No new features. No user-visible behavior changes.
- Not migrating any other synchronous subprocess wrappers outside `src/workspace/` (e.g., `HostIntegration::RevealPath`, `GitCommandUtil::Run`, `PluginProcessInterop`). Those have different correctness models and stay synchronous in this change.
- Not rewriting `FileWatcher` or the native inotify/FSEvents/RDCW backends — only the polling fallback path is touched.
- Not redesigning the formatter pipeline. The formatter still runs on save; it just runs off the foreground thread, with the result applied back on the main thread before the file is written.
- Not changing the persistence record format or the `PersistedRecordReader`/`Writer` contracts.

## Decisions

### Decision 1: Delete the legacy importer in this change rather than waiting for a separate cleanup

The `persisted-state-format` spec already says "the change that removes [the legacy importer] SHALL also delete remaining `<file>.legacy` files." Splitting that into a third change adds review overhead and another migration window without buying anything. Folding it into this cleanup pass means the deletion lands with the new lint rule that prevents re-introduction, in one reviewable commit.

**Alternative considered:** schedule a dedicated `legacy-persistence-cleanup` change. Rejected because the new architectural lint that rejects references to the deleted module is the load-bearing piece, and that lint belongs alongside the deletion to be meaningful.

**Risk:** users who have not launched MicroIDE since the 2026-04-29 migration would lose their pre-migration state if their `<file>.legacy` was their only copy. Mitigation: the existing EOL scenario already requires `cold_startup_*` to be green and requires the deletion to happen "in one commit." We comply.

### Decision 2: Move strings out of render TUs by extending `RenderViewModelBuilder`, not by `string_view`-only APIs

The render-time string allocation in `WorkspaceShellRenderSidebar.cpp:120-121` could be removed by passing a `std::string_view` to `DrawSingleLineTextTail`. But the underlying problem is that the render TU is composing strings at draw time at all. Extending the sidebar view model to carry the prebuilt `query_fallback` and `replace_fallback` strings, and forbidding any `std::string` materialization in render TU bodies via lint, fixes one instance and prevents the pattern from recurring elsewhere.

**Alternative considered:** add the missing `string_view` overload to `DrawSingleLineTextTail`. Rejected: same lint enforcement difficulty, and pushes the allocation cost to whoever calls the view model — we'd be playing whack-a-mole.

### Decision 3: Use `ProjectBackgroundExecutor` for both subprocess migrations

The executor already exists and is the right home per `performance-budgets` "Background Work Isolation" requirement. The formatter migration needs to capture stdout/stderr and apply the formatted text back to the editor buffer on the main thread; this is structured exactly like other completion-callback flows already on the executor. The SHA256 fallback chain (`sha256sum` → `shasum`) stays inside the dispatched job, so the foreground caller sees a single completion.

**Alternative considered:** spawn a one-off `std::thread`. Rejected: bypasses the executor's instrumentation (`BackgroundTaskCounter`, idle-soak attribution) and would itself need a new lint rule.

### Decision 4: Architectural lint enforces patterns mechanically, even when imperfect

The "no `std::string` materialization in render TU bodies" rule is not perfect — there are legitimate uses of `std::string` for, e.g., a transient debug log that's gated behind a flag. The rule will catch those false positives. We accept this because:

1. Render TUs already are not the right place for new `std::string` work; if a contributor genuinely needs one, the right move is to add a view-model field for it.
2. The lint failure message will explicitly point at the view-model-builder as the right home.
3. The current rule set already takes this stance for shell calls and geometry; this is consistent.

The four new rules and their detection strategies:

| Rule | Detection |
| --- | --- |
| No legacy persistence symbols | `rg` for `WorkspacePersistenceLegacyFormat`, `EncodeSessionNodePath`, `DecodeSessionNodePath`, `ParseUserConfigText`, `ParseProjectConfigText`, `ParseProjectSessionText`, `ParseWorkspaceSessionText` across `src/`, `tests/`, `tools/`. Hard-fail if any hit. |
| No synchronous subprocess in `src/workspace/` | Regex `\bplatform::RunSubprocess\s*\(` over all `*.cpp` in `src/workspace/`. Empty allowlist for now (the two existing call sites are deleted in this change). Hard-fail. |
| No string materialization in render TU bodies | Use the existing `BuildCodeMask` + function-body scanner already in the test. Inside any function whose enclosing TU matches `src/workspace/WorkspaceShellRender*.cpp`, reject `std::string\s*\(` constructor calls (except when followed immediately by an empty `()`), `+=` between string-typed expressions, and `+ std::string(` patterns. The view-model builder TUs are excluded by name. Hard-fail. |
| No full-document line copies in `TextViewport` mutators | Inside any non-const member function of `TextViewport` (regex match function signature in `src/editor/TextViewport.cpp`), reject the patterns `std::vector<std::string>\s+\w+\s*=\s*document_->lines` and `auto\s+\w+\s*=\s*document_->lines\b` (without a subscript). Hard-fail. |

### Decision 5: Performance harness gates the cleanup, not before/after micro-benchmarks

The `performance-budgets` capability already requires harness evidence for renames, viewport changes, and persistence cutovers. We extend that umbrella with one new scenario describing this cleanup; we do not add a new harness binary or scenario file. The change record cites green runs on `cold_startup_small_project`, `cold_startup_large_project`, `typing_steady_state`, `idle_soak_30s`, and `linter_on_save`.

## Risks / Trade-offs

- **[Formatter latency regression on save]** → Migrating the formatter to the executor changes its observable timing. Mitigation: harness `linter_on_save` is the regression oracle; if p99 moves outside tolerance the change author SHALL update the baseline with a `perf-baseline:` line in the change record.
- **[Legacy importer deletion strands user state]** → Already covered by the existing EOL scenario in `persisted-state-format`; we comply with its single-commit deletion rule.
- **[Render-string lint is heuristic and may yield false positives]** → Accepted per Decision 4. Failure messages name the right destination (view-model builder) so contributors know how to comply.
- **[New invariants block legitimate future code]** → All four rules are scoped narrowly to specific directories or functions. None applies project-wide. The render rule excludes `RenderViewModelBuilder*.cpp` by name. The TextViewport rule applies only to non-const mutator bodies.
- **[FileIndexWatcher poll-fallback rewrite introduces a regression]** → The poll path has fewer tests than native backends. Mitigation: add a focused unit test exercising create/modify/delete in a single tick before rewriting.

## Migration Plan

1. Land the perf paper-cuts (TextViewport reserve, range-based undo for replace-all, sidebar view-model strings, FileIndexWatcher single-pass) first. These are the lowest-risk edits and they bake in the green harness baseline before structural deletions.
2. Land the formatter-async and tool-downloader-async migrations. These cross a thread boundary; ship them with their unit-test updates and a green `linter_on_save` harness run.
3. Land the legacy importer deletion in its own commit, gated by green `cold_startup_*` runs. Delete `WorkspacePersistenceLegacyFormat.{h,cpp}`, `PersistenceService` importer call sites, and `<file>.legacy` files in the user data directory at first launch after upgrade. Remove unit tests covering the legacy parser; tests covering the structured format are unaffected.
4. Add the four new architectural-lint rules to `tests/ArchitectureInvariantsTests.cpp` in the same commit as the deletions they enforce. Run `ctest` and confirm zero violations.
5. Update `AGENTS.md` "Hard Architectural Invariants", `CLAUDE.md` mirror, and `dev-docs/project/known-tech-debt.md` to reflect the closures.

Rollback strategy: each step above is a separate commit; revert the offending step. The legacy-importer deletion is the only step with user-visible blast radius — its rollback restores the importer, which the new lint rule would then need to be removed alongside.

## Open Questions

- Should the formatter executor return its result via a callback on the main thread, or via a future polled by the editor save flow? Lean callback-on-main, matching the existing executor patterns. Confirm during implementation.
- Does any out-of-tree user still rely on `<file>.legacy` artifacts as a backup mechanism? If so, the EOL scenario still says delete; resolve through release notes rather than spec change.
