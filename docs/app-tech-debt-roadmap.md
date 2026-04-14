# App Tech Debt Roadmap

Updated on 2026-04-14.

This document tracks the remaining production-code debt after the recent session, compare,
chrome, and test-structure cleanup passes.

## Active Priorities

### 1. Window chrome facts are still partially duplicated

Status:
- In progress

Current state:
- `WorkspaceShell` now receives a single `WindowPresentationState` snapshot that carries logical
  size, presentation scale, and chrome mode together.
- `Application` no longer keeps its own `custom_window_chrome_enabled_` flag and instead derives
  custom chrome state from the live SDL window when capturing that snapshot.

Why this still matters:
- Internal shell consumers still read cached `last_window_width_`, `last_window_height_`, and
  `window_chrome_` fields separately after the snapshot is unpacked.
- Future window-state work can still drift unless more call sites consume a narrower presentation
  snapshot directly.

Next step:
- Keep reducing shell-internal use of loose width/height caches now that app-to-shell presentation
  state flows through one struct.

### 2. Window size remains a soft shared source of truth

Status:
- Not started

Current state:
- `last_window_width_` and `last_window_height_` are used widely in `WorkspaceShell`.
- App-owned presentation updates now arrive through one snapshot, but render-time refresh still
  writes the cached dimensions directly.
- Common callers now route repeated window-rect and layout lookups through
  `CurrentWindowRect()` and `CurrentWorkspaceLayout()` instead of rebuilding those checks ad hoc.

Why this still matters:
- Layout-sensitive features depend on mutable cached dimensions instead of one explicit window
  snapshot.
- It makes hit-testing, reveal logic, and text-input positioning easier to regress.

Next step:
- Replace more shell-internal width/height reads with narrower helpers built on top of the shared
  presentation snapshot model.

### 3. Compare ownership is still split between shell render and compare files

Status:
- In progress

Current state:
- Compare tab state, layout, and scroll helpers now live more clearly in
  `WorkspaceShellCompare.cpp`.
- `WorkspaceShellRender.cpp` still contains compare-specific render-adjacent logic such as compare
  text-input visual placement and compare scrollbar integration.

Why this still matters:
- The main render path still knows too much about compare internals.
- Small compare fixes still risk touching multiple large files.

Next step:
- Continue moving compare-specific text-input, scrollbar, and render support logic out of
  `WorkspaceShellRender.cpp`.

Progress in this pass:
- Compare editor text-input visual placement now routes through a compare-owned helper instead of
  being inlined inside the main `WorkspaceShell::Render()` text-input switch.
- Compare scrollbar and marker rendering now routes through a compare-owned helper instead of
  being inlined in the main render function.

### 4. Merge layout/state still mirrors the old compare debt shape

Status:
- Not started

Current state:
- Merge helpers remain spread across `WorkspaceShell.cpp` and `WorkspaceShellCompareRender.cpp`.
- Core merge layout, scroll, interaction, and reveal helpers now live in `WorkspaceShellMerge.cpp`.
- Merge text-input placement and merge scrollbar rendering now route through merge-owned helpers
  instead of remaining in the main `WorkspaceShellRender.cpp` path.

Why this still matters:
- Merge is structurally similar to where compare was before extraction.
- The same blast-radius problem is likely to repeat there.

Next step:
- Continue separating merge rendering ownership from compare rendering now that the general
  `WorkspaceShellRender.cpp` path no longer owns merge-specific text-input and scrollbar logic.

### 5. Git command execution is still shell-string based

Status:
- Not started

Current state:
- Git services still compose shell commands as strings and execute them via the current shell-based
  utility layer.

Why this still matters:
- Quoting, portability, and failure reporting remain fragile.
- Each new git query increases the chance of subtle command construction bugs.

Next step:
- Introduce an argument-vector command runner for git operations and migrate high-value git service
  paths first.

### 6. Layout constants are still duplicated across workspace units

Status:
- Not started

Current state:
- Scrollbar, compare, and chrome spacing constants are still declared in several shell
  implementation files.

Why this still matters:
- Behavioral tweaks can drift across files.
- It raises the cost of keeping compare, merge, and chrome interactions visually consistent.

Next step:
- Consolidate subsystem-owned constants near the subsystem that owns the behavior.
