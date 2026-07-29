# Changelog

All notable changes to microide are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project aims to follow semantic versioning. microide is a stable, actively developed
project (see [README](README.md)); versions track meaningful shipped work.

## [Unreleased]

A cross-subsystem review sweep, plus a UI/UX consistency pass over how the
surfaces answer the mouse and the keyboard. No public API or persisted-format
changes.

### Added

- **Double-click a resize divider to restore its default size.** Works on all
  six — sidebar, right/debug pane, editor split, bottom panel, and the compare
  and merge pane dividers. Either merge divider resets both, so one gesture
  recovers the layout.
- **Word and line selection in the terminal.** Double-click selects the word
  under the pointer, triple-click the whole row, matching the editor surface.
  Path/URL punctuation counts as part of a word, so double-clicking
  `src/foo/bar.cpp:42` in a build log selects the whole reference.
- **File > Open File… actually opens a file.** It (and its Ctrl+O accelerator,
  and the welcome screen's Open File action) now opens a native file picker;
  previously all three reported "open requires a path" and did nothing. Ctrl+K
  Ctrl+O for Open Folder, likewise advertised but unimplemented, now works.
- **Page/Home/End in the file tree**, which every other sidebar list already had.
- **Right-click menus on the search, problems and tests lists.** The file tree
  and git sidebar had them; the other three swallowed the right button, so the
  path of a search hit could only be copied by opening the file first. All three
  share one row menu: Reveal in File Tree, Show in File Explorer, Copy Relative
  Path, Copy Absolute Path.
- **Right-click menus in the debug pane.** Breakpoints rows open the same menu
  as the editor gutter (Disable/Enable, Set Condition…, Set Hit Count…, Set Log
  Message…, Remove); Variables and Watch rows offer Copy Value and Add to Watch.
  The pane was the last interactive list in the shell with no menu at all.
- **Keyboard scrolling in Help/About**, which swallowed every key but Escape. It
  answers the same Up/Down, Page Up/Down and Home/End contract as every other
  list, so a Help panel taller than the window is no longer mouse-only.
- **Page Up/Down and Home/End in Settings**, in both the category rail and the
  value list. Both moved one row at a time through lists hundreds of rows long.
- **Ctrl+PageDown / Ctrl+PageUp switch editor tabs**, wrapping at both ends.
  There was no key chord for this at all: tabs could be opened (Ctrl+P) and
  closed (Ctrl+W) from the keyboard, but moving between two open files needed the
  mouse. The typed `tabswitch` also takes `+n`/`-n` now, the form its sibling
  `tabmove` has always accepted.
- **Home/End in the menu bar and the context menus**, the last two lists that
  answered Up/Down and nothing else.

### Changed

- **Shift+wheel scrolls the editor sideways.** It already did in the compare and
  merge surfaces, so the same gesture moved sideways in a diff and downwards in
  the file being diffed.
- **The wheel no longer scrolls a tab strip past its last tab.** It clamped on
  the raw scroll index rather than on what was still hidden, so wheeling right
  ran on until one tab sat in an otherwise blank strip — a state the ⟨ ⟩ overflow
  buttons cannot produce. Affected the project, editor and panel strips.
- **The mouse wheel scrolls every list at the editor's speed.** Lists (file
  tree, git, search, debug pane, bottom panel, Settings, the overlay pickers)
  advanced one row per tick while the editor moved three, so the tree felt an
  order of magnitude heavier than the code beside it. All of them now share one
  step.
- **Scrolling no longer steals keyboard focus.** Wheeling over the sidebar, the
  panel or an unfocused editor pane used to hand focus to whatever was under the
  pointer, silently redirecting the next keystroke.
- **The wheel over a list overlay scrolls it instead of moving the selection**,
  so a scroll can no longer change what Enter is about to run. Only the project
  search overlay behaved this way before.
- **Home/End edit the query in every overlay that has one.** The command palette,
  commit picker and launch picker consumed them to jump the result list, leaving
  no way to reach the start of a typed command line. They still address the list
  in the two field-less popups (completion, code actions).
- **The debug pane is a first-class focus surface**: it draws the focus ring the
  other three surfaces draw, and Ctrl+Tab reaches it (the cycle previously
  skipped it, so it was clickable but not tabbable).
- **Every sidebar mode answers the same navigation keys.** Up/Down, Page Up/Down
  and Home/End resolved through six near-identical per-mode blocks that had
  already drifted: Home in the git sidebar could select a row hidden under a
  collapsed directory. They now share one resolver.
- **Escape peels one layer at a time in the sidebar.** It closes an auto-opened
  panel from every mode (the file tree and git sidebar previously ignored it),
  but no longer tears the whole search panel down while you are typing a query —
  that first Escape cancels the field edit, as it does everywhere else.

### Fixed

- **Every painted scrollbar can now be grabbed.** Three could not. The debug
  pane's had been painted since the pane shipped but never hit-tested, so it was
  decoration — and because the row hit test covered the whole content rect, a
  press on the bar activated the row behind it (in Breakpoints mode, navigating
  the editor to a random file). Help/About's was in the same state, with its
  scroll bound resolved inside the paint pass, so wheel scrolling did nothing
  until a frame had been drawn and used a stale bound after a resize. The
  font-picker dropdown's was the third; worse, because the dropdown floats over
  the settings scrollbar, clicking its bar jumped the rows behind it. An
  architecture lint now fails the build on a scrollbar that can never render as
  being dragged.
- **The resize cursor holds for the whole drag.** Only the sidebar and bottom
  panel kept their resize shape once the pointer left the divider; the right
  pane, editor split, compare and merge dividers flickered back to an arrow
  mid-drag. The merge dividers also had no widened grab margin, so they had to be
  hit within a glyph's width — compare's 12px margin now applies to all three.
- **Saving a file no longer leaks a phantom entry into the file finder.** Every
  save stages its bytes in a temp file beside the target and renames it into
  place; nothing filtered that temp, so a file-watcher batch landing inside the
  save window put it in the index, where it showed up in Ctrl+P and in project
  search results pointing at a file that no longer exists.
- **The control channel's client socket leaked into every spawned process.** It
  was created without close-on-exec, so terminal shells, LSP servers, DAP
  adapters, git, and plugin-launched tools all inherited a live connected handle
  to the interface that drives the editor headlessly.
- **Text handling no longer depends on the process locale.** Word boundaries,
  token classification, command parsing, and terminal escape parsing used
  `<cctype>`, which is locale-sensitive for bytes >= 0x80 — and SDL changes the
  process locale behind our back. JSON numbers were affected the same way: on a
  comma-decimal locale, doubles were written as `1,5` (malformed JSON to every
  LSP/DAP peer) and `1.5` failed to parse, silently resetting float settings and
  persisted pane splits to defaults.
- **Syntax highlighting no longer disappears on lines with invalid UTF-8.** A
  Latin-1 source file, or a single mis-encoded byte in an otherwise-UTF-8 file,
  silently rendered that entire line unhighlighted.
- **Case-insensitive regex search now folds Unicode case**, matching what literal
  search already did. Searching `Δ` finds `δ`, `É` finds `é`. ASCII queries keep
  the faster byte-oriented path.
- **Boolean settings honour `FALSE` and `no`.** A setting written as `FALSE` (any
  casing) or `no` read as *enabled* — the opposite of what was written.
- **Glyph rendering.** A character rendered wider than its measured advance could
  overwrite its neighbour in the glyph atlas, corrupting that character.
- **Reference snippets from large files** (Assist) returned extra unrelated lines
  and read the whole file instead of stopping at the requested range.

### Changed

- **Hover cards stop re-wrapping their text every frame.** Wrapping normalizes,
  tokenizes and measures the whole string, and ran several times per frame for as
  long as a card was on screen. It is now memoized on (text, width, line cap,
  font metrics).
- **The project-search sidebar stops rebuilding its empty-state line every
  frame** — two heap allocations per repaint on a surface that repaints on every
  search-progress tick.
- **Five paint paths stop allocating a string per label per frame.** Truncating a
  label to fit returned an owning string in the Settings overlay (three per
  visible row, plus six for the chrome), the debug pane (twelve sites), the
  sidebar, the hover card and the breadcrumb/prompt chrome, while the other
  render paths used the allocation-free variant. A lint keeps them aligned.
- **Much faster terminal output.** Runs of plain printable text are now written
  to the grid in bulk instead of one byte at a time through the full escape
  parser: 3-4x on build logs and compiler diagnostics, ~1.7x on mixed
  CJK/Unicode output.
- **Faster file finder.** Candidates are rejected on a character bitmask before
  the subsequence scan, cutting per-keystroke work 2.4-4x on a large project.
- **Faster diffs, again.** Lines are interned to integer ids before the LCS, so
  the table compares an integer per cell instead of re-running a full string
  compare: ~2x normally and ~9x with "ignore whitespace" enabled.
- **Faster editor scrolling and whole-file reads.** The document model memoizes
  the last line offset it resolved, roughly halving the tree lookups an
  in-order walk needs (~2x on viewport rendering and full-buffer scans).
- **Faster diffs.** Hunk alignment — the dominant phase of building a comparison
  — is ~39% faster on a diff with several thousand modified lines, via reused
  per-thread working buffers, a rolling-row token LCS, and skipping line-pair
  similarity work that cannot affect the result.
- **Faster whole-document operations.** Save, in-file find, replace-all, JSON
  formatting, compare review, and LSP sync now take the document straight from
  the piece tree in one walk, instead of first materializing one heap-allocated
  string per line.
- **Project indexing** does less work per filesystem entry (allocation-free path
  containment).

## [2.7.2] - 2026-07-19

A **search, compare, and editor** cycle on top of 2.7.1. Find & replace
gains full regex support in both the project-wide and in-file surfaces,
comparisons no longer require git, and a JSON formatter lands in the editor. A
focus click-through fix makes the first click on an inactive window land on its
target. No public API or persisted-format changes — a recommended upgrade for
all users.

### Added

- **Regex find & replace.** Both the project-wide and in-file find & replace
  surfaces accept PCRE2 regular expressions, with capture-group substitution in
  the replacement. A single shared engine backs both surfaces.
- **Non-git comparisons.** Compare arbitrary files, editor buffers, and the
  clipboard against each other — no git repository required. The review mode is
  sticky per session.
- **Format JSON command.** A new editor command reformats the active buffer as
  key-sorted, indented JSON in memory.

### Fixed

- **In-file regex across line breaks.** Multi-line regex matches (spanning `\n`)
  now resolve correctly in the in-file search surface, and matched newlines are
  visualized with an end-of-line marker.
- **Focus click-through.** Clicking an inactive microide window now both
  activates the window and delivers the click to the target underneath, instead
  of swallowing the activating click.

## [2.7.1] - 2026-07-19

A **performance, robustness, and hardening** cycle on top of 2.7.0 — the large
tech-debt burndown (`TD-2026-07-16/17` and the `TD-2026-07-17A` addendum) landed
end to end. Hot paths across editor, render, LSP/DAP, terminal, search, and
persistence are measurably faster on the reference runner; a broad "bound the
resource" sweep caps previously unbounded queues, buffers, and result sets; and
several real correctness and plugin-sandbox defects are fixed. The full suite
plus ASAN/UBSAN/TSAN stay green. No public API or persisted-format changes — a
recommended upgrade for all users.

### Performance

- **Off the shell thread.** Project-wide replace-all, forced file-index rescans,
  file-manager reveal, server-pushed `WorkspaceEdit` writes to closed files,
  syntax reload, and buffered PTY writes now run off the UI thread, so large
  operations no longer stall the frame. Shutting-down LSP clients retire to a
  host-owned pool instead of blocking.
- **Render view models.** Breadcrumb labels, git-sidebar entry labels and spans,
  settings edit-control values, output-reference paths, project-search line maps,
  bottom-panel tab models, and commit bodies are precomputed/memoized so paint
  stays allocation-free on the hot path.
- **Algorithmic rewrites.** Roughly a dozen quadratic paths became indexed or
  amortized (multi-caret result remap, snippet mirror shifts, deleted-directory
  stat sweep, soft-wrap edit caches now O(edit) in place, `PostLatest` dedup now
  O(1), Add-Cursor-at-All-Matches folds once and is capped). Replace-All and
  surround reuse existing matches instead of rescanning; merge/undo track applied
  edit spans rather than diffing the whole buffer.

### Fixed

- **Bounded resources.** A wide sweep caps in-flight and retained work that could
  previously grow without limit: LSP/DAP outbound queues (by retained bytes),
  the event drain, session-restore tab rebuilds, raster decode bytes, provider
  and code-action result sets, control-query responses, output channels, debug
  value trees, clipboard export, text-measurement, and more. Silent truncation
  now surfaces status where it matters.
- **Plugin sandbox.** Removed the Lua `loadfile`/`dofile` file loaders; embedded
  NULs and invalid env keys are rejected; the process sandbox is untied from
  filesystem capabilities; and provider results survive a single failing or
  slow provider instead of being dropped wholesale.
- **Filesystem & persistence safety.** Persisted-record writes rotate through the
  symlink target so a symlinked state file is followed, not clobbered; the
  control socket hardens its runtime dir before bind; the tool downloader does a
  strict `file://` decode.
- **Editor & workspace.** Dirty `CloseTab` prompts survive a tab close/reorder;
  background compare & merge tabs retarget or close on path mutation; ranged
  secondary caret anchors are preserved across shaping.

### Internal

- The test suite is sharded for parallel `ctest`, cutting sanitizer wall time
  substantially; new perf coverage gates the rewritten hot paths, with decoupled
  wall-clock vs. allocation baseline tolerances.

## [2.7.0] - 2026-07-15

A **correctness-hardening and tech-debt closeout** cycle on top of 2.6.8. Roughly
two dozen real defects were fixed across the editor, persistence, terminal,
git/diff/merge, LSP, DAP, and plugin subsystems — each with regression coverage —
while the full suite plus ASAN/UBSAN/TSAN stay green. The multi-batch deferred
tech-debt backlog is now fully triaged and closed, and several hot paths (syntax
highlight, git picker, compare, diagnostics) are measurably faster on the
reference runner. No public API or persisted-format changes — a recommended
upgrade for all users.

### Added

- **Terminal clipboard status.** When an oversized OSC 52 clipboard write is
  dropped, the terminal now surfaces a status instead of silently discarding it.

### Fixed

- **Workspace edits.** Both LSP appliers and `editor.apply_edits` now reject
  overlapping, beyond-EOF, and over-cap edits (including in closed files) rather
  than truncating or applying them partially; multi-caret apply paths refuse
  overlapping selections.
- **Editor & input.** Multi-line paste into single-line fields collapses to
  spaces; the mouse wheel scrolls the pane under the cursor; block-comment toggle
  is language-aware; a snippet linked-edit session drops on a multi-line insert
  and discards stale secondary carets.
- **Syntax highlighting.** `^`-anchored rules honor the true line start
  (`PCRE2_NOTBOL` on mid-line segments) so anchored patterns no longer match
  inside a line.
- **Persistence.** A corrupt primary state file is recovered-and-protected with a
  header-first bounded read instead of being trusted or clobbered.
- **Filesystem safety.** No-overwrite `RenamePath` closes the exists()-then-move
  race; symlinked save targets no longer risk data loss.
- **Git / diff / merge.** Merge delete-resolve is transactional; per-file search
  reads are capped; the status bar reflects in-session `git init` / `.git`
  removal; compare picker/ref git queries run off the UI thread.
- **DAP.** Function-breakpoint verification is bounded to the requested count;
  the stopped view is restored when the adapter rejects a resume; late
  thread-list / pause callbacks are state-guarded.
- **Status bar.** Plugin status items are stably ordered (`stable_sort`).
- **Rendering.** The renderer rejects non-finite or negative display-list content
  dimensions; the highlight prefetch worker is drained in the destructor.

### Performance

- Cross-subsystem speed sweep across syntax, LSP, editor, finder, and git.
- Syntax highlighting fast-paths empty skip masking.
- A dedicated git picker lane plus off-UI-thread compare/ref picker queries.
- O(1) debug sibling ordinal and a reused per-line visual-column map for
  diagnostic underline rects.

### Tech debt

- The multi-batch deferred tech-debt backlog (sweep batches A–W plus the residual
  triage tranches) is fully closed. Rationale for the won't-do items is recorded
  in `dev-docs/project/known-tech-debt.md`.

## [2.6.8] - 2026-07-10

A **correctness-hardening and polish** cycle on top of 2.6.7. Five further
cross-subsystem bug-hunt passes fixed real defects across the editor,
persistence, terminal, git/diff/merge, LSP, DAP, and plugin subsystems — each
with regression coverage — while the full suite plus ASAN/UBSAN/TSAN stay green.
This cycle also adds a user-facing **Reveal in File Tree** action, polishes the
Settings surface, closes the flagship plugin-metamethod tech-debt item, and
measurably speeds up the LSP wire-decode paths. No public API or format changes —
a recommended upgrade for all users.

### Added

- **Reveal in File Tree** — a new editor-tab context-menu item and
  `reveal-in-tree` command-palette command. It opens the sidebar on the Tree
  view, force-expands the file's collapsed ancestors, selects it, and scrolls it
  into view (VSCode "Reveal in Explorer" behavior).

### Changed

- **Settings surface polish.** The category rail now scrolls (wheel, keyboard
  reveal-on-navigate, draggable scrollbar) so the last category is no longer
  clipped on shorter windows. Every section renders a fixed header band, and the
  ~25-item ungrouped "General" catch-all is regrouped into named Editor /
  Appearance / Terminal / Diagnostics sections.
- **Faster LSP wire decode.** `semanticTokens/full` decode, `documentSymbol`
  outline parse, and `Content-Length` message framing are measurably faster on
  the reference runner this cycle (no baseline regressions elsewhere).

### Fixed

- **Path/URI security.** Strict `file://` parsing (local-authority-only, strict
  percent-decode, NUL reject); malformed LSP code-action/rename URIs no longer
  edit the active buffer; server `applyEdit` and plugin containment are confined
  to the project root and fail closed on canonicalization errors; watcher,
  batch-apply, and traversal filters reject `..`-escaping paths; exclusive
  (`O_EXCL`) file creation.
- **Split-view correctness.** Replace-all dirty-guard/reopen, compare/merge
  external-change invalidation, and control-channel tab listing now scan every
  editor group; file rename/delete propagates group-aware across all groups.
- **Editor & snippets.** Snippet backspace/delete honor UTF-8 and reject
  multi-line mirror inserts; click/drag horizontal-scroll double-count fixed;
  a failed file open no longer moves the caret in the previously-active buffer.
- **Terminal.** Correct scrollback-trim accounting for modern clear (ED2+ED3),
  `DECXCPR`/`CSI 6n` screen-relative row reporting, `CUU`/`CPL` scrollback climb,
  `DECSC`/`DECRC` and `DECOM` origin/scrollback handling, and soft-wrap flag
  preservation on hard LF.
- **Git / diff / merge.** Partial-stage warning under porcelain v2, compare
  changed-span double-dim, merge preview bottom clip, and copied compare patches
  are now real `git apply`-able diffs with git-quoted paths.
- **LSP / DAP.** Diagnostics use half-open ranges and honor the echoed version;
  `stopped`/`continued` honor optional `threadId`/`allThreadsContinued`;
  breakpoint verification keeps both requested and resolved lines; CRLF
  incremental `didChange` is re-encoded correctly.
- **Plugin host.** Metamethod-capable Lua field harvests are now performed
  under protection so a plugin-installed raising `__index` can no longer
  `longjmp` over live C++ destructors (closes the flagship deferred tech-debt
  item); a plugin-sidebar refresh use-after-free is fixed; harvest counts are
  clamped; editor position fields read at full double precision; raster
  dimensions are range-clamped before narrowing.
- **Robustness caps & validation.** RFC-8259 JSON grammar with control-char
  reject; snippet/env/decoration/MRU/debug-state decode caps; launch-config JSON
  and tool-SHA validation; atomic control-descriptor writes (temp+rename,
  0600/0700); protocol integer-range guards; non-throwing filesystem probes on
  UI paths; `O_CLOEXEC` on the durable-save staging fd and the control socket.

Deferred items (renameat2 no-replace, two-generation persisted backup,
cross-device move rollback, symlink-save root confinement, multi-file rename
atomicity, closed-file diagnostic re-encoding, plugin async-executor cluster,
and the merge/render perf batch) are recorded with rationale in
`dev-docs/project/known-tech-debt.md`.

## [2.6.7] - 2026-07-09

A **correctness-hardening and consolidation** cycle on top of 2.6.6. Eight
successive cross-subsystem bug-hunt passes fixed real defects across the editor,
persistence, terminal, git/diff/merge, LSP, and debugger subsystems; four
verbatim code duplications were consolidated into shared modules (net code
reduction); and the LSP/DAP performance surface gained deterministic,
baseline-gated regression coverage. No public API or format changes — a
recommended upgrade for all users.

### Added

- **Gated LSP performance scenarios** — deterministic micro-benchmarks over the
  language-server wire path: `semanticTokens/full` decode, `publishDiagnostics`
  parse, `documentSymbol` outline parse, and `Content-Length` message framing
  (`tests/perf/LspPerfScenarios.cpp`), each with a committed reference-runner
  baseline.
- **Promoted debugger/DAP performance scenarios** — the six pure-unit DAP
  micro-benchmarks (value-tree expand/rebuild/paging, protocol encode/decode,
  breakpoints-model rebuild, pane hit-test geometry) are now baseline-gated with
  committed baselines rather than advisory-only.

### Changed

- **Shared edit primitives consolidated.** Four byte-for-byte duplications folded
  into focused shared modules — filesystem ops (`platform/FsOps`), lexical path
  prefix replacement (`util/PathMatch`, dropping a per-path syscall on rename
  remap), terminal child-shutdown escalation and shell resolution
  (`platform/ShellProcess`), and the self-pipe wake + outbound-queue machinery
  shared between the LSP and DAP stdio clients (`util/WakePipe`,
  `workspace/StdioClientQueue`).
- **LSP/DAP transport parity.** The DAP client adopted the LSP client's
  bounded-acquisition shutdown so teardown can no longer stall behind an I/O
  thread blocked writing to a wedged-but-alive adapter; requests registered
  during a failed initialize handshake are now explicitly failed on both stdio
  transports instead of being silently dropped.

### Fixed

- **Multi-caret line-move allocation regression.** Grouping `move-line-up/down`
  for correct multi-caret redo had made the undo machinery deep-copy each grouped
  edit's line slices three times; a wide multi-caret line move now records the
  aggregate once, restoring allocation counts to parity (fixes a ~50% allocation
  and ~20% latency regression in the `editor_shaping_multi_caret` scenario).
- **Editor** — multi-caret redo keeps secondary carets and column; a caret
  round-trip breaks the typing-coalesce run; multi-caret copy/cut/paste aggregate
  every selection VSCode-style; snippet placeholder shifts stay in sync across
  delete/choice and lone-CR bodies; assorted caret, selection, and buffer-integrity
  fixes across the bug-hunt passes.
- **Persistence & patches** — patch/diff desync and corruption, and
  session/persistence data-loss edge cases.
- **Terminal** — scrollback, alt-screen, and VT-parsing correctness fixes; safe
  child fork/shutdown.
- **LSP/debugger** — LSP state drift, DAP hang/shutdown races, and line
  breakpoints that now shift correctly on edit.
- **Settings** — integer writes clamp to each spec's declared range at store time.

### Internal

- **Perf harness hardening.** Fixed a selection bug where a bare full
  `microide_perf` run aborted on an opt-in (`run_by_default = false`) scenario
  instead of skipping it; added per-scenario warmup iterations; and made the
  `search_first_result` scenario deterministic (it previously snapshotted a racing
  mid-search state, swinging ~80× run to run). Project search grew an optional
  worker-count cap (`MICROIDE_SEARCH_WORKER_LIMIT`, unset in production) and a
  non-consuming `WorkerFinished` completion signal so the harness can measure a
  settled, single-worker search reproducibly.

## [2.6.6] - 2026-07-08

An **LSP feature + performance** cycle on top of 2.6.5. It rounds out the built-in
language-server integration with a batch of new capabilities, moves the provider
model to a concurrent LSP-primary merge, and takes the per-keystroke serialization
work off the UI thread — all on top of a broad internal consolidation of the LSP
client.

### Added

- **Inlay hints** — mid-line virtual text (parameter names, inferred types) rendered
  in the editor grid, pulled on open/save/clean-undo (never per keystroke) and gated
  on `editor.inlay_hints.enabled` plus the server's `inlayHintProvider`.
- **Signature help** (`textDocument/signatureHelp`) surfaced into the caret-anchored
  popup as an LSP-primary source.
- **Project-wide symbol search** (`workspace/symbol`) via a `workspace-symbol <query>`
  command that renders navigable results into an output channel.
- **Go to type definition / implementation / declaration** for the symbol under the
  cursor.
- **Prepare-rename refinement** (the server's placeholder seeds the rename prompt) and
  **range formatting** (format the current selection, else the whole document).
- **Rename across unopened files** applies silently on disk (VSCode-style), and
  server-initiated `workspace/applyEdit` is now wired end to end.

### Changed

- **Concurrent LSP-primary providers.** Completion, code actions, go-to-definition, and
  find-references now fire the plugin worker and the language server at the same time
  and merge LSP-first, replacing the previous serial plugin-first path. Signature help
  and navigation are LSP-primary too.
- **Serialization is off the UI thread.** JSON-RPC message serialization — and the
  whole-document copy of a full-sync `didChange` — now runs on the per-server I/O
  thread instead of the calling thread, removing roughly 0.25–1 ms of per-keystroke
  UI-thread work on large files served over the utf-16 position encoding.
- **Fewer main-thread copies** parsing completion and code-action results (strings are
  moved out of the decoded response rather than copied).

### Internal

- Broad consolidation of the LSP client: the transport header was split into focused
  translation units, the `Content-Length` wire codec was extracted into a unit-tested
  `LspMessageFramer`, trace/tuning constants were hoisted, and repeated sync/guard and
  test-stub-dispatch boilerplate was deduplicated. Adds an opt-in real-clangd
  end-to-end harness and a deterministic `didChange`-serialization microbench. Clean
  under ASan/UBSan/TSan.

## [2.6.5] - 2026-07-05

### Added
- Shared overview-ruler lane painted left of the vertical scrollbar, used by the
  editor, compare, and merge surfaces to show diff/merge changes, search matches,
  diagnostics, and the caret at a glance. Enabled by
  `editor.view.overview_ruler.enabled` (default on).

### Fixed
- Reserve the overview-lane gutter in editor metrics so a long line's trailing
  glyphs never lay out under the lane.
- Rebuild baked-in overview-ruler marker colors on a live colorscheme switch
  (folded a theme token into the marker cache keys) instead of leaving them stale.
- Deterministic tie-break when equal-priority markers contend for a pixel
  (first writer wins by input order, not palette-insertion order).

## [2.6.4] - 2026-07-05

A **correctness and hardening** cycle on top of 2.6.3. It pairs a broad round of
user-visible bug fixes across editor, compare/merge, git, LSP/DAP, and plugin
surfaces with a continued defensive-caps sweep.

### Changed

- **Default keybindings aligned with VSCode.** The file finder now opens with
  `Ctrl+P` (was F6), the sidebar toggles with `Ctrl+B` (was F8), "add cursor at
  all matches" is `Ctrl+Shift+L` (was `Ctrl+Alt+L`, now matching the palette
  hint), and "go to line" gains `Ctrl+G`. The two freed function keys move to the
  debugger — `F6` = Pause (VSCode's default) and `F8` = Start Debugging — filling
  the gap where F5 only *continued* an already-paused session. `Ctrl+G` /
  "Go to Line…" now opens a dedicated single-line modal (it previously did
  nothing); typing `goto <line>` in the command palette still works too.

### Fixed

- **Compare/merge:** compare panes render on the cell grid so caret, selection, and
  text stay aligned; visible row layouts are cached outside the render loop. Merge
  "Mark Resolved" self-rejection and CRLF conflict-marker parsing fixed.
- **Editor:** fold gutter spacing corrected for wide line numbers; stale fold model
  after undo/redo and `tabmove +N` fixed; multi-caret paste remap desync on lone-CR /
  reversed line endings fixed; syntax highlighting carries the open-region stack across
  blank and over-long lines.
- **Git:** data loss when discarding or unstaging a staged rename fixed; sidebar discard
  validated against the confirmed path rather than a stale index.
- **LSP/DAP:** LSP shutdown deadlock, document leak, and stranded requests fixed; DAP
  scope overflow fixed.
- **Plugins:** plugin worker hang and control-channel reply race fixed.
- **Settings:** steppers use the real ranges for scrollback, blink, autosave, and scale.

### Security / resilience hardening

- Additional allocation and length caps: terminal paste payloads, output-channel
  retained bytes, control-socket connections, DAP protocol arrays, and terminal
  tabulation repeats.
- Hardened JSON/number parsing, persisted-record writes, and CR line-ending file loads.
- Tenth-round resilience sweep: git-v2 crash, DAP list flooding, terminal reply-write stall.

### Performance

- Faster JSON string parse/serialize on the LSP/DAP hot path; render hot-path allocations
  removed; plugin buffer open/save/close dispatch gated on subscriber interest.

## [2.6.3] - 2026-07-04

A **resilience-hardening** release on top of 2.6.2. It lands a multi-round
white-box pentest sweep: adversarial and pathological inputs across the
terminal, editor, compare/merge, git, filesystem-watch, LSP/DAP, and plugin
surfaces are now bounded by allocation caps, recursion-depth guards, and
list-length limits so malformed or hostile data degrades gracefully instead
of exhausting memory or stalling a thread. Changes are purely defensive —
no persisted-format or plugin-API breaks, and no user-visible workflow
changes beyond one new opt-in terminal-clipboard setting.

### Security / resilience hardening
- Terminal: cap allocation-driving CSI operations (`L`/`X`/`@`, cursor moves)
  and cursor-down to screen/width bounds; guard the CSI parser against
  oversized parameters; strip embedded `ESC[201~` end-markers from bracketed
  paste to close a paste-injection vector.
- OSC 52 terminal clipboard writes are now gated behind a new opt-in setting
  (**off by default**), so a remote program can no longer silently overwrite
  the system clipboard.
- Editor: bound per-gesture caret spans and enforce a byte budget on the undo
  history so pathological edits can't grow it without limit.
- Compare/merge: skip intra-line span refinement past a 64 KiB line budget and
  cap the anchored-fallback diff recursion at depth 256 with a correct coarse
  fallback.
- Git: cap git-status entry counts and other parser outputs; add a symlink-loop
  guard for tree traversal; bound patch generation against oversized inputs.
- Filesystem watch: bound the FileIndexWatcher inotify watch count (with clean
  partial-tree degradation) and cap filesystem-event floods.
- LSP/DAP/plugin/console: cap protocol and UI list lengths so a flooding peer
  or plugin can't overwhelm the host; bound plugin registration counts.
- Render: truncate over-long text to a bounded length before surface sizing to
  cap a render-thread allocation, and validate display-list Text-op offsets
  unconditionally.

### Performance
- git-diff backend deep pass: reduce parse allocations (NUL-delimited split
  helper replacing `std::stringstream`), tighten the recompute gate, and fix
  patch-generation paths.

### Fixes
- Balance the background-task counter on a synchronous git sidebar refresh so
  the in-flight task count no longer leaks.
- Walk back over-aggressive caps from the hardening rounds where an earlier
  ceiling clipped legitimate input (code-review follow-up).

### Testing
- Extensive new regression coverage for the caps and guards above, plus new
  fuzz harnesses and seed corpora for JSON parsing, search-regex
  (catastrophic-backtracking seeds), and the terminal CSI parser.

## [2.6.2] - 2026-07-03

A small **workspace-chrome** release on top of 2.6.1. The project tab strip
now hides itself while a single project (or none) is open, so the common
single-project workspace gains a row of vertical space; the strip reappears
the moment a second project opens. No persisted-format or plugin-API breaks.

### Workspace chrome
- New `chrome.project_tabs.hide_when_single` user setting (**on by default**):
  with one project or none open, the project tab strip collapses to a
  zero-height rect that suppresses both its render and its hit-testing.
- `ComputeLayout` gains a `project_tab_strip_visible` input so the render and
  hit-test paths agree on the collapsed geometry, and the render-chrome path
  skips all tab measuring, overflow controls, and drag-ghost work when the
  strip is hidden.

### Performance
- Cache the `ProjectTabStripVisible()` predicate: it runs inside the uncached
  `ComputeLayout` on the per-mouse-move window-drag hit-test path, so the
  settings lookup is now memoized and re-resolved only on a settings-store
  revision bump. On `perf-runner-v1` the predicate drops ~21 ns → ~0.8 ns per
  call and the full uncached layout recompute ~51 ns → ~31 ns per hit-test,
  allocation-neutral. Adds an advisory `project_tab_strip_layout_hittest`
  micro-benchmark as a regression guard.

## [2.6.1] - 2026-07-03

A **font-picker polish** release on top of 2.6.0. The installed-font
dropdown becomes properly scrollable, collapses weight/style variants into
single family names, and enables fontconfig by default so enumeration and
resolution use real system font families. No persisted-format or plugin-API
breaks.

### Settings / Font picker
- The font-family dropdown (editor + terminal) is now scrollable: explicit
  scroll offset with clamping, a real scrollbar when the list overflows, and
  mouse-wheel routing when the pointer is over the dropdown.
- Font enumeration deduplicates weight/style variants into one family name.
- fontconfig is enabled by default in the canonical build environments, so
  installed fonts are listed with real, weight-deduped family names.

### Fixes
- Harden the no-fontconfig fallback to strip trailing style tails (including
  abbreviations/concatenations like `BdIta`, `BoldOblique`) so variants
  collapse to their family.
- Fix `ResolveFamilyToFile`: `FcFontMatch` always returns a default font, so a
  garbage family silently switched to it; only accept a match whose font
  advertises the requested family, keeping unresolved families a no-op.

## [2.6.0] - 2026-07-03

A **settings & tabs** release on top of 2.5.2. The settings surface is
overhauled end to end, gains an installed-font picker, and tabs now reorder
with a Chrome-like sliding animation. No persisted-format or plugin-API breaks.

### Settings
- Full settings overhaul: previously inert settings are wired to live
  behavior, string values are editable inline, and settings can be saved as
  the default.
- Installed-font picker backed by a native file dialog, with portable font
  resolution.
- Two high-value deferred settings wired: autosave delay and terminal font.
- Setting descriptions wrap instead of being clipped with an ellipsis.

### Editor / Tabs
- Chrome-like sliding tab reordering.

### Fixes
- Fix a use-after-free where the font-family value painted freed heap memory.
- Stop the row description overlapping the scope label in the settings list.
- Live-apply font, theme, terminal, autosave, and gutter settings; resolve the
  settings-review and high-effort code-review findings on the overhaul.

## [2.5.2] - 2026-07-02

A **performance & internals** release on top of 2.5.1. The compare/merge
surfaces and sidebar shed per-frame work, the release binary now ships with
LTO, and a broad refactor collapses the workspace provider registries behind a
single generic. No persisted-format or plugin-API breaks.

### Performance
- Compare/merge rendering no longer allocates per visible row: both surfaces
  render through reused scratch `DecoratedTextRow` members and truncate hot
  labels allocation-free, and their render TUs are now under the render lint set
  that enforces it.
- Sidebar project-search status line is cached in the view model instead of
  being rebuilt every frame.

### Build
- The release `.deb` now ships with LTO (`-DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON`),
  matching the LTO codegen the perf gate already measures.

### Internal
- Collapse seven per-kind workspace provider registries into a single generic
  `ProviderRegistry<Spec>`.
- Default `operator==` for the compare/merge hover state.

### Performance baselines
- Refresh committed perf baselines to reflect the compare/merge render and
  sidebar status wins; the maintainer workstation is now the designated
  `perf-runner-v1` host for authoritative rebaselines.

## [2.5.1] - 2026-07-01

A **performance & internals** patch on top of 2.5.0. Startup and teardown get
lighter, editor hot paths shed allocations, and a broad dedup pass consolidates
buffer-renderer helpers. No persisted-format or plugin-API breaks; perf baselines
were refreshed to match.

### Performance
- Faster startup: syntax highlighting compiles its per-definition rules lazily on
  first use instead of eagerly at load.
- Lighter teardown: redundant work on buffer, project, and application close is
  trimmed.
- Fewer hot-path allocations across the editor and render paths, plus path-handling
  dedup, from the continued deep-review passes.

### Fixes
- Action availability: gate the `debug-run` (Start Debugging by program) command
  on `debug.enabled` like the other debug-launch actions, so it no longer appears
  enabled while the debugger is off.

### Internal
- Unify buffer-renderer batching, caret, whitespace, and gutter helpers behind
  shared code paths.
- Multi-caret and soft-wrap speed and correctness pass.
- Fix a latent out-of-bounds access surfaced during deep review.
- Silence the two remaining build warnings (unhandled `DebugRun` switch case;
  a `std::filesystem::path` copy in a test loop).

### Performance baselines
- Refresh committed perf baselines under `tests/perf/baselines/` to reflect the
  startup/teardown/render optimizations shipped since 2.5.0.

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
