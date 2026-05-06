## 1. Capture pre-change harness baseline

- [x] 1.1 Run `cmake --build build` and `ctest --test-dir build --output-on-failure` from a clean tree to confirm a green starting point.
- [x] 1.2 Run the perf harness against `cold_startup_small_project`, `cold_startup_large_project`, `typing_steady_state`, `idle_soak_30s`, and `linter_on_save`; record the run id and ensure all scenarios are green before any code changes land. (`typing_steady_state` is represented by `typing_small_file` + `typing_large_file` in the current harness; reports: `/tmp/codebase_cleanup_task1_2_smoke.{json,txt}` and `/tmp/codebase_cleanup_task1_2_idle.{json,txt}`)
- [x] 1.3 Snapshot current `tests/perf/baselines/<scenario>.json` files so any later baseline updates can be justified diffwise in the change record. (snapshot: `openspec/changes/codebase-cleanup-perf-and-debt/artifacts/prechange-baselines-2026-05-06/`)

## 2. Render-path string allocation removal

- [x] 2.1 Add `query_fallback_text` and `replace_fallback_text` fields (typed `std::string`) to the sidebar view model owned by `RenderViewModelBuilder` (`src/workspace/RenderViewModelBuilder*.{h,cpp}` — locate the exact builder in code).
- [x] 2.2 Populate the new fields in the builder by composing `"search> "` / `"replace> "` with the resolved query/replace text once per build, not once per frame.
- [x] 2.3 Replace lines 120-121 of `src/workspace/WorkspaceShellRenderSidebar.cpp` with view-model field reads; remove the per-frame `std::string` constructors and concatenation.
- [x] 2.4 Re-run the focused render fixtures and confirm the sidebar still draws identical text in literal/regex/replace modes. (`SDL_VIDEODRIVER=dummy ctest --test-dir build --output-on-failure` green)

## 3. TextViewport string-building cleanup

- [x] 3.1 In `src/editor/TextViewport.cpp`, modify `TextForRange` (around line 738) to compute the total byte length up front and call `text.reserve(...)` before the multi-line append loop. (current function name: `SelectedText`)
- [x] 3.2 Apply the same `reserve` treatment to `CurrentLineTextForClipboard` and any sibling helpers that build multi-line clipboard strings without reserving.
- [x] 3.3 Add a focused unit test that copies a 10k-line range and asserts a single allocation profile via instrumented allocator (or behavioral round-trip if the allocator harness is unavailable). (added behavioral round-trip test `TextViewport/SelectAllCopiesLargeDocumentRoundTrip`)

## 4. Range-based undo for `ReplaceAllCaseInsensitive`

- [x] 4.1 Trace how `TextViewport::ReplaceAllCaseInsensitive` currently captures undo state at the entry point (line ~479) — confirm the existing range-based undo helper used by ordinary edits. (current function name is `TextViewport::ReplaceAll`)
- [x] 4.2 Replace `const std::vector<std::string> before_lines = document_->lines;` with a per-range capture that records only the affected line indices and their original contents.
- [x] 4.3 Verify the existing `TextViewport` undo/redo tests still pass for replace-all cases, and add a coverage case for replace-all on a 50k-line document with sparse hits. (added `TextViewport/ReplaceAllUndoRedoHandlesLargeSparseDocument`)

## 5. Single-pass FileIndexWatcher poll fallback

- [x] 5.1 In `src/platform/FileIndexWatcher.cpp` poll fallback path (around line 540), replace the two-loop create/modify + delete walk with a single merge-walk over the union of `current` and `snapshot` keys.
- [x] 5.2 Add a unit test that drives create + modify + delete in a single tick and asserts each change appears exactly once in the emitted batch. (added `FileIndexWatcher/PollDiffSingleTickEmitsUniqueCreateModifyDelete`)

## 6. Async formatter dispatch

- [x] 6.1 Audit `src/workspace/WorkspaceTabCoordinatorShellBridge.cpp` `PrepareEditorViewportForSave` to confirm the formatter call sequence (`platform::RunSubprocess(...)`, exit code handling, stdout capture).
- [x] 6.2 Move the formatter invocation onto `ProjectBackgroundExecutor`. Apply the formatted result back to the editor buffer on the main thread via the executor completion callback before the file is written.
- [x] 6.3 Update or add tests covering: (a) successful format applied before save, (b) formatter exit non-zero leaves the buffer unchanged and the save proceeds with the unformatted text, (c) save while a previous format is still in flight cancels or coalesces correctly. ((a) covered by `WorkspaceShell/SavePipelineRunsParticipantsBeforeFormatter`; (b) covered by `WorkspaceShell/SavePipelineFormatterFailureLeavesBufferUnchanged`; (c) covered by `WorkspaceShell/SavePipelineOverlappingSavesCoalesceCorrectly`; TSAN race fixes validated in `microide-tsan`)
- [x] 6.4 Run `linter_on_save` harness; if p99 moves outside tolerance, update `tests/perf/baselines/linter_on_save.json` and add a `perf-baseline:` justification line in the change record. (reports: `/tmp/codebase_cleanup_task6_4_linter.{json,txt}`, `/tmp/codebase_cleanup_task6_4_linter_rebaseline.txt`)

## 7. Async tool SHA256 verification

- [x] 7.1 In `src/workspace/WorkspaceToolDownloader.cpp`, move `ComputeSha256` (lines 34-41) to dispatch `sha256sum` (and the `shasum` fallback) through `ProjectBackgroundExecutor`. Keep the fallback chain inside the dispatched job.
- [x] 7.2 Update the caller to await the executor result via the completion-callback pattern and surface failures through the existing tool-installation status pipeline.
- [x] 7.3 Confirm the tool-downloader unit tests still pass and add coverage for the case where `sha256sum` is missing and the `shasum` fallback succeeds. (added `WorkspaceToolDownloader/FallsBackToShasumWhenSha256sumMissing`)

## 8. Delete the legacy persistence importer

- [x] 8.1 Delete `src/workspace/WorkspacePersistenceLegacyFormat.h` and `src/workspace/WorkspacePersistenceLegacyFormat.cpp` (~870 LOC).
- [x] 8.2 Remove the importer call sites in `src/workspace/PersistenceService.cpp` and any helper functions whose only callers were the importer.
- [x] 8.3 Add a one-shot startup step in `PersistenceService` initialization that scans the user data directory for `project.state.legacy`, `user.config.legacy`, `session.workspace.legacy`, and `chat.conversations.legacy` files and deletes them after confirming the structured equivalents exist.
- [x] 8.4 Delete unit-test fixtures and tests that exclusively cover the legacy parser; keep tests covering the structured format unchanged.
- [x] 8.5 Run `cold_startup_small_project` and `cold_startup_large_project` from the harness; confirm both are green on the deletion commit and cite the run in the change record. (reports: `/tmp/codebase_cleanup_task8_5_coldstart.{json,txt}`, `/tmp/codebase_cleanup_task8_5_coldstart_rebaseline.txt`)

## 9. New architectural-lint invariants

- [x] 9.1 In `tests/ArchitectureInvariantsTests.cpp`, add `CheckNoLegacyPersistenceSymbols` (hard-fail). Scan `src/`, `tests/`, and `tools/` for any of: `WorkspacePersistenceLegacyFormat`, `EncodeSessionNodePath`, `DecodeSessionNodePath`, `ParseUserConfigText`, `ParseProjectConfigText`, `ParseProjectSessionText`, `ParseWorkspaceSessionText`. Use the existing `BuildCodeMask` helper so matches inside comments and string literals are ignored.
- [x] 9.2 Add `CheckNoSynchronousSubprocessInWorkspace` (hard-fail). Regex `\bplatform::RunSubprocess\s*\(` over `src/workspace/**/*.cpp` against the code mask. Empty allowlist; failure message names `ProjectBackgroundExecutor` as the destination.
- [x] 9.3 Add `CheckRenderTuDoesNotMaterializeStrings` (hard-fail). For each `src/workspace/WorkspaceShellRender*.cpp`, scan function bodies (reuse the body-extractor used by `CheckLspDidOpenIsNonBlocking`) for `std::string\s*\(` constructors with a non-empty argument list, `to_string`, `std::format`/`fmt::format`, and `+`/`+=` between string-typed expressions. Exclude `RenderViewModelBuilder*.cpp` by name. Failure message points contributors at the view-model builder.
- [x] 9.4 Add `CheckTextViewportNoFullDocCopy` (hard-fail). Scan non-const member function bodies of `TextViewport` in `src/editor/TextViewport.cpp` for `std::vector<std::string>\s+\w+\s*=\s*document_->lines` and `auto\s+\w+\s*=\s*document_->lines` (without a trailing subscript or iterator expression).
- [x] 9.5 Wire the four new rules into `TestArchitectureInvariants()` and confirm a clean tree fails zero rules.
- [x] 9.6 Add deliberate-violation fixtures (in test-only code that the lint never scans) to prove each rule catches the pattern it intends to catch; keep the fixtures excluded from the scan via path filter.

## 10. Documentation updates

- [x] 10.1 Update `AGENTS.md` § "Hard Architectural Invariants" to list the four new invariants alongside the existing ones, with one-line rationales.
- [x] 10.2 Mirror the new invariants in `CLAUDE.md` § "Hard Architectural Invariants" so the project context loaded by Claude Code stays accurate.
- [x] 10.3 Update `docs/known-tech-debt.md`: close item 5's "formatter and tool-validator follow-ups" subsection and the "legacy-persistence-cleanup" follow-up under "Open Follow-Ups After The 2026-04-29 Cleanup".
- [x] 10.4 Add a one-paragraph entry to `docs/active-work.md` under the current phase summarizing what shipped in this cleanup so future readers can find the trail without consulting the openspec archive.

## 11. Validation and harness re-run

- [x] 11.1 Run the full default test suite: `ctest --test-dir build --output-on-failure`. Resolve any failures before proceeding.
- [x] 11.2 Run the ASAN preset: `cmake --preset microide-asan && cmake --build build/microide-asan && ctest --test-dir build/microide-asan --output-on-failure`.
- [x] 11.3 Run the UBSAN preset on the same flow.
- [x] 11.4 Run the TSAN preset (after `sudo sysctl vm.mmap_rnd_bits=28`).
- [x] 11.5 Re-run all five harness scenarios listed in task 1.2; capture the run ids and cite them in the change record. (report artifacts: `/tmp/codebase_cleanup_task11_5_perf.{json,txt}`)
- [x] 11.6 Confirm `openspec validate codebase-cleanup-perf-and-debt --strict` passes before archive.

## 12. Archive

- [x] 12.1 After all tasks above are completed and the harness re-run is green, run `/opsx:archive` to move the change to `openspec/changes/archive/` and update the four touched specs (`persisted-state-format`, `workspace-architecture`, `performance-budgets`, plus any that grew implicit deltas).
