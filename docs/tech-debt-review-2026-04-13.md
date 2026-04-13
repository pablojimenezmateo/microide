# Tech Debt Review 2026-04-13

This follow-up review was written after addressing item `#11` from
`docs/tech-debt-review-2026-04-11.md`.

It focuses on the current remaining debt that still has a relatively high
maintenance cost or operational risk in the workspace shell.

## Findings

### 1. Dirty-prompt resolution still mixes prompt UI, cross-project activation, save loops, and quit side effects

Priority: Medium

Evidence:

- `src/workspace/WorkspaceShellPrompts.cpp:78-156` handles every dirty-prompt branch inline in `ConfirmDirtyPrompt()`.
- The same function still switches projects, saves dirty tabs, closes tabs/projects, and sets `quit_requested_` directly.

Impact:

- Dirty-prompt behavior still has a broad blast radius even after the newer project-catalog cleanup.
- Cross-project save-before-quit and save-before-close flows remain harder to reason about or test independently from prompt UI state.

Recommendation:

- Extract a dirty-prompt resolution coordinator that consumes `DirtyPromptState` and owns the save, discard, close, and quit transaction rules.

### 2. Config and session writes still use direct truncate-and-rewrite persistence

Priority: Medium

Evidence:

- `src/workspace/WorkspaceShellPersistence.cpp:89-94`, `139-151`, `452-585`, and `660-676` write user config, project config, project session, and workspace session by opening the target path with `std::ios::trunc` and streaming the full serialized payload directly.

Impact:

- A crash or interrupted write can leave config or session state partially written or empty.
- Persistence failures are effectively silent, which makes field debugging harder when state files become corrupted.

Recommendation:

- Move persistence writes behind an atomic file-save helper that writes to a sibling temp file, flushes, and renames into place, with explicit failure reporting hooks.

### 3. Session snapshotting still hand-builds per-tab persistence records inline

Priority: Medium

Evidence:

- `src/workspace/WorkspaceShellPersistence.cpp:463-585` contains three separate compare, merge, and editor branches that manually translate live tab state into `PersistedEditorTabState`.
- The corresponding text schema still lives separately in `src/workspace/WorkspaceShellShared.cpp:393-649`.

Impact:

- Any new persistable tab field still requires coordinated edits across live tab structs, snapshot logic, parser logic, and serializer logic.
- The persistence boundary is better than before, but it is still not expressed as narrow per-tab adapters with a single ownership point.

Recommendation:

- Introduce per-tab persistence adapters or builders so compare, merge, and editor tabs each own their own live-to-persisted translation.

### 4. Action dispatch is still spread across large switch-based command families with manual string arguments

Priority: Medium

Evidence:

- `src/workspace/WorkspaceShellActions.cpp:50-165` routes actions through a sequence of family dispatchers, then uses large switch blocks with manual `args` parsing and repeated rejection handling.
- Command behavior still depends on string payloads like `args[0]`, direct `std::filesystem::path(args[0])` construction, and repeated guards such as `No active project`.

Impact:

- Adding or changing commands still means editing several places and keeping rejection text, enablement rules, and argument parsing in sync.
- The command surface is more error-prone than the newer controller-style areas because there is no typed request object per action family.

Recommendation:

- Move action families toward table-driven handlers with typed argument parsing or small request structs so validation, execution, and user feedback stay co-located.
