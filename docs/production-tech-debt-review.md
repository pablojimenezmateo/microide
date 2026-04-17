# Production Tech Debt Review

Reviewed on 2026-04-14.

Scope:
- `src/app/*`
- `src/project/*`
- `src/workspace/*`
- non-test production code only

This document records the next meaningful production-code debt after the recent compare, merge,
window-presentation, layout-constant, and git argv-runner cleanup passes.

## Highest-Impact Findings

### 1. `WorkspaceShell` is still the main blast-radius object

Impact:
- High
- Small features and bug fixes still require touching one class that owns too many unrelated
  concerns.

Evidence:
- `WorkspaceShell` still owns window presentation, project catalog state, tree/index/finder,
  editor tabs, terminal tabs, overlays, git sidebar, prompts, clipboard hooks, cursor state, and
  dialog plumbing.
- The shell private surface still spans action dispatch, input handling, render helpers, compare,
  merge, persistence, and terminal behavior.

References:
- `src/workspace/WorkspaceShell.h:35`
- `src/workspace/WorkspaceShell.h:636`
- `src/workspace/WorkspaceShell.h:1446`

Why this matters:
- Ownership is clearer than before, but the shell remains the place where multiple subsystems meet
  without a narrow boundary.
- That keeps the regression surface high and makes seemingly local changes expensive to review.

Recommended next step:
- Continue splitting shell-owned state and APIs by subsystem, starting with project/editor/panel/
  sidebar state objects or a narrower shell-core facade.

### 2. `WorkspaceShellShared.*` is still a mixed-responsibility module

Impact:
- High
- One shared utility unit still carries unrelated parsing, persistence-facing text formats,
  command or path helpers, and project-presentation helpers even after the recent layout and text
  splits.

Evidence:
- `WorkspaceLayout*` and `WorkspaceTerminalSelection*` now own layout, scroll, compare-or-merge
  marker, and terminal-selection responsibilities.
- `WorkspaceTextSearch*` now owns UTF-8 helpers, line serialization, whitespace normalization,
  and literal-search helpers.
- `WorkspaceShellShared.*` still contains command parsing, config-and-session text parsing,
  path helpers, git-sidebar line helpers, project-state naming, colors, and path-or-breadcrumb
  presentation helpers.

References:
- `src/workspace/WorkspaceLayout.h`
- `src/workspace/WorkspaceLayout.cpp`
- `src/workspace/WorkspaceTerminalSelection.h`
- `src/workspace/WorkspaceTerminalSelection.cpp`
- `src/workspace/WorkspaceTextSearch.h`
- `src/workspace/WorkspaceTextSearch.cpp`
- `src/workspace/WorkspaceShellShared.h`
- `src/workspace/WorkspaceShellShared.cpp`

Why this matters:
- This is still a “shared dump” instead of a cohesive subsystem boundary.
- It increases compile-time coupling and makes unrelated changes land in the same file.

Recommended next step:
- Continue the split after the shipped `WorkspaceLayout*`, `WorkspaceTerminalSelection*`, and
  `WorkspaceTextSearch*` extractions, with the next focused unit centered on command-and-path
  parsing plus the remaining project-and-session presentation helpers.

### 3. Action handling is still too centralized in the shell

Impact:
- Medium
- Adding or changing commands still tends to require shell-header and shell-action edits even when
  the behavior belongs to a narrower subsystem.

Evidence:
- The action enum/spec model still lives in `WorkspaceShell`.
- Dispatch still routes through large domain switches in `WorkspaceShellActions.cpp`.

References:
- `src/workspace/WorkspaceShell.h:620`
- `src/workspace/WorkspaceShell.h:836`
- `src/workspace/WorkspaceShellActions.cpp:430`
- `src/workspace/WorkspaceShellActions.cpp:1209`

Why this matters:
- It preserves a shell-centric command model even after other ownership extractions.
- It makes command growth scale linearly with shell complexity.

Recommended next step:
- Move action registration and dispatch behind subsystem-owned handlers or an action registry with
  per-domain executors.

### 4. The main render path is still too wide

Impact:
- Medium
- The render entry point still combines frame preparation, state sync, terminal resize, editor/
  compare/merge dispatch, text-input area updates, and shell chrome drawing.

Evidence:
- `WorkspaceShell::Render()` still performs substantial pre-render and render-time orchestration in
  one function.

References:
- `src/workspace/WorkspaceShellRender.cpp:192`

Why this matters:
- The compare and merge extractions helped, but the top-level render path still knows too much.
- UI regressions remain harder to isolate than they should be.

Recommended next step:
- Split the render entry point into smaller shell-owned frame phases, for example:
- frame preparation
- chrome/layout draw
- active-surface draw
- text-input/IME presentation

### 5. The git process layer is still a low-level inline header implementation

Impact:
- Medium
- The git runner is safer than before, but process management still lives inline in a shared header
  and is tightly bound to `fork`/`execvp` details.

Evidence:
- `GitCommandUtil.h` contains the full command execution implementation.

References:
- `src/project/GitCommandUtil.h:14`

Why this matters:
- It limits error-model evolution, timeout handling, and future portability work.
- It also spreads process-level implementation through header inclusion instead of keeping it in a
  smaller compiled boundary.

Recommended next step:
- Move git command execution behind a `.cpp`-backed process service with a narrower public API.

## Recommended Order

1. Reduce `WorkspaceShell` ownership further.
2. Split `WorkspaceShellShared.*` into cohesive modules.
3. Decentralize action registration and dispatch.
4. Narrow the top-level render pipeline.
5. Move the git process layer behind a compiled service boundary.

## Non-Findings

- I did not identify a new high-priority duplicate-window-state issue; that area looks materially
  better after the recent window-presentation cleanup.
- I did not identify a new compare/merge ownership issue at the same severity as the ones already
  addressed.
