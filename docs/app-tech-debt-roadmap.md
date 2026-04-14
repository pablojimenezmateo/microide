# App Tech Debt Roadmap

Updated on 2026-04-14.

This document tracks the remaining production-code debt after the recent session, compare,
chrome, and test-structure cleanup passes.

## Active Priorities

### 1. Window chrome facts are still partially duplicated

Status:
- In progress

Current state:
- `WorkspaceShell` now uses `WindowChromeState`.
- `Application` still keeps its own `custom_window_chrome_enabled_` flag and separately derives SDL
  window mode before pushing a snapshot into the shell.

Why this still matters:
- There are still two caches of overlapping chrome facts across app and shell layers.
- It is easy for future changes to update one path and forget the other.

Next step:
- Reduce `Application` to raw SDL/window ownership and pass a single derived presentation/chrome
  snapshot into `WorkspaceShell`.

### 2. Window size remains a soft shared source of truth

Status:
- Not started

Current state:
- `last_window_width_` and `last_window_height_` are used widely in `WorkspaceShell`.
- Those values are refreshed from multiple pathways, including render-time updates.

Why this still matters:
- Layout-sensitive features depend on mutable cached dimensions instead of one explicit window
  snapshot.
- It makes hit-testing, reveal logic, and text-input positioning easier to regress.

Next step:
- Introduce a small window snapshot/presentation struct that owns logical size, scale, and chrome
  state together.

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
