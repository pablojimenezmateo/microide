# Changelog

All notable changes to microide are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project aims to follow semantic versioning. microide is a stable, actively developed
project (see [README](README.md)); versions track meaningful shipped work.

## [2.2.0] - 2026-06-23

Focused follow-up to the 2.1.0 navigation work. The **welcome surface** becomes
state-aware, the **command palette** absorbs the standalone command prompt to
become the single command surface, and a theme-switch repaint bug is fixed.

### Shell & navigation
- The welcome surface is now state-aware: a cold-start variant (Open Folder +
  recent projects) when no project is open, and a project-home variant (project
  name, recent files, New File / Open File / Find in Project) when a project is
  open with no editor tab. The duplicated command-palette hint is removed.
- The command palette (Ctrl+Shift+P) is now the single command surface: its
  query doubles as a command line, so queries with arguments or no fuzzy match
  run through the shared command-line executor (e.g. `colorscheme dark`), and
  Tab completes command/path tokens. The separate Ctrl+E command prompt and its
  bottom-panel command-mode UI are retired; the native-picker fallback opens the
  palette pre-filled instead.
- Editor tabs gain a "Copy Absolute Path" action.

### Theming
- Switching color theme via the command palette or keybinding (`toggle-theme` /
  `colorscheme <name>`) now repaints the whole window immediately, instead of
  leaving stale colors until the next unrelated event.

### Internal & tests
- Retire the lingering "command prompt" naming across the executor coordinator,
  state, and test accessors now that the surface is gone.
- Make the real-gdb function-breakpoint E2E test deterministic under CPU load,
  and fix a flaky `ProjectBackgroundExecutor` shutdown test.

## [2.1.0] - 2026-06-22

Feature release focused on **multi-view editing** and **navigation**. Editor tabs can now be
split into independent editor groups (right/down) with per-group tab strips, group-aware input,
and session persistence. A new searchable **command palette** (Ctrl+Shift+P) and **recent
projects/files** tracking make navigation faster, and the **welcome screen** is rebuilt into a
data-driven home surface. Rounding it out: a built-in **light theme**, a themed app icon, and
reverse-debugging support in the DAP integration.

### Editor groups & splitting
- Collapse legacy in-tab splits to a single viewport and introduce a first-class `EditorGroup`
  model, with per-group layout, render, and tab strips.
- Split/focus/close commands operate on editor groups, with group-aware keyboard input routed to
  the focused group.
- Split right / split down available from both the tab context menu and the file-tree context
  menu.
- Editor groups are persisted in session state and restored on reopen.

### Welcome / home surface
- Overhaul the welcome screen into a data-driven home surface with a bold single-card layout and
  fixed empty-state overlap.
- Recents on the home surface are clickable and correct, with a hand cursor and no color halo.

### Navigation & discovery
- Add a searchable command palette overlay (Ctrl+Shift+P).
- Track recent projects/files (MRU) and surface them in the file finder, backed by a new
  persistence record.

### Theming & branding
- Add a built-in light theme and a stronger selection focus bar.
- Add a themed two-tone "m" application icon with a hicolor multi-size icon set.

### Debugger
- Handle a late DAP capabilities event so reverse debugging is recognized when the adapter
  reports it after launch.

### Fixes
- Resolve the per-pane group viewport in split hit-test paths so clicks land in the correct
  group.

### Performance
- Dedup editor-group hot paths and harden group accessors.
- Cache the normalized focused path for per-pane path matching in the debug pane.

### Docs & tests
- Add a GitHub Pages showcase site for microide.
- De-flake fixed-wait timing races in search/subprocess tests and the control-socket self-heal
  test.

## [2.0.1] - 2026-06-20

Patch release adding agent-driven **review verbs** to the control channel. Three new commands
bulk-open the diff/merge tabs needed to review changes, switching to the Source Control view,
deduping against already-open tabs, and cleaning stale (clean) review tabs while preserving any
dirty/edited ones.

### Control channel
- `review-conflicts` — open one merge tab per conflicted working-tree file (non-mutating; pair it
  with your own `git merge`).
- `review-branch [ref]` — open one compare tab (working tree vs `ref`) per differing file; `ref` is
  any commit-ish and defaults to the repo base branch.
- `review-commit [commit]` — open one compare tab (`commit~1` vs `commit`) per file the commit
  changed; defaults to `HEAD`, accepts any commit hash.
- Verbs, recipes, and the generated man page document the new workflows; tab reconciliation is a
  pure, tested `ComputeReviewTabPlan` driven by a host-owned `ReviewSessionCoordinator`.

### Fixes
- Fix a stack-use-after-scope in the merge-tab conflict classifier (`string_view`s were bound to
  `SerializeLines` temporaries), caught by the new AddressSanitizer coverage.
- `tools/run-checks.sh` now folds sanitizer runtime reports into the main log so failures are
  captured in one place.

## [2.0.0] - 2026-06-20

Major release introducing an integrated **debugger**. microide now speaks the Debug Adapter
Protocol (DAP) end to end — breakpoints, stepping, call stacks, variable inspection, watches,
and multi-session debugging are first-class host surfaces, with gdb 17.2 wired up via a bundled
plugin. This release also adds an external **control channel** for headless and agent-driven
operation, plus a round of hot-path performance work. The 1.3.1 rendering fix below is included.

### Debugger (DAP)
- Host-owned DAP protocol client and `DebugSession` / `DapManager` / `DebugService` core, with a
  `ctx.debug.add` plugin seam and a bundled `gdb-dap` plugin for gdb 17.2.
- `debug.enabled` toggle, Start/Stop Debugging, and an always-visible Debug menu.
- Breakpoints with persistence: MATLAB-style gutter (yellow conditional, hollow disabled),
  conditional / hit-count / logpoint breakpoints, function breakpoints, and exception filters
  with conditions.
- Execution control: continue / step over / step into / step out, plus capability-gated reverse
  execution (`reverseContinue` / `stepBack`).
- Stopped-event handling with call stack, multi-thread support, and a session switcher for
  multiple concurrent sessions; restart and Stop All Sessions.
- Variables / Scopes panel with `setVariable`, a richer value tree, Locals open by default, and
  hover-to-inspect via `evaluate`.
- Debug surfaces moved to a dedicated right-side pane: debug toolbar, watch panel, structured
  console REPL, launch-config picker, and precise pane hitboxes with in-buffer cursors.
- Robustness: launch-ordering fixes for gdb 17.2, async stale-apply guards, breakpoint
  verification feedback, dead-adapter teardown, and a `terminated` broadcast on every session end.

### Control channel
- External control channel over a live AF_UNIX socket plus a cold-start `--control-spec` path.
- Headless, deterministic, observable agent-driving entry point, with a one-shot control-send
  client and an agent-drivable debug runbook.
- Notification toasts wired to real shell events.

### Performance
- O(1) settings store and a debug-off fast path.
- Dropped per-key allocations, redundant probes, and duplicate work across the DAP/LSP JSON paths.
- Killed hot-path allocations in editor/project and git paths; deduped transparent hashing.

### Fixes
- Stop a large-tree project file monitor from freezing the UI.
- Render caret and selection in the launch-config picker query field.
- Snap glyph-texture origins onto the physical pixel grid so text stays crisp under fractional
  display scaling (also released as 1.3.1 below).

## [1.3.1] - 2026-06-17

Patch release fixing blurry text on centered overlays under fractional display scaling.

### Rendering
- Snap glyph-texture origins onto the physical pixel grid so NEAREST-sampled text stays 1:1 with
  the device under fractional display scales (e.g. 125%). Fixes soft/blurry glyphs on the
  Help/About and Settings overlays; the editor (already grid-aligned) and integer scales are
  unchanged.

## [1.3.0] - 2026-06-17

Closes out the remaining open editor/folding/project topics, then does a deep documentation pass:
the closed tech-debt history is moved out of the known-debt journal into a dated archive, and the
public-facing and dev docs are refreshed.

### Editor
- Multi-caret brace-split on newline: pressing Enter now fans the single-caret brace-split geometry
  across every caret.
- Stop bogus fold markers on Markdown prose.

### LSP & Project
- Keep the language server warm across project-tab switches.
- Unblock project switch/open stalls.

### Internal
- Architecture size caps now count source lines (SLOC), with the duplicated line counters deduped;
  fix a headless-test flake.
- Docs: archive the closed tech-debt history under `guidelines/tech-debt/archive/` and trim the
  known-debt journal to open items only; refresh README, ROADMAP, active-work, and the release
  checklist; repoint references that named now-archived debt sections.

## [1.2.1] - 2026-06-16

Incremental release building on 1.2.0 with a file-tree convenience action and the standard release
procedure committed to the repo.

### Editor
- Add a "Show in File Explorer" file-tree context-menu action.

### Internal
- Docs: add the mandatory standard release procedure to the release checklist.

## [1.2.0] - 2026-06-16

Builds on 1.1.1 with a per-plugin capability sandbox, kernel-confined language servers, a
color-independent glyph-cell render path, and a round of cross-subsystem correctness and footprint
work.

### Plugins
- Enforce a per-plugin capability sandbox so contributed code runs against an explicit grant set.
- Kernel-confine contributed language-server processes.

### Rendering
- Add a color-independent ASCII glyph-cell atlas on the composite-on-miss path.

### Terminal
- Close deferred terminal debt: T3 split, T5a move-swap, and an output fuzzer.

### Internal
- Deep pass: dedup, correctness, and footprint improvements across render, app, util, and terminal.
- Add a headless Initialize/Render/Shutdown app lifecycle test.
- Docs: drop experimental status, retire the Git Workstation "Preview" naming, archive the
  `expand-git-diff-merge-perf-gates` change, and close R5a glyph-cell atlas tech debt.

## [1.1.1] - 2026-06-15

Incremental release building on 1.1.0: a centralized LSP backbone with more bundled language
servers, host-owned notifications, plugin enable/disable persistence, and shell polish.

### LSP
- Centralize the JSON-RPC codec, extract a dedicated `LspService`, and route all server traffic
  through a single I/O thread.
- Add clangd and .NET server enablers plus bundled `cpp-lsp` and `dotnet-lsp` plugins; refresh the
  `typescript-lsp` plugin.

### Notifications
- Add a host-owned `NotificationService` for transient, auto-dismissing toast messages that
  built-in code and plugins can post; the shell schedules a single wake at the next expiry rather
  than polling.

### Plugins
- Decompose provider registration into focused registration parsers and query interop (remove
  `PluginLuaProviderRegistrationInterop`).
- Persist per-plugin enable/disable state (`disabled_plugin_ids`) across sessions, surfaced in the
  Settings overlay.

### UI & Shell
- Settings: opaque selection highlight so the editor no longer ghosts through the overlay.

## [1.1.0] - 2026-06-14

First tagged release. Builds on the 1.0.0 baseline with editor, diff/merge, git, search, save,
and shell improvements.

### Editor
- Multi-caret position remap with region-stack highlighting and smarter copy-with-context.
- Coalesce typed runs into word-level undo steps.
- Suppress occurrence highlight while actively typing; centralize caret moves.
- Correct soft-wrap caret row resolution and fold/scroll spans; add hanging indent.
- Resolve folds for visible long methods and unify the fold marker as a single `+`/`-` button.

### Diff & Merge
- Unify decorated-row assembly across editor, compare, and merge surfaces.
- Centralize intra-line underline assembly and git conflict-marker / collapsed-run helpers.
- Speed pass: intern diff lines as `string_view` in compare model build; drop dead tokenization
  in exact line-ops; large-file fallback round-trip coverage.
- Unblock horizontal scroll and stop the change marker overlapping line numbers.

### Git
- Restructure the source-control panel and add a branch/commit ref picker.
- Make the commit message editable; improve the source tree and commit flow.

### Search
- Parallelize project search with count-all and match highlighting.

### Save
- Durable writes, save-time conflict guard, and a non-blocking external-change banner.

### UI & Shell
- Deferred-commit tab drag with ghost and consistent behavior across all three tab types.
- Two-pane Settings overlay redesign; collapse the Help menu.
- Simplify the top menu bar and dedup it against Settings; replace a dropdown with dedicated buttons.
- Centralize chrome primitives and share single-line input behaviors.
- Wrap Help/About text and add a Settings/Help overlay scrollbar.
- Keep tree focus when opening a file from the sidebar; detach project-search results scroll from
  the active result.

## [1.0.0]

Baseline native single-window IDE shell (bumped from 0.1.0, not separately tagged): built-in
editor with multi-project/file tabs and shared-buffer splits, compare and three-way merge tabs,
git sidebar with staging and commit, async project search and file finder, PTY-backed terminal
tabs, and a Lua 5.4 plugin runtime with host-owned registries.
