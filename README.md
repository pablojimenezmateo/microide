# microide

The fastest, lowest-footprint AI-focused desktop IDE — a native C++/SDL3 single-window application
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

### AI
- Host-owned chat pane with conversation rail, provider/model selector, markdown transcript rendering
- Per-conversation multiline draft composer with retention across conversation switches
- Ghost-text inline completion with explicit accept / dismiss; cancels on caret move or buffer change
- MCP tool execution with per-agent permission levels, session-scoped approvals, transcript events
- Native `microide_provider_bridge` binary for stdio-backed direct API-key provider communication
- `chat`, `inline-complete`, and `mcp` built-in commands

### Plugins
- Manual Lua 5.4 plugins from `~/.config/microide/plugins/` and project-local `.microide/plugins/`
- Lifecycle hooks, commands, sidebars, diagnostics, hover providers, syntax contributions
- Host-owned registries: settings, keybindings, status items, menus, formatters, save participants,
  completion providers, code actions, tasks, tools, tests, SCM, auth, annotations, AI providers
- `plugins-reload` command; file-watch–triggered asset reload on Linux
- Repo-owned dogfood examples: `plugins/eslint` (diagnostics), `plugins/llm` (chat/completion)

## Scope

In-scope and non-goals are declared in `openspec/specs/product-vision/spec.md`.

Short version: built-in editor, diff, merge, search, git, terminal, and AI workflows stay host-owned.
Out of scope: full debugger UI, plugin marketplaces, cloud/collaboration/sync, recent-project surfaces.

## Build

Requirements:
- CMake 3.28+
- C++20 compiler
- SDL3 development package
- optional: SDL3_ttf for the real font backend

```bash
cmake -S . -B build
cmake --build build
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
cmake -S . -B build && cmake --build build && sudo cmake --install build && cd ..

# SDL3_ttf
git clone https://github.com/libsdl-org/SDL_ttf.git && cd SDL_ttf
sudo apt install libfreetype-dev
cmake -S . -B build && cmake --build build && sudo cmake --install build && cd ..
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

Run commands with `Ctrl+e` (command prompt) or the `chat` sidebar:

```
auth-login <provider> [scope...]    auth-logout <provider> <session>
auth-refresh <provider> <session>   about
ai-provider                         chat [message]
code-actions                        colorscheme [name|list]
compare [path] [commit-prefix]      completion
debug-start <type>                  debug-stop
files [root]                        find <query>
find-references                     focus <editor|sidebar|panel>
git-refresh                         goto <line[:col]>
goto-definition                     indent-width [n]
inline-complete                     jump <line[:col]>
keyboard-shortcuts                  layout-mode-toggle
mcp <tool> [json]                   merge <base> <incoming> <current> [output]
open <path>                         output [channel]
plugins-reload                      project-close
project-next                        project-open [path]
project-prev                        project-search [query]
quit                                reopen
save                                search <query>
settings                            status-bar-toggle
sidebar-close                       sidebar-hide
sidebar-show [tool]                 sidebar-toggle [tool]
sidebar-width <n>                   soft-tabs [on|off]
split-first                         split-last
split-next                          split-prev
tab [path]                          tab-size [n]
tabmove <n>                         tabswitch <tab>
tasks [task-id]                     term [command]
tests-discover                      tests-run [test-id...]
tree [root]                         tree-refresh
ui-scale [n|up|down|reset]          unsplit
vsplit [path]                       wrap [on|off]
```

Current commands:
- `auth-login <provider> [scope...]`
- `auth-logout <provider> <session>`
- `auth-refresh <provider> <session>`
- `code-actions`
- `colorscheme [name|list]`
- `completion`
- `compare [path] [commit-prefix]`
- `debug-start <type>`
- `debug-stop`
- `merge <base> <incoming> <current> [output]`
- `files [root]`
- `find <query>`
- `find-references`
- `focus <editor|sidebar|panel>`
- `goto-definition`
- `goto <line[:col]>`
- `git-refresh`
- `indent-width [n]`
- `inline-complete`
- `jump <line[:col]>`
- `mcp <tool> [json]`
- `open <path>`
- `ai-provider`
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
- `chat [message]`
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
- `tasks [task-id]`
- `tests-discover`
- `tests-run [test-id...]`
- `tree [root]`
- `tree-refresh`
- `ui-scale [n|up|down|reset]`
- `layout-mode-toggle`
- `status-bar-toggle`
- `unsplit`
- `vsplit [path]`

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
  `ctx.tests.add`, `ctx.scm.add`, `ctx.annotations.add`, `ctx.auth.add`,
  `ctx.ai_providers.add`, `ctx.external_agents.add`, `ctx.mcp_tools.add`
- Syntax: `syntax/*.lua` inside plugin directories, loaded on project open and `plugins-reload`
