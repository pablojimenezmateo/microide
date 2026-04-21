# microide

Initial C++/SDL3 skeleton for the MicroIDE rewrite.

## Current State

This scaffold provides:

- CMake-based build
- SDL3 application bootstrap
- Linux-first single-window app shell
- milestone-1 workspace chrome placeholders
- event-driven rendering loop
- filesystem-backed sidebar tree
- startup uses the launch working directory as the project root
- `.gitignore`-aware tree and file indexing
- async built-in project search with literal-by-default matching, optional regex, explicit case mode, hidden-file toggles, and capped-result feedback
- standalone project-search benchmark utility for repeatable timing runs on larger repositories
- git-aware tree status markers
- file-backed editor viewport with a pluggable text backend
- nested shared-buffer editor split trees
- project-local session restore for editor tabs, compare tabs, and workspace chrome
- project-local editor preferences for tab size, indent width, and soft-tabs
- manual Lua 5.4 plugin loading from global and project-local plugin directories, with lifecycle hooks, plugin commands, sidebars, diagnostics, hover providers, syntax contributions, and `plugins-reload`
- tab-aware text layout with visual-column cursor positioning
- editor load/save now preserves detected line endings
- editor git blame shadow text for tracked on-disk files, including saved but uncommitted content, loaded asynchronously per viewport window and cached by file or line span, with only the caret line plus one line above and below annotated inline and hover details for author, date, commit message, and SHA copy
- extracted text renderer and editor view renderer modules
- optional `SDL3_ttf` text backend with debug-text fallback
- cached file-finder overlay
- commit picker overlay and compare tabs
- three-way merge tabs with incoming/result/current panes, per-hunk picks, and whole-side apply actions
- merge tabs now show a compact change-overview lane next to the vertical scrollbar, mirroring compare tabs
- sidebar git view for working-tree changes, merge conflicts, and outgoing branch files
- tree rename and delete flows that preserve affected editor, compare, and merge state instead of dropping it
- docked command prompt
- docked terminal tabs with fresh PTY sessions, header `+` creation, auto-removal when a terminal exits, alternate-screen scroll-region handling, explicit scroll-up/scroll-down sequences, application cursor-key mode, origin mode, autowrap control, bracketed paste mode, basic device/cursor query replies, charset-designation escape handling, common cursor/erase repaint handling, and terminal-tab context-copy for the last command transcript
- terminal text selection, clipboard copy, and clipboard paste shortcuts
- SDL text input wired into command/search/finder/sidebar-edit fields, the terminal, and editor insertion
- editor cursor movement, layout, and backspace/delete now respect UTF-8 codepoint boundaries
- editor and built-in text prompts now render IME preedit text and advertise caret-aligned text-input areas for candidate placement

The current shell draws:

- compact top tab strip
- left sidebar region
- breadcrumb/header bar
- editor surface
- docked terminal pane when a terminal tab or command prompt is active

Current controls:

- `F8`: toggle sidebar
- `F6`: toggle centered overlay
- `Ctrl+Tab`: switch focus between the sidebar, editor, and command or terminal pane
- `Ctrl+w`: close the active tab
- `Ctrl+f`: open buffer search
- `Ctrl+h`: open replace-in-buffer
- `Ctrl+Shift+f`: open temporary project-wide search in the sidebar
- `Ctrl+a`: select the whole buffer
- `Ctrl+c`: copy the current selection
- `Ctrl+x`: cut the current selection
- `Ctrl+v`: paste clipboard text into the editor
- `Edit > Copy with Context` or right click in the editor and choose `Copy with Context`: copy the selection prefixed with `relative/path:line` or `relative/path:start-end`
- hover editor blame shadow text: show the blamed commit author, date, message, and a `Copy SHA` button
- `Ctrl+c` in the terminal with an active terminal selection: copy the selected terminal text
- `Ctrl+Shift+v` or `Shift+Insert` in the terminal: paste clipboard text, using bracketed paste when the app requests it
- right click a terminal tab and choose `Copy Last Command + Output`: copy the submitted command plus its rendered transcript, or only the invoked command while a full-screen terminal app owns the alternate screen
- `Ctrl+z`: undo the last edit
- `Ctrl+y` or `Ctrl+Shift+z`: redo the last undone edit
- `Ctrl+s`: save the current file
- `Up` / `Down`: move in the focused area
- `Left` / `Right`: collapse or expand directories in the sidebar
- `Enter`: open the selected file from the sidebar
- `d` on a file in the tree: open the compare commit picker
- in the editor, typing inserts text
- `Tab`, `Enter`, `Backspace`, and `Delete` edit the current file
- `Shift` with arrows, `Home`, `End`, `Ctrl+Home`, or `Ctrl+End`: extend selection
- while buffer search is open, type to filter matches and use `Up` / `Down` to move between them
- `Enter` in buffer search: jump to the selected match and close search
- while replace is open, `Tab` switches between find and replace fields
- `Enter` in replace: replace the current match
- `Ctrl+Enter` in replace: replace all matches
- while sidebar search is open, use `Up` / `Down`, `j` / `k`, `Home`, and `End` to move between results
- `Enter` or `Right` in sidebar search: open the selected result
- `Esc` in temporary sidebar search: restore the previous sidebar tool
- `/` in sidebar search: edit the query
- click `Lit` / `Rx`, the case-mode button, or `Hide-` / `Hide+` in sidebar search: switch pattern mode, case mode, or hidden-file inclusion
- `r` in sidebar search: rerun the current query
- `R` in sidebar search: replace all matches when search mode is literal
- while editing the sidebar search query, `Enter` applies it and `Esc` cancels editing
- while the compare commit picker is open, type to filter commits and use `Enter` to open a compare tab
- in a compare tab, use `Up` / `Down`, `j` / `k`, `Home`, and `End` to move between compare rows
- `[` / `]` in a compare tab: jump to the previous or next hunk
- `Enter` or `o` in a compare tab: open the working-tree file at the corresponding line
- `Esc` in a compare tab: close the compare tab
- in a merge tab, use `Up` / `Down`, `j` / `k`, `Home`, and `End` to move between merge hunks
- `i`, `b`, `c`, `m` in a merge tab: apply incoming, base, current, or both for the selected hunk
- `a` in a merge tab: restore auto choices for all hunks
- `I`, `B`, `C`, `M` in a merge tab: apply incoming, base, current, or both to every hunk
- `o` in a merge tab: open the merge result file in a normal editor tab
- `Esc` in a merge tab: close the merge tab
- `sidebar-show git` or `sidebar-toggle git`: switch the sidebar into source-control mode
- in the git sidebar, `Up` / `Down`, `Home`, `End`, `PageUp`, and `PageDown` move between files
- `Enter` in the git sidebar: open the selected file, comparing `HEAD` against the working tree
- selecting a conflicted file in the git sidebar opens the three-way merge tab instead of a diff tab
- `s` in the git sidebar or clicking `Stage`: run `git add` for the selected modified file
- `x` in the git sidebar or clicking `Discard`: restore the selected modified file to `HEAD`
- click `Stage All` in the git sidebar: stage every working-tree change in the current repo
- click `Discard All` in the git sidebar: confirm and then discard tracked, untracked, and conflicted working-tree changes in the current repo
- `r` in the git sidebar: refresh working-tree and outgoing-file status
- reopening the same git diff or conflict file reuses the existing tab instead of opening duplicates
- selecting an outgoing file opens a compare tab against the resolved base branch `HEAD`
- renaming a file or directory keeps open compare tabs attached to the correct historical commit-side paths while retargeting the live working-tree side
- left click on a tab: activate that file tab
- left click on a tab `x`: close that tab
- middle click on a tab: close that tab
- mouse wheel on the tab strip: scroll tab overflow
- left click in the sidebar: toggle directories or open files
- reselecting an already open editor tab re-reveals that file in the tree, including reopening collapsed ancestor directories
- mouse wheel in the sidebar: scroll the active sidebar tool
- left click in the editor: move the caret
- left-click drag in the editor: select text
- left-click drag in the terminal: select terminal text
- left-click drag project, file, or terminal tabs: reorder them within their existing strip without creating detached windows
- left click a terminal tab: activate that terminal
- right click a terminal tab: open terminal tab actions
- left click the terminal header `+`: open a fresh terminal tab
- left click a terminal tab `x` or middle click a terminal tab: close that terminal tab
- mouse wheel in the editor: scroll the text viewport
- while the overlay is open, type to filter files and use `Enter` to open the selected match
- `PageUp` / `PageDown`: move through the editor viewport
- `Left` / `Right` in the editor: move the cursor horizontally
- `Home` / `End`: move to start or end of line
- `Ctrl+Home` / `Ctrl+End`: jump to start or end of file
- `Esc` in the terminal with an active selection: clear the terminal selection
- `Ctrl+e`: open the command prompt

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
- `focus <editor|sidebar|panel>`
- `goto <line[:col]>`
- `git-refresh`
- `indent-width [n]`
- `inline-complete`
- `jump <line[:col]>`
- `mcp <tool> [json]`
- `open <path>`
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
- `unsplit`
- `vsplit [path]`

Merge example:

- `merge /path/to/base.txt /path/to/incoming.txt /path/to/current.txt /path/to/result.txt`
- use any four plain-text files; the output path can point at the current-side file if you want to resolve in place

Project state:

- per-project config and session files now live under `~/.local/state/microide/projects/<project-name>-<hash>/`
- each project gets a persisted default base accent color on first open; override it in the per-project `config` file with `project-base-color "#rrggbb"`

Plugin runtime:

- manual plugins load from `~/.config/microide/plugins/<plugin-id>/init.lua`
- project-local plugins load from `<project-root>/.microide/plugins/<plugin-id>/init.lua`
- repo-owned example plugins live under `plugins/`
- plugin entry points return `require("microide").plugin({...})`
- current lifecycle hooks are `setup`, `on_project_open`, `on_project_close`, `on_buffer_open`, `on_buffer_save`, and `shutdown`
- the current host API includes `ctx.log(...)`, `ctx.commands.add(name, fn)`, `ctx.sidebar.add({...})`, `ctx.sidebar.show(id)`, `ctx.workspace.project_root()`, `ctx.workspace.open_file(path, line, column)`, `ctx.workspace.active_buffer()`, `ctx.files.read_text(path)`, `ctx.files.write_text(path, text)`, `ctx.files.exists(path)`, `ctx.process.run(argv, { cwd = ..., stdin = ... })`, `ctx.diagnostics.publish(path, diagnostics)`, `ctx.diagnostics.clear(path_or_nil)`, and `ctx.hover.add({ id, provide })`
- plugin sidebars use host-owned rendering, appear in the sidebar-mode dropdown when loaded, and can also be opened from the command line with `sidebar-show <sidebar-id>`
- plugin syntax definitions load from `syntax/*.lua` inside plugin directories on project open and `plugins-reload`
- the repo-owned `plugins/eslint` and `plugins/llm` directories are the current dogfood examples for diagnostics and host-owned AI workflows
- `plugins-reload` rebuilds the active plugin host and reloads commands for the current project

Diff benchmark:

- build `microide_diff_bench` and run `./build/microide/microide_diff_bench /path/to/repo path/to/file`
- example repro:
  `./build/microide/microide_diff_bench /path/to/repo src/main.cpp`
- add `--runs=N` for repeated runs
- the benchmark reports diff stage timings, compare syntax timing, shared row-decoration build and paint timing, warm editor render timing, and cache hit rates using the rewritten no-cutoff pipeline

Runtime profiling:

- live redraw and resize tracing is documented in `docs/runtime-profiling.md`
- example live trace:
  `env MICROIDE_PERF_TRACE=1 MICROIDE_PERF_TRACE_MIN_MS=1 MICROIDE_TRACE_REDRAW=1 ./build/microide/microide`
- use it to inspect editor, compare, merge, terminal, and retained-scene resize redraw cost interactively

Project search benchmark:

- build `microide_search_bench` and run `./build/microide/microide_search_bench /path/to/repo query [--regex] [--case=smart|sensitive|insensitive] [--hidden] [--runs=5]`
- example repro:
  `./build/microide/microide_search_bench /path/to/repo search --runs=5`
- the benchmark reports result count, truncation, per-run timings, and min/max/avg search time using the same built-in backend as the sidebar

## Build

Requirements:

- CMake 3.28+
- C++20 compiler
- SDL3 development package
- optional: SDL3_ttf development package for the real font backend

### Ubuntu

If your distro packages do not provide recent enough SDL3 or `SDL3_ttf` development files, build and install them from source:

```bash
sudo apt install build-essential cmake git

git clone https://github.com/libsdl-org/SDL.git
cd SDL/

sudo apt-get install build-essential git make pkg-config cmake ninja-build gnome-desktop-testing libasound2-dev libpulse-dev libaudio-dev libfribidi-dev libjack-dev libsndio-dev libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxfixes-dev libxi-dev libxss-dev libxtst-dev libxkbcommon-dev libdrm-dev libgbm-dev libgl1-mesa-dev libgles2-mesa-dev libegl1-mesa-dev libdbus-1-dev libibus-1.0-dev libudev-dev libthai-dev libpipewire-0.3-dev libwayland-dev libdecor-0-dev liburing-dev
cmake -S . -B build
cmake --build build
sudo cmake --install build

cd ..
git clone https://github.com/libsdl-org/SDL_ttf.git
cd SDL_ttf/

sudo apt install libfreetype-dev
cmake -S . -B build
cmake --build build
sudo cmake --install build
```

Then return to this repo and configure/build `microide` normally.

Example:

```bash
cmake -S . -B build/microide
cmake --build build/microide
./build/microide/microide
```

If CMake fails with an SDL3 error, install the SDL3 development package for your distro and rerun configure.

If `SDL3_ttf` is available at configure time, `microide` will use the `sdl3_ttf` backend and try to load a monospace font from `assets/fonts/JetBrainsMono-Regular.ttf` first, then common system font locations. If `SDL3_ttf` is not available, the app falls back to the SDL debug-text backend so the shell still runs.

The build now copies `assets/` next to the executable, so a bundled font in `assets/fonts/JetBrainsMono-Regular.ttf` will work even when launching from `build/microide/`.
