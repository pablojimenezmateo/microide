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

> **Resolved 2026-07-09 (eighth pass — deferred-item review):** a batch of the
> deferred items below were implemented after review (speed-first, correctness,
> low CPU/mem; VSCode taken as the reference where semantics were open). Each landed
> with regression coverage and the full suite + ASAN/UBSAN/TSAN stay green:
> - **LSP/DAP requests registered during init are now failed, not dropped**, on every
>   handshake-failure return site in both stdio transports (`FailPendingRequests(false)`
>   before teardown; not followed by `ResetProtocolState()` because that clears the
>   main-thread mailbox the failure callbacks were just posted to). No more stranded UI
>   loading state. (`WorkspaceLspClientLifecycle.cpp`, `WorkspaceDapClientInternal.h`.)
> - **`--control-spec` file read is capped at 1 MiB** (`Application.cpp`), matching the
>   control-channel descriptor cap.
> - **Undo coalescing breaks on a caret round-trip** — the four user navigation commands
>   (`MoveCursorVertical/Horizontal/LineStart/LineEnd`) now call
>   `TextViewportUndoHistory::NotifyCursorMoved()`. Hooked at the command layer, not the
>   `MoveCursorTo` leaf, to avoid breaking legitimate typing runs.
> - **Redo of a multi-caret line move keeps its carets** — `MoveLineUp`/`MoveLineDown`
>   are wrapped in `BeginUndoGroup`/`EndUndoGroup` so the aggregate `after_state` is
>   captured after `RestoreCaretsAfterLineMove`.
> - **Snippet cross-tab shift now covers delete and choice** (not just insert): the
>   backspace/delete-forward and `ApplyChoiceForTab` paths call the existing
>   `ShiftPlaceholdersAtOrAfter` (the now-unused `ExtendPlaceholderRanges` was removed).
> - **Snippet lone-CR body positions** — `PositionAfterOffsetInExpanded` collapses `\r`
>   and `\r\n` to one line break, matching the `NormalizeLineEndings` the inserted text
>   goes through.
> - **Multi-caret copy/cut/paste (VSCode parity)** — copy/cut aggregate every caret's
>   selection newline-joined in position order (`TextViewport::MultiCaretSelectedText`);
>   cut deletes them all atomically (`DeleteMultiCaretSelections`); paste distributes one
>   clipboard line per caret when the counts match, else inserts the whole text at each
>   (`TextViewport::PasteText`, wired through `InsertTextIntoActiveSurface`).
> - **Settings int values clamp at store time** — `SettingSpec` gained `min_int`/`max_int`/
>   `int_step`; `ParseSettingValue` clamps to the range and the Settings-overlay stepper
>   reads the same spec fields, so the stored/displayed value no longer diverges from the
>   applied (clamped) one.
> - **Line breakpoints shift on edit (VSCode parity)** — `BreakpointStore::ShiftForAppliedEdit`
>   mirrors the LSP diagnostics `AdjustPositionForReplace` (interior breakpoints slide to
>   the edit's end line; collisions dedupe). Driven per-keystroke from
>   `WorkspaceShell::RequestActiveEditableLastChangeRedraw` via
>   `DebugService::ShiftBreakpointsForAppliedEdit` (panel rebuild + live re-send); a cheap
>   no-op for files without breakpoints.
>
> The plugin-metamethod, file-index-scan-race, and AsyncSubprocess-wake-pipe items were
> reviewed and deliberately kept deferred (see their refreshed entries below).

> **Resolved 2026-07-09 (ninth pass — cross-subsystem bug hunt):** a fan-out
> bug-hunt across every subsystem landed the following fixes, each with regression
> coverage; the full suite stays green (SAN run at session end):
> - **Left/Right arrow now crosses line boundaries** (`TextViewportViewState.cpp`
>   `AdvanceCaretHorizontal`) — it only mutated `caret.column`, so Left at column 0 and
>   Right at end-of-line were no-ops instead of moving to the adjacent line (VS Code parity).
> - **`Ansi256Color` clamps out-of-range indices** (`render/AnsiPalette.cpp`) — indices > 255
>   fell through the grayscale ramp with no upper bound and wrapped via `Uint8` truncation.
> - **`DecodeUtf8Codepoint` rejects overlong forms, UTF-16 surrogates, and > U+10FFFF**
>   (`util/StringUtil.cpp`) — it sized sequences with the lead-byte-only classifier and
>   accepted invalid scalars (`ED A0 80`→U+D800, `E0 80 80`→overlong, `F4 BF BF BF`→>U+10FFFF).
> - **Merge conflict `BothDeleted` fallback** (`compare/MergeConflictKind.cpp`) — the
>   existence fallback omitted base-present/both-absent, leaving a real "DD" conflict `Unknown`
>   (offered an empty text view, suppressed the keep/delete choice).
> - **Binary override no longer clobbers existence-choice kinds** (same file) — a
>   delete/modify conflict with binary surviving content was overwritten to `Binary`, stripping
>   the keep-vs-delete decision. Now gated on `!RequiresExistenceChoice(kind)`.
> - **Branch-review hunk content hash length-prefixes left/right** (`compare/
>   BranchReviewStateTypes.cpp`) — the undelimited concat collided (`"hello"+"world"` ==
>   `"hell"+"oworld"`), so a changed hunk could stay marked reviewed.
> - **LSP accepts a float-echoed response id** (`workspace/WorkspaceLspClientDispatch.cpp`) —
>   the `IsInt()`-only gate dropped responses whose id was echoed as `5.0`; now accepts
>   `IsDouble()` too, mirroring the DAP `request_seq` gate.
> - **Plugin theme/file-icon registration rejects non-table args without longjmp**
>   (`plugin/PluginPresentationRegistrationParsers.cpp`) — replaced the internal
>   `luaL_checktype` (which longjmps over the caller's live `std::string error_message`) with
>   a non-raising type check, upholding the "no longjmp over live C++ locals" invariant.
> - **Plugin teardown releases all lifecycle refs** (`plugin/PluginStateTeardownInterop.cpp`) —
>   `on_buffer_change/on_cursor_move/on_selection_change/on_buffer_close` refs were not
>   `luaL_unref`'d (latent registry leak if teardown ever reuses the `lua_State`).
> - **PTY reader natural-EOF reaper no longer blocks on a reused pid**
>   (`platform/TerminalBackend.cpp`) — the EOF path had no sync edge with `Stop()`'s reaper, so a
>   blocking `waitpid` could latch onto a recycled pid and hang the join; now a bounded WNOHANG reap.
> - **File-watcher + file-index-watcher control pipes are CLOEXEC**
>   (`platform/FileWatcher.cpp`, `platform/FileIndexWatcher.cpp`) — bare `pipe()` leaked the
>   control fds into every forked child, contrary to the Subprocess/AsyncSubprocess hardening.
> - **`AsyncSubprocess` moved-from accessors + `Write` hardened**
>   (`platform/AsyncSubprocess.cpp`) — `pid()/exit_code()/stdout_fd()` locked `impl_->state_mutex`
>   before the null check (null-deref on a moved-from object); `Write` had no deadline and could
>   spin forever on a stalled-but-alive child (now a progress-resetting 30s stall budget).
>
> Second (deeper) round of the same pass added:
> - **Function-breakpoint event no longer corrupts a coincident line breakpoint**
>   (`workspace/DebugServiceCallbacks.cpp` + `editor/BreakpointStore.cpp`) — a DAP `breakpoint`
>   event was applied to BOTH stores; when a function bp resolved to a line holding a user line bp,
>   the line store's line-match fallback rewrote that bp's adapter id/verify state. Fixed by routing
>   the event to the function store first and skipping the line store when it claims the id by id,
>   and by restricting the line fallback to still-unbound (`adapter_id == 0`) breakpoints.
> - **Save-normalization preserves the caret/selection/scroll** (`editor/TextViewportFileIO.cpp`) —
>   the whole-document mirror `ReplaceLines` snapped them to (0,0)/top; the pre-normalization view is
>   captured and restored, with carets clamped into the trimmed content (VS Code format-on-save parity).
> - **Session restore remaps the active-project index after culling missing projects**
>   (`workspace/WorkspacePersistenceCoordinatorWorkspaceSession.cpp`) — the saved index (into the
>   original root list) was only clamped, so a missing project before the active one activated the
>   wrong project; it is now remapped to the surviving entry.
> - **Float settings clamp at store time** (`workspace/WorkspaceSettingsRegistry.{h,cpp}`) — added
>   `min_float`/`max_float` to `SettingSpec` and clamped `ui.scale` in `ParseSettingValue`, mirroring
>   the Int-clamp contract so the stored value can't diverge from the applied one.
> - **gitignore leading whitespace is significant** (`project/IgnoreMatcher.cpp`) — `ParseRule`
>   trimmed both ends; per gitignore(5) only trailing whitespace is stripped.
>
> **Resolved 2026-07-09 (tenth pass — deferred-item review):** three more open items were
> implemented after review (speed-first, correctness, low CPU/mem; VSCode as reference), each with
> regression coverage; the full suite + ASAN/UBSAN/TSAN stay green:
> - **`PieceTree::LineView` same-index re-read no longer dangles a prior view** (`editor/PieceTree.cpp`).
>   The spanning-line slow path now returns the already-materialized `line_view_cache_[index]` slot
>   when present (the whole map is cleared on every mutation via `BumpRevision`, so the bytes are
>   immutable within a revision) instead of `clear()`+`CopyRange`, which could reallocate the slot and
>   invalidate an earlier view. Strict improvement: removes the dangle **and** the redundant
>   re-materialization; the contiguous fast path is untouched. Covered by
>   `PieceTree/SpanningLineViewStableAcrossReReads` (forces a spanning line via a new
>   `InsertTextForTesting` seam and asserts equal content + identical backing pointer across re-reads).
> - **In-buffer regex "find next" is now cancellable on a pathological single-line file**
>   (`project/ProjectSearchService.cpp`). `FindNextRegexMatch` polls `cancel_requested_` every
>   `kRegexCancelPollInterval` (4096) iterations of the empty-match advance loop, so `Stop()` interrupts
>   an `x?`-style pattern on a huge single line (previously the per-line cancel check never re-ran mid
>   line). The function moved to `search_internal` (new `ProjectSearchServiceInternal.h`) so
>   `ProjectSearchService/RegexFindNextIsCancellable` can drive it deterministically with a preset
>   cancel flag (no Start/Stop timing race), asserting it bails with `*search_from` short of the line end.
> - **Autosave / save-on-quit / quit-count now flush a buffer dirtied in the non-focused split group**
>   (VSCode "Save All"). Added `GroupTabRef` (`WorkspaceProjectState.h`), a group-aware
>   `TabCoordinator::SaveGroupTab(group,index)` primitive (`Save(index)` delegates to it with the
>   clamped focused group — byte-for-byte behavior-preserving), and all-groups enumerators
>   `DirtyGroupTabs`/`DirtyGroupTabsForProject`. Wired through `EditorTabService`, the autosave loop +
>   arm gates (`WorkspaceTabCoordinatorShellBridge.cpp`), the quit/close-project count
>   (`WorkspaceShellPrompts.cpp`), and `DirtyPromptCoordinator::SaveDirtyGroupTabs` (quit + close-project
>   save). Focused-group semantics (`DirtyIndices`, close-tab/close-tabs UI) are unchanged; the quit
>   `dirty_tabs` payload stays focused-group (display-only — `ConfirmQuit`/`ConfirmCloseProject`
>   re-derive the all-groups set to save). Covered by
>   `ExternalRepoChange/AutosaveFlushesNonFocusedGroupDirtyTab` and the two
>   `WorkspaceShell/QuitWith*NonFocusedGroupDirtyTab` tests. (The old open bullet's blocked-on
>   group-aware save/index API is exactly what shipped.)
>
> The plugin-metamethod protected-harvest item (111 field-read sites across 15 TUs, needs a
> `lua_pcall`-trampoline helper + a lint extension) was reviewed and kept deferred as too large/risky
> for a combined session — it warrants its own pass. The `ParseFloat`/`ParseDouble` parity nit stays
> deferred (traced-benign; tightening it perturbs the JSON hot path for no live benefit). The
> environment-blocked (Windows/macOS) and product-decision (DAP multi-thread, branch-review
> `ChangedSinceReviewed`) items are unchanged.

> Reviewed and left as LOW / deferred (no live MEDIUM+ trigger): `DebugValueTree` duplicate
> `variablesReference` aliasing (`DebugValueTree.cpp` — one of two aliased rows never populates
> children; adapter-dependent); several terminal escape-sequence deviations (OSC/DCS not aborted on
> an interrupting bare ESC; multi-byte charset-designator leftover byte; SU/SD no-op on the primary
> screen; combining marks dropped after a wide glyph) — all cosmetic/malformed-input only; and the
> merge/compare preview `SliceVisibleColumns` visual-vs-codepoint slice for tab-bearing lines
> (`DecoratedTextGridRenderer.cpp`, unconfirmed unit mismatch, outside the main editor grid path).

> **Resolved 2026-07-09 (third deep round of the ninth pass):** three more MEDIUMs, each with
> regression coverage:
> - **Soft-wrapped collapsed-fold opener vertical motion** (`editor/TextLayoutCache.cpp`
>   `WrappedRowRangeForLine`) — the opener's row range was inferred from the next logical line's
>   offset, but a collapsed fold's hidden lines reuse the opener's offset, so a wrapping opener
>   collapsed to a single-row range `{R, R}` and down-arrow got stuck on it. Now the opener's last
>   row is found by scanning its own contiguous rows (tagged by `line_index`), robust to hidden lines.
> - **LSP float-echoed id dropped at the INIT handshake** (`workspace/WorkspaceLspClientLifecycle.cpp`)
>   — the prior pass fixed the steady-state dispatch gate but left the initialize-response gate on
>   `IsInt()` only, so a float-echoing server's init response was never matched and startup timed out
>   (server killed). Now accepts `IsInt() || IsDouble()` at both sites.
> - **`FileIndex` move ops dropped the `follow_out_of_root_symlinks_` atomic**
>   (`project/FileIndex.cpp`) — the hand-rolled move ctor/assignment (required by `files_mutex_`)
>   omitted the flag, so a project switch (which move-assigns `FileIndex`) silently reverted a user
>   who enabled following out-of-root symlinks to containment-enforced. Both move ops now carry it;
>   a `FollowOutOfRootSymlinks()` getter was added for coverage.
> - Also hardened the workspace-session save/restore to emit `active_project_index` in the same
>   filtered index space as `project_roots` (was entries-space; masked by the non-empty-root
>   invariant) so the Pass-2 restore remap stays correct regardless of that invariant.
> - **Search-preview highlight no longer collapses to zero length** (`util/StringUtil.cpp`
>   `CollapseAsciiWhitespaceTrackingMatch`) — a match whose last byte was whitespace and that
>   consumed the whole trailing run (match_end at the next word) missed `mapped_end`; now the
>   exclusive end maps to just after the flushed collapsed space.
>
> **Fourth (deepest) round — verdict: only LOW remains.** Deep passes over editor/render,
> project/DAP, and compare/util/persistence found no further HIGH/MEDIUM. Remaining LOW leads left
> as deferred (niche trigger and/or self-healing, or intentional v1 limitations):
> - inotify leaks descendant watches when a watched subdirectory is moved OUT of the tree
>   (`platform/FileIndexWatcher.cpp` — only the directly-moved dir gets `IN_MOVE_SELF`; grandchild
>   watches leak with stale in-tree paths, bounded by `kMaxIndexWatchEntries` and self-heals on the
>   next `IN_Q_OVERFLOW` full rescan);
> - plugin end-of-line decorations and tab whitespace-markers on the wrapped HEAD row of a
>   soft-wrapped line anchor past the visible glyphs (`render/EditorViewRenderer.cpp`) — cosmetic,
>   matches the same-file inlay-suppression v1 limitation;
> - `FileIndex::Refresh()` does not refresh `truncated_`, and `FileIndex::files()` returns a bare
>   reference into shared_ptr-owned storage (no live caller) — latent footguns, not live defects.

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
  `ReadStringArrayField` — was converted to `lua_rawgeti` this pass.) **Reviewed 2026-07-09
  (eighth pass) and kept deferred.** New finding: it is currently *mitigated* (not clean)
  by `lua_atpanic`→`throw LuaPanicError` (`LuaRuntime.cpp:28-40,61`) — the raise reaches
  the panic handler with no protected frame and is thrown as a C++ exception, which
  unwinds the C++ destructors **only if** Lua's C is built with unwind tables (the
  fragile assumption a protected harvest removes). A clean fix is medium-to-large: add a
  reusable `lua_pcall`-trampoline harvest helper across the ~14 interop TUs (churn
  concentrates in the ~6 container-building harvest loops), and ideally extend the lint
  `CheckPluginLuaErrorDoesNotLongjmpOverCppLocals` to also flag unprotected
  `lua_getfield`/`lua_geti`. **Blocked on:** that protected-harvest helper + design (do
  NOT blanket-convert to `lua_rawget` — it would break `__index` field defaults).

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
  concurrent with shutdown would block on a stale/reused fd. **Reviewed 2026-07-09 (eighth
  pass) and kept deferred: not a live defect** — no infinite-timeout caller exists, so a
  medium self-pipe change would only harden a latent path. **Blocked on:** a future
  infinite-timeout caller; fix by adding a self-pipe like `TerminalBackend.cpp:153-222`,
  or document the finite-timeout requirement.

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
  here. Speed-first + untestable + self-healing ⇒ record, don't rush. **Reviewed 2026-07-09
  (eighth pass) and kept deferred:** the safe fix (worker buffers incremental events until
  the initial batch is delivered, then flushes) must be applied across all three native
  backends and is timing-flaky to test, so it stays out of a speed-sensitive open path
  for now. **Blocked on:** a measured, wake-driven delivery gate that keeps the two walks
  parallel, plus a deterministic test hook.

- **inotify watch-descriptor leak when a subdirectory tree is moved OUT of the
  project.** `FileIndexWatcher.cpp` `WorkerMain` emits a recursive `Deleted` on
  `IN_MOVED_FROM`/`IN_DELETE` for a directory but only removes the watch for the dir
  itself; descendant subdirectory watches (no self-event, inodes still exist at the new
  location) leak in `wd_to_path` + the kernel table. **Reviewed 2026-07-09 (eighth pass):
  mostly already mitigated** — the `IN_IGNORED` handler (`FileIndexWatcher.cpp:575-578`)
  reactively erases each stale `wd_to_path` entry when the kernel auto-removes a child
  watch, so the common case is handled; the residual leak is only the window where an
  `IN_Q_OVERFLOW` drops the `IN_IGNORED` events. LOW/MED, bounded. **Blocked on:** nothing
  — a belt-and-suspenders proactive prune (on `is_dir && (IN_DELETE|IN_MOVED_FROM)`,
  `inotify_rm_watch` + erase every `wd_to_path` entry at/under the path) still has marginal
  value for the overflow edge; deferred as low-value against a hot inotify path.

- **macOS FSEvents `StopNative` can deadlock if `Unwatch()` runs before the worker
  publishes `run_loop`.** `FileIndexWatcher.cpp` Apple backend: `StopNative` reads
  `run_loop` (set later by the worker) and skips `CFRunLoopStop` when it is still null,
  so `CFRunLoopRun()` never terminates and `worker.join()` hangs. A rapid open→close
  triggers it. MED but **macOS-only and not compilable/testable on this Linux
  workstation**. **Blocked on:** a macOS build; fix is to publish `run_loop` under a
  mutex/condvar and have `StopNative` wait for it.

A fourth sixth-pass sweep (command-palette / settings / actions) fixed the palette
Enter routing a multi-word command *label* to the command-line executor instead of
the highlighted row, the signed-overflow UB in the settings int stepper
(`WrapSteppedInt` now steps in `int64`), and the bool toggle no-op for non-canonical
truthy tokens (now shares `SettingFlagEnabled`). It deferred:

- **Folding / sticky-scroll / overview-ruler / clipboard round-trips were only
  partially swept this session** — the dedicated hunter hit a session/API limit, so the
  lens was finished by direct review. The sticky-scroll compute (`ComputeStickyScroll-
  LinesUncached`, depth clamped to [1,8], `take = min(max_depth, openers.size())`) and
  single-caret `SelectedText` (reversed/multi-line normalized correctly) are sound. The
  one real gap found here — multi-caret copy/cut capturing only the primary selection —
  was **implemented in the 2026-07-09 eighth pass** (see the resolved note at the top;
  aggregate copy/cut + distribute-paste). Overview-ruler marker math and column-selection
  extraction still merit a future dedicated pass.

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
