# MicroIDE Migration TODO

This document tracks what is still missing from the legacy Micro IDE fork behavior in the C++/SDL rewrite.

The original `micro/` Go fork is no longer vendored in this repository after the repo-root flattening.
Historical `micro/...` paths below are retained only as audit anchors from the earlier migration review.

It was revalidated against the current repo-root source on 2026-04-07 by:

- reading `docs/implementation-guide.md`
- reading `docs/agent-handoff.md`
- reading the current `src/*` implementation
- comparing against the previously audited IDE-specific Go code in `micro/internal/action/*`
- confirming `cmake -S . -B build/microide`
- confirming `cmake --build build/microide`
- confirming a short dummy launch with `timeout 2s env SDL_VIDEODRIVER=dummy ./build/microide/microide`

## Status Rules

- `[x]` verified present in this repository
- `[~]` partially implemented, but not yet at the intended parity or polish level
- `[ ]` still missing from the migration
- `[-]` intentionally out of scope for this rewrite

## Scope Baseline

The migration target is the IDE-oriented legacy Micro fork behavior, not stock upstream micro.

The most important reference points are:

- `micro/internal/action/workspace.go`
- `micro/internal/action/project.go`
- `micro/internal/action/treepane.go`
- `micro/internal/action/filefinder.go`
- `micro/internal/action/searchpane.go`
- `micro/internal/action/gitcomparepicker.go`
- `micro/internal/action/diffcomparepane.go`
- `micro/internal/action/termpane.go`
- `micro/runtime/help/commands.md`
- `docs/implementation-guide.md`

## Verified Baseline

These migration pieces are already present and should not be tracked as missing:

- `[x]` standalone root CMake project exists and builds
- `[x]` SDL3 window bootstrap exists
- `[x]` event-driven main loop exists without a forced continuous repaint loop
- `[x]` top tab strip exists
- `[x]` persistent left sidebar region exists
- `[x]` breadcrumb/header row exists
- `[x]` bottom panel host exists
- `[x]` status bar exists
- `[x]` centered overlay host exists
- `[x]` real file open/save works
- `[x]` editable text viewport exists
- `[x]` line/column movement, page movement, `Home`/`End`, `Ctrl+Home`/`Ctrl+End`
- `[x]` selection, select-all, mouse caret placement, and mouse drag selection
- `[x]` clipboard copy/cut/paste
- `[x]` undo/redo
- `[x]` line-number gutter
- `[x]` dirty-state tracking in the editor/status text
- `[x]` editor save/load now preserves detected file line endings and surfaces file format state in the status bar
- `[x]` editor tabs retain per-buffer state, including unsaved edits, cursor position, and undo history, across tab switches
- `[x]` horizontal scrolling and tab-aware visual column handling
- `[x]` tabs can open, activate, close, and avoid duplicate opens for the same file
- `[x]` dirty-tab markers in the tab strip
- `[x]` save/discard/cancel confirmation for dirty tab close and app shutdown
- `[x]` tab overflow scrolling exists
- `[x]` switching tabs now reveals and selects the corresponding file in the tree
- `[x]` filesystem-backed tree exists with expand/collapse, keyboard navigation, mouse activation, refresh, and scrolling
- `[x]` mouse-resizable sidebar and bottom panel
- `[x]` project-wide filename finder overlay exists with cached file indexing and ranked matching
- `[x]` in-buffer search exists
- `[x]` in-buffer replace current / replace all exists
- `[x]` project-wide content search overlay exists
- `[x]` command prompt exists in the bottom panel
- `[x]` bottom-panel terminal tabs now keep separate live PTY sessions, and closed or exited terminals are forgotten entirely
- `[x]` project-local editor preferences now persist tab size, indent width, and soft-tabs
- `[~]` command prompt now has quoted-argument parsing, per-session history recall, and first-pass Tab completion
- `[x]` visible scrollbars exist for the main editor, compare view, sidebar tools, bottom logs, and centered overlays
- `[x]` current commands: `help`, `open`, `tab`, `tabswitch`, `tabmove`, `compare`, `reopen`, `save`, `quit`, `term`, `find`, `files`, `tree`, `search`, `grep`, `rg`, `goto`, `jump`, `vsplit`, `hsplit`, `unsplit`, `split-next`, `split-prev`, `split-first`, `split-last`, `tree-refresh`, `sidebar-toggle`, `sidebar-show`, `sidebar-hide`, `sidebar-close`, `sidebar-width`, `panel-show`, `panel-hide`, `focus`, `tab-size`, `indent-width`, `soft-tabs`, `colorscheme`
- `[x]` pluggable text renderer exists
- `[x]` debug-text backend exists
- `[x]` optional `SDL3_ttf` backend exists
- `[x]` runtime font discovery exists
- `[x]` bundled fallback font exists in `assets/fonts/JetBrainsMono-Regular.ttf`
- `[x]` shell theme/base visual direction exists
- `[x]` standalone runtime-syntax highlighting now exists through an in-tree generated snapshot plus a PCRE2-backed region/pattern engine, with distinct colors for keywords, types, strings, comments, numbers, constants, preprocessor directives, and operators
- `[x]` sharper LCD/subpixel text rendering exists on stable background surfaces

## Intentional Scope Cuts

- `[-]` Lua plugin runtime
- `[-]` plugin manager and plugin install/update flows
- `[-]` plugin-defined commands and runtime extension APIs
- `[-]` backward compatibility for Micro plugins
- `[-]` extension marketplace, cloud, collaboration, or account features

## Remaining Migration Gaps

### 1. Workspace Model And Sidebar Parity

- `[x]` sidebar tool model parity from `workspace.go`
- `[x]` persistent search sidebar, not just a centered overlay
- `[x]` temporary sidebar replacement with restoration of the previous sidebar tool
- `[x]` sidebar close command parity (`sidebar-close`)
- `[x]` sidebar width command/session behavior parity (`sidebar-width`)
- `[x]` sidebar root/tool switching parity with the original `tree`, `files`, and `sidebar-show` flows
- `[x]` project-tab strip above file tabs
- `[x]` project-local workspace state so tabs, tree/search state, overlays, command prompt state, logs, panel/terminal state, colorscheme, and editor preferences move with the active project tab instead of living in one global `WorkspaceShell`
- `[x]` explicit open/close/switch project flows that do not mutate the only live project root in place
- `[~]` a user-facing open-project affordance now exists through `project-open <path>` in the command prompt; menu and tree-driven entrypoints are still pending

### 2. Project Service Parity

- `[-]` automatic project root detection removed by design; startup now uses the launch working directory
- `[x]` `.gitignore` handling parity in project file enumeration
- `[x]` ignore matcher parity beyond hidden-file skipping
- `[ ]` shared project service boundary closer to `project.go` instead of UI-driven refreshes
- `[x]` git status collection service
- `[x]` async/cancellable project search jobs closer to the original ripgrep-backed behavior

### 3. Tree And Git Integration

- `[x]` git-aware tree markers for modified, added, deleted, and untracked files
- `[x]` tree selection follows the active file when switching tabs
- `[x]` tree command parity with `tree ['root']`
- `[x]` file-finder command parity with `files ['root']`
- `[x]` tree-driven compare entry point (`d` on a file in the tree)
- `[~]` tree context menus now exist for files, directories, the project root, and empty tree background, including first-pass create/rename/delete actions plus the earlier non-mutating actions
- `[~]` direct tree/menu compare action for `Compare Against HEAD` now exists through the file context menu; the main Project menu shortcut is still pending
- `[~]` tree/menu compare action for `Compare Against...` now exists through the file context menu and reuses the existing picker flow; the main Project menu entry is still pending
- `[~]` rename/create/delete flows now exist in the tree through a reusable prompt surface, with compare-tab retarget/close handling and conservative dirty-tab blocking; save/discard mutation prompts and finer split-tab handling are still pending
- `[x]` delete flows now move files and directories to the OS trash/recycle bin on Linux/macOS instead of permanently removing them

### 4. Search Workflow Parity

- `[x]` persistent project search sidebar from `searchpane.go`
- `[x]` temporary `rg`-style search behavior that restores the previous sidebar tool
- `[x]` search rerun/edit workflow parity (`r` and `/` style interaction in the original search pane)
- `[x]` project search result list behavior closer to the original sidebar search pane
- `[x]` replace-in-project
- `[ ]` richer search options if parity with the accepted workflow still requires them

### 5. Compare Workflow

- `[x]` commit picker overlay
- `[x]` compare workflow launched from the tree
- `[x]` compare tab in the normal tab model
- `[x]` side-by-side compare editor
- `[x]` aligned diff rows / hunks
- `[x]` next/previous hunk navigation
- `[x]` open the working-tree file at the matching line from compare
- `[x]` compare-specific syntax coloring
- `[x]` behavior parity with `gitcomparepicker.go` and `diffcomparepane.go`
- `[ ]` direct `HEAD` compare shortcut/action alongside the existing picker-based compare flow

### 6. Terminal Pane

- `[x]` PTY-backed terminal process management
- `[~]` terminal rendering now handles basic ANSI SGR, common CSI cursor/erase/edit sequences, cursor visibility, plus alternate-screen switching/save-restore flows, but it is still not a full terminal emulator
- `[x]` terminal keyboard input handling
- `[x]` bottom-panel terminal tabs with fresh-session launches and auto-removal on terminal exit
- `[~]` terminal mouse text selection/copy and xterm-style app mouse reporting now exist, but terminal interaction is still not full emulator parity
- `[x]` terminal scrollback
- `[x]` terminal command parity (`term`)
- `[x]` terminal placement integrated into the workspace model

### 7. Editor Capability Gaps

- `[x]` split editors inside tabs now support nested shared-buffer split trees with mouse focus switching, divider drag, and command-driven creation
- `[x]` split tree/focus model
- `[~]` visible scrollbars for editor, tree/search, compare, logs, and centered overlays; overlay result drawing is still fixed-row rather than virtualized
- `[x]` mouse-resizable sidebar and bottom panel
- `[~]` UTF-8 and non-ASCII text input now works across the editor, command/search/finder/sidebar-edit fields, and the terminal via SDL text input, with codepoint-aware cursor motion plus backspace/delete in the editor; IME/preedit validation is still pending
- `[~]` IME/input-method composition rendering and text-input-area anchoring now exist for the editor and built-in text prompts, but real IME/manual validation is still pending
- `[x]` configurable tab size, indent width, and soft-tabs settings with project-local persistence
- `[x]` CRLF/LF handling surfaced in the UI
- `[x]` encoding detection and status reporting
- `[ ]` soft wrap
- `[ ]` large-file behavior validation against the original editor

### 8. Syntax And Theme Migration

- `[x]` standalone runtime syntax snapshot now drives syntax highlighting, including multiline regions, nested includes, filetype detection, and compare-view/editor state carry-over
- `[x]` reuse or conversion of `micro/runtime/syntax/*`
- `[x]` vendored `micro/runtime/colorschemes/*` assets now load through `assets/themes/*` plus a runtime colorscheme parser/mapping layer for the SDL shell
- `[ ]` diagnostics underline / problem styling

### 9. Command Parity Gaps

The current command set is much smaller than the original IDE fork.
The command bar now has shell-style quoted/escaped parsing, history recall, and first-pass completion, but it is still simpler than the original prompt buffer.

- `[~]` command parser parity beyond simple space-splitting
- `[x]` quoted argument handling closer to the original command bar
- `[~]` command history / completion
- `[x]` `quit`
- `[x]` `reopen`
- `[x]` `tree ['root']`
- `[x]` `files ['root']`
- `[x]` `rg 'query'`
- `[x]` split editor commands now cover nested split-tree creation and split focus movement via `vsplit`, `hsplit`, `unsplit`, `split-next`, `split-prev`, `split-first`, and `split-last`
- `[x]` `sidebar-close`
- `[x]` `sidebar-width 'n'`
- `[x]` `term ['exec']`
- `[x]` compare-related command entry now exists through `compare [path] [commit-prefix]`

### 9A. Menu And Action Surfaces

- `[~]` shared action registry now backs the command bar, the core keyboard shortcuts, and the first menu-bar dispatch path, and exposes action metadata plus enablement hooks for upcoming context menus
- `[~]` custom top menu bar now exists with click-driven menus, context-aware enablement, and accelerator labels; broader menu coverage and keyboard polish are still pending
- `[~]` reusable popup menu surfaces now back both the menu bar and the tree context menus, and a first reusable prompt surface now covers tree create/rename/delete confirmation flows; open-project prompting and broader prompt reuse are still pending

### 10. Persistence And Config

- `[~]` config loading now exists for project-local editor preferences plus colorscheme selection, and initial app-level user config now restores UI scale, but broader user settings are still missing
- `[~]` config save/update now exists for the same project-local editor preferences plus colorscheme selection, and initial app-level user config now persists UI scale, but broader user settings are still missing
- `[x]` session restore for open tabs now restores editor tabs and compare tabs from a project-local session file
- `[~]` session restore for sidebar visibility/width now exists through the same project-local session file
- `[~]` persistent workspace state across launches now covers editor tabs, compare tabs, sidebar/bottom-panel visibility and sizing, plus project-local editor preferences and colorscheme selection, but not terminal session restore or broader app config
- `[x]` app-level workspace session for open project tabs
- `[ ]` recent files/projects, if still desired

### 11. Rendering And Performance Work Still Missing

These are real implementation gaps in the current SDL app, but they are architecture/performance work rather than direct user-facing parity:

- `[ ]` glyph atlas backend
- `[x]` bounded visible-line layout and syntax-token caching now exists for the main editor viewport
- `[ ]` dirty-rect invalidation
- `[ ]` caret blink invalidation without wider redraw
- `[~]` sharper `SDL3_ttf` text path exists on stable backgrounds, but highlighted editor fragments still fall back to the older blended path
- `[ ]` audit broad synchronous filesystem work from UI paths
- `[ ]` low-idle-CPU verification and redraw profiling

### 12. Validation And Testing

- `[x]` generated fixture corpus now exists under `tests/fixtures` for large-file, syntax, and diff scenarios
- `[x]` first automated tests now exist for `CompareModel` fixtures and temporary-git `GitCompareService` coverage
- `[ ]` unit tests for text editing primitives
- `[ ]` unit tests for undo/redo
- `[ ]` unit tests for search/replace
- `[ ]` unit tests for filename ranking
- `[ ]` integration tests for open/edit/save
- `[ ]` integration tests for sidebar persistence across tab switches
- `[ ]` integration tests for file-finder tab reuse
- `[ ]` integration tests for project search result opening
- `[ ]` compare workflow tests

## Highest-Value Remaining Work

These are the biggest migration blockers relative to the original IDE fork:

- `[ ]` project-tab workspace extraction
- `[~]` menu and context-menu action model
- `[~]` integrated terminal pane
- `[~]` config/session persistence
- `[ ]` richer project-search options
- `[~]` remaining command/workspace parity is now mostly centered on broader terminal emulation and deeper completion behavior

## Current QoL Gaps

These are not all migration blockers, but they are the main user-facing polish gaps still visible in the current build:

- `[~]` visible scrollbars exist across the main workspace, but centered overlays still use fixed-row rendering rather than virtualization
- `[x]` sidebar and bottom panel sizes can be changed by direct mouse drag
- `[ ]` text sharpness improvements do not yet cover every highlighted editor case

## Notes

- If a feature is intentionally dropped, change it to `[-]` instead of deleting it.
- If a feature is redesigned, keep the item but update the wording to the accepted replacement behavior.
- Keep this file tied to verified source state rather than aspirational plans.
- Use `[~]` when the implementation exists but is still clearly partial.
