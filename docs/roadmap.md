# MicroIDE Roadmap

This document is forward-looking.
`docs/todo.md` remains the source of truth for what is implemented today.

## Reviewed Code Anchors

This roadmap was rechecked against the current codebase on 2026-04-09 with emphasis on:

- `src/project/ProjectSearchService.cpp`
- `src/project/FileIndex.cpp`
- `src/project/DirectoryTree.cpp`
- `src/project/GitStatusService.cpp`
- `src/project/GitCompareService.cpp`
- `src/project/FileOperationService.cpp`
- `src/workspace/WorkspaceShell.cpp`
- `src/workspace/WorkspaceShellCompare.cpp`
- `src/workspace/WorkspaceShellSidebar.cpp`
- `src/workspace/WorkspaceShellOverlay.cpp`
- `src/workspace/WorkspaceShellMenu.cpp`
- `src/workspace/WorkspaceShellPrompts.cpp`
- `src/workspace/WorkspaceShellPersistence.cpp`
- `src/terminal/TerminalSession.cpp`
- `src/editor/TextViewport.cpp`
- `tests/*`

## Near-Term Priorities

### 1. Built-in project search is now in good enough shape

The backend unification and polish pass are done: project search no longer depends on `rg`
being installed, and it no longer blocks other roadmap work.

Current state:

- `ProjectSearchService` now uses one built-in PCRE2-backed engine
- the sidebar now exposes literal or regex mode, explicit case mode, and hidden-file inclusion
- literal mode avoids regex compilation for common text searches
- file enumeration stays aligned with project ignore handling
- capped searches now report that only the first 200 matches are shown
- backend coverage now checks stable ordering, latest-run restart behavior, capped result sets,
  and workspace-driven option reruns
- `microide_search_bench` now provides repeatable larger-repo timing runs outside the UI

Search-specific future work is now optional scope expansion rather than a blocking priority:

- keep regex-aware replace out of scope unless it is deliberately started as a separate phase
- rerun `microide_search_bench` before adding more search UI or search syntax

### 2. Finish terminal polish without turning it into a second product

The terminal is already useful, but it is still only a partial emulator.

Current state:

- PTY tabs, scrollback, keyboard input, selection/copy, and alternate-screen handling exist
- cursor-key input now switches between normal CSI and application SS3 sequences when terminal apps request DECCKM
- origin mode now rebases cursor addressing to the active scroll region when terminal apps request DECOM
- autowrap mode control and explicit `CSI S` / `CSI T` scrolling now work for alternate-screen apps
- terminal paste shortcuts now honor bracketed paste mode when terminal apps request it
- basic device-attribute and cursor-position queries now receive terminal replies
- charset-designation escapes such as `ESC(B` no longer leak selector bytes into terminal output
- common ANSI cursor, erase, edit, and scroll-region sequences work
- app mouse reporting exists for mouse-aware terminal programs

Remaining work:

- broaden ANSI and control-sequence coverage where real tools still break
- keep resize, redraw, and scrollback behavior robust under long-running output
- run more real-app validation instead of guessing from escape-sequence lists

The goal is a dependable embedded terminal, not a separate terminal-emulator project.

### 3. Tree and project workflows are now in good enough shape

The main tree-mutation sharp edges have been closed, and this area no longer blocks higher-value
work.

Current state:

- tree context menus and prompt surfaces exist
- rename retargets affected tabs and delete closes affected tabs
- dirty rename and delete now prompt to save or discard affected editors instead of silently discarding work
- split editor tabs now keep unaffected panes open during rename or delete flows instead of treating the whole tab as disposable
- compare tabs now preserve their historical commit-side paths across working-tree renames instead of silently degrading into empty historical sides
- renamed working-tree compare tabs now restore with the correct live path and historical ref paths from session state
- project open now prefers a native folder picker from the File menu and bare `project-open`, while `project-open [path]` remains available as a direct fallback

### 4. Fill the editor gaps that still matter for a micro-IDE

The biggest remaining editor-facing gaps are:

- explicit threshold tuning for editor large-file mode after the new edit-time reevaluation path
- continued UTF-8 and IME hardening while the buffer model is still byte-oriented
- editor copy-with-context for LLM-heavy workflows, starting as a context-menu action that copies
  `relative/path:line` or `relative/path:start-end` plus the selected text
- per-line git blame shadow text only if it is implemented asynchronously, cached aggressively,
  scoped to visible lines, and kept off the hot path for typing, scrolling, and repaint

Diagnostics and problem styling belong here only if a diagnostics phase is intentionally started.

The blame item is performance-sensitive enough to deserve a hard constraint: no synchronous
`git blame` work on cursor movement, viewport scrolling, or normal editor redraws.

### 5. Polish workspace and source-control ergonomics

These are small in surface area but high in day-to-day value.

Current state:

- the source-control sidebar now supports single-file stage, discard, compare, and merge flows plus bulk `Stage all` and confirmed `Discard all`
- project, file, and terminal tabs are stable but fixed-order once opened
- tree path selection expands ancestors when targeting a path directly
- copy operations today are plain text only

Remaining work:

- support reordering project tabs, file tabs, and terminal tabs
- allow collapsing a tree directory even when it contains the currently open or selected file
- if that file is explicitly selected from the tree again later, auto-expand its ancestors again
- add terminal copy-with-context for the last command plus its rendered output
- for full-screen or non-persistent terminal apps such as `vim` or `htop`, fall back to copying
  just the invoked command when no durable output exists in the normal terminal transcript

### 6. Harden project services

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

- broaden session restore and multi-project workflow coverage
- tree mutation flows
- source-control bulk actions and their confirmation semantics
- tab reordering across project, file, and terminal strips
- context-copy formatting for editor selections and terminal command transcripts
- blame-cache invalidation and viewport-scoped loading behavior if blame is started
- compare and merge workflow coverage beyond the current model-level tests
- terminal behavior checks where deterministic harnesses are practical

### Performance

The highest-value remaining performance work is:

- glyph-atlas style text rendering work
- dirty-rect and caret-only invalidation
- replace the current UI zoom path, which stretches a reduced logical framebuffer and blurs text, with a proper HiDPI scaling model that keeps layout and font rendering sharp at non-100% scales
- preserve typing and scrolling latency if blame shadow text is added by keeping blame collection
  asynchronous and out of the normal repaint path
- startup and idle redraw profiling driven by `docs/startup-tracing.md`

## Out of Scope

These are not roadmap items for the current product:

- debugging
- plugin runtimes or marketplaces
- cloud sync and collaboration
- built-in chat surfaces
- recent project or recent file affordances
- soft wrap
