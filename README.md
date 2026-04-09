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
- async built-in project search with literal-by-default matching, optional regex, explicit case mode, and hidden-file toggles
- git-aware tree status markers
- file-backed editor viewport with a pluggable text backend
- nested shared-buffer editor split trees
- project-local session restore for editor tabs, compare tabs, and workspace chrome
- project-local editor preferences for tab size, indent width, and soft-tabs
- tab-aware text layout with visual-column cursor positioning
- editor load/save now preserves detected line endings
- editor large-file mode disables syntax highlighting above size or line-count thresholds
- extracted text renderer and editor view renderer modules
- optional `SDL3_ttf` text backend with debug-text fallback
- cached file-finder overlay
- commit picker overlay and compare tabs
- three-way merge tabs with incoming/result/current panes, per-hunk picks, and whole-side apply actions
- sidebar git view for working-tree changes, merge conflicts, and outgoing branch files
- docked command prompt
- docked terminal tabs with fresh PTY sessions, header `+` creation, auto-removal when a terminal exits, alternate-screen scroll-region handling, explicit scroll-up/scroll-down sequences, application cursor-key mode, autowrap control, bracketed paste mode, and common cursor/erase repaint handling
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
- `Ctrl+c` in the terminal with an active terminal selection: copy the selected terminal text
- `Ctrl+Shift+v` or `Shift+Insert` in the terminal: paste clipboard text, using bracketed paste when the app requests it
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
- `r` in the git sidebar: refresh working-tree and outgoing-file status
- reopening the same git diff or conflict file reuses the existing tab instead of opening duplicates
- selecting an outgoing file opens a compare tab against the resolved base branch `HEAD`
- left click on a tab: activate that file tab
- left click on a tab `x`: close that tab
- middle click on a tab: close that tab
- mouse wheel on the tab strip: scroll tab overflow
- left click in the sidebar: toggle directories or open files
- mouse wheel in the sidebar: scroll the active sidebar tool
- left click in the editor: move the caret
- left-click drag in the editor: select text
- left-click drag in the terminal: select terminal text
- left click a terminal tab: activate that terminal
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

- `help`
- `open <path>`
- `tab [path]`
- `tabswitch <tab>`
- `tabmove <n>`
- `compare [path] [commit-prefix]`
- `merge <base> <incoming> <current> [output]`
- `reopen`
- `save`
- `quit`
- `term [command]`
- `find <query>`
- `files [root]`
- `tree [root]`
- `search <query>`
- `project-search [query]`
- `goto <line[:col]>`
- `jump <line[:col]>`
- `tab-size [n]`
- `indent-width [n]`
- `soft-tabs [on|off]`
- `vsplit [path]`
- `hsplit [path]`
- `unsplit`
- `split-next`
- `split-prev`
- `split-first`
- `split-last`
- `tree-refresh`
- `sidebar-toggle`
- `sidebar-show`
- `sidebar-show git`
- `sidebar-toggle git`
- `sidebar-hide`
- `sidebar-close`
- `sidebar-width <n>`
- `focus <editor|sidebar|panel>`

Project state:

- per-project config and session files now live under `~/.local/state/microide/projects/<project-name>-<hash>/`
- each project gets a persisted default base accent color on first open; override it in the per-project `config` file with `project-base-color "#rrggbb"`

Diff benchmark:

- build `microide_diff_bench` and run `./build/microide/microide_diff_bench /path/to/repo path/to/file`
- example repro:
  `./build/microide/microide_diff_bench /home/pablo/Documents/projects/dolfin-app translations/locales/de/messages.po`
- the benchmark reports read, diff-build, and syntax-highlight timings separately and uses the same large-file syntax-highlight cutoff as compare tabs

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
