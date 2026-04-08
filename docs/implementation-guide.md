# MicroIDE C++/SDL Rewrite Implementation Guide

## Goal

Rewrite this modified `micro`-based IDE from Go/TUI into a lean C++ desktop application with an SDL GUI, while preserving the IDE behavior that already exists in this fork.

The GUI design direction should be explicitly based on Zed's desktop UI language, minus the features this project does not need.

The rewrite is performance-sensitive by design. When tradeoffs conflict, use this order:

1. startup speed
2. low idle CPU usage
3. low memory footprint

After those, the rewrite should prioritize:

- fast startup
- low idle CPU usage
- low memory footprint
- low input latency
- simple architecture
- a compact, dense desktop GUI similar to Zed
- native-feeling keyboard and mouse workflows
- feature parity with the current built-in IDE shell

Plugin support can be removed.

## What Must Be Preserved From This Fork

This repo is not just stock `micro`. The current fork already has IDE-specific behavior that the rewrite should keep:

- global workspace shell above editor tabs
- persistent left sidebar that survives tab switches
- file tree as a built-in global tool
- project-wide filename finder overlay
- project-wide content search
- tabbed editor area on the right
- split editors inside tabs
- command bar driven workflows
- integrated terminal pane
- git-aware file tree markers
- git commit picker from the tree
- source-control sidebar mode for working-tree changes and outgoing branch files
- side-by-side compare view with aligned diff hunks

The current behavior was originally defined mostly in these Go areas from the now-removed vendored legacy fork:

- `micro/internal/action/workspace.go`
- `micro/internal/action/project.go`
- `micro/internal/action/treepane.go`
- `micro/internal/action/filefinder.go`
- `micro/internal/action/searchpane.go`
- `micro/internal/action/gitcomparepicker.go`
- `micro/internal/action/diffcomparepane.go`
- `micro/internal/action/termpane.go`
- `micro/runtime/help/commands.md`
- `micro/runtime/help/tutorial.md`

## Recommended Rewrite Strategy

Do not try to transliterate Go files into C++ one by one.

That would preserve implementation accidents from the terminal version and make the SDL port harder. The correct move is:

1. capture the legacy Go behavior reference in documentation or an external checkout
2. build the new product at the repo root
3. implement feature parity by subsystem
4. carry over reusable assets and workflows, not the old rendering model

## Suggested Product Shape

The new application should be a single-window IDE with these visible regions:

- top menu bar
- project-tab strip
- file-tab strip
- left sidebar
- right editor workspace
- bottom panel host
- status bar
- centered transient overlays for file finder, command palette, and similar tools

This mirrors the current fork closely, but removes the constraints imposed by a TUI screen grid.

The current implementation already matches the single-project subset of this shell.
The next planned expansion is a multi-project workspace layer plus menu/context-menu surfaces.
Detailed design notes for that expansion now live in `docs/workspace-expansion-plan.md`.
Startup tracing instructions now live in `docs/startup-tracing.md`.

## Visual Direction: Zed-Inspired, Reduced Scope

The visual and interaction target should be close to the Zed screenshot provided by the user, but deliberately reduced to the subset this project needs.

Preserve from that direction:

- dark, low-glare graphite UI chrome
- compact tab strip with dense horizontal layout
- persistent left project sidebar
- editor-first layout with very little decorative chrome
- subtle separators instead of thick boxed panels
- integrated bottom panel for terminal or status output when needed
- centered overlays for finder and command workflows
- strong emphasis on content density over oversized controls

Do not copy blindly:

- real-time collaboration
- AI/chat surfaces
- extension marketplace
- account or cloud features
- complex git staging dashboards
- multi-pane tool ecosystems that hurt clarity or CPU usage

The goal is not "clone Zed." The goal is "use Zed as the visual baseline for density, hierarchy, and polish, while keeping the product much smaller."

## Concrete GUI Layout Target

Use this layout as the default shell:

- top menu bar
- project tabs above file tabs
- compact file-tab strip scoped to the active project
- left sidebar host for tree and search tools
- left sidebar host for tree, search, and source-control tools
- central editor region with breadcrumbs/path header
- bottom panel for logs, command entry, and terminal tabs
- minimal bottom status bar for mode, file state, encoding, line endings, and cursor position

Recommended behavior:

- the tree stays visible while switching tabs
- the sidebar host can switch between tree, search, and git/source-control views without changing the main layout
- the source-control view should expose an explicit manual refresh control because git state can change outside the app
- switching project tabs swaps the entire workspace context below the project-tab strip
- the bottom panel can be shown or hidden without disturbing the main editor layout
- overlays such as file finder and command palette open centered above the editor
- top chrome, sidebar, and bottom panel sizing should feel tight and information-dense, not padded

## First Milestone Wireframe

The first SDL milestone should implement this shell, even before the full editor feature set is complete.

### Primary Window Wireframe

```text
+--------------------------------------------------------------------------------------------------+
| Menu: File  Edit  View  Search  Project  Terminal  Help                                            |
+--------------------------------------------------------------------------------------------------+
| Projects: [microide] [landing] [docs]                                                   [+] [x] |
+--------------------------------------------------------------------------------------------------+
| Files: [main.cpp] [project_service.cpp] [compare]                                                |
+--------------------------------------------------------------------------------------------------+
| Sidebar                              | Breadcrumb / Path Header                                  |
| src/                                 +------------------------------------------------------------+
|   app/                               |  Line numbers | Editor viewport                           |
|   project/                           |  and gutter   |                                            |
|   render/                            |               |                                            |
|   workspace/                         |               |                                            |
| docs/                                |               |                                            |
|                                      |               |                                            |
|                                      |               |                                            |
|                                      |               |                                            |
+--------------------------------------+------------------------------------------------------------+
| Bottom Panel: logs, command output, search results now; terminal later                           |
+--------------------------------------------------------------------------------------------------+
| Status Bar: file state | mode | encoding | line endings | cursor position | notifications         |
+--------------------------------------------------------------------------------------------------+
```

### Overlay Wireframe

File finder and command workflows should open as centered overlays above the editor, not as sidebars and not as separate dialogs.

```text
                     +------------------------------------------------------+
                     | Find File                                            |
                     | > workspace                                           |
                     |                                                      |
                     | src/workspace/workspace.cpp                          |
                     | src/workspace/sidebar_host.cpp                       |
                     | src/workspace/tab_strip.cpp                          |
                     | docs/implementation-guide.md                         |
                     +------------------------------------------------------+
```

### Compare Tab Wireframe

The compare flow should reuse the main tab strip and editor region rather than opening a detached tool window.

```text
+--------------------------------------------------------------------------------------------------+
| Tabs: [main.cpp] [workspace.cpp] [compare: project.go]                                           |
+--------------------------------------------------------------------------------------------------+
| Sidebar                              | Breadcrumb: project.go  Compare: HEAD~3 <-> Working Tree  |
+--------------------------------------+------------------------------------------------------------+
| Tree / Search                        | left gutter | old revision | new revision | right gutter  |
|                                      |             |              |              |               |
|                                      |             | aligned diff rows with syntax colors         |
|                                      |             |                                            |
+--------------------------------------+------------------------------------------------------------+
| Bottom Panel                                                                        Hidden/Shown |
+--------------------------------------------------------------------------------------------------+
```

## Layout Metrics And Density Rules

These numbers are targets, not rigid constants, but they should keep the UI close to the desired Zed-like density:

- menu bar height: 24-26 px
- project-tab strip height: 28-30 px
- file-tab strip height: 30-36 px
- breadcrumb/header row height: 24-28 px
- sidebar width default: 260-320 px
- tree row height: 20-24 px
- bottom panel default height: 160-220 px
- status bar height: 20-24 px
- panel padding should stay tight, usually 6-10 px
- dividers should be 1 px visual separators, not thick framed borders

## Layout Decisions Captured

These are now fixed unless changed later:

- use a single sidebar surface, not a Zed-like activity strip plus sidebar
- include the bottom panel in the first GUI milestone
- keep the initial theme visually close to the provided Zed screenshot
- keep the launch working directory as the project root; do not reintroduce automatic project-root detection or hardcoded workspace markers
- when project tabs land, they sit above file tabs and switch the entire lower workspace context
- use simple chevrons and text cues in the tree, not pictorial folder/file icons
- defer SCM indicators in the tree until after the core shell is stable

## Interaction Notes For The Wireframe

The wireframe implies these behavior rules:

- sidebar is persistent and does not remount on tab switch
- breadcrumb/header reflects the active editor or compare view
- bottom panel can show logs, command output, and search summaries before terminal support exists
- centered overlays always return focus cleanly to the previous editor or sidebar target
- compare is a normal tab with a specialized editor surface
- no floating utility windows in the first milestone

## Planned Workspace Expansion

The next accepted shell expansion adds:

- a compact custom menu bar backed by shared actions
- a project-tab strip above file tabs
- project-scoped workspace state so everything currently visible below the project-tab strip, including tabs, search, tree state, overlays, command prompt state, panel state, logs, and terminals, moves with the active project tab
- project-local colorscheme state, including a persisted per-project base accent color, so switching project tabs can also switch the full shell theme
- tree context menus with file and directory operations routed through the OS trash/recycle-bin flow for delete actions
- compare actions in menus/context menus for both `Compare Against HEAD` and `Compare Against...`
- app-level multi-project restore across restarts

Implementation details are tracked in `docs/workspace-expansion-plan.md`.

## Current Implementation Notes

As of the current repo-root state, these parts of the behavior target are already implemented in the SDL rewrite:

- the sidebar is a real tool host, not just a permanently-mounted tree
- startup uses the launch working directory as the project root
- project search can run in the sidebar as either a persistent tool or a temporary replacement that restores the previous sidebar tool
- project search now runs asynchronously through a project service, using ripgrep when available and a background file-scan fallback otherwise
- the project search sidebar now includes a replacement field and a replace-all action for literal smart-case project-wide replacements
- the tree and cached project file index both respect `.gitignore` rules, including nested ignore files and negation rules
- the tree shows git-aware status markers and simple chevron-based expansion indicators
- pressing `d` on a file in the tree opens a commit picker overlay
- selecting a commit opens a compare tab in the normal tab strip
- compare tabs support side-by-side aligned rows, hunk jumps, and opening the live working-tree file at the corresponding line
- git sidebar compare and conflict entries reuse an existing tab for the same file/ref pair instead of opening duplicates
- activating or opening a file tab now reveals and selects the matching file in the tree
- per-project config/session persistence now lives under `~/.local/state/microide/projects/<project-name>-<hash>/`
- compare tabs skip syntax highlighting automatically for very large diffs to keep large-text workloads responsive
- `microide_diff_bench` provides a CLI benchmark for compare preparation and syntax-highlighting cost on real repository files
- the tab strip now keeps the active tab visible while still showing as many earlier tabs as will fit
- editor tabs now retain unsaved buffer state when switching between files
- editor tabs now support nested shared-buffer split trees with mouse focus switching, divider drag, and `vsplit` / `hsplit` / `unsplit` / `split-next`
- dirty tabs are now marked in the tab strip and dirty close or quit flows require save/discard/cancel confirmation
- the sidebar and bottom panel can now be resized directly with the mouse by dragging their dividers
- visible scrollbars now exist in the main editor, compare view, sidebar tools, bottom logs, and centered overlays
- the bottom panel can now host a PTY-backed terminal with scrollback, keyboard input, command launching, common ANSI cursor/erase repaint handling, and its own focus target
- the command prompt now supports quoted arguments, per-session history recall, Tab completion, and `reopen`
- the editor has a real centered welcome card instead of a fake numbered placeholder buffer
- the bottom panel exists but starts hidden by default so the editor gets the space first
- the editor has starter lexical syntax coloring for C/C++-like files, Go, Rust, CMake, Markdown, JSON, shell, and Python
- the main editor viewport now caches visible-line layout and syntax tokenization work so large-file scrolling does less repeated CPU work
- `SDL3_ttf` rendering now uses a crisper LCD/subpixel path on stable background surfaces in the shell and ordinary editor text
- project-local session persistence now restores editor tabs, compare tabs, plus sidebar/bottom-panel visibility and sizing across launches
- project-local editor preferences now persist tab size, indent width, and soft-tabs through a lightweight config file

Compare tabs now reuse the lexical highlighter so both sides of the diff get language-aware token coloring.

## Current UI Quality Notes

The shell has moved out of the placeholder stage and is now close to the intended compact desktop feel:

- full-width compact tabs with working overflow behavior
- subtle 1 px separators instead of heavy boxed panels
- a simple VSCode-like tree presentation with chevrons, not decorative icons
- denser text and crisper small-size rendering
- a centered welcome surface for the empty editor state

Known visual and quality-of-life gaps still left:

- split navigation command parity is still lighter than the old editor even though nested split trees now exist
- centered overlays now have their own scrollbars, but they still use simple fixed-row rendering rather than virtualization
- editor text that sits on selection/search highlight rectangles still uses the older blended text path

For a clean contributor handoff, see `docs/agent-handoff.md`.

## Zed-Like GUI Elements Worth Keeping

These GUI details are worth making explicit in the rewrite:

- compact tabs with close buttons and dirty-state markers
- a file/breadcrumb line above the editor
- a narrow, efficient left tree with small row height
- muted panel backgrounds with slightly brighter active surfaces
- thin dividers, not heavy borders
- diagnostic underlines and inline problem styling in the editor
- fast overlay switchers with keyboard-first interaction
- a bottom panel that feels docked, not like a floating terminal window

## Zed-Like Elements To Exclude

These should be treated as out of scope unless added intentionally later:

- collaboration presence avatars
- cloud sync
- built-in GitHub or remote workspace UX
- integrated assistant/chat panes
- extension/plugin marketplace
- source-control workflows beyond what the existing IDE actually needs

## Theme And Typography Direction

Use the Zed screenshot as the tone reference:

- dark neutral background
- cool gray panel hierarchy
- restrained accent colors
- syntax colors with strong readability, not rainbow saturation
- monospace editor font with crisp small-size rendering
- separate UI font only if it materially improves readability without increasing bundle complexity

The UI should feel technical, dense, and calm. Avoid oversized controls, bright gradients, or "gaming UI" styling.

## Proposed Tech Stack

Recommended baseline:

- C++20
- SDL3 for windowing, input, clipboard, IME, high-DPI, and rendering bootstrap
- CMake
- `libuv` or a very small internal job system for async work
- `tree-sitter` only if you want semantic highlighting later; otherwise keep lexical highlighting first

Release target:

- ideally self-contained on Linux, without depending on external `rg` or `git`

Pragmatic bootstrap path:

- it is acceptable to use external `rg` and `git` early during development if that shortens the path to a working prototype
- the architecture should still keep search, git status, and compare behind service interfaces so they can later be replaced by embedded implementations

Recommended rendering approach:

- one SDL window
- one renderer backend
- glyph atlas with cached text runs
- dirty-rect invalidation instead of continuous full redraw
- redraw only on input, blink ticks, async job completion, resize, or animation

The rewrite should avoid an always-on 60 FPS loop. That is the main trap for CPU usage in small editors.

## Core Architecture

Use a small set of concrete subsystems instead of a generic widget framework.

### 1. App Layer

Owns:

- startup and shutdown
- config loading
- SDL init
- main event loop
- renderer lifetime
- worker/job lifetime

Recommended classes:

- `App`
- `MainWindow`
- `EventLoop`

### 2. Workspace Shell

This is the direct replacement for the current global workspace concept.

Owns:

- sidebar visibility and width
- active sidebar tool
- active overlay
- focus routing
- tab strip layout
- editor content layout
- status/command bar layout

Recommended classes:

- `Workspace`
- `SidebarHost`
- `OverlayHost`
- `TabStrip`
- `BreadcrumbBar`
- `BottomPanel`
- `StatusBar`

### 3. Editor Core

Owns:

- text buffers
- cursor state
- selections
- multi-cursor support
- undo/redo
- viewport state
- split layout
- tabs

Recommended data structures:

- piece table or rope for text storage
- immutable snapshots only where async tasks need them
- gap buffer only if you intentionally choose simpler code over large-file behavior

I recommend a piece table. It is simple enough, handles undo well, and avoids a lot of copying.

### 4. Project Services

This replaces the current Go `Project` service boundary.

Owns:

- project path handling
- file enumeration
- ignore handling
- git status collection
- filename search index
- content search dispatch

Recommended classes:

- `ProjectService`
- `FileIndex`
- `IgnoreMatcher`
- `GitService`
- `SearchService`

This layer must stay separate from rendering code.

### 5. Tool Panes And Overlays

Built-in tools should be first-class, not plugins:

- file tree sidebar
- persistent search sidebar
- transient filename finder
- commit picker overlay
- compare view tab
- terminal pane later, if it still proves necessary

Recommended classes:

- `TreePanel`
- `SearchPanel`
- `FileFinderOverlay`
- `CommitPickerOverlay`
- `CompareEditor`
- `TerminalPanel`

### 6. Rendering

Owns:

- font loading
- glyph atlas
- syntax-colored text drawing
- line numbers
- gutters
- selection rectangles
- cursors
- separators
- tabs and overlays

Recommended classes:

- `Renderer`
- `FontManager`
- `GlyphAtlas`
- `TextLayoutCache`
- `Theme`

The renderer should consume already-computed layout state. It should not decide behavior.

## Mapping From Current Go Code To New C++ Modules

- `internal/action/workspace.go` -> `Workspace`, `SidebarHost`, `OverlayHost`
- `internal/action/project.go` -> `ProjectService`, `IgnoreMatcher`, `GitService`
- `internal/action/treepane.go` -> `TreePanel`
- `internal/action/filefinder.go` -> `FileFinderOverlay`
- `internal/action/searchpane.go` -> `SearchPanel`, `SearchService`
- `internal/action/gitcomparepicker.go` -> `CommitPickerOverlay`
- `internal/action/diffcomparepane.go` -> `CompareEditor`
- `internal/action/termpane.go` -> `TerminalPanel`
- `internal/buffer/*` -> `Buffer`, `Cursor`, `Selection`, `History`
- `runtime/syntax/*` -> syntax assets or a converted syntax bundle
- `runtime/colorschemes/*` -> theme assets

## Preserve Behavior, Not TUI Constraints

These current workflows should remain recognizable:

- `F8` toggles the file tree
- `F6` opens the filename finder
- `F5` opens project search
- tree survives tab switches
- temporary search can restore the previous sidebar tool
- file finder reuses already-open files instead of duplicating tabs
- compare opens in a dedicated tab and can jump back to the working file
- compare scrollbars can show add/modify/delete diff markers as a compact change map
- command-driven workflows remain available even in the GUI

The GUI should improve the visuals and input model, but the operational model should stay close to the current fork.

At the visual level, this should feel much closer to Zed than to a terminal editor in a window.

## Lean GUI Design Rules

To keep the new app fast and low-overhead:

- no DOM-style widget tree
- no retained animation system unless a view actually uses animation
- no continuous repaint
- no polling-based search refresh
- no background filesystem walk on every frame
- no full text relayout when only the cursor moved
- no full-window redraw when only the caret blink changed
- no overbuilt retained widget system just to emulate a simple IDE shell

Instead:

- keep explicit rectangles for invalidation
- cache glyphs
- cache shaped or measured lines
- cache syntax-highlighted visible lines
- update only visible tree/search rows
- cancel stale async work aggressively
- debounce expensive project refreshes

## Text Engine Recommendations

The editor core is the hardest part to get right. Treat it as a standalone subsystem.

Required in v1:

- UTF-8 text storage
- line index cache
- multiple cursors
- rectangular and normal selections if already expected
- undo/redo history
- soft wrap
- tabs/spaces handling
- clipboard support
- search/replace
- syntax highlighting

Recommended model:

- `Buffer` owns text and history
- `View` owns scroll position, wrap state, and pixel-to-text mapping
- `Editor` binds a `Buffer` to a `View`
- `SplitNode` owns editor splits inside a tab

## Syntax Highlighting Strategy

The current implementation uses a standalone generated snapshot of the old syntax assets that lives entirely inside this repository.

Current shape:

- `src/editor/RuntimeSyntaxGenerated.cpp` contains the checked-in syntax snapshot
- `src/editor/RuntimeSyntaxRegistry.cpp` compiles those rules with PCRE2 and handles filetype detection plus multiline region state
- `src/editor/SyntaxHighlighter.cpp` is now a thin facade over that runtime registry
- `assets/themes/*.micro` now vendors the old Micro colorscheme assets into the standalone app tree
- `src/render/Theme.cpp` now keeps a Zed-inspired built-in default shell theme and also parses the vendored `.micro` colorschemes for optional runtime switching

Why this path was chosen:

- keeps the shipped app independent from any sibling legacy checkout
- preserves broad language coverage and nested-region behavior
- keeps colorscheme switching/persistence low-friction without introducing a separate theme format yet
- avoids introducing a runtime YAML parser or a build-time converter dependency into the shipping project

What is still missing:

- the GUI theme mapping is still an interpreted translation of the old highlight groups rather than a pixel-perfect reproduction of the original terminal/tcell theme model
- this is still regex/region highlighting, not a structural parser like tree-sitter

If the project later needs deeper syntax-aware features, `tree-sitter` is still the obvious long-term alternative, but it is not required for the current parity target.

Recommendation: use Option A first, then consider `tree-sitter` later.

Because the user explicitly allows a new highlighting and theme system, this can be revisited if the old assets slow down the rewrite.

## Search And Project Indexing

Keep search asynchronous and cancellation-aware like the current Go implementation.

Recommended v1 approach:

- project scan builds a cached file list
- filename finder ranks against that cached list
- content search uses an internal indexed search implementation or a bundled backend
- git status and compare should ideally use internal repository inspection or a bundled git-compatible layer

If this proves too expensive for the first runnable milestone, temporary external command adapters are acceptable during bootstrap only.

The key constraint is to keep the public architecture independent from whether the backend is external or embedded.

## Terminal Strategy

The embedded terminal is a real scope multiplier in a GUI rewrite.

Recommended approach:

- defer it until after editor/sidebar/search/compare are working
- isolate it behind `TerminalPanel`
- separate PTY/process management from rendering and input translation
- support split/tab embedding the same way editor panes do

Terminal is important, but not mandatory for the first usable milestone.

## Compare View Strategy

The compare flow is one of the most IDE-specific features in this fork and should survive the rewrite.

Preserve:

- launch compare from tree
- commit picker overlay
- side-by-side aligned rows
- syntax-colored left/right text
- next/previous hunk navigation
- open working file at corresponding line

The compare model itself is UI-agnostic, so it should live in a service/model layer and feed a dedicated compare editor widget.

## Configuration And Commands

Even in a GUI, keep the command bar.

It is already part of the current user model and gives you a low-cost power-user surface for:

- navigation
- search
- sidebar control
- settings
- future commands

Recommended v1 compatibility target:

- simplify settings and bindings where that reduces complexity
- remove Lua/plugin commands
- keep command names for tree/files/search/sidebar features only where they still make sense in the GUI

Linux is the only required target initially, so packaging and platform abstractions should optimize for Linux first instead of carrying full cross-platform weight from day one.

## Suggested Directory Layout

Recommended target layout at the repo root:

```text
.
├── CMakeLists.txt
├── assets/
├── docs/
├── src/
└── tests/
```

That “keep `micro/` untouched” advice applied only during early development.
The active C++ codebase now intentionally lives at the repo root.

## Phased Implementation Plan

### Phase 0: Freeze Behavior

- document the current IDE workflows
- capture parity checklist from existing help docs and action code
- decide which non-IDE `micro` features are mandatory in v1

Exit criteria:

- one checklist for sidebar, search, file finder, compare, terminal, tabs, splits, command bar, and editor basics

### Phase 1: App Skeleton

- create CMake project at the repo root
- initialize SDL window and renderer
- build event loop and invalidation model
- render empty Zed-inspired workspace chrome

Exit criteria:

- resizable window
- tab strip, sidebar region, breadcrumb bar, editor region, bottom panel host, and status bar placeholders
- no continuous repaint loop required

### Phase 2: Text Core

- implement buffer, cursor, selection, undo/redo
- implement editor viewport and scrolling
- implement text painting with line numbers and selections
- add file open/save

Exit criteria:

- open/edit/save real files
- smooth keyboard and mouse editing
- stable large-file behavior on normal source files

### Phase 3: Tabs And Splits

- add tab model
- add split tree model
- add focus routing between editors

Exit criteria:

- split editors inside tabs
- tab switching does not disturb sidebar state

### Phase 4: Project Services

- add launch-path project selection
- add ignore handling
- add cached file enumeration
- add git status and compare data fetchers

Exit criteria:

- reusable project snapshot service
- no UI code performs direct filesystem walks

### Phase 5: Sidebar And Overlays

- implement file tree sidebar
- implement filename finder overlay
- implement search sidebar
- support temporary versus persistent sidebar tools

Exit criteria:

- `tree`, `files`, and `rg` flows match the current fork behavior

### Phase 6: Compare Workflow

- implement commit picker overlay
- implement compare model
- implement side-by-side compare editor

Exit criteria:

- tree -> commit picker -> compare tab path works end to end

### Phase 7: Terminal

- embed PTY-backed terminal panel
- map input correctly
- support split/tab placement

Exit criteria:

- terminal works without destabilizing editor focus or rendering

### Phase 8: Polish And Optimization

- protect startup speed first
- then reduce idle CPU usage
- then reduce memory footprint
- theme support
- syntax asset conversion
- keybinding compatibility
- command bar compatibility
- startup, memory, and redraw profiling
- Zed-inspired density and layout polish

Exit criteria:

- low idle CPU
- no obvious redraw waste
- acceptable typing latency on large files

## Performance Checklist

Before calling the rewrite successful, verify:

- startup feels immediate on medium-sized projects and remains explainable with traced timings
- idle CPU near zero when the window is unfocused and unchanged
- memory growth is bounded during ordinary editing and navigation
- no full redraw on caret blink
- no full project rescan on every tree focus
- search cancellation works
- stale async results cannot overwrite newer UI state
- file finder ranking remains responsive on large repos
- compare view does not re-diff on every cursor move
- text layout cache is bounded and can evict safely

## Validation Strategy

Use three layers of validation:

### Unit Tests

- buffer edits
- undo/redo
- cursor movement
- ignore matching
- filename ranking
- compare row mapping

### Integration Tests

- open file, edit, save
- open tree, switch tabs, tree persists
- open file finder, select result, existing tab reused
- run project search, open result, previous sidebar restored
- open compare from tree, jump to working file

### Golden Tests

- tree rendering
- search rendering
- compare rendering
- syntax highlighting snapshots

If possible, build a small headless render harness for pixel or cell-like golden comparisons.

A generated fixture corpus now lives under `tests/fixtures` to seed these tests with large files, syntax samples, and deterministic diff pairs.

## Recommended Scope Cuts

To keep the rewrite realistic, explicitly cut these from v1 unless you later decide otherwise:

- Lua plugins
- plugin manager
- plugin runtime APIs
- plugin commands
- exposing global tools as extension points
- LSP and diagnostics unless you intentionally add them as a separate phase

## Decisions Captured From User Answers

These decisions are now treated as part of the plan:

- SDL3 is the baseline
- Linux is the only required target initially
- the repo root is the C++ project root
- terminal can land after the first usable editor/sidebar/search/compare milestone
- configuration can be simplified instead of preserving full backward compatibility
- a new highlighting and theme system is acceptable
- the current IDE features are important references, but not every single one is mandatory parity if a better leaner design emerges
- long-term release direction should be self-contained
- use a single sidebar surface
- include the bottom panel in the first GUI milestone
- keep the first theme close to the Zed reference
- defer SCM tree indicators until after the shell is stable

## Recommended Migration Order For Assets

- carry over themes first
- carry over syntax definitions second
- carry over help text and command names third
- only then decide whether settings and bindings should be fully backward compatible

## Recommended Immediate Next Step

After you answer the questions above, the next concrete deliverable should be:

- a parity checklist document for the current IDE workflows
- the initial repo-root CMake + SDL app skeleton
- the first implementation slice for the workspace shell and text editor core
