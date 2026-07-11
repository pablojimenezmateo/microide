# MicroIDE Known Tech Debt

Reviewed on 2026-07-10.

This file is the queue for tech debt that is **open, actionable, and still present in the tree**.
It is deliberately short. Closed debt does not live here — it is archived (see below).

Use `dev-docs/project/active-work.md` for current priorities.

## Open items

These were surfaced by the 2026-07 cross-subsystem bug-hunt passes and
**deliberately deferred** — each is either entangled with a semantics decision or
a latent API-contract hazard with no live trigger, so a rushed fix risked a
regression worse than the defect. Recorded here so they are not silently lost.

> **Deferred 2026-07-11 (pass 16 — cross-subsystem bug hunt):** a four-way fan-out
> landed 4 fixes (editor Ctrl+D ranged multi-carets, terminal primary-screen RI
> scrollback floor, LSP `RebindReference` first-writer-wins, git conflict-marker
> false positive). These genuine lower-severity findings were **not** fixed and
> are recorded here:
> - **Merge: non-default `merge.conflictMarkerSize` breaks conflict-output parsing.**
>   `WorkspaceShellMergeState.cpp` `is_conflict_separator` matches the separator with
>   an exact `line == "======="` (exactly 7) while the start/base/end sigils use a
>   length-tolerant `starts_with`. If a repo sets `merge.conflictMarkerSize > 7`, the
>   `<<<<<<<<` line still enters the conflict branch but the `========` separator never
>   matches, so `ParseGitConflictOutput` returns nullopt and the resolver falls back to
>   raw text. Fix: match the separator with the same marker-size tolerance. Only affects
>   non-default marker size. (Git hunter #2.)
> - **Compare: ignore-whitespace toggle on a working-tree compare silently narrows
>   staging.** `ToggleCompareIgnoreWhitespace` applies to WorkingTree tabs that feed
>   patch-apply; under `ignore_whitespace` a whitespace-only-different line is emitted as
>   an `Unchanged` row and the patch generator uses `left_text` as context, so staging a
>   hunk omits the whitespace-only change (index stays != worktree) and discard/reverse
>   fail their `git apply --check`. No data loss (preflight gate protects discards) but
>   staging stages less than shown. Needs a semantics decision (disable patch-apply while
>   ignore_whitespace is active, or generate the apply patch from a non-ws-ignoring
>   model). (Git hunter #3.)
> - **Terminal: alt-screen linefeed below a custom scroll region scrolls the whole
>   screen.** `AdvanceCursorRowLocked` alt-screen fallback, when the cursor is *below* a
>   custom scroll region and on the last physical row, calls
>   `ScrollRegionUpLocked(0, terminal_rows-1, 1)` (whole screen). Per DEC/xterm, IND with
>   the cursor outside the region at the physical bottom should not scroll. Rare (needs a
>   header/status-split layout on the alt screen); low confidence. (Terminal hunter #2.)
>
> **Deferred 2026-07-10 (pass 5 — cross-subsystem bug hunt):** a six-way fan-out
> landed 8 fixes (see commit); the following genuine findings were deliberately
> **not** fixed this pass and are recorded here:
> - **Terminal: DECSTBM scroll region is not honored on the *primary* screen.**
>   `ScrollRegionUp/DownLocked` and the LF path early-return / ignore margins unless
>   `use_alternate_screen_`, and SU/SD (`CSI S`/`CSI T`) are alt-only. A bottom
>   status-line program that stays on the primary buffer scrolls the whole screen.
>   Implementing primary-screen regions touches scrollback interaction and is a
>   feature, not a one-line fix. (Terminal hunter #2.)
> - **Terminal: `CSI r` with an out-of-range bottom margin discards the whole
>   command** rather than clamping the bottom to the screen height (xterm/VTE clamp).
>   Low severity behavioral deviation. (Terminal hunter #6.)
> - **Terminal: primary-screen DL (`CSI M`) at absolute row 0 does not bump
>   `scrollback_trim_total_`**, so workspace scroll/selection mirrors can strand.
>   Narrow reachability; IL/DL are general content edits the mirror only partly
>   tracks — needs a semantics decision, not a rushed accounting patch. (Terminal #5.)
> - **Terminal: resize full-window scroll-margin re-expansion is applied only to
>   the active screen**, not the saved primary/alternate `scroll_region_*`; a resize
>   between alt-screen switches can restore a stale region. Masked in practice by
>   apps re-issuing `CSI r` on redraw. (Terminal hunter #1.)
> - **Plugin: contribution registrations lack the per-kind count cap** that
>   `registry_interop` enforces (`kMaxPluginContributionsPerKind`). The parallel
>   `contribution_interop::Register*` path (completions/code-actions/providers/etc.)
>   `push_back`s uncapped, so a tight `setup()` loop can balloon host RSS within the
>   750 ms watchdog. Best fixed by routing all contribution registers through the
>   shared cap helper — a focused refactor worth doing deliberately. (Plugin #3.)
> - **Windows-only platform gaps** (not built on the Linux-primary tree, so
>   informational): `AsyncSubprocess` Windows `Impl::state_mutex` is declared but
>   never acquired (Read/Write/Shutdown race → HANDLE UAF); `Subprocess::RunSubprocess`
>   Windows ignores `timeout_ms` (`WaitForSingleObject(INFINITE)`) and writes stdin
>   with a blocking `WriteFile` that can deadlock on a full pipe; `DurableFile::
>   RenameReplacing` is non-atomic on Windows (`remove`+`rename`). (Compare/platform
>   hunter #1–4.)
> - **Git porcelain-v2:** a `'2'` rename record consumes the following record as
>   origPath unconditionally; only matters on truncated/garbled `-z` output. Very low.

> **Deferred 2026-07-10 (pass 6 — cross-subsystem bug hunt):** a four-way fan-out
> (search/git, editor/compare/merge, persistence/util/control, terminal/plugin)
> landed 7 fixes (5 plugin harvest-loop clamps, the multi-caret `DeleteCurrentLine`
> stale-`last_applied_edit_` fix, and the `git diff --end-of-options` guard — see
> commit). Persistence/util/control and terminal came back clean. Two genuine
> findings were deliberately **not** fixed and are recorded here:
> - **Editor: `MoveLineUp`/`MoveLineDown` drop the selection when it ends at
>   column 0 of the line after the block.** `ResolveLineRange` decrements the block's
>   `last` to exclude that trailing line, but `RestoreCaretsAfterLineMove` gates
>   selection restoration on `selection->end.line <= range_last`, so the original
>   `end.line == range_last+1` fails the guard and the code falls to the single-caret
>   branch — the block stays moved but the selection is lost (self-corrects on next
>   selection). A correct fix must shift the trailing column-0 boundary by `delta`
>   and clamp an out-of-bounds end across all orientations (upward selections,
>   MoveLineUp at top-of-file); a subtly-wrong selection is worse than the safe
>   fallback, so this wants a deliberate fix with dedicated tests. (Editor obs #2.)
> - **Git porcelain-v2 empty-path `'2'` record** (already noted under pass 5): the
>   re-audit confirmed it is truncation-only and harmless — the stray origPath
>   record is dropped by `default: break`. Left as-is. (Search/git #2.)

> **Deferred 2026-07-10 (pass 7 — cross-subsystem bug hunt):** a four-way fan-out
> (workspace coordinators, LSP/DAP clients, render/view-model/layout, async/subprocess/
> debug) landed 11 fixes (see commit) plus folding in the pre-staged gdb-detection
> fix. The following genuine findings were deliberately **not** fixed and are recorded:
> - **DAP: capabilities race if `initialized` precedes the `initialize` response.**
>   If a non-conformant adapter emits the `initialized` event before its `initialize`
>   response and the main thread drains it before the response is parsed,
>   `DebugSession::HandleEvent("initialized")` reads default (all-false) capabilities,
>   so `supports_configuration_done_request` is misread and launch/configurationDone
>   ordering flips. The DAP spec requires `initialized` *after* the response, so gdb/
>   lldb-dap/debugpy never trigger it. Close by gating the handler on
>   `client_->IsInitialized()` (defer/re-post if false). (LSP/DAP hunter #2.)
> - **render: `ColorMath::BlendColors` omits the `std::clamp` that `CompositeOver`
>   has.** Provably in-range for the current lerp inputs, so it's a harmless asymmetry
>   rather than a defect; left as-is to avoid adding a branch to a hot color path.
>   (Render hunter hardening note.)

> **Deferred 2026-07-11 (pass 8 — cross-subsystem bug hunt):** a four-way fan-out
> (sidebar/tree/finder, settings/config/responsive, plugin lifecycle/sandbox,
> terminal PTY/session) landed 6 fixes (see commit): terminal bracketed-paste
> marker reconstitution (HIGH security), settings store-path divergence (font_size
> 999 stored raw not clamped), plugin setup-failure UAF (HIGH — live contributions
> left bound to a destroyed lua_State), DirectoryTree byte-prefix containment, and
> the git-sidebar keyboard-nav / collapse-snap desync. The following were
> deliberately **not** fixed and are recorded:
> - **Plugin: provider-query loops dereference `provider.state` (StackResetGuard +
>   lua_rawgeti) before the `find_plugin_by_state == nullptr` guard** (~15 sites in
>   PluginProviderQueryInterop / PluginLanguageProviderQueryInterop). With the pass-8
>   setup-failure UAF fixed, the runtime vectors only ever hold live states, so this
>   is latent hardening — the guard is currently non-functional (it dereferences the
>   state it means to reject). Fix by resolving the plugin and bailing BEFORE touching
>   `state`. Deferred to avoid reordering 15 loops (each with distinct return
>   semantics) in the same pass. (Plugin hunter #2.)
> - **Plugin: `ResetForDisabledRuntime`/`ShutdownForDisabledRuntime` clear only a
>   subset of contribution containers** (commands/sidebars/hovers/plugins), not the
>   runtime-provider vectors. Currently UNREACHABLE — `enabled()` only ever goes
>   true→false once at startup before any plugin loads, with no path back — so the
>   disabled containers are always empty. Route through the full container reset if a
>   runtime enable/disable toggle is ever added. (Plugin hunter #3.)

> **Deferred 2026-07-11 (pass 9 — cross-subsystem bug hunt):** a four-way fan-out
> (snippet/folding/advanced-editor, compare/merge patch+staging, control-channel
> deep, undo/redo internals) landed 6 fixes (see commit): folding scroll-resolve
> gate (folds below the 2000-line compute budget never appeared on scroll), folding
> incremental-resume dead path (perf — always full-rescanned unless a fold was
> collapsed), control-channel `--json` id:null correlation (false timeout/exit 2),
> the MoveCursorTo typing-coalesce boundary (undo merged edits across a click/goto
> jump), and two git-apply-verified patch-generation bugs (one-sided phantom
> trailing line inflating the @@ old count; `\ No newline` marker on a non-terminal
> isolated hunk). The following were deliberately **not** fixed and are recorded:
> - **Control: `--timeout N` can block ~2N seconds** when a peer accepts but never
>   reads — SendLine and the read loop use two independent full-timeout deadlines
>   instead of one shared overall deadline. Low. Fix: compute one deadline before
>   SendLine and pass the remaining budget to both. (Control hunter #2.)
> - **Control: command replies can carry stale `panel.feedback`** from a prior
>   unrelated action (ExecuteControlCommand reports the shared panel feedback string
>   without snapshotting/clearing it before dispatch). Low, somewhat uncertain — a
>   headless driver may see a misleading `feedback` on an ok:true reply. Fix: snapshot
>   feedback before ExecuteCommandLine and report only if it changed. (Control #3.)
> - **Undo: nested undo groups double-fold child edits into the outer frame.**
>   RecordEntry folds each child into every frame on group_stack_, so a re-pushed
>   inner aggregate is folded into the outer frame on top of the raw children it
>   already received. LATENT — no current BeginUndoGroup caller nests a second group
>   inside an open one — but the multi-frame design invites future nesting. Fix: fold
>   each edit only into the innermost frame and propagate inner aggregates outward on
>   finish. (Undo hunter #2.)

> **Deferred 2026-07-11 (pass 10 — cross-subsystem bug hunt):** a fan-out over
> text-search, LSP code-actions, and the git repository/compare services landed 4
> fixes (see commit): incremental-search refine of a self-overlapping needle
> (`RefineLiteralSearchMatches` kept overlapping ranges — e.g. "aa" over "aaaa" —
> desyncing the count/next/prev/replace vs a fresh scan; now de-overlaps like the
> cold path), LSP code-action **context** diagnostics were sent with raw editor byte
> columns instead of the server's position encoding (quick-fix mis-targeted on
> non-ASCII lines under clangd/UTF-16; now routed through `ByteColumnToLspPosition`
> like the request range), a HIGH git-refresh state-machine freeze/counter-leak
> (`GitRepositoryService`: the `PublishSnapshot` generation-mismatch early-return and
> the `ScheduleRefresh` best-effort early-out bypassed the counter decrement and the
> deferred-follow-up hand-off, so a generation-bump race left `refresh_in_flight_`
> stuck true — sidebar silently stopped updating until `Reset()` — and leaked the
> background-task counter; both now route through one `HandleSupersededRefresh`
> helper), and a LOW `--end-of-options` consistency gap on the explicit-revision git
> args (`diff-tree <hash>`, `cat-file -e <rev>:<path>`, `show <rev>:<path>`). The
> following was deliberately **not** added and is recorded:
> - **LSP: no dedicated integration test for `CollectLspContextDiagnostics` column
>   conversion.** The fix routes diagnostic byte columns through the same
>   `ByteColumnToLspPosition` primitive already covered by `LspPositionEncodingTests`
>   / `LspViewportPositions` round-trip tests, and identical to the code-action range
>   path in the same function. A full-shell test would need TestAccess to expose the
>   private method plus a populated diagnostics store and a non-ASCII viewport —
>   disproportionate for a one-line routing change over a tested primitive. If the
>   context-diagnostics path grows logic beyond the raw conversion, add the shell test.
> - The syntax-highlighting subsystem was not fully re-hunted this pass (a hunter
>   aborted on an environment/session limit); carry it into the next pass.

> **Deferred 2026-07-11 (pass 11 — cross-subsystem bug hunt):** a four-way fan-out
> (syntax highlighting, terminal/PTY, editor primitives, persistence + plugin
> runtime) landed 4 fixes (see commit): a syntax-highlight perf win (state-only
> `AdvanceState` replay ran and discarded every pattern-rule regex per line —
> `HighlightLineScoped` now skips token work when `want_tokens` is false, halving
> regex work on the visible screen's leading lines and eliminating it entirely for
> the up-to-512-line synchronous resume-state replay prefix), single-line paste
> injecting raw CR/LF (a whole-line copy carries a trailing `\n`; `SingleLineEditor::
> Insert` — the choke point for both Ctrl+V and text-input paste — now strips line
> breaks so a control byte can't corrupt the search needle / goto-line / rename
> value), a terminal soft-wrap-onto-existing-row flag bug (`AdvanceCursorRowLocked`
> only stamped `wrapped_from_previous` on freshly-created rows, so a line printed
> after a cursor-up that wrapped onto an existing row below split the logical line
> in two for reflow / command capture), and terminal invalid-UTF-8 storage (an
> overlong/surrogate scalar was stored as raw bytes; now substituted with U+FFFD).
> Also a persistence micro-perf: `PersistedRecordReader::Decode` no longer copies
> the whole input buffer (up to the 256 MB cap) before decoding from a span. The
> following were investigated and deliberately **not** changed, recorded:
> - **Terminal: a combining mark after a double-width glyph attaches to the
>   wide-trailing spacer, not the lead** (hunter #2). Investigated: NOT a live bug —
>   the inline cell is a 4-byte buffer and the combining-append already guards
>   `cell.length + glyph.size() <= 4`. Every double-width codepoint is ≥3 UTF-8
>   bytes and every combining mark is ≥2, so 3+2 > 4 means the mark is dropped
>   regardless of which cell it targets. The retarget would be dead code unless the
>   cell buffer grows; revisit only if `TerminalCell::bytes` is ever widened.
> - **Terminal: ESC inside a charset-designation (`ESC ( <ESC>`) is consumed as the
>   designator final** instead of aborting the designation (`EscapeMode::
>   CharsetDesignate`). VERY LOW, malformed-input only. Fix: treat `0x1b/0x18/0x1a`
>   as an abort there like the CSI/OSC states do. (Terminal hunter #4.)
> - **Syntax: the thread-local `ReusableMatchData` cache keyed by `CompiledRegex*`
>   is not invalidated on `ReloadDefinitions`** — a rebuilt rule can reuse a freed
>   address's match-data block. Benign in practice (syntax rules only read
>   ovector[0]/[1], always present, so no overflow), but latent. Fix: clear the
>   cache on registry-revision change or key it by revision. (Syntax hunter.)

> **Deferred 2026-07-11 (pass 12 — cross-subsystem bug hunt):** a four-way fan-out
> (compare/merge, DAP debugger, LSP service, project file-index + control) landed 4
> fixes (see commit): a HIGH compare data-loss bug (`PatchApplyService::BuildRequest`
> line-scope used `selection->end.line` directly, but `selection_range()` reports an
> EXCLUSIVE end — a whole-line selection ending at column 0 of line N+1 dragged in
> line N+1, so Discard Selected Lines silently destroyed an unselected working-tree
> change; now applies the same `end.column==0 && end.line>start.line` correction used
> everywhere else), a HIGH LSP desync (an on-disk buffer reload replaced the editor
> content without any `didChange`, so the server kept the pre-reload text and the next
> incremental edit corrupted its mirror; `TabCoordinator::ReloadEditorTabsForPath` now
> re-syncs via a new `notify_lsp_buffer_reloaded` hook → `SyncLspForBufferChange`), a
> LOW file-index teardown stall (the inotify overflow-recovery rescan polled the
> initial-scan thread's stop flag, which `StopNative` resets to false before joining
> the worker, so a teardown mid-recovery blocked for the full scan budget; now polls
> the worker-scoped `stop_native_setup`), and a LOW DAP correctness gap (`SwitchThread`
> did not bump `stop_epoch_`, so a stale stackTrace from the previous thread was not
> dropped despite `RequestStackTrace`'s comment claiming thread-switch supersession;
> now bumps the epoch like `Reactivate`). Also a persistence micro-perf from the pass-11
> hunter's note (`PersistedRecordReader::Decode` no longer copies the input buffer).
> The following were deliberately **not** changed and are recorded:
> - **File index: initial bulk-load can clobber/resurrect files changed during the
>   scan window (MEDIUM).** `FileIndexWatcher::Watch` starts the native inotify worker
>   and the initial-scan worker as two threads that both drive the single SetCallback
>   lambda with only a shared generation guard; an incremental batch applied before the
>   trailing `is_initial` batch (which `FileIndex::ApplyBatch` applies as a wholesale
>   `files_` replace) is lost — a file created during the scan goes invisible until its
>   next modification, a file deleted during the scan resurrects as a ghost entry.
>   Data-race-free (mutex) but logically wrong; reachable on large trees during an
>   active build/`git checkout`. NOT fixed because the correct fix is a concurrency
>   refactor (buffer non-initial batches behind an "initial-applied" gate per watcher
>   generation, then replay them on top of the initial baseline) across two concurrent
>   callback threads that cannot be deterministically regression-tested; deferred rather
>   than rush a threading change validated by a single end-of-session *SAN pass. Fix
>   direction: add a `std::mutex` + `bool initial_applied` + pending-batch vector to the
>   coordinator (reset in `WatchProjectFileIndex` before `SetCallback`, safe because
>   `StopFileIndexWatcher` joins first), refactor the callback body into a per-batch
>   helper, and on the initial batch replay the buffered incrementals in order.
> - **LSP: no end-to-end regression test for the reload `didChange`.** Driving it needs
>   a full shell + fake LSP server with an open served document (SyncLspForBufferChange
>   no-ops without one), disproportionate for a hook that funnels into the already-tested
>   `SyncLspForBufferChange` bulk-sync path. If the reload path grows logic beyond the
>   single full-document sync, add the integration test.
> - **LSP: tracked-request UI expiry (10s) < transport deadline (30s) + single
>   `request_in_flight` flag** can't represent two concurrent interactive requests
>   (hover during an in-flight completion), so the "LSP: working…" indicator can flicker
>   or clear early. LOW, cosmetic only — callbacks are correctly balanced, no lifecycle
>   leak. (LSP hunter #2.)
> - **Merge: `WorkspaceShellRenderMerge.cpp:497` hover-preview allocates a
>   `vector<string>` per frame** via `MergeChoiceLines` while a result-action button is
>   hovered — bounded, hover-only, not a regression; cache if that path gets hot.

> **Deferred 2026-07-11 (pass 13 — cross-subsystem bug hunt):** a four-way fan-out
> (render/view-model, git commit workflow/blame, prompt/finder/fuzzy, snippet/layout)
> landed 6 fixes (see commit): hover/diagnostic/signature popup no longer overflows its
> unclipped card when a single word exceeds the wrap width (`WrapEditorHoverPopupText`
> truncates the oversized word); the IME/text-input caret anchor guards its size_t
> `visual_column - h_scroll` / `visual_row - scroll` subtractions against underflow;
> the empty file-finder recents list no longer drops in-root dot-directory files
> (`.github/…`, `.vscode/…`) — the escape-root guard now tests the first path COMPONENT
> for `..` instead of the first byte for `.`; the commit/compare picker and the command
> palette filter via a precomputed lowercased `search_text` instead of re-lowercasing +
> concatenating per item on every keystroke (the commit picker can hold thousands of
> commits); and the commit-workflow refresh no longer runs the identical `git diff
> --cached --numstat` staged-diff subprocess twice per refresh (`RunCommitPreChecks`
> takes an optional precomputed summary). Deliberately **not** changed and recorded:
> - **Merge conflict-preview overlay slices by codepoint using a visual-column scroll
>   offset** (`WorkspaceShellRenderMerge.cpp:536`): `SliceVisibleColumns`' start arg is a
>   codepoint count (`Utf8ByteOffsetForCodepointCount`) but `merge_tab->horizontal_scroll`
>   is a tab-expanded visual column, so a preview line with tabs before the scroll offset
>   slices at the wrong byte when horizontally scrolled. LOW, cosmetic — independently
>   flagged by two hunters; the preview draws via non-grid `DrawStringOn` which renders
>   tabs poorly anyway, so a partial fix wouldn't fully align. Fix direction (if made
>   airtight): derive a byte offset via `TextLayout::TextColumnForVisualColumn(line,
>   horizontal_scroll, tab_size)` rather than feeding the visual column to a codepoint slice.
> - **Commit-workflow `RefreshDerivedState` still runs its git work synchronously on the
>   shell thread** (`git diff --cached --numstat` + the full `git diff --cached` marker
>   scan). The duplicate subprocess is now removed, but moving the whole refresh onto
>   `ProjectBackgroundExecutor` (marshalling results back via the completion mailbox, as
>   `DispatchCommit` already does) remains a larger follow-up. MEDIUM-perf, throttled to
>   field-switch (not per-keystroke) so bounded. (Git hunter #1, second half.)

> **Deferred 2026-07-11 (pass 14 — cross-subsystem bug hunt):** a four-way fan-out
> (sidebar/tree/catalog, session-restore/tab-service, plugin interop, app/event-loop/
> settings) landed 3 fixes (see commit): a MEDIUM plugin use-after-free (the blocking
> provider-query failure branches in `PluginProviderQueryInterop.cpp` — DiscoverTests,
> RunTests, SnapshotScm, auth login/refresh/logout, mcp tool — read `it->id` after a
> plugin PCall that runs with allow_registration=true, so a callback that registers
> another provider reallocates the runtimes vector and dangles `it`; now uses the
> caller-owned `provider_id`/`tool_id`, which equals `it->id` for the matched item);
> a MED-perf sidebar tree fix (`DirectoryTree::AppendDirectory` copied the accumulated
> `IgnoreMatcher` + stat/opened a usually-absent `.gitignore` for every COLLAPSED
> directory child on every refresh, then discarded it — the dir's own .gitignore only
> affects grandchildren walked when expanded, so that work now happens only for
> expanded dirs); and a background-task-counter leak in `ProjectSearchService::Start`
> (the decrement lived in the task body, leaked when `Stop()`→`CancelAll()` dropped a
> still-queued search — now an RAII shared_ptr guard that fires on task destruction
> whether it ran or was dropped). The session-restore/tab-service subsystem came back
> clean. NOTE: the plugin UAF fix is validated by the end-of-session ASAN run (a
> deterministic trigger needs a realloc mid-PCall). Deliberately **not** fixed, recorded:
> - **`GitRepositoryService` double-counts the global background-task counter (LOW,
>   latent).** `ScheduleRefresh` increments via `wake_callbacks_.increment_...` (wired
>   to `app::IncrementBackgroundTaskCount`) AND `background_executor_.PostLatest` fires
>   the `SerialWorkQueue` on_enqueue hook (also `app::Increment`). Normal completion
>   decrements twice (manual + on_complete hook); a PostLatest dedup-drop / Shutdown-
>   cancel fires only the hook → net +1 leak per dropped refresh, plus one redundant
>   idle wake per completion. LATENT: `GetBackgroundTaskCount()` has ZERO readers (the
>   counter value drives nothing; only the decrement's SDL wake is functional), so no
>   functional impact today. NOT fixed because the round-10 refresh state machine AND
>   its tests (`TestSyncRefreshBalancesBackgroundTaskCount`, `ConcurrentRefreshBurst...`)
>   are built around the manual counter (tests override `wake_callbacks` to a local
>   counter the queue hooks don't touch), so removing the redundant manual pair is a
>   coupled refactor for a dead counter. Fix direction: drop the manual increment/
>   decrement, rely on the executor hooks, and move the sync-test path's balancing into
>   `RunRefreshSynchronouslyForTesting` locally; update those two tests.
> - **`DirectoryTree` `expanded_paths_` / `manually_collapsed_paths_` are never pruned
>   for deleted directories (LOW).** Only wholesale-`clear()`ed (SetRoot/CollapseAll/
>   RestoreExpansionState); a slow bounded leak (distinct dirs ever expanded) that also
>   writes stale keys into the persisted session via `ExpandedRelativePaths()`.
>   Functionally harmless (`IsExpanded` on an absent path is never queried). Fix: after
>   `RebuildEntries`, intersect the sets with directories still present in `entries_`,
>   or drop non-existent keys lazily when serializing.
> - **Plugin: `QueryAnnotations` range-for + the sync completion/code-action/symbol
>   overloads share the same reallocation hazard (LOW, latent/test-only).**
>   `QueryAnnotations` holds a range-for across a registering PCall (worse — can fault on
>   the success path too) but has no production caller; the sync `*Query*` overloads are
>   test-only (production uses the `*Async` variants with allow_registration=false). Fuller
>   systemic fix: run these blocking provider callbacks with allow_registration=false
>   (matching the async path + the stated design intent) so the vector can't mutate mid-iteration.

> **Deferred 2026-07-11 (pass 15 — cross-subsystem bug hunt, session wrap-up):** a
> four-way fan-out (undo/folding, control-channel deep, DAP variables/eval, cross-cutting
> lifetime/thread-safety) landed 3 fixes (see commit): a control-channel TOCTOU hardening
> (`ControlChannelService` descriptor read now caps the ACTUAL read at
> kMaxDescriptorFileBytes instead of trusting the `file_size` pre-check, so a hostile
> local writer that grows the descriptor after the stat can't OOM/stall the driver),
> strict `enabled` boolean validation in `ControlSpec` breakpoints/function-breakpoints
> (was silently coerced via AsBool(true)), and first-writer-wins for DAP
> `reference_to_node_` (a non-conformant adapter recycling a live variablesReference can
> no longer remap it and graft a later page onto the wrong node). Undo/redo internals and
> the DAP variables/scopes generation-guard paths came back clean. Two MEDIUM findings were
> deliberately **not** fixed in this wrap-up pass and are recorded for a focused follow-up:
> - **Folding: collapsed folds silently re-expand after a line-count-changing edit ABOVE
>   them (incl. undo/redo of such edits).** `FoldingModel::RemapCollapsedFlags`
>   (FoldingModel.cpp:316) is the sole carrier of user collapse state across the full
>   recompute that runs after every edit, and it matches previous→new collapsed openers by
>   ABSOLUTE (opener_line, closer_line). The model never shifts fold ranges by an edit
>   delta (it is recompute-based; `incremental_resume_line` only anchors the bracket
>   rescan). So inserting/deleting a line above a collapsed fold shifts its opener, the
>   absolute match fails, and the collapse is dropped (the fold visually re-expands).
>   Edits below, or in-line edits that don't change the line count, preserve collapse (line
>   numbers unchanged) — which is why it's easy to miss. VS Code shift-preserves. MED (UX,
>   not a crash/data-loss). NOT fixed now because the correct fix plumbs the net edit
>   line-delta (available on the edit path: the fold edit anchor + inserted/removed line
>   counts) through `ComputeWithBudget` into `RemapCollapsedFlags`, which then shifts the
>   previous collapsed openers at/after the edit anchor by the delta BEFORE the
>   (opener_line, closer_line) compare — a multi-layer change in a perf-sensitive subsystem
>   that needs its own test pass, not a wrap-up rush.
> - **`AsyncSubprocess` Windows backend is unsynchronized (MEDIUM, Windows-only).** The
>   POSIX `Impl` takes `state_mutex` and re-fetches the fd under the lock before
>   read/write (guarding against a concurrent Close() letting the OS reuse the descriptor);
>   the `#if defined(_WIN32)` `Impl` (AsyncSubprocess.cpp:74) DECLARES `state_mutex` but
>   never locks it, and uses a plain `bool running` (POSIX uses `std::atomic<bool>`). The
>   LSP/DAP io thread (`Read`/`PeekNamedPipe`/`ReadFile`), shutdown thread (`Shutdown`→
>   `CloseHandle`), and UI thread (`IsRunning`) touch `running`/`exit_code`/the HANDLEs
>   lock-free → data race + use-after-close/handle-reuse on Windows. NOT fixed now because
>   it can't be compiled or TSAN-validated on this Linux workstation; writing untested
>   Windows synchronization risks breaking the Windows build. Fix direction: mechanically
>   mirror the POSIX branch — lock `state_mutex` around every state access, re-fetch the
>   HANDLE under the lock immediately before Peek/Read/Write and bail if it changed, and
>   make `running` atomic. (Cross-cutting lifetime/thread hunter; POSIX paths verified clean.)

> **Resolved 2026-07-10 (bug-inventory review pass — `microide-2026-07-10-bug-inventory.md`):**
> a ~90-item inventory built against `main` was triaged against the current tree (many
> items were already closed by the twelfth/thirteenth passes) and ~50 still-live items
> were fixed with regression coverage; the full suite stays green. Highlights:
> - **Path/URI security cluster** — strict `file://` parser (empty/`localhost` authority
>   only, strict percent-decode, NUL reject) in `FileUri.cpp`/`Hex.h`; malformed
>   code-action/rename edit URIs are dropped instead of falling back to the active buffer
>   (`AssistService.cpp`); server `workspace/applyEdit` targets are confined to the project
>   root or an open buffer (`LspService.cpp`); file-index watcher, `FileIndex::ApplyBatch`,
>   and `ProjectTraversalFilter::Includes` reject `..`-escaping / absolute relative paths;
>   plugin fs containment fails **closed** on canonicalization error and drops its stale
>   root cache (`PluginPathInterop.cpp`).
> - **Split-view coverage** — replace-all dirty-guard + post-write reopen, compare/merge
>   external-change invalidation, and control-channel `tabs`/`tabCount` now scan every
>   editor group, and replace-all surfaces a precise "save open changes first" error.
> - **Correctness** — compare `review_mode`/`staging_view` persisted in the binary tab
>   schema; snippet backspace/delete honor UTF-8 boundaries and reject multi-line mirror
>   inserts; LSP diagnostic ranges treated as half-open; DAP `stopped`/`continued` respect
>   optional `threadId`/`allThreadsContinued`; breakpoint verify stores requested+resolved
>   lines; compare/git-blob truncation and unreadable/binary worktree files no longer
>   render as empty; copied compare patches are real `git apply`-able unified diffs with
>   git-quoted header paths; CRLF incremental `didChange` re-encodes replacement text.
> - **Robustness caps / validation** — JSON RFC-8259 number grammar + raw-control-char
>   rejection; snippet/plugin-env/decoration/MRU/debug-state decode caps; plugin launch
>   config JSON + tool sha256 validation; `O_EXCL` exclusive file create; atomic control
>   descriptor write (temp+rename, 0600/0700 perms); porcelain-v2 ahead/behind full-consume;
>   protocol int-range narrowing guard; hover-content byte cap.
> - **Throwing-probe / crash hardening** — workspace `std::filesystem::exists`/`relative`
>   probes on the UI command path switched to the `error_code` overloads.

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

- **LSP incremental `didChange` LF-joins replacement text on CRLF documents — RESOLVED
  2026-07-10 (bug-inventory pass).** `SyncLspForActiveEditableLastChange` now re-encodes
  the incremental replacement text to CRLF when `viewport->line_ending() == CRLF` (guarded
  so LF documents skip the work entirely), exactly the deferred "do the transform only for
  CRLF" plan. (The sibling "cross-language go-to-definition uses the source server's
  encoding" suspicion was investigated and is NOT a bug — a `definition` response's
  `character` is in the responding/source server's encoding regardless of the target file,
  which is exactly what `AssistService::NavigateToLspLocation` uses.)

- **DAP partial `continued` / breakpoint relocation — RESOLVED 2026-07-10 (bug-inventory
  pass).** `continued` now performs a full resume only when `allThreadsContinued` is true
  (a single-thread continue leaves the focused stopped view intact), and
  `DebugServiceCallbacks` anchors verified breakpoints to the requested line while
  recording the adapter id so the async `breakpoint`-event path reconciles by id instead of
  clobbering by relocated line. Full per-thread stop/resume *state* (re-keying
  `BreakpointStore`, per-thread run flags) remains an explicit non-goal until multi-thread
  debugging is a funded feature.

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

The 2026-07-10 bug-inventory review pass fixed ~50 items but **deliberately deferred**
these (entangled with a semantics/data-model decision, needing a platform primitive, or
a perf change that must be measured before landing — speed is the priority):

- **`FileOperationService::RenamePath` still has a check-then-rename TOCTOU window** (HIGH
  by label, tiny window in practice). `H1` (create) was fixed with `O_EXCL`; the no-replace
  rename needs a `renameat2(RENAME_NOREPLACE)` primitive with a portable fallback (old
  kernels/filesystems return `ENOSYS`/`EINVAL`) plus a Windows equivalent. **Blocked on:** a
  platform `MovePathNoReplace` seam. The synchronous exists-check already covers the
  non-racing common case.
- **Persisted-record commit is not two-generation.** `PersistedRecordWriter::WriteFile`
  removes the old `.bak` before the new primary is durable, so a crash mid-rename can lose
  the last-known-good backup. Needs a `.bak.next`-then-atomic-promote protocol plus
  crash-point tests. (Reader-side recovery signalling was already fixed — `I16`.)
- **Cross-device `platform::MovePath` fallback leaves partial/duplicated trees on failure**
  (`FsOps.cpp` rename→copy→remove). Needs a staged temp-destination move with rollback and
  partial-failure reporting.
- **Symlinked project-file save can write a target outside the workspace root.**
  `WriteTextFileAtomically` resolves the symlink and writes the resolved target with no
  root check. A real fix threads the project root through every save caller and adds an
  explicit "edit target of an out-of-root symlink" policy (reject vs. confirm) — a
  cross-cutting save-API change entangled with a UX decision.
- **Following out-of-root directory symlinks still indexes escaping `..` paths.** The
  untrusted-input paths (native watcher, `ApplyBatch`, traversal filter) now reject `..`,
  but `CollectProjectFiles` with the opt-in `project.follow_out_of_root_symlinks` setting
  still yields `../outside/...` relative entries by design. A clean fix stores a typed
  symlink-root identity instead of `..` paths, or drops the follow feature. **Blocked on:**
  deciding whether out-of-root symlink *contents* are a supported feature.
- **Rename/delete/trash dirty-guards and the replace-all all-group work do not yet carry
  `(group_index, tab_index)`.** `DirtyPathTargetsForPath`/`AffectedCompareTabIndices`/
  `AffectedMergeTabIndices` still detect dirty tabs in the focused split only (retarget/close
  are already all-group). Making detection group-aware needs `(group,tab)` addressing threaded
  through the prompt payloads — a data-model change too risky for a rushed pass.
- **LSP `workspace/applyEdit` still can't report partial application, and ignores
  `TextDocumentEdit.version`.** The parser drops create/rename/delete resource ops and
  silently truncates at the file/edit caps, then replies `applied: true`; and stale-version
  edits are applied without a version check. Needs parser status plumbed into the reply plus
  optional-version tracking through the flattened `CodeActionEdit`.
- **Multi-file rename can partially commit, and closed-file LSP edits can overwrite external
  modifications.** `CommitPendingRenameSave` mutates+saves open buffers before writing closed
  files with no rollback, and stores no `(mtime,size,hash)` signature to detect a closed
  target changed since the prompt. Needs a preflight-all-then-commit-all rewrite of the save
  path.
- **Closed-file LSP diagnostics published in UTF-16/32 are stored as byte columns and not
  re-encoded on open** (`E1`). Store raw LSP diagnostics for closed files and convert lazily
  against a viewport, or force a reconversion in `EnsureLspDocumentOpen`.
- **Plugin host mutations carry no project/reload epoch** (`D2`); **plugin `ctx.process.run`
  and save participants run synchronously on the single Lua worker with no cancellation/budget**
  (`D3`/`J11`); **tool download + hashing block the caller and shell out via
  `project::RunSubprocess` from a workspace TU** (`J20`/`A3`, formatter has a 5 s backstop);
  **plugin tool `url` only resolves local sources — HTTP(S) fetch is unimplemented** (`J21`).
  Each needs a bounded async executor / worker-isolation design.
- **Plugin string-field readers truncate embedded NUL on some paths** (`J34`) — route all
  extraction through a length-preserving `lua_tolstring` helper with a per-field NUL policy.
- **Snippet parsing does not implement the LSP/TextMate escape + nesting grammar** (`J48`)
  despite advertising `snippetSupport` — implement the supported subset or downgrade the
  capability.
- **Inline git blame for a dirty buffer uses the on-disk file, not the editor buffer**
  (`J5`) — feed viewport text via `git blame --contents -`, or suppress blame for dirty
  buffers.
- **Project search does not surface skipped unreadable/oversized files** (`I7`) and
  **`count_all_matches` has no time/work budget** (`J58`) — add a skipped-file count to the
  result and a budgeted "10000+" total.
- **Terminal scroll-region ops are alternate-screen only** (`F3`) — define + implement (or
  explicitly ignore) primary-screen DECSTBM semantics.
- **Control runtime dir is not refused when not owned by the current user** (`J53` residual)
  — the descriptor is now atomic + 0600 and the dirs 0700, but an ownership check is still
  missing.
- **Perf follow-ups (speed is the top priority, so these are the highest-value next batch,
  deferred only because each needs `MICROIDE_PERF_TRACE` before/after measurement):** merge
  side-effect edits snapshot the whole result document per keystroke (`A4`); merge
  hit-testing/rendering scans conflicts O(rows×conflicts) and rebuilds candidate/context
  vectors (`B5`/`B6`); deep merge jumps tokenize the off-screen prefix first (`B7`); grouped
  undo fallback snapshots a full `TextBuffer` (`B8`); several render TUs still materialize
  label/URI/preview strings per frame (`B1`/`B2`); merge previews recompute choice lines
  during render (`B2`).

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
