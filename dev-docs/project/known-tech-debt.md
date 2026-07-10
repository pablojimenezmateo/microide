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

> **Resolved 2026-07-10 (thirteenth pass — cross-subsystem bug hunt):** a fan-out across
> every subsystem landed nine fixes (one HIGH, several MEDIUM), each with regression
> coverage where testable; the full suite (1786 tests) plus ASAN stay green.
> - **Plugin sidebar refresh no longer use-after-frees the transient coordinator** (HIGH)
>   (`workspace/WorkspaceSidebarCoordinatorRefresh.cpp` + `WorkspaceSidebarCoordinator.{h,cpp}`)
>   — `SidebarCoordinator` is always a stack temporary, but `RefreshPlugin`'s
>   `SnapshotSidebarAsync` completion captured `[this]` and dereferenced `state_`/member
>   functions on a later main-thread drain, by which point the coordinator was destroyed.
>   Now mirrors the sibling `RefreshOutline`: the callback captures only an
>   `apply_plugin_sidebar_result` shell applier + the view id, which rebuilds a fresh
>   coordinator (`ApplyPluginSidebarResult`) to apply the snapshot. Fires on any
>   plugin-sidebar refresh/view-switch/project-change while a plugin worker is running.
> - **Durable-write staging fd is O_CLOEXEC / _O_NOINHERIT** (`util/DurableFile.cpp`) — the
>   open→write→fsync→close window of every document save and persisted-state write leaked a
>   writable fd into any child forked mid-window (terminal/git/LSP/DAP); matches the
>   Subprocess/FileWatcher/ControlSocket hardening standard.
> - **Plugin completion / code-action / test-discovery harvests clamp their count**
>   (`plugin/PluginProviderQueryInterop.cpp`) — every sibling harvester caps `lua_rawlen`
>   (sidebar/diagnostics/language-providers); these four loops did not, so a provider
>   returning a genuinely huge (or sparse-border-overstated) array stalled the worker and
>   grew an unbounded host vector. Added `kMaxCompletionCandidates`/`kMaxCodeActions`/
>   `kMaxCodeActionArguments`/`kMaxDiscoveredTests` clamps.
> - **Terminal ED3 (`CSI 3J`) accounts its scrollback front-trim** (`terminal/
>   TerminalSessionScreen.cpp`) — it erased the front of the deque without bumping
>   `scrollback_trim_total_` like `TrimScrollbackLocked`, so after a modern `clear` (ED2 then
>   ED3) the workspace scroll/selection mirrors (which rebase off that counter's delta) were
>   stranded `trim_count` rows too high.
> - **Terminal DECXCPR (`CSI ?6n`) reports the screen-relative row** (`terminal/
>   TerminalSessionCsi.cpp`) — the private cursor-position reply sent the raw absolute deque
>   index while the public `CSI 6n` path was already fixed to subtract `PrimaryScreenTopLocked()`;
>   an app querying `?6n` with scrollback got a wildly overstated row.
> - **Terminal hard LF onto an existing row keeps its soft-wrap flag** (`terminal/
>   TerminalSessionScreen.cpp` `AdvanceCursorRowLocked`) — a `\n` landing on a pre-existing
>   wrapped continuation (reached via cursor-up + LF) unconditionally relabeled it as a hard
>   boundary, corrupting reflow / selection-by-logical-line; now only stamps the flag on a
>   freshly-created row.
> - **Plugin editor position fields read at full `double` precision** (`plugin/
>   PluginRuntimeApiInterop.cpp` `ReadIndexField`) — line/column indices went through a 24-bit
>   `float`, rounding positions ≥ 2^24 to the wrong line for `set_cursor`/`apply_edits`/etc.
> - **Plugin raster width/height clamp before the double→int narrowing** (`plugin/
>   PluginSurfaceInterop.cpp`) — an out-of-range/NaN/negative plugin dimension was UB when cast
>   straight to `int` and poisoned `RasterHandle`; now clamped to `[0, 65535]`.
> - **`AsyncSubprocess::Read(0)` returns empty instead of tearing down the child** (`platform/
>   AsyncSubprocess.cpp`) — a zero-length request fell through to `read(fd, buf, 0)` (returns 0),
>   indistinguishable from EOF, which closed stdout and reaped the child. Now early-returns.
> - **`OpenFileAtLocation` only moves the caret once on the requested file** (`workspace/
>   WorkspaceTabCoordinatorShellBridge.cpp`) — a failed open (tab cap / unreadable) left the
>   active viewport on the previous tab and the go-to-location moved *its* caret; now guarded
>   on a confirmed path match.
>
> **Deferred from the thirteenth pass (recorded, not fixed):**
> - **Compare syntax highlighting is never computed for hunks deep in a collapsed large-file
>   diff** (MEDIUM) (`workspace/WorkspaceShellRenderCompare.cpp`
>   `PopulateCompareSyntaxTokensForWindow`). The render loop indexes `left/right_tokens_by_row`
>   by *model* row (`model_row_index`, potentially ~50k), but the tokenizer's window bound is
>   the *presentation* `scroll_row` (collapsed runs hidden), so it caps far below the visible
>   model rows and those rows draw unhighlighted. A correct fix needs the window in model space
>   **and** a way to reach a deep model row — the tokenizer is cumulative (threads syntax state
>   line-to-line) and capped at 256 rows/frame with no continued-redraw trigger, so scrolling
>   past a giant collapsed run can't catch up without either an uncapped catch-up (frame jank)
>   or a background tokenizer (VS Code's model). **Blocked on** that design decision.
> - **Windows `RunSubprocess` ignores `options.timeout_ms`** (MEDIUM, Windows-only)
>   (`platform/Subprocess.cpp`) — the branch waits `INFINITE`; a hung child blocks the caller
>   forever and `result.timed_out` never sets. The POSIX path enforces the deadline. Cannot be
>   compiled/tested on this Linux host, so deferred rather than landed blind.
> - **Terminal combining marks / ZWJ dropped after a double-width base glyph** (LOW-MED)
>   (`terminal/TerminalSessionOutput.cpp` `PutGlyphLocked`) — the base cell is resolved at
>   `cursor_column_-1` (the wide-trailing spacer, length 0), so the `length > 0` guard drops the
>   mark instead of attaching it to the lead at `cursor_column_-2` (emoji+VS, wide CJK+diacritic).
> - **Terminal IL/DL (`CSI L`/`M`) on the primary buffer edit the absolute deque** (LOW-MED)
>   (`terminal/TerminalSessionCsi.cpp`) — mixes a mid-deque insert with a front trim, shifting
>   scrollback/visible boundaries; rare (most such apps use the alt screen).
> - **Terminal C0/ESC bytes embedded mid-CSI/OSC are swallowed** (LOW, spec deviation)
>   (`terminal/TerminalSessionOutput.cpp`) — ECMA-48 says execute an intervening C0 and let a
>   bare ESC cancel the sequence; here every non-final byte is buffered.
> - **Terminal `[process exited]` marker on the alternate screen can evict visible rows** (LOW)
>   (`terminal/TerminalSessionOutput.cpp` `EmitProcessExitMarkerLocked`) — appends past the
>   alt-grid cap, and the trim erases from the top of the live grid.
> - **`ParseCsiParameters` heap-allocates a params vector per non-SGR CSI** (LOW, perf)
>   (`terminal/TerminalSessionCsi.cpp`) — the SGR fast-path already reuses a thread_local buffer;
>   these allocations count against the process-global alloc counter the perf harness measures.
> - **`AsciiGlyphAtlas::BlitInto` straight-copies (BLENDMODE_NONE)** (LOW) (`render/
>   AsciiGlyphAtlas.cpp`) — an adjacent glyph's transparent left padding erases a prior glyph's
>   right overhang; negligible for typical monospace fonts, visible for overhang fonts.
> - **`AlignHunkLines` 1×1 pairing gate ignores per-line similarity** (LOW / possibly by-design)
>   (`compare/CompareModel.cpp` `CanPairAlignedLines`) — a 1-del/1-add of two 0%-similar lines
>   always renders as a `Modified` row with a full intra-line diff.
> - **Closing the active tab onto a still-deferred neighbor loses its restored cursor/scroll**
>   (LOW-MED) (`workspace/WorkspaceTabCoordinator.cpp` `Close`) — the promote-deferred branch
>   does a bare `OpenFile` instead of applying the `deferred_handle`'s persisted caret/scroll
>   like `Activate()`, and leaves both `editor_state` and `deferred_handle` set.
> - **freedesktop trash `.trashinfo` written non-atomically** (LOW) (`platform/Trash.cpp`) — the
>   name is reserved only in `Trash/files/`; a lingering orphan `info/foo.trashinfo` is clobbered
>   and two concurrent trashes can race. Spec wants `O_EXCL` info-file creation.
> - **`ParseFloat`/`ParseDouble` are more permissive than the integer parsers** (LOW)
>   (`util/Parse.cpp`) — `strtof`/`strtod` accept leading whitespace, `+`, and hex-float that
>   `from_chars` rejects; a validation inconsistency, no live corruption.

> **Resolved 2026-07-10 (twelfth pass — cross-subsystem bug hunt):** a fresh fan-out
> across every subsystem landed nine fixes, each with regression coverage; the full
> suite + ASAN/UBSAN/TSAN stay green.
> - **gitignore `**/<wildcard>` now crosses directories** (`project/IgnoreMatcher.cpp`
>   `GlobMatches`) — a `**/` segment followed by a non-crossing `*`/`?` (e.g. `**/*.txt`,
>   `src/**/*.cpp`, `**/*.min.js`) had its cross-slash backtrack clobbered by the trailing
>   wildcard, so it matched at the top level only. Added a dedicated cross-slash `**`
>   fallback backtrack slot (`dstar_pi/dstar_ti`); O(1) extra state, fast path untouched.
> - **Editor left-click / drag honors horizontal scroll** (`workspace/
>   WorkspaceEditorMouseCoordinator.cpp`) — the caller fed `LogicalPositionForVisualHit`
>   an absolute visual column while the function re-adds the row's `visual_start`
>   (== `horizontal_scroll` in the non-wrap grid), double-counting the scroll and snapping
>   the caret to the right edge on every click once the line scrolled. Now passes the
>   screen-relative column (no-op under soft wrap). Distinct from the fifth-pass
>   vertical-motion double-count.
> - **Terminal CUU/CPL (`CSI A`/`F`) clamp to the visible-screen top**, not the scrollback
>   deque top (`terminal/TerminalSessionCsi.cpp`) — on the primary buffer `ESC[500A`
>   ("go to top of screen") climbed above the visible rows into history and overwrote it.
> - **Terminal VT (0x0B) / FF (0x0C) index like a line feed** (`terminal/
>   TerminalSessionOutput.cpp`) — they fell through the control switch and were dropped.
> - **Terminal DECSC/DECRC (ESC 7/8, `CSI s`/`u`) save/restore SGR + origin mode**, not just
>   the cursor position (`terminal/TerminalSessionScreen.cpp` + `TerminalSession.h`) — an SGR
>   change between save and restore leaked past the restore.
> - **Plugin `GetFieldProtected` no longer longjmps on a non-table base**
>   (`plugin/PluginLuaInterop.cpp`) — the metatable-less fast path called `lua_getfield`,
>   which raises "attempt to index a X value" for a number/boolean/nil base (e.g.
>   `ctx.completion.add(42)`), longjmping over the caller's live C++ locals. Now guarded on
>   `lua_istable`, reporting the field as nil for a non-indexable base — fixes ~16 registration
>   verbs at one centralized site.
> - **LSP stale-diagnostics version gate accepts a float-echoed version**
>   (`workspace/WorkspaceLspClientDispatch.cpp`) — `IsInt()`-only skipped the drop for a
>   server that serializes `"version": 3.0`, painting superseded ranges on the newer buffer.
>   Now `IsInt() || IsDouble()`, mirroring the response-id gate.
> - **Split-editor-with-path into an already-two-groups layout opens a new tab** in the target
>   group instead of overwriting its active tab in place (`workspace/WorkspaceActionServices.cpp`
>   + `editor_group_count` operations hook) — the in-place replace silently discarded that
>   group's unsaved edits. VS Code "open to the side" parity.
> - **Plugin text-style under/strike lines use grid geometry on the grid path**
>   (`editor/RowDecorationBuilder.cpp` `AppendTextStyleUnderlinesGrid`) — the proportional
>   `MeasureWidth` path mispositioned them on lines with a tab, a wide glyph, or horizontal
>   scroll; now resolved through `ResolveVisualColumn` like the changed-span sibling.
>
> **Second round of the twelfth pass** (a fresh fan-out after the first round) landed eight
> more fixes, each with regression coverage; full suite green:
> - **Terminal DECOM toggle (`CSI ?6h`/`?6l`) homes to the visible-screen top on the primary
>   buffer** (`terminal/TerminalSessionModes.cpp`) — passing a screen-relative 0 as an
>   absolute deque index jumped into scrollback and overwrote history (same family as the
>   CUU fix). Alt screen still selects the scroll-region top under origin mode.
> - **LSP completion documentation accepts `MarkupContent`** (`workspace/
>   WorkspaceLspClientRequests.cpp`) — the bare-string `take` dropped the `{kind,value}` object
>   form that clangd/rust-analyzer/gopls/pyright/tsserver all send, blanking the completion
>   doc pane. Now extracts either shape, mirroring hover/signature-help.
> - **Git partial-stage warning fires under porcelain v2** (`project/CommitWorkflowChecks.cpp`
>   + a new `worktree_dirty` bit on `GitRepositoryEntry`, set from the `Y` status in
>   `GitPorcelainV2Parser`) — v2 emits ONE record per path, so the old two-entries-per-path
>   `PathHasStagedAndUnstaged` never fired; a `1 MM` file is now correctly detected as both
>   staged and worktree-dirty.
> - **Control-socket fds are CLOEXEC** (`platform/ControlSocketServer.cpp`) — the listen
>   socket, accepted clients (`accept4`), and wake pipe (`pipe2`) leaked into every child
>   forked while the channel was live; matches the FileWatcher/Subprocess hardening.
> - **File rename/delete propagates to ALL editor groups** (`workspace/
>   WorkspacePathMutationCoordinatorTabs.cpp` + new group-aware `CloseGroupTab`/`CollapseGroupAt`)
>   — a split view of the affected file in a non-focused group kept a stale/defunct path and,
>   with all-groups autosave, wrote back to the old path (resurrecting a renamed/deleted file).
>   Non-focused groups are processed before the focused close so a focused-group collapse can't
>   promote a still-affected group to focused and skip it.
> - **Compare intraline changed-span underlines no longer double-dim** (`editor/
>   RowDecorationBuilder.cpp`) — the appenders pre-scaled alpha by 0.55 and `RenderRow` scaled
>   it again (~0.30); now they push full alpha like diagnostics/plugin underlines.
> - **Merge conflict-preview text is clipped at the pane bottom** (`workspace/
>   WorkspaceShellRenderMerge.cpp`) — a "Both" preview near the bottom drew trailing lines past
>   the clamped highlight into the bottom chrome; added the symmetric bottom guard.
> - **Terminal scrollback trim rebases the workspace-side absolute-row mirrors**
>   (`terminal/TerminalSession` exposes `ScrollbackTrimTotal()`; `workspace/
>   WorkspaceShellTerminal.cpp` `RebaseActiveTerminalForScrollbackTrim`) — a coalesced trim
>   decremented the session cursor rows but left the tab's `scroll_row`/selection/last-command
>   rows only clamped, so a scrolled-up view jumped forward by the trim batch and a held
>   selection / copied text pointed at the wrong cells.
> - **Plugin diagnostic `end_column` default saturates** (`plugin/PluginDiagnosticsInterop.cpp`)
>   — a plugin `column == INT64_MAX` made the `column + 1` default a signed-overflow UB before
>   the positivity check; now saturates.
>
> **Deferred from the twelfth pass (recorded, not fixed):**
> - **Terminal basic-color (30–37) brightness is baked from the bold flag at set-time and not
>   reverted by SGR 22** (`terminal/TerminalSessionSgr.cpp`). `\e[31;1m` vs `\e[1;31m` diverge,
>   and `\e[1;31m…\e[22m` keeps bright red. A fully-correct fix needs the cell to remember the
>   foreground is a basic-palette index so brightness resolves live at render (a value-comparison
>   hack can't distinguish an explicit bright `\e[91m` from a bold-brightened `\e[1;31m` for the
>   SGR-22 revert). Cosmetic; **blocked on** a palette-index cell representation.
> - **Terminal URL hit-test compares a grid column against a byte offset**
>   (`workspace/WorkspaceShellTerminal.cpp` `TerminalUrlAtColumn`). Only desyncs when a wide/
>   multibyte glyph precedes the URL on the line (uncommon); LOW.
> - **DECSTBM (`CSI r`) home ignores the scroll-region top under origin mode on the primary
>   buffer** (`terminal/TerminalSessionCsi.cpp`). Consistent with this terminal's primary CUP
>   (which ignores origin/scroll-region on primary), so it is a design-consistent LOW, not a
>   live divergence.
>
> (The `AsyncSubprocess::operator=`→`Shutdown` moved-from null-deref surfaced in this pass was
> fixed outright — a one-line `impl_ == nullptr` guard on BOTH the POSIX and Windows branches —
> rather than deferred.)
>
> **Third (convergence) round of the twelfth pass** verified all of the above against the code
> and found the tree converged. Two follow-through fixes landed:
> - **Non-focused rename retarget honors the user's Discard choice** (`workspace/
>   WorkspacePathMutationCoordinatorTabs.cpp`) — the all-groups retarget added earlier this pass
>   always kept the dirty buffer (`SetPath`); when the rename discards unsaved edits it now
>   reopens fresh from disk in background groups too, mirroring the focused path, so
>   "discarded" edits are not written back by all-groups autosave.
> - **Windows `AsyncSubprocess::Shutdown` moved-from guard** (mirror of the POSIX fix; can't be
>   compiled on this Linux host but keeps the branches symmetric).
> The only residual observations were the recorded deferrals above plus a cosmetic
> selection-copy choice (empty interior cells dropped vs. spaced) — not a defect. The full
> suite (1782 tests) plus ASAN, UBSAN and TSAN are all green.

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
> The `ParseFloat`/`ParseDouble` parity nit stays
> deferred (traced-benign; tightening it perturbs the JSON hot path for no live benefit). The
> environment-blocked (Windows/macOS) and product-decision (DAP multi-thread, branch-review
> `ChangedSinceReviewed`) items are unchanged.

> **Resolved 2026-07-09 (eleventh pass — the plugin-metamethod protected-harvest, its own pass):**
> the flagship deferred item — *"Plugin metamethod reads can longjmp over live C++ destructors"* —
> is fixed. The clean fix turned out smaller than feared: the **only** raising Lua ops in every
> harvest function are `lua_getfield`/`lua_geti`/`lua_gettable`; all of `lua_to*`/`lua_is*`/`lua_pop`/
> `lua_rawgeti`/`lua_rawlen` are metamethod-free and never raise. So only the field-fetch itself
> needs protection and every existing C++ marshaling step stays byte-for-byte unchanged.
> - **New single primitive `lua_interop::GetFieldProtected`** (`src/plugin/PluginLuaInterop.{h,cpp}`)
>   pushes `table[field]` through a nested `lua_pcall` (a zero-upvalue `LUA_VLCF` light-C-function
>   trampoline — no allocation) so a raising `__index` is caught as a status and the field is reported
>   as absent (nil) rather than longjmping over the caller's live locals. A table with **no metatable**
>   (the universal case for plugin config tables — and for registry/globals reads) takes an
>   allocation-free fast path identical to `lua_getfield` plus one cheap raw `lua_getmetatable` probe,
>   so there is no measurable perf change on the diagnostics-publish path. Benign `__index` *defaults*
>   still resolve (VSCode/JS prototype-chain parity) because the slow path uses `lua_gettable`.
> - **All field reads routed through it:** the ~9 centralized `Read*Field` helpers + `ReadSidebarItem`
>   were rewritten to call it (protecting the ~210 helper-mediated reads at a stroke), a new
>   `ReadOptionalIntegerField` helper was added, and the ~90 direct `lua_getfield` sites across the 14
>   harvest TUs were converted. The wrong "safe to call while C++ locals are live" comment on the
>   helpers (`PluginLuaInterop.h`) was corrected.
> - **New hard-fail lint `CheckPluginFieldReadsAreMetamethodProtected`**
>   (`tests/architecture/PluginArchitectureRules.{h,cpp}`) bans raw
>   `lua_getfield`/`lua_gettable`/`lua_geti` anywhere in `src/plugin` except `PluginLuaInterop.cpp`
>   (the sanctioned home of the primitive), so backsliding fails the build. `lua_rawgeti` stays
>   allowed. +/- fixtures added in `tests/ArchitectureInvariantsTests.cpp`.
> - **Regression coverage** (`tests/PluginHostTests.cpp`): `GetFieldProtected` catches a raising
>   `__index` without throwing (returns nil, stack-balanced), still resolves a benign `__index`
>   default, and reads a metatable-free table on the fast path; and end-to-end
>   `diagnostics_interop::PublishDiagnostics` on a list whose entry carries a raising `__index` fails
>   cleanly (no publish, error set) without longjmping over its live `std::vector<Diagnostic>` — ASAN
>   confirms no skipped destructor / leak. The full suite + ASAN/UBSAN/TSAN stay green.
>
> This retires the `lua_atpanic`→`throw` *mitigation*'s fragile "Lua C built with unwind tables"
> assumption for the read-in direction. The write-out direction stays governed by the existing
> `luaL_error` ban + `lua_error_util::PushMessage` idiom.

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

- **Plugin metamethod reads can longjmp over live C++ destructors — FIXED 2026-07-09
  (eleventh pass).** The protected-harvest primitive `lua_interop::GetFieldProtected`
  now backs every plugin field read, a hard-fail lint bans raw `lua_getfield`/`lua_gettable`/
  `lua_geti` outside its home TU, and regression coverage exercises a raising `__index`
  end-to-end under ASAN. See the eleventh-pass Resolved note above. Left here only as a
  pointer; no longer open. (The design shipped exactly as this entry contemplated — a
  nested-`lua_pcall` trampoline, NOT a blanket `lua_rawget` swap, so benign `__index`
  defaults still resolve.)

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

- **`TerminalBackend` `setenv`-after-fork — FIXED (seventh pass; entry was stale).**
  `src/platform/TerminalBackend.cpp` no longer calls `setenv` in the child: the child
  environment (inherited `environ` filtered of `TERM`/`COLORTERM`, plus
  `TERM=xterm-256color`/`COLORTERM=truecolor`) is built in the **parent** (`:88-111`) and
  the child does a single async-signal-safe pointer store `environ = env_pointers.data()`
  (`:142`). Only residual is a marginal **dedup**: `TerminalBackend` open-codes the
  parent-built-`environ` pattern instead of reusing `BuildChildEnvironment` (file-local
  static in `Subprocess.cpp`, would need extraction to a shared header). Left deferred as
  low-value — the real PTY fork path is still untested here (`SetUsePlaceholderTerminals-
  ForTesting`), so an extraction refactor carries regression risk for no correctness gain.

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
