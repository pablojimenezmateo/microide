# Tech Debt Review

Date: 2026-04-11

Scope:

- Read the current workspace, editor, compare/merge, persistence, test, and top-level docs code.
- Built `microide_tests`.
- Ran `./build/microide/microide_tests`.
- Collected line-count and documentation-drift signals.

## Cleanup Applied

- Removed the stale `help` command-completion branch from `src/workspace/WorkspaceShellCommand.cpp`.
- Added `WorkspaceShell::DocumentedCommandUsages()` and a README consistency test so command docs are now checked against the action table.
- Extracted pure chrome text helpers in `src/workspace/WorkspaceShellShared.cpp` for tab titles, tooltips, and breadcrumbs, and added direct tests for those helpers.
- Moved line-based persistence encode/decode into shared serializer helpers with round-trip coverage for user config, project config, project session, and workspace session state.
- Moved `src/editor/RuntimeSyntaxGenerated.cpp` behind a dedicated CMake object target so the generated translation unit no longer sits in the hand-edited core source list.
- Restored the documented runtime output layout under `build/microide/` during the CMake cleanup.
- Removed contributor-local README examples and the broken merge-example link path so the top-level docs no longer point at `/home/pablo/...`.

## Additional Debt Found During Cleanup

### 9. Top-level docs had contributor-local example paths and a broken merge-example link

Priority: Low

Evidence:

- `README.md` previously linked to `/home/pablo/Documents/projects/microide/docs/examples/merge-multiline/README.md`.
- Benchmark examples in the README also used contributor-local absolute paths.

Impact:

- Fresh clones had misleading examples and at least one dead path in the primary user-facing doc.
- The top-level README was leaking author-machine state into committed documentation.

Resolution:

- Replaced those examples with generic `/path/to/...` forms in this cleanup pass.

## Executive Summary

The project is functional and has materially better test coverage than a typical SDL rewrite at this stage, but most of the debt has concentrated into the workspace shell. The main risk is not one isolated bug; it is the amount of behavior routed through one mutable object with many overlapping UI modes and many file-format or action contracts expressed as strings.

The three highest-value cleanup areas are:

1. Split `WorkspaceShell` into smaller controllers/services with narrower state ownership.
2. Replace flag-combination UI flow with a more explicit surface or mode state model.
3. Put persistence and command/docs contracts on structured, testable schemas instead of ad hoc strings.

## Structural Snapshot

Largest source files in the current tree, excluding generated fixtures:

| File | Lines |
| --- | ---: |
| `src/workspace/WorkspaceShell.cpp` | 3439 |
| `src/workspace/WorkspaceShellMouse.cpp` | 2542 |
| `src/workspace/WorkspaceShellRender.cpp` | 2028 |
| `src/terminal/TerminalSession.cpp` | 1792 |
| `src/workspace/WorkspaceShellShared.cpp` | 1378 |
| `src/workspace/WorkspaceShellCompare.cpp` | 1345 |
| `src/workspace/WorkspaceShellInput.cpp` | 1338 |
| `src/workspace/WorkspaceShell.h` | 1335 |
| `src/editor/TextViewport.cpp` | 1285 |
| `src/workspace/WorkspaceShellPrompts.cpp` | 1028 |
| `src/workspace/WorkspaceShellPersistence.cpp` | 991 |

Largest test helpers and test files:

| File | Lines |
| --- | ---: |
| `tests/WorkspaceShellSessionTests.cpp` | 825 |
| `tests/WorkspaceShellProjectTests.cpp` | 797 |
| `tests/WorkspaceShellTestAccess.h` | 670 |
| `tests/WorkspaceShellPromptTests.cpp` | 655 |

This is enough concentration to treat workspace-shell complexity as the dominant technical-debt theme.

## Findings

### 1. `WorkspaceShell` is still a god object

Priority: High

Evidence:

- `src/workspace/WorkspaceShell.cpp:242-313` defines the full action catalog.
- `src/workspace/WorkspaceShell.cpp:1903-2745` handles most command and shortcut behavior in one switch.
- `src/workspace/WorkspaceShell.h:1209-1328` stores project, editor, compare, merge, terminal, menu, prompt, search, drag, clipboard, blame, and persistence state in one class.
- The implementation is split across many files, but they all mutate the same private state bag.

Impact:

- Almost every workspace feature change has a high blast radius.
- Refactors are expensive because invariants are implicit and spread across files.
- The class boundary is no longer meaningful; it is effectively the application.

Recommendation:

- Split by behavior, not by current file names.
- A reasonable target shape is:
  - `WorkspaceChromeController`
  - `EditorWorkspaceController`
  - `CompareMergeController`
  - `TerminalWorkspaceController`
  - `WorkspacePersistenceController`
- Start by moving pure computations and narrow state structs first, then convert mutations behind small interfaces.

### 2. UI flow is still modeled as overlapping flags instead of explicit states

Priority: High

Evidence:

- `src/workspace/WorkspaceShell.h:1223-1300` includes parallel flags for sidebar, overlay, menu bar, command mode, dirty prompt, prompt surface, focus target, search-edit state, and tab drag state.
- `src/workspace/WorkspaceShellInput.cpp` and `src/workspace/WorkspaceShellMouse.cpp` branch on many combinations of those flags to decide what input means.

Impact:

- The number of valid UI states is combinatorial, but there is no central transition graph.
- It is easy to create illegal combinations during future edits.
- Input bugs will tend to be “only when surface A is open, sidebar is temporary, and focus is B” class regressions.

Recommendation:

- Replace the modal booleans with a small number of explicit state objects:
  - active primary surface
  - optional modal surface
  - focused interaction target
- Centralize transitions in named helpers, not distributed `if` trees.
- Add a focused state-transition test suite around those helpers before larger refactors.

### 3. Persistence is stringly typed and brittle

Priority: High

Evidence:

- `src/workspace/WorkspaceShellPersistence.cpp:122-219` manually parses project config line-by-line with token strings like `editor-tab-size`.
- `src/workspace/WorkspaceShellPersistence.cpp:243-466` manually parses session rows such as `tab-begin`, `view`, `compare-right-ref`, `merge-choice`, and `split-node`.
- `src/workspace/WorkspaceShellPersistence.cpp:714-989` writes the same state back using manual text emission.

Impact:

- Small format changes require touching both parse and write code in multiple places.
- Invalid input handling is mostly “ignore or bail,” which makes migrations hard to reason about.
- Session behavior is difficult to evolve because the schema is implicit in control flow, not explicit in data types.

Recommendation:

- Move session/config persistence to a structured format with an explicit versioned schema.
- Add round-trip tests for each persisted tab kind plus migration tests for old versions.
- Even if the storage remains text-based, isolate encode/decode into dedicated serializer modules instead of embedding it in `WorkspaceShellPersistence.cpp`.

### 4. Command/docs contracts are drifting

Priority: Medium

Evidence:

- `README.md:141` still lists the `help` command.
- `src/workspace/WorkspaceShell.cpp:242-313` no longer includes a `help` action in `ActionSpecs()`.
- `src/workspace/WorkspaceShellCommand.cpp:163-164` still contains command-completion logic for `help`.

Impact:

- User-facing documentation is no longer authoritative.
- Dead or half-removed UX paths accumulate.
- Future cleanup becomes harder because it is unclear whether a behavior is intentionally removed or accidentally stale.

Recommendation:

- Generate the command list in the docs from `ActionSpecs()` or add a test that validates README command sections against the action table.
- Remove dead command-completion branches when command actions are removed.
- Treat docs drift as a CI failure for top-level command/menu documentation.

### 5. Tests rely heavily on white-box access rather than stable product seams

Priority: Medium

Evidence:

- `src/workspace/WorkspaceShell.h:1330-1332` exposes `WorkspaceShellTestAccess` as a friend.
- `tests/WorkspaceShellTestAccess.h:17-45` directly mutates private members like `project_root_`, `directory_tree_`, `file_index_`, `open_tabs_`, and `focus_`.
- The helper is 670 lines long and contains a large amount of setup and inspection logic.

Impact:

- Internal refactors become expensive because tests are coupled to representation, not behavior.
- Missing public or helper-level abstractions stay hidden because tests can always reach into internals.
- Integration tests risk becoming alternate implementations of the shell instead of user-level validation.

Recommendation:

- Keep `WorkspaceShellTestAccess` only as a migration aid.
- Move deterministic behavior into pure helpers or narrow subsystem classes with direct unit tests.
- Prefer tests that exercise public actions and stable outputs over tests that manually stitch private state.

### 6. Chrome rendering changes are still hard to test directly

Priority: Medium

Evidence:

- Recent tab/breadcrumb simplification is implemented in `src/workspace/WorkspaceShellRender.cpp`.
- Existing tests mostly assert computed labels and helper-returned strings such as breadcrumb labels and tab tooltips, not the actual rendered chrome composition.
- The render path still mixes layout, truncation, text selection, and draw calls in the same functions.

Impact:

- Visual regressions can ship even when all tests pass.
- Small chrome changes require manual visual inspection because there is no render-model layer to assert.

Recommendation:

- Extract a lightweight render model for workspace chrome text and hit targets.
- Test that model directly without requiring pixel tests.
- Keep SDL draw calls as the last translation step, not where the chrome decisions are made.

### 7. The old status-message system is gone, but nothing has replaced it

Priority: Medium

Evidence:

- Command and workflow validation now mostly short-circuit silently in `src/workspace/WorkspaceShell.cpp:1924-2745`.
- Many operations still return success/failure without any visible user feedback surface.

Impact:

- Keyboard- and command-driven workflows now have weak discoverability on failures.
- Some operations effectively no-op from the user’s perspective when preconditions are not met.

Recommendation:

- Introduce a small transient notification/toast queue or an explicit command-output panel.
- Keep it decoupled from the old breadcrumb/status-strip pattern so it does not reintroduce chrome clutter.
- Add tests around failure messaging once a replacement surface exists.

### 8. Generated syntax data sits directly in core compile paths

Priority: Low to Medium

Evidence:

- `src/editor/RuntimeSyntaxGenerated.cpp` is 3625 lines and is compiled into the main app, tests, and benches from `CMakeLists.txt`.

Impact:

- Regenerated syntax snapshots will create noisy diffs and longer rebuilds than necessary.
- Generated assets are tightly coupled to core binary compilation instead of isolated as a data artifact or dedicated object target.

Recommendation:

- Move generated syntax data into a dedicated object library or data asset pipeline.
- Keep the generated file out of the main hand-edited core source set where possible.

## Cross-Cutting Observations

- The project already has meaningful automated coverage. That is an asset worth preserving during refactors.
- `ActionSpecs()` is a good central source of truth, but it is not yet treated as the single source of truth for docs, completion, and UX metadata.
- `docs/todo.md` is currently more trustworthy than `README.md`. That should be inverted.

## Suggested Refactor Order

### Phase 1: Low-risk cleanup

- Remove dead command/docs branches and add a docs-consistency test.
- Extract a render-model layer for top chrome.
- Add serializer round-trip tests around the current persistence format before changing it.

### Phase 2: Boundary cleanup

- Extract persistence out of `WorkspaceShell`.
- Extract compare/merge state and behavior out of `WorkspaceShell`.
- Reduce `WorkspaceShellTestAccess` by replacing private-state setup with smaller public test seams.

### Phase 3: State-model cleanup

- Replace modal booleans with explicit state objects.
- Collapse duplicated input-routing logic across command, overlay, sidebar, and prompt surfaces.
- Add transition-focused tests that lock down valid UI states.

## Quick Wins

- Remove the remaining `help` completion branch in `src/workspace/WorkspaceShellCommand.cpp`.
- Regenerate the README command list from `ActionSpecs()`.
- Add one testable chrome render-model helper so future breadcrumb/tab changes stop depending on manual visual verification.
