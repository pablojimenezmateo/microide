# Changelog

All notable changes to microide are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project aims to follow semantic versioning. microide is a stable, actively developed
project (see [README](README.md)); versions track meaningful shipped work.

## [2.5.0] - 2026-06-30

A **UI, session & packaging** release on top of 2.4.1: seamless session
restore, friendlier tab context menus, a runtime window icon, sharper resize
affordances, and a Nix flake for reproducible builds. The session format gains
additive fields only — old session files still load — and there are no
plugin-API breaks.

### Session
- Seamless restore: per-buffer scroll position is now honored on reopen instead
  of snapping back to the caret, via an authoritative
  `TextViewport::ApplyRestoredViewState` (cursor/selection first, scroll last).
- File-tree state is now persisted and restored: expanded/collapsed folders,
  the selected node, sidebar scroll row, and the active sidebar view. New
  session-record fields are additive (tags 15–19); older session files decode
  with empty tree state.

### UI
- Tab context menus now lead with copy-path and trail with close, putting the
  common action first.
- The application window icon is set at runtime via `SDL_SetWindowIcon`.
- Precise resize handles with consistent mouse hover affordances across the
  shell.

### Packaging
- Add a Nix flake for reproducible build, run, dev shell, and test.

## [2.4.1] - 2026-06-29

A small **fixes & presentation** patch on top of 2.4.0. The git sidebar's
per-row actions move to a right-click context menu, cursor-shape changes now
show on idle Wayland compositors, and the repository gains a generated showcase
gallery (screenshots + hero demo video) surfaced from the README. No
persisted-format or plugin-API breaks.

### Fixes
- Git: replace the inline per-row buttons in the sidebar with a right-click
  context menu, decluttering the change list.
- Cursor: surface cursor-shape changes on idle Wayland compositors that
  previously coalesced the update away.

### Docs & media
- Ship a generated showcase gallery and hero demo video under `docs/media/`,
  produced by `tools/capture-media.sh` and regenerated every release.
- Embed the hero shot in the README and point the former "no screenshots" notes
  at the [project site](https://pablojimenezmateo.github.io/microide/).

## [2.4.0] - 2026-06-29

A **performance & correctness** cycle. The bulk of this release is a sustained
deep-review pass that drives allocation out of the editor, render, and terminal
hot paths, swaps the document model to a piece tree for fast mid-file edits, and
gates the glyph atlas on the GPU renderer. The debugger is surfaced as a
first-class feature and several correctness defects (LSP encoding, whitespace
diffing, stale completions) are fixed. No persisted-format or plugin-API breaks.

### Editor & debugger
- Debugger is now a first-class feature surfaced in the shell (`PluginHost`
  decomposed along the way).
- Project-scoped editor font size, persisted per project.
- Document storage routed behind an `editor::TextBuffer` seam, now backed by a
  **piece tree** that beats the previous vector model on mid-file edits; large
  files load directly past the old split/rejoin round-trip.

### CLI
- New `--version` / `-V` flag prints the version (`microide <x.y.z>`) and exits.

### Fixes
- LSP: negotiate UTF-8 position encoding (correct multi-byte column mapping).
- Compare: honor `ignore_whitespace` inside changed hunks.
- Assist: drop stale LSP completion / code-action responses.
- Plugins: bound provider-query harvest loops to the raw array spine.
- App: map the startup window once to kill the black-flash double-popup.

### Performance
- Allocation-free editor, render, and terminal hot paths (search-match cache,
  multi-caret undo capture, per-row layout, SGR parsing on the reader thread).
- Incremental buffer-local find and bounded-head highlight reads — no whole-doc
  snapshots; off-thread checkpoint backfill removes the first-paint syntax freeze.
- GPU-gated, row/gutter-batched glyph atlas; glyph texture cache bounded by a VRAM
  budget; render gates on the reported SDL renderer backend.
- Skip building the git sidebar view model when hidden; O(1) file-finder recents
  and narrowed candidate set on forward typing; caret-window-only blame snapshots.
- Coalesce search wakes; bound `RunSubprocess` with an optional timeout and cap
  format-on-save; copy trace labels only when tracing is enabled.

### Build & tooling
- Shared `microide_core` object library plus a shared PCH (~15% faster test
  build); ccache / ld.lld / split-dwarf auto-used when present.
- Untrack regenerable perf fixtures and harden `.gitignore`; advisory GPU renderer
  lane and sustained-scroll scenario added to the perf harness.

## [2.3.0] - 2026-06-27

Substantially widens the **plugin rendering surface**. Plugins now move onto a dedicated worker
thread (off the UI thread) and contribute rich editor and presentation content under a strict
host-renders-data model: plugins emit size-capped, validated *data* and the host owns all drawing,
input, and lifecycle. Every new surface is zero-cost when unused.

### Plugins
- **Editor decorations** (`ctx.decorations`): per-file text styles (recolor, background, underline,
  strikethrough, bold/italic, whole-line), gutter marks (built-in icon shapes with color/priority),
  end-of-line inline text (Error Lens / blame style), and clickable code lenses (end-of-line or an
  above-line inset strip via `plugins.code_lens_above`).
- **Content surfaces** (`ctx.surface`): standalone charts/previews drawn from a structured display
  list (`rect` / `line` / `polyline` / `text` / clip ops) or a raster image (PNG/JPEG/RGBA8, decoded
  off-thread and texture-cached), shown in a bottom/side panel tab or anchored inline (experimental,
  gated by `plugins.inline_surfaces`), with clickable hit regions that dispatch host commands.
- **Ghost-text inline suggestions** (`ctx.editor.set_ghost_text` / `clear_ghost_text`): Copilot-style
  caret-anchored dimmed proposals with host-owned lifecycle (Tab accepts, Esc dismisses); gated by
  `plugins.ghost_text`.
- **Host-owned buffer edits** (`ctx.editor.apply_edits`) plus **reactive editor events**
  (`on_buffer_change`, `on_cursor_move`, `on_selection_change`, `on_buffer_close`) for live linting
  and paired-edit workflows.
- **Language providers**: plugin-native go-to-definition, find-references, signature help, and
  document symbols (`ctx.definition` / `ctx.references` / `ctx.signature_help` /
  `ctx.document_symbols`) alongside LSP.
- **Presentation contributions**: color themes (`ctx.themes`), file-icon themes (`ctx.file_icons`),
  and rich status items with icon, tone, click command, and live progress (`ctx.status`).
- **Tree sidebars**: sidebar snapshots may return collapsible nodes (`depth` / `collapsible` /
  `collapsed`) with plugin-owned expand/collapse state via `on_toggle`.
- Execution model: all `lua_State` access runs on a dedicated plugin worker thread behind a
  UI-owned snapshot/mailbox boundary, with a per-call watchdog and non-blocking reload.
- New repo-owned dogfood plugins: `eol-annotations`, `surface-preview`, `presentation-demo`,
  `language-tools` (plus `todo-highlight`), each exercising the same narrow host APIs as user plugins.

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
