# MicroIDE TODO

This file tracks the verified current state of the SDL rewrite.
It is not a design document.

Revalidated on 2026-04-09 by reading the current `src/*` and `tests/*` implementation together
with the remaining docs in this directory.

## Status Rules

- `[x]` verified present in this repository
- `[~]` implemented but still clearly partial or rough
- `[ ]` missing or still needs dedicated work
- `[-]` intentionally out of scope

## Verified Implemented Baseline

These are done and should not be treated as open migration work:

- `[x]` standalone CMake project with SDL3 app bootstrap
- `[x]` event-driven rendering loop without a forced continuous redraw
- `[x]` custom in-window menu bar, project tabs, file tabs, breadcrumb row, sidebar, and docked terminal pane
- `[x]` project-local workspace state below the project-tab strip, plus app-level restore of open project tabs
- `[x]` editor open, save, reopen, selection, clipboard, undo/redo, line numbers, horizontal scrolling, and dirty-state tracking
- `[x]` nested shared-buffer editor splits with focus movement and divider drag
- `[x]` UTF-8 text input across editor and built-in prompt surfaces, plus IME preedit rendering hooks
- `[x]` project file tree with `.gitignore` handling, git markers, keyboard and mouse navigation, and refresh
- `[x]` tree context menus plus create, rename, delete, and trash-backed file operations
- `[x]` project file finder overlay backed by a cached file index
- `[x]` persistent sidebar project search with result grouping, rerun/edit flow, and literal replace-in-project
- `[x]` compare tabs from tree, git sidebar, and commit picker workflows
- `[x]` three-way merge tabs for conflicts and manual merge inputs
- `[x]` git sidebar for working-tree changes, conflicts, outgoing files, stage, unstage, discard, and refresh
- `[x]` PTY-backed terminal tabs with scrollback, selection/copy/paste, application cursor-key mode, origin mode, autowrap control, bracketed paste mode, basic device/cursor query replies, charset-designation escape handling, common ANSI handling, and alternate-screen support
- `[x]` runtime syntax highlighting from an in-tree generated snapshot of the old syntax assets
- `[x]` project-local editor preferences and colorscheme persistence
- `[x]` project-local session restore for editor, compare, and merge tabs plus core workspace chrome
- `[x]` app-level user config for UI scale
- `[x]` fixture-driven tests for compare, merge, git services, file operations, and workspace shared helpers

## Remaining Work

### Search

- `[x]` async project search now uses one built-in PCRE2-backed backend and no longer depends on `rg` being installed
- `[x]` project search exposes literal or regex mode, explicit case mode, and hidden-file inclusion from the sidebar
- `[x]` sidebar and overlay project search now report when results are capped to the first 200 matches
- `[~]` replace-in-project is intentionally limited to literal search mode; regex-aware replace is still out of scope
- `[x]` project-search backend coverage now includes literal mode, regex mode, case controls, hidden files, invalid patterns, binary skipping, stable result ordering, capped result sets, and latest-run restart behavior
- `[x]` workspace search coverage now checks option-driven reruns for pattern mode, case mode, hidden files, and truncation reset
- `[x]` `microide_search_bench` provides repeatable larger-repo timing runs for the built-in search backend

### Tree And Project Workflows

- `[x]` create, rename, and delete flows now target only the affected editors inside split tabs while preserving compare and merge tab state
- `[x]` compare tabs now preserve historical commit-side paths across working-tree renames and restore correctly from session state after those renames
- `[x]` open-project now uses a native folder picker from the File menu and bare `project-open`, while `project-open [path]` still works as a direct fallback
- `[x]` the project tree now allows collapsing a directory even when it contains the currently open file, and selecting that open file tab again re-expands its ancestors in the tree
- `[-]` recent files and recent projects

### Terminal

- `[~]` the terminal is useful for embedded shell work, including alternate-screen scroll regions, explicit scroll-up/scroll-down sequences, reverse-index scrolling, region-aware line insert/delete, application cursor-key mode, origin mode, autowrap control, bracketed paste, basic device/cursor query replies, and charset-designation escape handling for common full-screen apps, but it is still not a full terminal emulator
- `[ ]` broaden real-world terminal validation and fill the remaining ANSI or control-sequence gaps that matter in practice
- `[x]` terminal-tab context menus now support copying the last command plus its rendered output, and they fall back to copying only the invoked command while full-screen apps own the alternate screen

### Editor

- `[~]` UTF-8 entry and IME preedit rendering exist, but the underlying text model is still byte-oriented
- `[~]` editor large-file mode now disables syntax highlighting above size or line-count thresholds and reevaluates as edits or undo cross those thresholds, but the thresholds still need more validation on real repositories
- `[x]` editor copy-with-context is available from the Edit menu and right-click editor popup, and it prefixes the selected text with a relative path plus line or line-range header
- `[~]` per-line git blame shadow text now renders for tracked non-large on-disk files, including saved but uncommitted content, through the async viewport-scoped cache described in `docs/blame-shadow-text.md`, but broader repo validation and any policy expansion still need work
- `[-]` soft wrap
- `[ ]` diagnostics or problem styling only if a diagnostics phase is intentionally started

### Git And Project Services

- `[x]` compare, merge, git status, outgoing-file views, and stage or discard flows are all present
- `[x]` the source-control sidebar now includes `Stage all` and `Discard all` buttons
- `[x]` bulk discard now uses an explicit confirmation flow, and `Discard all` covers tracked edits, untracked files, and conflicted entries
- `[-]` git services still shell out to the system `git`; error handling, portability, and async behavior can improve

### Workspace Chrome

- `[x]` project tabs, file tabs, and terminal tabs can be reordered by dragging within their existing strip, without creating detached windows

### Menus And Prompts

- `[x]` the shared action model, menu bar, tree context menus, and prompt surfaces exist
- `[~]` menu coverage and prompt reuse can still be polished, but the core architecture is in place

### Rendering And Performance

- `[ ]` glyph-atlas style text rendering backend
- `[ ]` dirty-rect invalidation
- `[ ]` caret-only invalidation without wider redraw
- `[~]` highlighted editor fragments still fall back to the older blended text path
- `[ ]` broader low-idle-CPU and redraw profiling

### Testing

- `[~]` editor-core coverage now checks large-file mode on open plus edit-time threshold crossings and undo, but broader text editing primitives and undo/redo still need dedicated tests
- `[x]` project-search coverage now checks capped-result handling, stable ordering, latest-run restart behavior, and workspace option reruns
- `[~]` terminal-session coverage now checks alternate-screen scroll regions, explicit scroll-up/scroll-down sequences, reverse index, region-aware line insert/delete, application cursor-key mode, origin mode, autowrap control, bracketed paste behavior, device/cursor query replies, and charset-designation escape handling; workspace coverage now checks terminal paste shortcuts, DECCKM-aware navigation keys, and `Esc` delivery, but broader real-app validation is still needed
- `[~]` project-open workflow tests now cover native picker launch, selection, cancellation, and menu fallback behavior
- `[~]` multi-project restore and workspace-session tests now cover active-project restore plus compare-session reopen, but broader scenarios still need more validation
- `[~]` tree-mutation workflow tests now cover dirty rename/delete prompts, split-editor consequences, compare-path preservation across renames, and renamed working-tree compare session restore, but create flows and broader directory workflows still need more validation
- `[x]` source-control bulk-action coverage now checks bulk stage/discard services plus the confirmed workspace discard-all flow
- `[x]` tab-reordering coverage now checks project, file, and terminal drag reordering
- `[x]` context-copy coverage now checks editor selection formatting, the editor right-click menu, terminal last-command transcript copying, alternate-screen fallback behavior, and the terminal tab context-menu entry point
- `[~]` blame coverage now exercises the incremental parser, cache invalidation, dirty-buffer suppression, viewport-window loading, and narrow-pane suppression, but broader repo-scale validation is still missing
- `[ ]` broader compare and merge workflow tests

## Intentional Scope Cuts

- `[-]` debugging
- `[-]` plugin runtimes, plugin marketplaces, and Micro-plugin compatibility
- `[-]` cloud, collaboration, account, or assistant surfaces

## Notes

- If a feature is intentionally dropped, mark it `[-]` instead of quietly removing it.
- Keep this file tied to verified source state rather than aspirational planning.
- Put forward-looking design and prioritization in `docs/roadmap.md`, not here.
