# microide

The fastest, lowest-footprint desktop IDE — a native C++/SDL3 single-window application
with no GPU acceleration requirement, keyboard-first workflows, and best-in-class diff and merge.
Think VSCode-shaped surface area with Zed-class responsiveness.

For the authoritative in-scope/non-goal list see `openspec/specs/product-vision/spec.md`.

## Highlights

### Editing
- Multi-project tabs, file tabs, nested shared-buffer splits
- UTF-8 codepoint boundaries, IME preedit, line-ending detection and preservation
- Syntax highlighting with per-file checkpointed state (fast random jumps in large files)
- Undo/redo storing line-range patches rather than full-buffer snapshots
- Git blame shadow text — asynchronous, viewport-scoped, caret-local annotations with hover commit details
- Project-local colorscheme, tab size, indent width, and soft-tabs preferences
- Session restore across restarts

### Diff & Merge
- Compare tabs: working-tree vs `HEAD`, arbitrary commits, outgoing base-branch files
- Three-way merge tabs: incoming / result / current panes with per-hunk picks and whole-side apply
- Shared decorated text-grid pipeline across editor, compare, and merge surfaces
- Change-overview lane alongside the scrollbar in both compare and merge tabs
- `[` / `]` hunk navigation; `o` opens the working-tree file at the matched line
- State preservation across rename, delete, and reopen — reopening the same target reuses the tab
- Standalone `microide_diff_bench` for repeatable before/after timing on compare hot paths

### Git
- Sidebar git view: working-tree changes, staged files, merge conflicts, outgoing branch files
- Per-file stage (`s`), discard (`x`), bulk stage-all, confirmed discard-all
- Conflict files open directly in the three-way merge tab
- Commit picker overlay for compare target selection

### Search
- Async project-search sidebar: literal (default) or regex, case control, hidden-file toggle
- Capped-result feedback; replace-in-project in literal mode
- File finder overlay with cached index
- Standalone `microide_search_bench` for repeatable timing

### Terminal
- PTY-backed terminal tabs with scrollback and selection
- Alternate-screen support, application cursor-key mode, origin mode, autowrap, bracketed paste
- OSC 52 clipboard copy, focus notifications, basic device/cursor query replies
- Terminal text selection, copy, and paste shortcuts
- Tab drag reordering; right-click for "Copy Last Command + Output"

### Plugins
- Manual Lua 5.4 plugins from `~/.config/microide/plugins/` and project-local `.microide/plugins/`
- Lifecycle hooks, commands, sidebars, diagnostics, hover providers, syntax contributions
- Host-owned registries: settings, keybindings, status items, menus, formatters, save participants,
  completion providers, code actions, tests, SCM, auth, annotations
- `plugins-reload` command; file-watch–triggered asset reload on Linux
- Repo-owned dogfood examples: `plugins/eslint` (diagnostics)

## Scope

In-scope and non-goals are declared in `openspec/specs/product-vision/spec.md`.

Short version: built-in editor, diff, merge, search, git, and terminal workflows stay host-owned.
Out of scope: full debugger UI, plugin marketplaces, cloud/collaboration/sync, recent-project surfaces.

## Build

Requirements:
- CMake 3.28+
- C++20 compiler
- SDL3 development package
- optional: SDL3_ttf for the real font backend

```bash
cmake -S . -B build
cmake --build build -j8
./build/microide/microide
```

### Ubuntu — building SDL3 and SDL3_ttf from source

If your distro does not package recent enough SDL3 or SDL3_ttf:

```bash
sudo apt install build-essential cmake git

# SDL3
git clone https://github.com/libsdl-org/SDL.git && cd SDL
sudo apt-get install build-essential git make pkg-config cmake ninja-build gnome-desktop-testing \
  libasound2-dev libpulse-dev libaudio-dev libfribidi-dev libjack-dev libsndio-dev \
  libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxfixes-dev libxi-dev libxss-dev \
  libxtst-dev libxkbcommon-dev libdrm-dev libgbm-dev libgl1-mesa-dev libgles2-mesa-dev \
  libegl1-mesa-dev libdbus-1-dev libibus-1.0-dev libudev-dev libthai-dev \
  libpipewire-0.3-dev libwayland-dev libdecor-0-dev liburing-dev
cmake -S . -B build && cmake --build build -j8 && sudo cmake --install build && cd ..

# SDL3_ttf
git clone https://github.com/libsdl-org/SDL_ttf.git && cd SDL_ttf
sudo apt install libfreetype-dev
cmake -S . -B build && cmake --build build -j8 && sudo cmake --install build && cd ..
```

Font: if `SDL3_ttf` is available at configure time, MicroIDE looks for
`assets/fonts/JetBrainsMono-Regular.ttf` (copied to `build/microide/assets/` by CMake), then
common system font locations, and falls back to the SDL debug-text backend if neither is found.

## Project State

Per-project config and session files live under
`~/.local/state/microide/projects/<project-name>-<hash>/`.

Each project gets a persisted default accent color on first open; override it with
`project-base-color "#rrggbb"` in the per-project `config` file.

## Controls

| Key | Action |
|-----|--------|
| `F8` | Toggle sidebar |
| `F6` | Toggle centered overlay |
| `Ctrl+Tab` | Cycle focus: sidebar → editor → command/terminal pane |
| `Ctrl+w` | Close active tab |
| `Ctrl+f` | Buffer search |
| `Ctrl+h` | Replace in buffer |
| `Ctrl+Shift+f` | Project-wide search in sidebar |
| `Ctrl+a` | Select whole buffer |
| `Ctrl+c` / `Ctrl+x` / `Ctrl+v` | Copy / Cut / Paste |
| `Ctrl+z` / `Ctrl+y` | Undo / Redo |
| `Ctrl+s` | Save |
| `Ctrl+e` | Open command prompt |
| `Shift+arrows`, `Home`, `End`, `Ctrl+Home`, `Ctrl+End` | Extend selection |
| `PageUp` / `PageDown` | Scroll viewport |
| `d` on sidebar file | Open compare commit picker |
| `[` / `]` in compare/merge | Previous / next hunk |
| `i` `b` `c` `m` in merge | Apply incoming / base / current / both for selected hunk |
| `I` `B` `C` `M` in merge | Apply choice to every hunk |
| `a` in merge | Restore auto choices for all hunks |
| `o` in compare/merge | Open working-tree file at corresponding line |
| `s` in git sidebar | Stage selected file |
| `x` in git sidebar | Discard selected file |
| `r` in git sidebar | Refresh |
| `Ctrl+c` in terminal (with selection) | Copy terminal selection |
| `Ctrl+Shift+v` or `Shift+Insert` in terminal | Paste into terminal |
| Middle-click tab | Close tab |
| Mouse wheel on tab strip | Scroll tab overflow |

Mouse: click to focus/open, drag to reorder tabs within their strip, wheel to scroll editor/sidebar/terminal.

Right-click in editor: **Copy with Context** (copies `relative/path:line` + selection).  
Right-click terminal tab: **Copy Last Command + Output**.

## Commands

Run commands with `Ctrl+e` (command prompt):

```
about                               code-actions
colorscheme [name|list]             compare [path] [commit-prefix]
completion                          files [root]
find <query>                        find-references
focus <editor|sidebar|panel>        git-refresh
goto <line[:col]>                   goto-definition
indent-width [n]                    jump <line[:col]>
keyboard-shortcuts                  layout-mode-toggle
merge <base> <incoming> <current> [output]
open <path>                         output [channel]
plugins-reload                      project-close
project-next                        project-open [path]
project-prev                        project-search [query]
quit                                reopen
save                                search <query>
settings                            sidebar-close
sidebar-hide                        sidebar-show [tool]
sidebar-toggle [tool]               sidebar-width <n>
soft-tabs [on|off]                  split-first
split-last                          split-next
split-prev                          status-bar-toggle
tab [path]                          tab-size [n]
tabmove <n>                         tabswitch <tab>
term [command]                      tests-discover
tests-run [test-id...]              tree [root]
tree-refresh                        ui-scale [n|up|down|reset]
unsplit                             vsplit [path]
wrap [on|off]
```

Current commands:
- `code-actions`
- `colorscheme [name|list]`
- `completion`
- `compare [path] [commit-prefix]`
- `merge <base> <incoming> <current> [output]`
- `files [root]`
- `find <query>`
- `find-references`
- `focus <editor|sidebar|panel>`
- `goto-definition`
- `goto <line[:col]>`
- `git-refresh`
- `indent-width [n]`
- `jump <line[:col]>`
- `open <path>`
- `about`
- `keyboard-shortcuts`
- `settings`
- `project-close`
- `project-next`
- `project-open [path]`
- `project-prev`
- `project-search [query]`
- `plugins-reload`
- `quit`
- `reopen`
- `save`
- `search <query>`
- `output [channel]`
- `sidebar-close`
- `sidebar-hide`
- `sidebar-show [tool]`
- `sidebar-toggle [tool]`
- `sidebar-width <n>`
- `soft-tabs [on|off]`
- `wrap [on|off]`
- `split-first`
- `split-last`
- `split-next`
- `split-prev`
- `tab [path]`
- `tab-size [n]`
- `tabmove <n>`
- `tabswitch <tab>`
- `term [command]`
- `tests-discover`
- `tests-run [test-id...]`
- `tree [root]`
- `tree-refresh`
- `ui-scale [n|up|down|reset]`
- `layout-mode-toggle`
- `status-bar-toggle`
- `unsplit`
- `vsplit [path]`
- `jump-to-matching-bracket`
- `toggle-line-comment`
- `toggle-block-comment`
- `move-line-up`
- `move-line-down`
- `duplicate-line`
- `delete-line`
- `indent-lines`
- `outdent-lines`
- `sort-lines-ascending`
- `sort-lines-descending`
- `add-cursor-next-match`
- `add-cursor-all-matches`
- `fold`
- `unfold`
- `fold-all`
- `unfold-all`
- `toggle-fold`
- `toggle-editor-folding`
- `toggle-editor-sticky-scroll`
- `toggle-editor-indent-guides`
- `toggle-editor-render-whitespace`
- `toggle-editor-outline`
- `toggle-editor-bracket-match-highlight`
- `toggle-editor-auto-close`
- `toggle-editor-surround`
- `toggle-editor-smart-indent`
- `toggle-editor-toggle-comment`
- `toggle-editor-line-ops`
- `toggle-editor-sort-lines`
- `toggle-editor-add-cursor-at-match`
- `toggle-editor-occurrences-highlight`
- `toggle-editor-snippets`
- `toggle-editor-save-trim`
- `toggle-editor-save-ensure-newline`
- `toggle-editor-auto-detect-indent`

Merge example:
```
merge /path/to/base.txt /path/to/incoming.txt /path/to/current.txt /path/to/result.txt
```

## Benchmarks & Profiling

**Diff benchmark:**
```bash
./build/microide/microide_diff_bench /path/to/repo path/to/file [--runs=N]
```
Reports diff stage timings, compare syntax timing, row-decoration build/paint timing, warm render timing, and cache hit rates.

**Search benchmark:**
```bash
./build/microide/microide_search_bench /path/to/repo query [--regex] [--case=smart|sensitive|insensitive] [--hidden] [--runs=5]
```

**Runtime profiling:**
```bash
env MICROIDE_PERF_TRACE=1 MICROIDE_PERF_TRACE_MIN_MS=1 MICROIDE_TRACE_REDRAW=1 \
  ./build/microide/microide
```
See `docs/startup-tracing.md` and `docs/runtime-profiling.md` for full workflows.

## Plugin Runtime

- User plugins: `~/.config/microide/plugins/<plugin-id>/init.lua`
- Project plugins: `<project-root>/.microide/plugins/<plugin-id>/init.lua`
- Repo examples: `plugins/`
- Entry point: `return require("microide").plugin({...})`
- Lifecycle hooks: `setup`, `on_project_open`, `on_project_close`, `on_buffer_open`, `on_buffer_save`, `shutdown`
- Host API: `ctx.log`, `ctx.commands.add`, `ctx.sidebar.add/show`, `ctx.workspace.*`,
  `ctx.files.*`, `ctx.process.run`, `ctx.diagnostics.*`, `ctx.hover.add`,
  `ctx.settings.*`, `ctx.menus.add`, `ctx.keybindings.add`, `ctx.status.*`,
  `ctx.formatters.add`, `ctx.save_participants.add`, `ctx.completion.add`,
  `ctx.code_actions.add`, `ctx.tasks.add`, `ctx.tools.add`, `ctx.debuggers.add`,
  `ctx.tests.add`, `ctx.scm.add`, `ctx.annotations.add`, `ctx.auth.add`
- Syntax: `syntax/*.lua` inside plugin directories, loaded on project open and `plugins-reload`
