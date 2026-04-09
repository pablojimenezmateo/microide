# MicroIDE Roadmap

This document is forward-looking.
`docs/todo.md` remains the source of truth for what is implemented today.

## Reviewed Code Anchors

This roadmap was rechecked against the current codebase on 2026-04-08 with emphasis on:

- `src/project/ProjectSearchService.cpp`
- `src/project/FileIndex.cpp`
- `src/project/GitStatusService.cpp`
- `src/project/GitCompareService.cpp`
- `src/project/FileOperationService.cpp`
- `src/workspace/WorkspaceShell.cpp`
- `src/workspace/WorkspaceShellSidebar.cpp`
- `src/workspace/WorkspaceShellOverlay.cpp`
- `src/workspace/WorkspaceShellPrompts.cpp`
- `src/workspace/WorkspaceShellPersistence.cpp`
- `src/terminal/TerminalSession.cpp`
- `src/editor/TextViewport.cpp`
- `tests/*`

## Near-Term Priorities

### 1. Polish built-in project search

The first backend unification is done: project search no longer depends on `rg` being installed.

Current state:

- `ProjectSearchService` now uses one built-in PCRE2-backed engine
- the sidebar now exposes literal or regex mode, explicit case mode, and hidden-file inclusion
- literal mode avoids regex compilation for common text searches
- file enumeration stays aligned with project ignore handling

Remaining work:

- decide whether replace-in-project should stay literal-only or grow regex-aware replace behavior
- broaden backend coverage with more tests for result ordering, cancellation, and larger projects
- benchmark literal-mode search on bigger repositories before adding more search UI or search syntax

### 2. Finish terminal polish without turning it into a second product

The terminal is already useful, but it is still only a partial emulator.

Current state:

- PTY tabs, scrollback, keyboard input, selection/copy, and alternate-screen handling exist
- common ANSI cursor, erase, and edit sequences work
- app mouse reporting exists for mouse-aware terminal programs

Remaining work:

- broaden ANSI and control-sequence coverage where real tools still break
- keep resize, redraw, and scrollback behavior robust under long-running output
- run more real-app validation instead of guessing from escape-sequence lists

The goal is a dependable embedded terminal, not a separate terminal-emulator project.

### 3. Improve tree-mutation UX

Tree create, rename, delete, and trash flows are present, but they still have sharp edges.

Current state:

- tree context menus and prompt surfaces exist
- rename retargets affected tabs and delete closes affected tabs
- dirty rename and delete now prompt to save or discard affected tabs instead of silently discarding work

Remaining work:

- tighten consequences for split tabs, compare tabs, and prompt cleanup around mutations
- decide whether project open should graduate from a typed path flow to a dedicated picker

### 4. Fill the editor gaps that still matter for a micro-IDE

The biggest remaining editor-facing gaps are:

- explicit validation and threshold tuning for editor large-file mode
- continued UTF-8 and IME hardening while the buffer model is still byte-oriented

Diagnostics and problem styling belong here only if a diagnostics phase is intentionally started.

### 5. Harden project services

Search is not the only service boundary that still needs work.

Current state:

- search lives behind `ProjectSearchService`
- git state, compare, stage, discard, and outgoing-file queries still shell out to `git`

Remaining work:

- decide how much external-`git` dependence is acceptable for the product
- if external `git` stays, tighten subprocess handling and error reporting
- move any avoidable filesystem or git refresh work off latency-sensitive UI paths

## Cross-Cutting Work

### Testing

The highest-value missing automated coverage is:

- project search behavior under cancellation, large repositories, and UI-driven option changes
- session restore and multi-project workflow coverage
- tree mutation flows
- compare and merge workflow coverage beyond the current model-level tests
- terminal behavior checks where deterministic harnesses are practical

### Performance

The highest-value remaining performance work is:

- glyph-atlas style text rendering work
- dirty-rect and caret-only invalidation
- replace the current UI zoom path, which stretches a reduced logical framebuffer and blurs text, with a proper HiDPI scaling model that keeps layout and font rendering sharp at non-100% scales
- startup and idle redraw profiling driven by `docs/startup-tracing.md`

## Out of Scope

These are not roadmap items for the current product:

- debugging
- plugin runtimes or marketplaces
- cloud sync and collaboration
- built-in chat surfaces
- recent project or recent file affordances
- soft wrap
