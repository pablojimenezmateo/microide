# microide

A native, low-footprint C++/SDL3 desktop IDE focused on built-in editor, diff, merge, git,
search, and terminal workflows. Single-window, keyboard-first, runs without GPU acceleration.

For the authoritative in-scope / non-goal list see `openspec/specs/product-vision/spec.md`.

> **Status: experimental.** Tagged `v1.1.0` (see [CHANGELOG](CHANGELOG.md)), but no signed binaries
> and no third-party comparative benchmarks. Build from source or package locally. Expect rough
> edges. Read [Known Limitations](#known-limitations) and
> [Security & Trust Model](#security--trust-model) before using on a real project.

## Start Here

- [What microide is](#about)
- [Status: experimental](#experimental-release-status)
- [Current UI preview](#current-ui-preview)
- [What works today](#what-works-today)
- [Build or package locally](#build)
- [Known limitations](#known-limitations)
- [Plugin trust warning](#security--trust-model)
- [Performance methodology summary](#performance--benchmark-methodology)
- [Deeper docs](#companion-docs)

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
- Multi-project tabs, file tabs, nested shared-buffer splits, deferred-commit tab drag with ghost
- UTF-8 codepoint boundaries, IME preedit, line-ending detection and preservation
- Multi-caret editing with position remap, region-stack highlighting, and copy-with-context
- Soft word wrap with hanging indent; long-method fold resolution
- Syntax highlighting with per-file checkpointed state (fast random jumps in large files)
- Undo/redo storing line-range patches rather than full-buffer snapshots; word-level undo coalescing
- Durable writes with a save-time conflict guard and non-blocking external-change banner
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
- Editable commit message; branch/commit ref picker for compare and review
- Commit picker overlay for compare target selection

### Search
- Parallelized project-search sidebar: literal (default) or regex, case control, hidden-file toggle
- Count-all totals with match highlighting; replace-in-project in literal mode
- File finder overlay with cached index
- Standalone `microide_search_bench` for repeatable timing

### Terminal
- PTY-backed terminal tabs with scrollback and selection
- Alternate-screen support, application cursor-key mode, origin mode, autowrap, bracketed paste
- OSC 52 clipboard copy, focus notifications, basic device/cursor query replies
- Terminal text selection, copy, and paste shortcuts
- Tab drag reordering; right-click for "Copy Last Command + Output"

### Plugins
- Manual Lua 5.4 plugins from `~/.config/microide/plugins/`
- Lifecycle hooks, commands, sidebars, diagnostics, hover providers, syntax contributions
- Host-owned registries: settings, keybindings, status items, menus, formatters, save participants,
  completion providers, code actions, tests, SCM, auth, annotations
- `plugins-reload` command; file-watch–triggered asset reload on Linux
- Repo-owned dogfood examples: `plugins/eslint` (diagnostics)

## Scope

In-scope and non-goals are declared in `openspec/specs/product-vision/spec.md`.

Short version: built-in editor, diff, merge, search, git, and terminal workflows stay host-owned.
Out of scope: debugger/DAP support, plugin marketplaces, cloud/collaboration/sync, recent-project surfaces.

The strongest, most validated workflow today is the **native diff / merge / git workstation**:
compare tabs (working-tree vs HEAD, arbitrary commits, outgoing base-branch files), three-way
merge with whole-side apply, shared decorated-text-grid rendering across editor / compare / merge,
hunk navigation, and a standalone `microide_diff_bench` for repeatable timing. Other surfaces work
but are less proven outside the developer's own machine.

Current validation flow is still intentionally narrow and practical:
`open repo -> inspect changes -> diff files -> resolve merge conflict -> stage/commit`.

## Experimental Release Status

- Tagged `v1.1.0`, but no signed binaries are published. The supported paths today are: build from
  source or create a local Debian package from this repository. See [CHANGELOG](CHANGELOG.md) for
  what shipped.
- No screenshot or demo gallery is committed yet. That is deliberate for now: the UI is still
  changing quickly enough that stale marketing images would be less honest than current workflow
  docs.

## Current UI Preview

- Screenshot/demos are intentionally pending while the UI is still unstable.
- When the shell visuals and layout settle, this section will include a captioned screenshot:
  "Current experimental UI; layout and visuals are not stable."

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

- LSP transport: implemented and tested against fake servers; real-world server validation is
  ongoing
- Tool downloader / SHA verification: implemented, not exercised against production tool catalogs
- Native file-watch backend: Linux `inotify` is wired into `FileIndex`; project search and file finder consume index snapshots
  instead of rescanning on each refresh. First-load indexing and large watcher bursts can still
  show refresh lag in large repositories.

## Git Workstation Workflow

Current validated flow:

1. Open a local repository (`microide /path/to/repo`) and switch to the **Git** sidebar.
2. Inspect grouped sections: **Conflicts**, **Staged**, **Unstaged**, **Untracked**, **Outgoing**.
3. Open unstaged or staged diffs from sidebar rows (or from compare tabs), then stage/unstage by
   file, hunk, or selected lines where text mapping is available.
4. Use discard actions only after confirmation/preview prompts.
5. Open conflicted files into the merge tab, resolve hunks, then stage resolved files.
6. Open commit workflow from the Git sidebar, verify staged summary, write subject/body, and
   commit.
7. On commit failure, read status/output feedback, fix the issue, and retry without losing draft
   text.

Known workflow boundaries in preview:

- Binary, submodule, and some complex rename/file-directory conflicts are recognized but not fully
  interactive in the three-way merge UI.
- Patch staging for hunk/selected-lines can fail when the diff is stale; refresh and retry.
- Branch review markers are preview scope and should not be treated as a durable review database.

## Known Limitations

Honest list of what this is not, or what is unfinished. Read this before adopting microide for
serious work.

- **No signed binaries.** Releases are git-tagged (`v1.1.0`), but build from source or package
  locally; no signed binaries are published.
- **No comparative benchmarks.** Internal baselines compare microide against itself; the project
  has not been measured against VSCode, Zed, Helix, or any other editor. Claims like "fastest" or
  "lower CPU than X" are not supported here and are not made.
- **Byte-oriented text model.** The editor handles UTF-8 at codepoint boundaries for cursor
  movement and IME, but the underlying storage is `std::vector<std::string>` line-by-line. Very
  large files past a few MB will get slower; large-file thresholds are still under measurement.
- **Multi-caret has a known gap.** Per-caret selection-range surround now works for
  single- and multi-line ranges when carets are set up with `AddSecondaryCaretWithRange`
  or add-at-match promotion; mouse-driven multi-line block selections on secondary
  carets are not yet exposed.
- **No sandbox for plugins.** Plugins are trusted local code. See
  [Security & Trust Model](#security--trust-model).
- **Preview-safe startup only.** `--disable-plugins` and `--safe-mode` skip user-scope plugins
  and (for safe mode) workspace/session restore. These are recovery/trust aids, not a sandbox.
  Opening a repository still does not load plugin code from that repository; only user-installed
  plugins under `~/.config/microide/plugins/` run when plugins are enabled.
- **Single-window only.** No detached OS windows. This is deliberate (see
  `openspec/specs/product-vision/spec.md`), not a bug.
- **No native OS menu bar.** The menu bar is rendered by the app.
- **Terminal escape coverage is "what real shells need," not exhaustive.** Programs that depend on
  uncommon DEC/xterm sequences may render incorrectly.
- **Linux-only.** Linux is the only supported host. macOS and Windows are not supported build
  targets; building and running on them is unsupported.
- **No debugger/DAP support.** Debugging is out of scope unless a dedicated phase is opened.
- **No recent-project / recent-file UI.** Deliberate non-goal.
- **No plugin marketplace, remote install, or signed-plugin verification.** Deliberate non-goal.

If you find a bug or a limitation that is not listed, that itself is a bug — please file it.

## Security & Trust Model

microide is a local desktop application. It does not, today, implement any meaningful sandbox
for plugins or for code it executes on your behalf. Treat it accordingly.

**Plugins are trusted local code.** When you open a project, microide loads Lua plugins only from:

- `~/.config/microide/plugins/<plugin-id>/init.lua` — user-scope plugins

Project-local directories such as `<project-root>/.microide/plugins/` are ignored. That prevents
cloned repositories from executing plugin code just because you opened them.

User-scope plugins run with the same privileges as your microide process. Through the host
API a plugin can:

- read and write project-relative files (`ctx.files.read_text` / `write_text` / `exists`)
- run arbitrary subprocesses with argv, cwd, stdin, and environment overrides
  (`ctx.process.run`, `ctx.process.run_async`)
- register language servers whose argv is then launched by the host
- contribute diagnostics, sidebars, status items, code actions, and save participants that fire
  on every save

There is **no allowlist, no signature check, no capability prompt, and no per-plugin
namespacing of filesystem access**. The embedded Lua runtime is configured with a narrow
stdlib subset (`base`, `table`, `string`, `math`, `utf8`, `package`) — it does not expose
`io` or `os`, so plain Lua cannot directly open arbitrary files or shell out. The host API,
however, gives plugins exactly those capabilities through `ctx.files.*` and `ctx.process.run`,
and `package` still permits `require` plus Lua-module path resolution. Only install plugins you
trust into `~/.config/microide/plugins/`.

**Recommendations until that changes:**

- Treat user-installed plugins as equivalent to running arbitrary local code with your editor
  privileges.
- Only copy or symlink plugins into `~/.config/microide/plugins/` when you trust their source.
- The `plugins-reload` command picks up changes; there is no per-plugin disable in the UI yet
  beyond editing user config or starting with `--disable-plugins` / `--safe-mode`.
- Project-local plugin loading remains out of scope. See [SECURITY.md](SECURITY.md) and
  [dev-docs/project/git-workstation-preview.md](dev-docs/project/git-workstation-preview.md) for preview scope.

**Out of scope.** A meaningful plugin sandbox (capability-scoped APIs, restricted Lua standard
library, per-plugin allowlists) is not planned for the immediate roadmap. If a plugin marketplace
or remote install flow is ever pursued — currently a deliberate non-goal — a sandbox would have
to land first.

For the full plugin trust documentation see [guidelines/plugin-trust-model.md](guidelines/plugin-trust-model.md).

## Performance & Benchmark Methodology

microide ships a perf harness with committed baselines. The harness is reproducible locally; it
does not currently produce numbers that can be compared to other editors.

What is measured:

- **Startup** — `cold_startup_no_project`, `cold_startup_small_project`, `cold_startup_large_project`
- **Editing throughput** — `typing_small_file`, `typing_large_file`, `scroll_large_file`,
  `multi_tab_cycle`
- **Search / index** — `project_search_literal`, `project_search_regex`, `search_first_result`,
  `file_finder_cold`
- **Shell surfaces** — `compare_tab_open`, `merge_tab_open`, `compare_scroll_large_fixture`,
  `merge_scroll_large_fixture`, `git_sidebar_activate`
- **Repo-open memory** — `repo_open_rss_idle` (asserts a steady-state RSS budget after open)
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
- large-file open-to-first-paint is not yet a dedicated gate scenario. Advisory scenario
  `large_file_open_first_paint` exists for explicit local runs, but the gated suite is still
  stronger on typing, scrolling, save normalization, and compare/merge interaction than on the
  initial-paint path for very large files.
- behavior on GPU-accelerated paths. The reference harness uses the software renderer.
- multi-host comparison. `perf-runner-v1` is one self-hosted runner class; results from other
  machines are advisory only.
- LTO as a proof that extraction costs are gone. LTO currently helps recover some
  cross-translation-unit optimization loss from editor extractions, but any remaining
  sticky-scroll/render-path regression should be profiled directly. Do not treat LTO as proof
  the extraction is free.

Quick local run:

```bash
cmake --preset microide-perf
cmake --build build/microide-perf-make -j8
xvfb-run -a ./build/microide-perf-make/microide/microide_perf --smoke
```

Full docs: [`dev-docs/performance/perf-harness.md`](dev-docs/performance/perf-harness.md),
[`dev-docs/performance/runtime-profiling.md`](dev-docs/performance/runtime-profiling.md),
[`dev-docs/performance/startup-tracing.md`](dev-docs/performance/startup-tracing.md).

## Companion Docs

- [`CHANGELOG.md`](CHANGELOG.md) — release history
- [Project site](https://pablojimenezmateo.github.io/microide/) (GitHub Pages; `docs/` in the repo)
- [`dev-docs/README.md`](dev-docs/README.md) — developer documentation index
- `dev-docs/project/active-work.md`
- `dev-docs/project/implementation-guide.md`
- `dev-docs/project/known-tech-debt.md`
- `guidelines/plugin-trust-model.md`
- `dev-docs/performance/perf-harness.md`
- `dev-docs/performance/runtime-profiling.md`
- `dev-docs/performance/startup-tracing.md`
- `openspec/specs/product-vision/spec.md`

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

Local Debian package:

```bash
./scripts/package-deb.sh
sudo ./scripts/install-deb.sh
```

The package installs `microide` into `/usr/bin`, shared assets into
`/usr/share/microide/assets`, and desktop-launcher metadata into the standard
XDG application and icon locations.

Platform-specific setup, dependency install, and bring-up notes live in dedicated docs:

- [dev-docs/platform/linux-build.md](dev-docs/platform/linux-build.md) — Ubuntu/Debian, including building SDL3/SDL3_ttf from source
- [dev-docs/platform/host-platform-bringup.md](dev-docs/platform/host-platform-bringup.md) — local build, launch, and focused host-facing regression slice

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
- `mark-branch-file-reviewed`
- `mark-branch-hunk-reviewed`
- `clear-branch-review-state`
- `branch-review-note <text>`
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
See `dev-docs/performance/startup-tracing.md` and `dev-docs/performance/runtime-profiling.md` for full workflows.

## Plugin Runtime

- Safe startup (preview): `microide --disable-plugins` or `microide --safe-mode [project-path]` —
  see [SECURITY.md](SECURITY.md) and [dev-docs/project/git-workstation-preview.md](dev-docs/project/git-workstation-preview.md)
- User plugins: `~/.config/microide/plugins/<plugin-id>/init.lua`
- Repo examples: `plugins/` (copy or symlink into the user plugin directory)
- Entry point: `return require("microide").plugin({...})`
- Lifecycle hooks: `setup`, `on_project_open`, `on_project_close`, `on_buffer_open`, `on_buffer_save`, `shutdown`
- Host API: `ctx.log`, `ctx.commands.add`, `ctx.sidebar.add/show`, `ctx.workspace.*`,
  `ctx.files.*`, `ctx.process.run`, `ctx.diagnostics.*`, `ctx.hover.add`,
  `ctx.settings.*`, `ctx.menus.add`, `ctx.keybindings.add`, `ctx.status.*`,
  `ctx.formatters.add`, `ctx.save_participants.add`, `ctx.completion.add`,
  `ctx.code_actions.add`, `ctx.tasks.add`, `ctx.tools.add`, `ctx.tests.add`,
  `ctx.scm.add`, `ctx.annotations.add`, `ctx.auth.add`
- Syntax: `syntax/*.lua` inside the user plugin directory, loaded at startup and on `plugins-reload`

## License

microide is released under the [MIT License](LICENSE).
Copyright © 2026 Pablo Jiménez Mateo.
