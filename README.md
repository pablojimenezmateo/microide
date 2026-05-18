# microide

A native, low-footprint C++/SDL3 desktop IDE focused on built-in editor, diff, merge, git,
search, and terminal workflows. Single-window, keyboard-first, runs without GPU acceleration.

For the authoritative in-scope / non-goal list see `openspec/specs/product-vision/spec.md`.

> **Status: experimental.** No tagged releases. No third-party comparative benchmarks. Build from
> source. Expect rough edges. Read [Known Limitations](#known-limitations) and
> [Security & Trust Model](#security--trust-model) before using on a real project.

## About

microide is **100% vibecoded**: every source file, test, and document in this repository
was written by AI coding agents (a mix of Claude, GPT, and other tools) under human
direction. There is no hand-written code path. The repo is published as a real-world
experiment in agent-driven development — interesting to read, useful to build on, but
not pitched as production-ready software. Expect rough edges, expect the architecture
notes to reflect what the agents settled on rather than a hand-curated design.

This project does not claim to be the fastest or smallest editor. It claims to be a native,
responsive, single-window IDE with internal regression baselines for startup, typing, scroll,
diff, and search. See [Performance & Benchmark Methodology](#performance--benchmark-methodology)
for what is actually measured, and what is not.

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

The strongest, most validated workflow today is the **native diff / merge / git workstation**:
compare tabs (working-tree vs HEAD, arbitrary commits, outgoing base-branch files), three-way
merge with whole-side apply, shared decorated-text-grid rendering across editor / compare / merge,
hunk navigation, and a standalone `microide_diff_bench` for repeatable timing. Other surfaces work
but are less proven outside the developer's own machine.

## Experimental Release Status

- No tagged releases and no signed binaries. The supported path today is: build from source.
- CI workflows under `.github/workflows/` produce artifacts for branch validation, but those are
  not positioned as stable releases.
- No screenshot or demo gallery is committed yet. That is deliberate for now: the UI is still
  changing quickly enough that stale marketing images would be less honest than current workflow
  docs.

## What Works Today

Mature enough to use day-to-day on the maintainer's own work:

- editor: open / save / undo / redo, soft wrap, syntax highlighting with checkpointed state,
  multi-cursor (within a current set of limitations — see below), folding, indent guides, bracket
  match, auto-close / surround driven by a language contract, snippets, save normalization
- compare and merge tabs: working-tree vs HEAD, vs arbitrary commit, outgoing-base-branch files,
  three-way merge with per-hunk picks and whole-side apply, `[`/`]` navigation
- git sidebar: working-tree changes, staging / discard, conflicts open into the merge tab,
  outgoing-branch file view, commit-picker overlay
- project search: async, literal and regex, replace-in-project for literal mode, file finder
- terminal: PTY tabs, scrollback, selection / copy / paste, alternate screen, common ANSI paths
  needed by real interactive programs
- session restore across restarts; per-project config and accent color
- Lua 5.4 plugin runtime with host-owned registries

Shipped but with caveats (see [Known Limitations](#known-limitations)):

- LSP / DAP transports: implemented and tested against fake servers; real-world server validation
  is ongoing
- Tool downloader / SHA verification: implemented, not exercised against production tool catalogs
- Native file-watch backends: Linux `inotify`, macOS `FSEvents`, Windows `ReadDirectoryChangesW`
  exist; the watcher is not yet wired into project-search and file-finder call sites — those still
  fall back to directory traversal

## Known Limitations

Honest list of what this is not, or what is unfinished. Read this before adopting microide for
serious work.

- **No tagged releases.** Build from source. CI artifacts exist in
  `.github/workflows/` but no signed binaries are published.
- **No comparative benchmarks.** Internal baselines compare microide against itself; the project
  has not been measured against VSCode, Zed, Helix, or any other editor. Claims like "fastest" or
  "lower CPU than X" are not supported here and are not made.
- **Byte-oriented text model.** The editor handles UTF-8 at codepoint boundaries for cursor
  movement and IME, but the underlying storage is `std::vector<std::string>` line-by-line. Very
  large files past a few MB will get slower; large-file thresholds are still under measurement.
- **Multi-caret has a known gap.** Per-caret selection-range surround is partial; see the archived
  `editor-essential-capabilities` change `tasks.md`. Use single-caret surround when in doubt.
- **No sandbox for plugins.** Plugins are trusted local code. See
  [Security & Trust Model](#security--trust-model).
- **Single-window only.** No detached OS windows. This is deliberate (see
  `openspec/specs/product-vision/spec.md`), not a bug.
- **No native OS menu bar.** The menu bar is rendered by the app.
- **Terminal escape coverage is "what real shells need," not exhaustive.** Programs that depend on
  uncommon DEC/xterm sequences may render incorrectly.
- **Cross-platform is uneven.** Linux is the primary host. macOS and Windows have bring-up
  documented in `docs/host-platform-bringup.md`, but day-to-day validation happens on Linux.
- **Debugger UI is first-pass only.** Start/stop and output-channel plumbing only. Full debug
  surfaces are an explicit non-goal unless a dedicated phase is opened.
- **No recent-project / recent-file UI.** Deliberate non-goal.
- **No plugin marketplace, remote install, or signed-plugin verification.** Deliberate non-goal.

If you find a bug or a limitation that is not listed, that itself is a bug — please file it.

## Security & Trust Model

microide is a local desktop application. It does not, today, implement any meaningful sandbox
for plugins or for code it executes on your behalf. Treat it accordingly.

**Plugins are trusted local code.** When you open a project, microide loads Lua plugins from
both of these locations:

- `~/.config/microide/plugins/<plugin-id>/init.lua` — user-scope plugins
- `<project-root>/.microide/plugins/<plugin-id>/init.lua` — **project-scope plugins, loaded when
  you open that project**

Project-scope plugins run with the same privileges as your microide process. Through the host
API a plugin can:

- read and write project-relative files (`ctx.files.read_text` / `write_text` / `exists`)
- run arbitrary subprocesses with argv, cwd, stdin, and environment overrides
  (`ctx.process.run`, `ctx.process.run_async`)
- register language servers and debug adapters whose argv is then launched by the host
- contribute diagnostics, sidebars, status items, code actions, and save participants that fire
  on every save

There is **no allowlist, no signature check, no capability prompt, and no per-plugin
namespacing of filesystem access**. The embedded Lua runtime is configured with a narrow
stdlib subset (`base`, `table`, `string`, `math`, `utf8`, `package`) — it does not expose
`io` or `os`, so plain Lua cannot directly open arbitrary files or shell out. The host API,
however, gives plugins exactly those capabilities through `ctx.files.*` and `ctx.process.run`,
and `package` still permits `require` plus Lua-module path resolution. If you `git clone` a repository
that ships a `.microide/plugins/` directory and open it in microide, that plugin runs.

**Recommendations until that changes:**

- Treat opening a microide project as equivalent to running arbitrary code from that repository.
  This is also true of `Makefile`, `package.json` scripts, and `direnv` files in other editors,
  but microide makes the surface explicit and you should know it is there.
- For repositories you do not trust, inspect `.microide/plugins/` before opening, or open them in
  a VM / container.
- The `plugins-reload` command picks up changes; there is no per-plugin disable in the UI yet
  beyond editing the user / project config.

**Out of scope.** A meaningful plugin sandbox (capability-scoped APIs, restricted Lua standard
library, per-plugin allowlists) is not planned for the immediate roadmap. If a plugin marketplace
or remote install flow is ever pursued — currently a deliberate non-goal — a sandbox would have
to land first.

For the full plugin trust documentation see [docs/plugin-trust-model.md](docs/plugin-trust-model.md).

## Performance & Benchmark Methodology

microide ships a perf harness with committed baselines. The harness is reproducible locally; it
does not currently produce numbers that can be compared to other editors.

What is measured:

- **Startup** — `cold_startup_no_project`, `cold_startup_small_project`, `cold_startup_large_project`
- **Editing throughput** — `typing_small_file`, `typing_large_file`, `scroll_large_file`,
  `multi_tab_cycle`
- **Search / index** — `project_search_literal`, `project_search_regex`, `search_first_result`,
  `file_finder_cold`
- **Shell surfaces** — `compare_tab_open`, `merge_tab_open`, `git_sidebar_activate`
- **Terminal** — `terminal_scroll_long_output`
- **Idle behavior** — `idle_soak_30s` (asserts near-zero wake events at rest), `long_soak_8h`,
  `switch_and_idle`
- **Diff hot paths** — standalone `microide_diff_bench`
- **Search** — standalone `microide_search_bench`

How it is measured:

- isolated app-root: `XDG_CONFIG_HOME` / `XDG_STATE_HOME` / `XDG_CACHE_HOME` / `XDG_DATA_HOME`
  are redirected into a per-process tempdir so the developer's real config never leaks in
- fixed seed (`MICROIDE_PERF_SEED=1337`)
- software renderer hint (`SDL_HINT_RENDER_DRIVER=software`), fixed window size, dummy audio
- committed fixtures under `tests/perf/fixtures/`
- per-scenario JSON baselines under `tests/perf/baselines/`
- explicit "smoke" vs "gate" split: smoke covers fast regression signal, gate provides the
  reference baseline used by `--reference-runner=perf-runner-v1`
- report metadata records `runner_class`, `provenance`, and resolved SDL drivers; baseline updates
  must come from `provenance: reference` runs

What is **not** measured:

- microide's startup / memory / CPU vs VSCode, Zed, Helix, Sublime, or any other editor. The
  project has no third-party comparative numbers and does not publish any.
- resident memory after opening a real repository is not yet a committed gate metric. The harness
  records allocation counts broadly and logs RSS during `long_soak_8h`, but there is no
  first-class "open repo, assert memory budget" scenario yet.
- large-file open-to-first-paint and steady-state compare / merge scroll on large fixtures are not
  yet dedicated gate scenarios. Existing coverage is stronger on typing, scrolling, tab open, and
  standalone diff-model timing than on those remaining cases.
- behavior on GPU-accelerated paths. The reference harness uses the software renderer.
- multi-host comparison. `perf-runner-v1` is one self-hosted runner class; results from other
  machines are advisory only.

Quick local run:

```bash
cmake --preset microide-perf
cmake --build build/microide-perf-make -j8
xvfb-run -a ./build/microide-perf-make/microide/microide_perf --smoke
```

Full docs: [`docs/perf-harness.md`](docs/perf-harness.md),
[`docs/runtime-profiling.md`](docs/runtime-profiling.md),
[`docs/startup-tracing.md`](docs/startup-tracing.md).

## Build

Requirements:
- CMake 3.28+
- C++20 compiler
- SDL3 development package
- optional: SDL3_ttf for the real font backend

Default build:

```bash
cmake -S . -B build
cmake --build build -j8
./build/microide/microide
```

Platform-specific setup, dependency install, and bring-up notes live in dedicated docs:

- [docs/linux-build.md](docs/linux-build.md) — Ubuntu/Debian, including building SDL3/SDL3_ttf from source
- [docs/windows-build.md](docs/windows-build.md) — MSYS2 UCRT64 setup and Windows-specific notes
- [docs/host-platform-bringup.md](docs/host-platform-bringup.md) — short macOS / Linux / Windows package summary and focused host-facing regression slice

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
open <path>                         plugins-reload
project-close                       project-next
project-open [path]                 project-prev
project-search [query]              quit
reopen                              save
search <query>                      settings
sidebar-close                       sidebar-hide
sidebar-show [tool]                 sidebar-toggle [tool]
sidebar-width <n>                   soft-tabs [on|off]
split-first                         split-last
split-next                          split-prev
status-bar-toggle                   tab [path]
tab-size [n]                        tabmove <n>
tabswitch <tab>                     term [command]
tests-discover                      tests-run [test-id...]
tree [root]                         tree-refresh
ui-scale [n|up|down|reset]          unsplit
vsplit [path]                       wrap [on|off]
```

Current commands:
- `code-actions`
- `colorscheme [name|list]`
- `completion`
- `insert-snippet`
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
- `toggle-editor-bracket-match-highlight`
- `toggle-editor-auto-close`
- `toggle-editor-surround`
- `toggle-editor-smart-indent`
- `toggle-editor-toggle-comment`
- `toggle-editor-line-ops`
- `toggle-editor-sort-lines`
- `toggle-editor-add-cursor-at-match`
- `toggle-editor-occurrences-highlight`
- `toggle-editor-search-case-sensitive`
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

## License

microide is released under the [MIT License](LICENSE).
Copyright © 2026 Pablo Jiménez Mateo.
