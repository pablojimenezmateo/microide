# MicroIDE Known Tech Debt

Reviewed on 2026-07-09.

This file is the queue for tech debt that is **open, actionable, and still present in the tree**.
It is deliberately short. Closed debt does not live here — it is archived (see below).

Use `dev-docs/project/active-work.md` for current priorities.

## Open items

These were surfaced by the 2026-07 cross-subsystem bug-hunt passes and
**deliberately deferred** — each is either entangled with a semantics decision or
a latent API-contract hazard with no live trigger, so a rushed fix risked a
regression worse than the defect. Recorded here so they are not silently lost.

> **Resolved 2026-07-09 (seventh pass):** the terminal `ED` (`CSI 2J`) scrollback-loss
> item that headed this list is now fixed. Primary-screen absolute-row addressing
> (`CSI H`/`f`/`d`, DECSTBM home, CPR report) was reworked to be viewport-relative via
> `TerminalSession::PrimaryScreenTopLocked()`, and `ED 1`/`ED 2` now preserve scrollback
> and blank only the visible screen — so `clear` keeps history like xterm/VTE.

- **Branch-review cross-generation `ChangedSinceReviewed` status is effectively
  unreachable.** `src/compare/BranchReviewStateService.cpp` /
  `src/compare/BranchReviewStateTypes.h` (`BranchReviewMarkerStatus::ChangedSinceReviewed`).
  The status can only be produced if a target's snapshot generation advances while a
  reviewed marker survives, but the current generation/identity bookkeeping never
  reaches that combination, so the branch is dead. Resolving it is a **product
  decision** on snapshot-generation semantics and touches the persisted
  `BranchReviewTargetIdentity`, so it is not a mechanical fix. **Blocked on:** defining
  the intended "changed since I reviewed it" semantics, then either wiring the
  producer or removing the status (and migrating persisted state).

- **`PieceTree::LineView` same-index re-read can dangle a previously returned view.**
  `src/editor/PieceTree.cpp:327`. When a line spans multiple pieces, `LineView`
  returns a `string_view` into `line_view_cache_[index]`; a second `LineView(index)`
  call for the same index on the non-contiguous path does `cached.clear()` +
  `CopyRange`, reallocating that slot and invalidating the earlier view. No live
  caller holds a `LineView` result across a re-read of the *same* index today, so this
  is a latent API-contract hazard, not a live crash — but it is a hot path, so the
  fix (e.g. documenting single-live-view-per-index, or returning owned storage on the
  slow path) must stay allocation-free in the common contiguous case. **Blocked on:**
  nothing external — deferred only to avoid perturbing a hot path without a measured
  guard; pick up when the LineView contract is next touched.

The 2026-07-09 fourth-pass sweep also surfaced these lower-severity items, deferred
as benign/cosmetic or as design constraints rather than live defects:

- **Terminal background-color erase (BCE) is inconsistent across erase paths.**
  `TerminalSession::EraseInDisplayLocked` (whole-line `cells.clear()` / `assign`) and
  the scroll-region fills (`ScrollRegionUpLocked`/`ScrollRegionDownLocked` insert
  default `TerminalLine{}`) drop the current SGR background, while `EraseInLineLocked`
  honours it via `current_style_`. So `\x1b[44m\x1b[2J` on the alt screen renders with
  the default background whereas `\x1b[44m\x1b[K` paints blue. Fixing it means filling
  erased lines with `columns_` styled cells instead of the empty-line fast path, which
  touches the memory model the terminal-scroll perf work relies on — measure before
  changing. **Blocked on:** a perf-neutral styled-blank-line representation. Cosmetic
  (BCE is an optional xterm feature) so low priority.

- **LSP incremental `didChange` LF-joins replacement text on CRLF documents.**
  `src/workspace/LspService.cpp:928-940` sends `applied_edit->replacement_text` (built
  LF-joined in `TextViewportUndoHistory.cpp`) even for a CRLF buffer, whose `didOpen`
  used the real CRLF ending. The server mirror's edited regions drift toward LF. Traced
  as benign: LSP positions are line/character (never byte offsets), replacement text
  never contains a stray `\r`, edit ranges align to line-content boundaries so a
  `\r\n` pair is never split, and the next full sync (save/clean transition) reconciles.
  Not fixed because the correction adds per-keystroke string work on the hot
  incremental-sync path to fix an inconsistency with no demonstrable wrong-position
  outcome (speed is the priority). **Blocked on:** a demonstrated corruption case, or
  doing the transform only when `line_ending() == CRLF` if a case ever surfaces.
  (Re-confirmed by an independent 2026-07-09 fifth-pass hunter as a real server-mirror
  desync, but it again could not exhibit a concrete wrong-*position* outcome, and no
  LF→CRLF string helper exists, so the guarded send-site re-encode stays deferred
  pending a demonstrated corruption case — the disciplined call given speed is priority.)
  (Note: the sibling "cross-language go-to-definition uses the source server's encoding"
  suspicion was investigated and is NOT a bug — a `definition` response's `character`
  is in the responding/source server's encoding regardless of the target file, which is
  exactly what `AssistService::NavigateToLspLocation` uses.)

- **DAP multi-thread semantics are single-thread-simplified.** Two spots assume the
  app's single-thread debugging focus: `DebugServiceCallbacks.cpp` records a verified
  breakpoint at the *requested* line rather than the adapter's relocated `breakpoint.line`
  (the store is keyed by requested line, so it structurally cannot represent a
  relocation — the async `breakpoint`-event path uses the real line, so the two can
  disagree), and `DebugSession.cpp:258-264` treats a `continued` event as a full resume,
  ignoring `allThreadsContinued`/`threadId`. Both are correct for single-threaded targets
  and incorrect per spec for multi-threaded ones. **Blocked on:** a decision to support
  per-thread stop/resume state, which would re-key `BreakpointStore` and add per-thread
  run state.

The 2026-07-09 fifth-pass sweep surfaced these, deferred as latent/broad rather
than live defects (the pass fixed nine concrete bugs outright — terminal SGR mouse
release, IL/DL alt-screen margins, CPR pending-wrap column, resize tab-stop loss;
the editor horizontal-scroll caret double-count; the LSP pre-init shutdown drift and
dropped integer diagnostic code; the plugin string-array raw read; and the
ProjectFileScanner symlink-cycle crash):

- **Plugin metamethod reads can longjmp over live C++ destructors (same invariant,
  different trigger than `luaL_error`).** Plugin-controlled tables are harvested with
  the metamethod-capable `lua_getfield`/`lua_geti` while non-trivial C++ locals are
  alive — representative sites `src/plugin/PluginDiagnosticsInterop.cpp` (`ReadDiagnosticTable`,
  with `std::vector<Diagnostic>`/`std::filesystem::path` live), the `Read*Field` helpers
  in `src/plugin/PluginLuaInterop.cpp`, and the provider/registration parsers. A plugin
  that puts a raising `__index` metamethod (`setmetatable`+`error` are in the exposed
  base lib) on a harvested table makes the field read `longjmp` to the enclosing
  `LuaRuntime::PCall`, skipping those destructors (heap leak + UB) — the same hard
  invariant `luaL_error` is banned to protect. Reachable, but leak-only and requires a
  self-sabotaging plugin. NOT a clean drop-in fix: a blind `lua_getfield`→`lua_rawget`
  swap would break well-behaved plugins that legitimately use an `__index` metatable
  for field defaults; the correct fix is to harvest each plugin table inside a nested
  protected call (`lua_pcall`) so a metamethod raise is caught before it crosses the
  C++ frame. (The one *sequence* read where raw is unambiguously correct —
  `ReadStringArrayField` — was converted to `lua_rawgeti` this pass.) **Blocked on:**
  a protected-harvest helper (or a decision that raw field reads are acceptable),
  applied across ~13 interop TUs.

- **In-buffer regex "find next" is uncancellable on a pathological single-line file.**
  `src/project/ProjectSearchService.cpp:50` (`FindNextRegexMatch` empty-match advance
  loop). Cancellation is checked once per *line* (`:367`), never inside the per-offset
  empty-match loop, so a pattern that matches empty at every offset (e.g. `x?`) against
  a very large single-line file (up to the 512 MiB read cap) spins ~N PCRE2 `Match`
  calls one byte at a time before returning — PCRE2's `match_limit`/`depth_limit` bound
  a single match, not the *count* of separate `Match` calls, so `Stop()` cannot
  interrupt it. Responsiveness/soft-DoS bounded by line length, not corruption.
  **Blocked on:** a measured per-line iteration/token check that does not slow the hot
  search path (speed is the priority), so deferred rather than rushed.

- **`PieceTree` `add_` buffer offset uint32 truncation — FIXED 2026-07-09 (sixth pass).**
  `src/editor/PieceTree.cpp` `InsertText` now guards: when `add_.size() + text.size()`
  would exceed `UINT32_MAX` it calls `CompactAddBuffer`, which materializes the live
  (uint32-bounded) document into `original_` and empties `add_`, preserving content and
  `line_count_`. Covered by `PieceTree/AddBufferCompaction` (exercises the compaction
  path directly via the `CompactAddBufferForTesting` seam, since 4 GiB is impractical to
  allocate). Left here only as a pointer; no longer open.

The 2026-07-09 sixth-pass sweep fixed nine concrete bugs (terminal: wide-glyph
overwrite orphaning a paired cell, DEL-in-output destructive delete, tab-at-pending-wrap
moving backward, EL2 over-grow at pending-wrap; editor: PieceTree `add_` overflow now
compacts, soft-wrap `ClampCursorColumn` relative-vs-absolute caret snap; persistence:
`TryReadSpecificFile` reporting a stat I/O error as NotFound; git: `ParseLog` field
desync on a tab-in-author name, now US-delimited; LSP/DAP: LSP missing the DAP idle
death-nudge `PushWake`, and both clients stalling the drain loop ~1s on a valid-framed
invalid-JSON message). It also surfaced these, deferred:

- **Combining marks after a double-width glyph (and any base+mark exceeding 4 bytes)
  are dropped.** `src/terminal/TerminalSessionScreen.cpp` `PutGlyphLocked` (width==0
  branch). A hunter noted the zero-width path attaches to `cursor_column_ - 1`, which
  after a wide glyph is the trailing spacer. Stepping back to the wide lead was
  investigated and reverted: a wide glyph is ≥3 UTF-8 bytes and a combining mark ≥2, so
  `cell.length + glyph.size()` always exceeds the 4-byte inline `TerminalCell::bytes`,
  and the existing size guard drops the mark regardless of which cell it targets. So the
  base-cell fix is unobservable; the *real* limitation is the 4-byte inline cell. Benign
  (combining-on-wide is rare; display-only). **Blocked on:** a larger/overflow cell
  representation for combining sequences, which touches the trivially-copyable cell model
  the terminal-snapshot/scrollback memcpy fast paths rely on — measure before changing.

- **`ParseFloat`/`ParseDouble` accept grammar `ParseInt` rejects.** `src/util/Parse.cpp`
  (`ParseRealExact` via `strto{f,d}`) vs `ParseExact` (`from_chars`). The float path
  accepts a leading `+`, leading ASCII whitespace, and hex floats (`0x1p4`); the int path
  rejects all three. `inf`/`nan` are correctly gated out. Every caller was traced
  (`JsonValue` feeds a pre-tokenized numeric slice; settings/command callers treat the
  leniency benignly) and none misbehaves, so this is a parity nit, not a live defect.
  **Blocked on:** nothing external; tighten only if strict cross-parser parity is wanted
  (reject when the first char is whitespace/`+`), deferred to avoid perturbing the JSON
  hot path for no live benefit.

A second sixth-pass sweep (workspace-services / control+platform / render / syntax)
fixed the HIGH plugin-display-list clip-restore that blanked full-repaint frames
(`SDL_GetRenderClipRect` return value was misread as "clip enabled"; now
`SDL_RenderClipEnabled`, plus a shared `render::ScopedRenderClip` guard applied to the
three status-bar/banner/bottom-panel sites that restored to `nullptr` and broke
dirty-region confinement), the Linux fork async-signal-safety hazard in `RunSubprocess`
/ `AsyncSubprocess` (child no longer calls `setenv`/`unsetenv` or allocates — the env
array and argv are built in the parent and `environ` is assigned in the child), and the
compare/merge reveal-on-close scroll. It deferred:

- **`TerminalBackend` forks the shell then calls `setenv("TERM"/"COLORTERM")` in the
  child** (`src/platform/TerminalBackend.cpp:108-110`) — the same async-signal-unsafe
  pattern fixed in `RunSubprocess`. Deferred, not fixed, because the real PTY fork path
  is not exercised by the test suite here (tests use placeholder terminals via
  `SetUsePlaceholderTerminalsForTesting`), terminal creation is usually on the main
  thread (narrower race window), and an untested change to the interactive-shell
  environment risks a worse regression than the latent hazard. **Blocked on:** applying
  the parent-built-`environ` pattern (reuse `BuildChildEnvironment`) with a way to
  validate the real shell environment.

- **Windows `RunSubprocess` ignores `timeout_ms` and the capture ceiling** →
  `WaitForSingleObject(hProcess, INFINITE)` (`src/platform/Subprocess.cpp` Windows branch).
  A hung/firehose formatter run synchronously on the UI thread during format-on-save
  (`WorkspaceTabCoordinatorShellBridge`) freezes the editor permanently on Windows,
  where the 5 s cap is a no-op (the POSIX path enforces it). Real MED bug, but Windows-only
  and **cannot be compiled or tested on this Linux workstation**, so shipping the Win32
  fix (`WaitForSingleObject(timeout)` + `TerminateProcess` on `WAIT_TIMEOUT`, break the
  drain at the ceiling) blind risks breaking the Windows build. **Blocked on:** a Windows
  build/test environment.

- **`AsyncSubprocess` has no shutdown wake-pipe.** `Read`/`ReadExact` `poll()` `stdout_fd`
  outside `state_mutex`; a concurrent `Shutdown()` closes that fd. Benign today because
  every LSP/DAP caller passes a finite `timeout_ms` (the guarded read re-checks the fd
  after each short poll), but any future caller passing `timeout_ms <= 0` (infinite poll)
  concurrent with shutdown would block on a stale/reused fd. **Blocked on:** nothing —
  add a self-pipe like `TerminalBackend`, or document the finite-timeout requirement.

- **`--control-spec` file read is uncapped** (`Application.cpp` slurps with
  `istreambuf_iterator`, no size limit) whereas descriptor files are capped at 1 MiB.
  Operator-supplied path, not attacker input, so cosmetic. **Blocked on:** nothing; add a
  size cap for consistency.

- **LSP diagnostics published for a not-yet-open file store the server's UTF-16
  `character` as a byte column and are never re-encoded on open.** `LspService.cpp`
  `PublishLspDiagnostics` passes a null viewport into `LspRangeToEditorRange`, whose
  `LspInboundColumn` returns the raw value; `EnsureLspDocumentOpen` only sends `didOpen`,
  not a re-encode. For a whole-project-check server (rust-analyzer) reporting on a line
  with a non-ASCII character, the editor underline / hover target lands at the wrong
  column until the server re-publishes or the user edits (line/gutter and the Problems
  sidebar are correct throughout). Deferred because the stored column can't be told apart
  from a correctly-converted one post-hoc — the clean fix stores closed-file diagnostics
  in raw LSP form and converts lazily against the viewport at render time, a
  diagnostics-store data-model change. **Blocked on:** that store change, or forcing a
  diagnostics refresh on open.

A third sixth-pass sweep (debug-semantics / undo-multicaret / watchers-session)
fixed the multi-caret selection-aware edit gap (Backspace/Delete/Enter/paste now
replace each caret's active selection instead of editing one char per caret) and
the dead "show more…" pagination affordance in the debug Variables/Watch panes
(click/Enter now page in the next batch). It deferred:

- **File-index initial full-scan races the incremental inotify worker** (MED/HIGH,
  Linux). `src/platform/FileIndexWatcher.cpp` starts `StartNative` (registers
  watches + `WorkerMain` incremental delivery) and `StartInitialScan` (the wholesale-
  replace initial batch) as two independent threads with no delivery ordering. During
  the project-open window an incremental event can be applied before the initial
  batch, which then clobbers it: a file created in an already-walked directory is
  dropped from the finder, or a file deleted mid-scan is resurrected as a ghost — until
  the next full refresh. Deferred because every safe fix (gate incremental delivery
  behind an initial-delivered latch + wake; or serialize watch-registration → scan →
  worker) either adds a wake/overflow-handling path to the hot inotify loop or
  serializes the currently-parallel watch-registration and scan walks, slowing the
  latency-critical project-open path — and the race is not deterministically testable
  here. Speed-first + untestable + self-healing ⇒ record, don't rush. **Blocked on:** a
  measured, wake-driven delivery gate that keeps the two walks parallel, plus a
  deterministic test hook.

- **inotify watch-descriptor leak when a subdirectory tree is moved OUT of the
  project.** `FileIndexWatcher.cpp` `WorkerMain` emits a recursive `Deleted` on
  `IN_MOVED_FROM`/`IN_DELETE` for a directory but only removes the watch for the dir
  itself; descendant subdirectory watches (no self-event, inodes still exist at the new
  location) leak in `wd_to_path` + the kernel table. Repeated move-outs grow it toward
  the 100k cap, then the watcher degrades to partial-tree tracking. LOW/MED, bounded.
  **Blocked on:** nothing — on `is_dir && (IN_DELETE|IN_MOVED_FROM)`, also
  `inotify_rm_watch` + erase every `wd_to_path` entry at/under the path (mirror the
  recursive index delete). Deferred with the race above to avoid piling untested changes
  into the watcher in one pass.

- **macOS FSEvents `StopNative` can deadlock if `Unwatch()` runs before the worker
  publishes `run_loop`.** `FileIndexWatcher.cpp` Apple backend: `StopNative` reads
  `run_loop` (set later by the worker) and skips `CFRunLoopStop` when it is still null,
  so `CFRunLoopRun()` never terminates and `worker.join()` hangs. A rapid open→close
  triggers it. MED but **macOS-only and not compilable/testable on this Linux
  workstation**. **Blocked on:** a macOS build; fix is to publish `run_loop` under a
  mutex/condvar and have `StopNative` wait for it.

- **Line breakpoints do not shift when the file is edited.** `src/editor/BreakpointStore`
  keys breakpoints by absolute 0-based line with no anchor, and no edit path adjusts the
  store on insert/delete. Insert lines above a breakpoint and its gutter disc, the
  Breakpoints panel, and the transmitted `SendBreakpointsForFile` line all stay on the
  now-shifted-down old line number, binding to the wrong statement. VSCode shifts
  breakpoints on edit, so this is a real correctness gap — but a proper fix is a feature
  (subscribe the store to edit events; shift entries at/after an inserted/deleted range;
  drop a breakpoint whose exact line is removed) that deserves focused work, not a
  loop-iteration bolt-on onto the hot edit path. **Blocked on:** an editor-edit-event
  subscription + line-shift/anchor model for the breakpoint store.

- **Redo of a multi-caret line move loses secondary carets and column** (LOW).
  `src/editor/ShapingActions.cpp` `RestoreCaretsAfterLineMove` fixes the live carets via
  `MoveCursorTo`/`SetSecondaryCarets` (which record nothing), so the undo entry's
  `after_state` still snaps to `(range_first, 0)` with no secondaries; Redo restores that
  degraded state. Content is correct; only caret/selection state regresses. **Blocked
  on:** nothing — refresh the top entry's `after_state` after restoring carets, or wrap
  move+restore in an undo group so the aggregate captures the final carets.

- **Undo coalescing merges a keystroke after a round-trip caret move** (LOW).
  `TextViewportUndoHistory.cpp` `TryCoalesceWithTop` breaks a run only on kind or
  position mismatch; typing `a`, Right, Left (same column), `b` coalesces `a`+`b` into
  one undo. Buffer/dirty state stay correct; only undo granularity. **Blocked on:**
  nothing — call `EndCoalesceRun` from the explicit cursor-move paths.

A fourth sixth-pass sweep (command-palette / settings / actions) fixed the palette
Enter routing a multi-word command *label* to the command-line executor instead of
the highlighted row, the signed-overflow UB in the settings int stepper
(`WrapSteppedInt` now steps in `int64`), and the bool toggle no-op for non-canonical
truthy tokens (now shares `SettingFlagEnabled`). It deferred:

- **Settings int writes are validated for parseability but not range**, so an
  out-of-range value is stored and displayed verbatim while the editor applies a
  clamped one (`set-setting editor.font_size 999` shows 999, renders 32). The value is
  *applied* correctly (clamp at `ApplyCanonicalEditorPreference`); only the stored/
  displayed value diverges. LOW/MED, cosmetic-ish. **Blocked on:** nothing structural —
  clamp int values to the spec `[min,max]` at store time (or reject out-of-range in
  `ParseSettingValue`); deferred only to avoid touching the settings write path under
  time pressure now that the memory-safety (UB) half is fixed.

- **Folding / sticky-scroll / overview-ruler / clipboard round-trips were only
  partially swept this session** — the dedicated hunter hit a session/API limit, so the
  lens was finished by direct review. The sticky-scroll compute (`ComputeStickyScroll-
  LinesUncached`, depth clamped to [1,8], `take = min(max_depth, openers.size())`) and
  single-caret `SelectedText` (reversed/multi-line normalized correctly) are sound. One
  real gap found and deferred (below). Overview-ruler marker math and column-selection
  extraction still merit a future dedicated pass.

- **Multi-caret copy/cut only captures the primary selection.** `TextViewport::
  SelectedText`/`has_selection` (`src/editor/TextViewport.cpp:521-569`) read only
  `selection_anchor_` + the primary cursor, so Ctrl-D-selecting several occurrences and
  copying (or cutting) yields just one — the secondary caret selections are dropped.
  This is the clipboard sibling of the multi-caret selection-*delete* gap fixed this pass,
  but a correct fix is a feature, not a one-liner: copy must aggregate every caret's
  selection in position order (newline-joined, VSCode-style) AND paste must distribute
  one clipboard line per caret when the counts match — otherwise pasting an N-line block
  at each of N carets yields N×N lines. **Blocked on:** designing the aggregate-copy +
  per-caret-distribute-paste pair together (and the cut path, which composes copy with
  the now-multi-caret-aware delete). Deferred to focused work rather than a rushed half.

The **timing-dependent test flakiness** item (added 2026-06-21) is now closed. The
single-check-after-a-fixed-`sleep_for` anti-pattern was audited across every
`sleep_for` site under `tests/` and remediated:

- `ControlChannelService/SocketSelfHealsAfterExternalDeletion` — poll-drains
  `ConsumeControlCallbacks()` until the descriptor reappears (commit `ae2523bb`).
- `ProjectSearchService/StopDiscardsLateUpdates` — now drains `TakePendingUpdate()`
  over a bounded window and asserts no update ever surfaces after `Stop()`, instead of
  checking once after a fixed 25 ms wait (`tests/ProjectSearchServiceTests.cpp`).
- `Subprocess` async read/write concurrency fixture — now poll-waits on an
  `std::atomic<bool> reader_running` signal so a read is genuinely in flight before the
  timed write, instead of sleeping a fixed 100 ms (`tests/SubprocessTests.cpp`).

The candidates flagged for follow-up review (`FileIndexWatcherTests`,
`ExternalRepoChangeTests`, `ProjectChangeTests`, `WorkspaceDapClientTests`,
`WorkspaceLspClientTests`, `TerminalSessionTests`) were audited and already use the
bounded poll-until-condition shape; no changes were needed. The durable convention —
**poll a condition until a bounded deadline for every cross-thread assertion; never
assert state immediately after a fixed `sleep_for`** — is recorded in
`guidelines/testing.md`.

The 2026-07-09 seventh-pass sweep fixed a broad batch outright (terminal `ED 2`
scrollback preservation + viewport-relative primary CUP, `EL 1` pending-wrap
off-by-one, combining-mark-after-wide-glyph, scroll-region re-expand on grow;
the `PatchGenerator` no-newline-at-EOF fuse that silently corrupted staged blobs;
`IgnoreMatcher` leading-slash/mid-slash anchoring; `JsonValue::AsInt` out-of-range
double UB; the plugin `lua_geti`→`lua_rawgeti` metamethod cluster;
`FileWatcher`/`FileIndexWatcher` inotify shutdown-hang + watch-leak; `TerminalBackend`
`setenv`-after-fork; `Trash` `.trashinfo` percent-encoding; the LSP/DAP send-failure
double-fire; and the `SnippetEngine` cross-tab insert shift). It left these deferred:

- **Combining marks / variation selectors after a double-width glyph are dropped.**
  `TerminalSession::PutGlyphLocked` (`src/terminal/TerminalSessionScreen.cpp`). A
  zero-width mark folds into the previous cell only if it fits the 4-byte inline
  `TerminalCell` (`std::array<char,4>`). A double-width base glyph is always ≥3 UTF-8
  bytes (East-Asian-Wide starts at U+1100) and every combining mark/VS is ≥2 bytes, so
  base+mark is ≥5 bytes and never fits — the mark is dropped regardless of which cell
  it targets. (An earlier "attach to the wide lead cell instead of the trailing
  spacer" tweak was reverted: it pointed at the correct cell but changed nothing
  observable because of the byte budget, while adding a branch to the hot glyph path.)
  A real fix needs larger cell storage or an overflow side-table for grapheme
  clusters — a terminal memory-model change the scroll-perf work is sensitive to.
  **Blocked on:** a perf-neutral variable-length grapheme representation (same
  constraint as the deferred BCE styled-blank-line item). Low priority — CJK-plus-
  combining-mark is rare.

- **Snippet cross-tab shift is fixed for insert but not delete/choice.**
  `src/editor/SnippetEngine.cpp` — `SnippetTryInsertText` now shifts every other
  placeholder (across all tab stops) on the edited line via `ShiftPlaceholdersAtOrAfter`,
  but `SnippetTryBackspace`/`SnippetTryDeleteForward` (which call
  `ExtendPlaceholderRanges`, current-tab `.end` only) and `ApplyChoiceForTab` still
  leave OTHER tab stops' recorded starts stale after a delete/choice that changes
  length. The multi-mirror reverse-order deletion interaction makes a correct signed
  shift delicate, so it was deferred to avoid a snippet regression. Only affects
  multiple tab stops on one line where an earlier one is shortened. **Blocked on:**
  nothing external; extend the `ShiftPlaceholdersAtOrAfter` treatment to the delete
  and choice paths with focused tests.

- **Snippet placeholder positions desync on a lone-CR snippet body.**
  `src/editor/SnippetEngine.cpp` (`PositionAfterOffsetInExpanded`, ~line 112). Tab-stop
  offsets are computed against the raw body, but the inserted text is
  `NormalizeLineEndings`-canonicalized (a bare `\r` becomes a newline), so a body
  containing a lone CR mislocates every following tab stop. `\r\n` bodies survive
  (the following `\n` resets the phantom column). Very low severity — snippet bodies
  with lone CRs are near-nonexistent. **Blocked on:** compute positions against the
  normalized expanded text (or strip CRs in `ParseSnippetBody`).

- **LSP/DAP requests registered during init are dropped (not failed) if the handshake
  fails.** `src/workspace/WorkspaceLspClientLifecycle.cpp` / `WorkspaceDapClientInternal.h`
  init-failure paths call `ClearDeferredMessages()` and return without failing
  `pending_requests`, so a feature request issued while `!initialized` has its handler
  dropped by the later `ResetProtocolState()` rather than failed — a stranded UI
  loading state. Shared by both transports (not a drift) and normally unreached because
  requests are gated on readiness. **Blocked on:** nothing external; on init failure,
  run the EOF-style `FailPendingRequests(false)` before resetting protocol state.

## Guardrails — rejected experiments, do not retry

These are dead ends proven by the perf gate. Re-attempting them in the same shape wastes effort and
the gate will reject them again.

- **Editor glyph atlas on the draw path** (GPU / `SDL_RenderGeometry`). RESOLVED 2026-06-28 on
  `perf/gpu-render-path`: the three preconditions were met with measurement (GPU backend confirmed +
  measurable via the new `--renderer=auto` advisory lane; the `editor_scroll_fresh_content_large`
  sweep shows ~9.7% texture-cache miss with heavy eviction churn), and a **GPU-gated, row/gutter-batched**
  atlas shipped — pixel-identical to the composite path (0-pixel-diff certified on `opengles2`),
  −8% to −15% on heavy text scenarios, software path unchanged, default-on for GPU with
  `MICROIDE_RENDER_GLYPH_ATLAS=0` as escape hatch. The original *per-quad* shape stayed wrong (it
  regressed whitespace +27% by flapping the batcher); the fix was batching per row + per gutter flush.
  The 2026-05-15 +48–83% figures were a software-renderer artifact (`SDL_RenderGeometry` rasterizes
  per-pixel there). Detail:
  `guidelines/tech-debt/archive/2026-06-16-terminal-headless-and-glyph-atlas-closeout.md` (§13 Update
  2026-06-28). (The endorsed *miss-path* colour-independent coverage atlas also remains —
  `src/render/AsciiGlyphAtlas.{h,cpp}` — now also the GPU atlas's texture source.)
- **`TextDocumentModel` ownership extraction** from `TextViewport`. Rejected: it regressed hot
  editor/render scenarios (~+15% to +30% wall) and broadly increased allocations. Do not reintroduce
  in the same shape without first proving line access, mutation, revision updates, and cache
  invalidation are allocation-free and performance-neutral in the editor benchmarks. Detail:
  `guidelines/tech-debt/archive/2026-05-20-textviewport-and-shell-decomposition.md`.
- **`UNITY_BUILD` on `microide_tests`** (build-speed). Rejected 2026-06-29: 116 of 121 test TUs
  define helpers in anonymous namespaces, with confirmed same-name collisions across files
  (`MakeService`, `MakeViewport`, `MakeFixtureRoot`, …). CMake's unity batching concatenates files
  into one TU without isolating their unnamed namespaces, so those become colliding
  `(anonymous namespace)::` symbols → redefinition errors. Making it compile would mean rewriting
  ~116 files (per-file `UNITY_BUILD_UNIQUE_ID` namespace wrapping) for a speculative win the PCH pass
  below already captured the safe part of. Do not retry without first solving the anonymous-namespace
  collision mechanically. The shipped win instead was **precompiled headers** (stable std + SDL set,
  shared by `microide`/`microide_tests`/`microide_perf`): clean `microide_tests` build 141.3 s → 120.0 s
  (~15%), suite still green, runtime byte-for-byte unchanged.

## Shipped build-speed wins (2026-06-29, after the PCH pass)

- **Shared `microide_core` object library.** `MICROIDE_CORE_SOURCES` (≈373 TUs) is now an
  `OBJECT` library compiled once and spliced (`$<TARGET_OBJECTS:microide_core>`) into
  `microide`/`microide_tests`/`microide_perf`, instead of being re-listed in each target. This
  removed the previous double compile of the entire core. The prerequisite was making core
  **`MICROIDE_TESTING`-free**: ABI-neutral seams (`*ForTesting` methods, `TestAccess`/test friends,
  the `before_cache_apply_hook`, `test_sent_bytes_`) are now compiled unconditionally, and the
  genuinely behavioral forks (placeholder-vs-real terminal startup, project-init default terminal,
  `SendBytes` capture) are gated at **runtime** via `terminal::SetUsePlaceholderTerminalsForTesting`
  (the test/perf `main()` enables it), mirroring the existing `SetHostPlatformOverrideForTesting`
  precedent. Core must stay free of `#ifdef MICROIDE_TESTING`, or the shared object set diverges by
  ABI between binaries. Measured: clean build of `microide` + `microide_tests` 148 s → 91 s (~38%,
  ccache disabled for the comparison). `WorkspaceShellTestAccess.h` keeps its guard — it is a
  test-only header, never compiled into core.
- **ccache + ld.lld + split-dwarf**, all conditionally enabled in `CMakeLists.txt` (no-ops when the
  tool is absent; toggle with `-DMICROIDE_USE_CCACHE=OFF` / `-DMICROIDE_USE_LLD=OFF`). lld is skipped
  under LTO so the `microide-perf` preset keeps its default linker. `tools/run-checks.sh` exports
  `CCACHE_SLOPPINESS=pch_defines,time_macros` so PCH TUs cache.
- **Scoped inner-loop build.** `tools/run-checks.sh tests` and the documented loop build
  `--target microide_tests` (the only binary `ctest` invokes), skipping `microide` and the bench
  binaries.
- **Splitting `microide_tests` into multiple filtered `add_test` invocations for `ctest -j`**
  (test-run speed). Rejected 2026-06-29: partitioning ~121 files' worth of tests by name-substring
  filters is a silent-coverage hazard — a test matching no subset is dropped from the run with no
  signal, trading a correctness guarantee for ~2× wall-clock on an already-acceptable 47 s suite. The
  in-binary substring filter (`TestRunnerCli`) already covers focused local iteration. Do not retry
  without a mechanically-proven complete-and-disjoint partition (e.g. generated from the registry, not
  hand-maintained filter strings).

## Where the history lives

The detailed record of closed debt — the 2026-04-29 comprehensive cleanup, the render/layout perf
batch, the throughput-pass follow-ups, the layout-revision tiers, the event-driven search/index
work, the `TextViewport` / `WorkspaceShell` decomposition, the 2026-06-11 deep correctness audit, and
the 2026-06-15/16 render/app/util/terminal closeouts — now lives in:

- `guidelines/tech-debt/archive/` — per-pass archive records (with reproduction notes and lessons)
- `CHANGELOG.md` — shipped, user-facing release history
- `openspec/changes/archive/` — the full proposal/spec/tasks record per shipped change

The broader 2026-04-20 architectural review is archived at
`dev-docs/archive/production-tech-debt-review.md`.
