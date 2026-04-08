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

### 1. Make project search independent of `rg`

This is the clearest remaining product-level gap.

Current state:

- `ProjectSearchService` tries `rg --vimgrep` first
- if `rg` is unavailable, it falls back to an internal file scan
- the fallback is not behaviorally equivalent to the `rg` path

The important mismatches today are:

- the fallback only performs case-insensitive literal substring search
- the `rg` path supports regex syntax and smart-case semantics
- the current UI path always runs with `show_hidden = false`
- replace-in-project only works for literal queries, based on query heuristics

Desired end state:

- shipped search behavior does not rely on `rg` being installed
- result ordering, matching rules, and cancellation behavior are consistent
- `rg` can remain an optional accelerator, not the source of truth

Concrete work:

- define a backend-independent search options model
- implement the default engine over `FileIndex` with batching and cancellation
- decide whether regex support stays in scope for the built-in engine
- keep ignore handling and path filtering aligned with the tree and file index
- add parity tests for search results, cancellation, and replace eligibility

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
- dirty tabs block rename and delete instead of silently discarding work

Remaining work:

- offer save or discard prompts instead of hard-blocking every dirty rename/delete case
- tighten consequences for split tabs, compare tabs, and prompt cleanup around mutations
- decide whether project open should graduate from a typed path flow to a dedicated picker

### 4. Fill the editor gaps that still matter for a micro-IDE

The biggest remaining editor-facing gaps are:

- soft wrap
- explicit large-file behavior validation and, if needed, large-file mode heuristics
- recent project or recent file affordances if the current typed-path workflow proves too thin
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

- project search behavior, especially fallback behavior without `rg`
- session restore and multi-project workflow coverage
- tree mutation flows
- compare and merge workflow coverage beyond the current model-level tests
- terminal behavior checks where deterministic harnesses are practical

### Performance

The highest-value remaining performance work is:

- glyph-atlas style text rendering work
- dirty-rect and caret-only invalidation
- highlighted-text sharpness cleanup
- startup and idle redraw profiling driven by `docs/startup-tracing.md`

## Out of Scope

These are not roadmap items for the current product:

- debugging
- plugin runtimes or marketplaces
- cloud sync and collaboration
- built-in chat surfaces
