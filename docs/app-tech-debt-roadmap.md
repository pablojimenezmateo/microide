# App Tech Debt Roadmap

Updated on 2026-04-14.

This document tracked the remaining production-code debt after the session, compare, chrome, and
source-control cleanup passes. The roadmap items below are now complete and kept here as a record
of what was closed.

## Completed Items

### 1. Window chrome facts were partially duplicated

Status:
- Done

Completed state:
- `WorkspaceShell` now keeps one `WindowPresentationState` snapshot instead of unpacked window
  size and chrome caches.
- Shell consumers reuse `CurrentWindowRect()`, `CurrentWorkspaceLayout()`, and
  `CurrentWindowChromeState()` instead of reading separate mutable fields.
- Tests now drive the same presentation snapshot model through `WorkspaceShellTestAccess`.

### 2. Window size was a soft shared source of truth

Status:
- Done

Completed state:
- `last_window_width_` and `last_window_height_` are gone from `WorkspaceShell`.
- Render refresh updates the shared presentation snapshot, and layout-sensitive callers now read
  from the shared accessors built on top of that snapshot.

### 3. Compare ownership was split between shell render and compare files

Status:
- Done

Completed state:
- Compare layout, scroll, reveal, input-visual, and scrollbar rendering helpers now live in
  compare-owned units.
- The main shell render path now dispatches to compare helpers instead of owning compare-specific
  mechanics inline.

### 4. Merge layout/state mirrored the old compare debt shape

Status:
- Done

Completed state:
- Merge layout, interaction, hover, reveal, and result-input helpers now live in
  `WorkspaceShellMerge.cpp`.
- Merge rendering ownership now lives in `WorkspaceShellMergeRender.cpp` instead of being mixed
  into compare rendering and the general shell render path.

### 5. Git command execution was shell-string based

Status:
- Done

Completed state:
- Git repository, compare, and blame services now run through an argument-vector command layer in
  `GitCommandUtil.h`.
- Regression coverage now includes quoted and spaced paths so git command construction does not
  drift back toward shell-string assembly.

### 6. Layout constants were duplicated across workspace units

Status:
- Done

Completed state:
- Shared workspace geometry constants now live in `WorkspaceShellShared.h`.
- Compare, merge, chrome, menu, tab-drag, render, and shared layout code now consume those shared
  constants instead of repeating raw values in multiple files.

## Remaining Active Items

- None in this roadmap.
