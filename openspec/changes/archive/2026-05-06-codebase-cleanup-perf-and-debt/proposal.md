## Why

Recent cleanup passes archived the workspace god-class layer, replaced the legacy text persistence reader with a structured one, and tightened the render-path contract — but mechanical leftovers stayed in tree. The one-shot legacy persistence importer (`WorkspacePersistenceLegacyFormat.{h,cpp}`, ~870 LOC, six dead public functions) is still compiled in even though `persisted-state-format` already schedules it for end-of-life. Several hot-path call sites still do per-frame `std::string` materialization, unreserved multi-line buffer building, and synchronous subprocess waits behind workspace-level wrappers (`RunSubprocess(...)` for the on-save formatter and `sha256sum` tool verification). And the architectural-lint test does not yet cover any of these patterns, so the cleanup is exactly as durable as a code review. We do this now because the existing `persisted-state-format` and `workspace-architecture` specs already point at the deletions and let us close them in a single pass — and the user explicitly asked for new invariants so the patterns we remove cannot be reintroduced.

## What Changes

- **BREAKING** Delete `src/workspace/WorkspacePersistenceLegacyFormat.{h,cpp}` (870 LOC), the one-shot importer call sites in `PersistenceService.cpp`, and any `<file>.legacy` artifacts written by the original migration. This closes the `One-Shot Legacy Importer Has A Documented End-Of-Life` requirement under `persisted-state-format`.
- Trim `TextViewport::TextForRange` (`src/editor/TextViewport.cpp:754-760`) to reserve total capacity before the loop instead of incrementally reallocating across multi-line clipboard copies.
- Replace the full-document `lines` snapshot in `TextViewport::ReplaceAllCaseInsensitive` (`src/editor/TextViewport.cpp:479`) with a range-only undo capture, matching the range-based undo model already shipped for ordinary edits.
- Move per-frame `std::string` allocations in `src/workspace/WorkspaceShellRenderSidebar.cpp:120-121` (search/replace fallback strings) into the `RenderViewModelBuilder`, so the render TU draws from prebuilt view-model fields and stops allocating on every frame the search panel is visible.
- Route the on-save formatter (`src/workspace/WorkspaceTabCoordinatorShellBridge.cpp:128-135`) and tool SHA256 verification (`src/workspace/WorkspaceToolDownloader.cpp:34-41`) through `ProjectBackgroundExecutor` instead of calling `platform::RunSubprocess(...)` synchronously from workspace code. This closes the formatter and tool-validator follow-ups called out by tech-debt item 5.
- Collapse the `FileIndexWatcher` poll-fallback double-scan (`src/platform/FileIndexWatcher.cpp:546-575`) into a single pass that emits create/modify/delete in one walk over a merged keyset.
- **Add new architectural invariants** to `tests/ArchitectureInvariantsTests.cpp` (all hard-fail), enforced by `ctest`:
  - No symbol or include from the deleted legacy persistence module appears anywhere in `src/`, `tests/`, or `tools/`.
  - No `platform::RunSubprocess(` call site exists in `src/workspace/` outside an explicit allowlist of background-executor wrappers.
  - No `std::string` is materialized (via `operator+`, `operator+=`, or a `std::string(` constructor that takes a `std::string_view`) inside any function body in `src/workspace/WorkspaceShellRender*.cpp`; all draw-time strings must come from prebuilt view-model fields.
  - No `TextViewport` member function takes a full-document copy of `document_->lines`; range-based snapshots only.
- Update `docs/known-tech-debt.md` to close item 5's formatter/tool-validator follow-ups and the legacy-persistence cleanup follow-up.

No user-visible features change. No on-disk format changes other than the deletion of `.legacy` migration leftovers covered by the existing EOL scenario.

## Capabilities

### New Capabilities

(none)

### Modified Capabilities

- `persisted-state-format`: close `One-Shot Legacy Importer Has A Documented End-Of-Life` by deleting the importer and add a new requirement that the architectural-lint test rejects any reference to the removed legacy module.
- `workspace-architecture`: add hard-fail invariants for (a) synchronous `platform::RunSubprocess` calls in `src/workspace/`, (b) draw-time `std::string` materialization inside `WorkspaceShellRender*.cpp`, and (c) full-document `lines` snapshot copies inside `TextViewport` mutators.
- `performance-budgets`: add a cleanup-cutover scenario requiring the harness to be green across `cold_startup_*`, `typing_steady_state`, `idle_soak_30s`, and `linter_on_save` after this change lands; the formatter-async migration in particular SHALL be cited with `linter_on_save` evidence.

## Impact

- Affected code: `src/workspace/WorkspacePersistenceLegacyFormat.{h,cpp}` (deleted), `src/workspace/PersistenceService.cpp` (importer call sites removed), `src/editor/TextViewport.cpp`, `src/workspace/WorkspaceShellRenderSidebar.cpp`, `src/workspace/RenderViewModelBuilder*.{h,cpp}` (new fields), `src/workspace/WorkspaceTabCoordinatorShellBridge.cpp`, `src/workspace/WorkspaceToolDownloader.cpp`, `src/platform/FileIndexWatcher.cpp`, `tests/ArchitectureInvariantsTests.cpp`, `tests/PersistenceMigrationTests.cpp` (importer cases removed).
- Affected APIs: internal-only; no plugin-facing Lua API changes; no public C ABI exists.
- Affected on-disk artifacts: `<file>.legacy` files left over from the original migration are deleted on first launch after this change ships, per the existing EOL scenario.
- Affected docs: `docs/known-tech-debt.md`, `docs/active-work.md`, `AGENTS.md` § "Hard Architectural Invariants" (new invariants documented), `CLAUDE.md` (mirror).
- Affected tests: legacy-format unit tests removed; `ArchitectureInvariantsTests` extended with four new rules and their fixtures; existing perf harness scenarios re-baselined where this change moves the numbers.
- Risk: low. All edits are bounded in scope, the architectural-lint changes are mechanical, and the harness already covers the surfaces touched. The largest unknown is whether the formatter-async migration moves `linter_on_save` p99 enough to require a baseline update — this is captured as a `perf-baseline:` budget in tasks.md.
