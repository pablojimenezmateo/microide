# MicroIDE Known Tech Debt

Reviewed on 2026-07-13. A follow-on deferred-backlog full sweep is in progress
(see "Fixed in the 2026-07-13 deferred-backlog full sweep" below); closed tranche
entries have been pruned as they land.

This file is the queue for tech debt that is **open, actionable, and still present in the tree**.
Keep current priorities summarized first; deep-audit backlogs may be longer when they are intended
as intake for later agents. Closed debt does not live here — it is archived (see below).

Use `dev-docs/project/active-work.md` for current priorities.

## Open items

These are the tech-debt items that are still open and present in the tree, plus the
verified won't-do decisions worth recording so they are not re-filed. Fixed items do
not live here — the 2026-07-12 deferred-backlog sweep (which cleared the pass 5–24
backlog) is archived at
`guidelines/tech-debt/archive/2026-07-12-deferred-backlog-sweep.md`, and per-item
detail lives in the `Deferred backlog sweep — Batch A…I` commits.

### Deferred from the 2026-07-17 audit sweep (TD-2026-07-17-*)

A 95-finding external audit was worked in one pass: **34 fixed** (each with a
regression test), 1 partial, 10 won't-do, and the **50 deferred items below**.
Fixed items are not listed here (they are in the tree + the pass's commit). Each
deferred item is a multi-file refactor, platform-specific (no build/verify on this
Linux host), or a test-infra/coverage sweep — the kind the audit itself flagged as
"do as a focused reviewed pass, not bundled". Numbers are `TD-2026-07-17-NNN`.

**Async / off-thread refactors** (move blocking work off the shell/UI thread):

- **047 — compare/merge model construction runs synchronously on the shell path.**
  Fingerprint-cached + budgeted, but a content change rebuilds on-thread; move large
  `BuildCompareModel`/`BuildMergeModel` to a generation-gated background job. Same
  family as **TD-2026-07-16-19** below.
- **048 — format-on-save can block the UI up to 5s.** The synchronous formatter
  subprocess is a deliberate, lint-allowlisted product choice; an async save
  transaction (write → format in background → apply if same generation → cancellable)
  is a UX redesign.
- **055 — project search spawns/joins per-run helper threads.** Worker count is
  capped and catastrophic-regex is bounded (034, fixed); reuse a bounded pool with
  finer-grained cancellation.
- **061 — file-manager reveal can block the UI up to 10s.** Common path (xdg-open
  forks) is fast; only a wedged launcher hits the bounded timeout. Needs an async
  host-integration service with SDL-event completion (the reveal returns a
  success bool that drives an error message).
- **081 / 082 — forced project refresh + change-batch stat run on the shell thread.**
  Route forced refresh and per-file metadata stat through the background executor
  with a generation/token SDL-wake result; land together.
- **080 — live exclude-glob changes don't rebuild the native watch set.**
  `SetExcludeGlobs` updates the filter but not `SetRoots`; needs a
  `ReconfigureFilter` that rebuilds the prepared native backend with a config
  generation. Native-backend work; pairs with 036/081.
- **086 — file-index watcher buffers unbounded incremental batches before the
  initial scan lands.** Replace the pending-vector with a capped path-keyed
  coalescer / rescan-after-baseline flag.
- **091 — LSP shutdown can synchronously block project reset / app teardown.**
  Preserve retiring clients in a host-owned lifecycle service that outlives
  project-state replacement. Pairs with debug teardown (026).
- **016 / 017 — save-participant + `process.run_async` can occupy plugin worker
  capacity.** Cancellation/generation ownership + a dedicated process worker pool
  returning handles to Lua.
- **014 — POSIX terminal write can block without a deadline.** Route PTY writes
  through a nonblocking bounded writer queue with backpressure surfaced to the panel.
- **075 — DAP debug trace serializes + flushes whole messages under a global mutex.**
  Trace-only (off by default); move to a bounded async writer with payload caps.
- **003 — commit-workflow `&state` capture across executor + mailbox.** Non-actionable
  today (owners outlive queued work) but fragile; value-owned op context / shared_ptr
  generation. Same item as **TD-2026-07-16-*** commit-lifetime note; see 093 (fixed)
  for the analogous git-executor ownership fix.

**Render view-model / string-allocation cleanup:**

- **083 — commit-body render mutates viewport state + `Snapshot()`s during paint.**
  Move sizing/scroll-clamp to input/layout prep; render visible rows via `LineView`.
- **084 — overlay view model holds live `OverlayState*`/`ProjectWorkspaceState*`
  pointers and rebuilds labels in the render TU.** Expand into an owned/precomputed
  row+string model; move label composition into `RenderViewModelBuilder`; add a
  view-model-pointer lint. Same family as **TD-2026-07-16-26**.
- **029 / 030 — expand render-string lint coverage + add per-frame-prep regression
  counters** (layout recompute / view-model build / frame-prep counts).

**Editor / Unicode (schedule as one coherent pass):**

- **021 — centralize grapheme segmentation + visual width into one editor service**
  and route cursor movement / hit-testing / layout / selection through it.
- **023 — inlay hints should consume the same display-column map as text layout**
  (part of 021; 070 already bounded inlay width).
- **[RESOLVED 2026-07-17] 068 — grouped-undo FALLBACK still `Snapshot()`s the whole
  document** for non-contiguous multi-caret/snippet edits. Replaced the whole-buffer
  fallback with a sparse disjoint-range model: `UndoGroupFrame` now holds a sorted,
  pairwise-non-adjacent `std::vector<Entry>` (current after-coords); each child folds
  into a containing/adjacent range or inserts a new one via `MergeChildIntoDisjoint`
  (integer delta-reindex of lower ranges, neighbour coalesce — no line copies), and
  `FinishActiveGroup` stitches them into one undo Entry reading only untouched gap
  lines via `LineView` (bounded by the touched span, never the whole doc). The two
  `Snapshot()` calls are gone; contiguous fast path stays a single-element vector so
  `editor_shaping_multi_caret` is unchanged. Test seam `TextBuffer::snapshot_build_count()`
  + regressions `EditorMultiCaret/DisjointGrouped{Insert,Join}NoSnapshotRoundTrips`
  (±delta, round-trip + zero-snapshot). `run-checks.sh tests` + ASAN green.

**Debug / DAP:**

- **025 — debug request/response paths need session-generation + request-id gating**
  (REPL / hover / watch, matched by stable id).
- **026 — DAP stop/terminate needs a bounded escalation** (disconnect → wait → kill →
  bounded drain) with timeout surfaced to the pane. Pairs with 091.

**Scanner / search incomplete-state plumbing (land together):**

- **008 / 033 — surface a scanner result status** (complete / truncated_by_budget /
  permission_limited / error) and thread searched/skipped/incomplete-catalog through
  the file-finder / search / tree view models so partial results are never silently
  authoritative.
- **009 — fallback watcher snapshots are expensive on huge trees;** degrade to
  manual-refresh-with-banner + incremental directory hashing.

**LSP feature completeness:**

- **011 — `WorkspaceEdit` resource ops (create/delete/rename) + version-aware edits**
  with atomic/rollback-safe staging. Same item as **TD-2026-07-16-18**.
- **012 — replace empty-success-shaped LSP timeouts with explicit result variants**
  (success / empty_success / timeout / cancelled / protocol_error).

**Plugin caps / policy:**

- **018 — per-field byte caps for every provider result surface** (078 landed the
  string-array caps; the remaining diagnostics/hover/status/sidebar/tools/tasks/
  scm/auth surfaces are a bounded parser sweep).
- **019 — re-derive plugin status/contribution caps from measured render/registry
  budgets** (a measurement task).
- **077 — plugin contribution registration is O(N²)** via linear duplicate scans;
  per-kind id sets threaded through every `Register*` signature. Memory already
  capped; only a pathological plugin hits it.
- **[RESOLVED 2026-07-17] 090 — plugin decoration aggregate cap applies after full
  concat + sort.** `PluginDecorationStore::RebuildPath`'s multi-owner branch replaced
  the concatenate-all / sort-all / resize-down path with a bounded k-way merge
  (`CappedSortedMerge`): every owner's per-kind vector is already sorted at publish, so
  a min-heap merge emits the same total render order but reserves at most `kMaxMergedPerKind`
  and stops there — both allocation and O(N log N) work are now bounded by the retained
  cap, not the (arbitrarily larger) total contributed. The four per-kind orderings were
  factored into named comparators (`TextStyleLess`/`GutterMarkLess`/`InlineTextLess`/
  `CodeLensLess`) shared by the publish-time sort and the merge. Coverage: extended
  `AggregatePerFileCapTruncatesMergedKinds` with a peak-allocation assertion
  (`capacity() <= cap`) and added `AggregateCapBoundsEveryKind` (all four kinds
  independently capped + peak-bounded + render order). `run-checks.sh tests` (3/3) green.

**Platform-specific (no build/verify on this Linux host — dedicated Windows/macOS
passes):**

- **004 — Windows async subprocess HANDLE lifetime race** (ref-counted handle
  ownership + loop stress test).
- **005 — Windows ConPTY terminal backend unsynchronized stop/read** (lifecycle state
  machine + Windows-only stress suite).
- **010 — Windows ignore rules corrupt literal backslash semantics** (separate
  gitignore escape parsing from separator normalization).
- **035 — Windows terminal launch quoting + `lpApplicationName`** (single quoting
  helper + test matrix; has a security dimension).
- **006 — macOS FSEvents watcher ignore-filter + run-loop hazards** (ignore filtering
  in canonical event normalization + run-loop shutdown handshake). Pairs with 036.
- **062 — macOS trash exists-then-rename TOCTOU** (atomic O_EXCL reservation like the
  Linux path already uses, or the native macOS trash API). 063 (dangling symlink)
  was fixed cross-platform.

**Tab identity:**

- **024 — dirty-prompt state keyed by tab indices is fragile.** Add a stable per-tab
  id to `TabEntry`, thread it through the prompt flow + persistence, revalidate on
  completion.

**Architecture lint / test infrastructure:**

- **020 / 058 — mechanical no-longjmp-across-C++-locals audit.** Needs a clang-tidy
  AST matcher (a regex would false-positive on the ~30 legitimate entry-position
  `luaL_check*` calls); the concrete instance (001) was fixed + tested.
- **032 — architecture lint for direct persistence file I/O outside
  `PersistenceService`** (hard to make precise by regex; the legacy-symbol ban
  already guards the biggest regression; 050 retired the stale legacy-importer spec).
- **037 — convert more policy invariants into narrow lint checks with failing
  negative fixtures** (no `lua_State*` outside LuaRuntime, no render-TU project-state
  reads, etc.).
- **036 — one backend-independent watcher contract test suite** run against every
  backend (pairs with 006/010/080).
- **022 — large-buffer insert/delete/undo perf scenario + a lint for direct
  `document_->lines` copies** in mutation paths (the affected-range architecture is
  already invariant-enforced).
- **030 — per-frame-prep regression counters** (see render section).
- **015 — terminal lifecycle platform stress suite** (tab-close-during-output/exit,
  alt-screen shutdown, multi-terminal shutdown, open/close loops).
- **052 — seed the seed-light fuzz corpora** (PersistedRecordReader / GitBlameParser /
  PluginDisplayListParse / SurfaceRasterDecode / DebugStateRecord); the
  `run-checks.sh fuzz` runner landed (049).
- **088 — replace ~132 duplicated fixed-sleep polling loops** with shared WaitUntil /
  DrainUntilIdle helpers + a no-raw-sleep lint.

**Security:**

- **040 — control-socket `/tmp/microide` fallback trusts parent ownership too late.**
  Add a secure-directory helper (reject symlinks, require current-user ownership, mode
  ≤ 0700, else disable the channel) with hostile-fallback-dir / symlinked-/tmp /
  forged-descriptor fixtures. The socket is already chmod 0600.

### Deferred from the 2026-07-16 audit sweep (TD-2026-07-16-*)

The prior day's 70-finding audit closed 60 fixes; these 10 remained deferred/won't-do
(all multi-file refactors flagged for their own reviewed pass). Several overlap the
2026-07-17 set above and should be merged when tackled.

- **TD-2026-07-16-18 — server-pushed LSP `WorkspaceEdit` can synchronously load and
  save thousands of files on the main thread.** Dedicated async pass. Same item as
  **TD-2026-07-17-011**.
- **TD-2026-07-16-19 — compare/merge/review open paths run git blob reads
  synchronously on the workspace thread.** Dedicated async pass. Overlaps
  **TD-2026-07-17-047**.
- **TD-2026-07-16-21 — replace-all in project performs bulk file I/O synchronously on
  the shell thread.** The pre-loop path-vector copy was removed
  (**TD-2026-07-17-094**, fixed); the full move to a cancellable background
  preflight/commit workflow remains deferred.
- **TD-2026-07-16-22 — plugin extension surfaces still expose raw `lua_State*` despite
  the LuaRuntime boundary spec.** Won't-do pending a dedicated plugin-API refactor;
  same Lua-safety family as **TD-2026-07-17-020/058**.
- **TD-2026-07-16-26 — render view models smuggle mutable project-state pointers back
  into render TUs.** Dedicated architecture pass. Same item as **TD-2026-07-17-084**.
- **[RESOLVED 2026-07-17] TD-2026-07-16-31 (== TD-2026-07-17-068's sibling) — merge
  result edit side effects snapshot the full buffer on each mutation.** The edit
  engine now stamps a whole-line-trimmed applied-edit line span (`AppliedEditLineSpan`,
  `TextViewport::last_applied_edit_line_span()`), computed once from the undo entry's
  before/after slices in lockstep with the existing char-level `AppliedEdit` (via the
  DRY `SetLastAppliedEditFromEntry`/`ClearLastAppliedEdit` helpers). The span matches
  `WorkspaceShell::ComputeChangedLineSpan` exactly — the entry's slice already isolates
  the changed region, so trimming within it and offsetting by `start_line` is identical
  to a whole-document diff, at O(edit) cost — which is why the boundary semantics (Enter
  at end-of-line = pure insertion) are preserved and the conflict shift/invalidate logic
  is byte-for-byte unchanged. `UpdateMergeTrackingAfterViewportEdit` now reads that span
  instead of snapshotting + diffing the whole result buffer before/after every keystroke,
  and updates max-visual-columns from a bounded `LineView` slice. Dropped the whole-buffer
  `before_lines` capture at all 8 human call sites + the plugin path (multi-region edits
  leave the span empty → safe scroll-only resync, matching the old no-change early-out).
  Coverage: `TextViewport/LastAppliedEditLineSpanMatchesWholeLineDiff` (mid-line insert,
  end-of-line newline pure-insertion, line-join backspace, undo/redo reverse/forward,
  multi-caret empty). `run-checks.sh tests` (3/3, perf incl.) green; ASAN batched.
- **TD-2026-07-16-38 — stage/unstage/discard serialize unified patches synchronously
  on the UI path.** Dedicated async pass. Overlaps **TD-2026-07-17-082**.
- **TD-2026-07-16-39 — encoded raster surfaces publish layout dimensions before decode
  knows the real image size.** Dedicated cross-layer pass; related to the raster
  budget work (**TD-2026-07-17-043/092**, fixed).
- **TD-2026-07-16-60 — bottom-panel plugin surface previews cannot scroll their own
  content.** Dedicated UI-feature pass.
- **TD-2026-07-16-61 — plugin surface hit regions are parsed and documented but never
  dispatched.** Dedicated UI-feature pass.

### Fixed in the 2026-07-16 cross-subsystem bug-hunt pass

A broad correctness sweep across every major subsystem — util, terminal, compare,
merge, project (patch/ignore/blame/search), platform (subprocess), editor
(PieceTree, inlay), workspace/lsp, persistence, render (color/theme/ansi/display-
list), and plugin (Lua runtime). The audited surface was already exceptionally
hardened by the prior pass 5–24 / pentest history; every edge case probed carried
a defensive fix with a rationale comment. One genuine correctness defect surfaced
and was fixed:

- **`Ctrl+Alt+Backspace` no longer drops the Meta (ESC) prefix.** The `Char`
  encoding path already prefixed `Ctrl+Alt+<key>` with `ESC` (Meta) so `M-C-`
  chords reach TUI apps (regression-tested), but the `Backspace` path in
  `src/terminal/TerminalSessionInputEncoding.cpp` emitted a bare `0x08` for
  `Ctrl+Alt+Backspace` — indistinguishable from plain `Ctrl+Backspace`. Now emits
  `ESC 0x08` when Alt is held, mirroring the `Char`/`Escape` cases. Added a
  regression (`TerminalSession` key-encoding test) covering both
  `Ctrl+Backspace` → `0x08` and `Ctrl+Alt+Backspace` → `ESC 0x08`. Full suite
  green (`tools/run-checks.sh tests`, 3/3). No deferred items resulted — the
  defect was fixed, not deferred; this entry is the audit-trail record.

### Fixed in the 2026-07-15 dead-coverage / dead-code pass (session 3)

Compiler-warning-driven sweep (`-Wunused-function` across the full test build)
surfaced three genuinely dead entities — two of them regression tests that were
written but never wired into their `Register*Tests` table, so the behaviors they
guard had **no** active coverage and a refactor could have silently reintroduced
the original bugs.

- **`TestOverRangeFloatDoesNotAbortParse` now runs.** The test
  (`tests/JsonValueTests.cpp`) verifies that a syntactically valid but
  magnitude-overflowing float literal (`1e400`) in an LSP/DAP message decodes to a
  non-finite double instead of aborting the enclosing object parse (which would
  silently drop the whole message). It was defined but absent from
  `RegisterJsonValueTests`; now registered as
  `JsonValue/OverRangeFloatDoesNotAbortParse` (passes).
- **`TestWorkspaceShellPromptCancelButtonDoesNotExecuteAction` now runs.** This
  guards a destructive-action bug: the modal's Cancel button (selected_button==1)
  must decline a special-cased prompt action (Go to Line stands in for commit
  amend / rename symbol), not execute it. `ConfirmPromptSurface` once ignored the
  selected button. The test (`tests/WorkspaceShellPromptTests.cpp`) was unwired;
  now registered as `WorkspaceShell/PromptCancelButtonDoesNotExecuteAction`
  (passes — the product behavior is already correct, so this locks it in).
- **Dead duplicate `ThreadIdArgs` removed.** `DebugSession.cpp` carried an
  anonymous-namespace `ThreadIdArgs(int)` left behind when execution-control was
  split into `DebugSessionExecution.cpp` (which owns the live copy). Removed the
  dead definition.

Full test suite green after the changes (`tools/run-checks.sh tests`, 3/3).

Auditor's note: the broader tree is exceptionally clean under this pass — terminal
(base64/OSC/CSI/SGR/screen/mouse), editor (multi-caret/undo/snippet/bracket/
highlight), compare (patience-LIS + anchored fallback), git parsers (porcelain-v2/
blame), and util (parse/pathmatch/durable-file/json) were read closely and no
correctness or perf defects were found; each carries dense rationale comments and
prior-fix regression pins. No new deferred items.

### Fixed in the 2026-07-15 cross-subsystem speed pass (session 2)

Four contained speed wins across LSP registration, the editor bracket scanner,
the file finder, and the git sidebar. Each shipped regression coverage.

- **LSP `RegisterServer` no longer serializes JSON to compare configs.**
  `JsonValue` gained a defaulted structural `operator==`
  (`src/util/JsonValue.h`); `LspManager::RegisterServer`
  (`WorkspaceLspManager.cpp`) compares `initialization_options`/`settings`
  structurally instead of materializing four `SerializeJson` strings on every
  registration / project-activation / plugin-refresh. Also fixes a latent bug:
  object comparison is now key-order-independent (the string compare was
  hash-iteration-order-sensitive). Regression:
  `JsonValue/StructuralEqualityIsOrderIndependentAndDeep`.
- **Bracket matching is O(window), not O(file).** `FindBracketMatch`
  (`src/editor/BracketScanner.cpp`) previously wrote an empty `std::string_view`
  for every line in the whole buffer (`views.assign(line_count, {})`) each time
  the caret sat next to a bracket — a multi-MB memset per frame on large files.
  It now materializes only the `[caret-max, caret+max]` slice and indexes it
  through a `WindowLines` accessor (absolute line numbers, `base` offset). The
  public `FindBracketMatchInLines` keeps identical behavior (base 0). Regression:
  `EditorEssentials/BracketScanner/WindowedDeepCaret` (plus the existing bracket
  suite pins the base-0 path).
- **File finder no longer deep-copies the whole index per keystroke.**
  `FileFinder::EnsureCacheBuilt` (`src/project/FileFinder.cpp`) called
  `SnapshotWithVersion()` — an O(index) copy of every `ProjectFile` under the
  index lock — on every Refresh just to read the version, then discarded it when
  the cache was warm. It now checks the cheap scalar `index_->version()` first
  and only fetches the snapshot when the index actually changed. Regression:
  `FileFinder/WarmRefreshDoesNotRebuildPerKeystroke`. (The one-time rebuild on a
  version change is still on the UI thread — see the deferred follow-up below.)
- **Git sidebar outgoing-base resolution is memoized.**
  `GitRepositoryService::BuildSidebarSnapshot` ran `ResolveGitOutgoingBase`
  (several git subprocesses on the Auto path: `symbolic-ref`, `config`,
  `show-ref`, `rev-parse`) inside every full refresh. A new
  `ResolveOutgoingBaseCached` memoizes the result keyed by
  `(root, choice, head_oid, branch_name, upstream, repo_available)`, so a status
  refresh after a file edit (HEAD/branch unchanged) spawns no git process; any
  HEAD movement or branch/upstream/choice change re-resolves. Regression:
  `GitRepositoryService/OutgoingBaseResolutionIsMemoizedByRepositoryIdentity`.

### Fixed in the 2026-07-15 syntax-reload speed pass

Three low-risk latency wins from the "RuntimeSyntaxRegistry regex/match perf
budgets" cluster (the behavior-risky region/match-budget sub-items stay deferred
below). Each shipped regression coverage in
`tests/SyntaxDefinitionLoaderTests.cpp`.

- **Syntax-source fingerprint no longer re-reads every `.lua` file on each reload
  check.** `DefinitionSourceFingerprint` (a free function) is replaced by
  `SyntaxSourceFingerprint` (`src/editor/SyntaxDefinitionLoader.{h,cpp}`), a
  cache object keyed `path → {mtime, size, content_hash}`. `Compute` stats each
  discovered file and reuses the cached content hash when mtime+size are
  unchanged, only re-reading (and rehashing) files that actually changed. The
  fingerprint stays a pure function of paths+contents, so change-detection is
  byte-for-byte equivalent to the old full-read version — a poll that finds
  nothing changed no longer reads any source bytes. Owned/cleared by
  `WorkspacePluginRuntime` (`syntax_fingerprint_cache_`).
- **`DiscoverDefinitionFiles` dedups files across directories.** A syntax
  directory contributed via two routes (or two entries normalizing to the same
  path) previously loaded/hashed/compiled every file twice with order-dependent
  precedence. It now dedups both the directory list and the normalized file keys
  (first directory wins → deterministic precedence), which also shrinks the
  fingerprint's stat/hash work.
- **Cold filetype rule-regex compile is prewarmed off the UI thread.** The first
  visible-line highlight of a cold filetype compiled the definition's rule
  regexes in `EnsureDefinitionCompiled` on the render path (the visible-band
  prefetch runs only *after* a frame settles). New public
  `runtime_syntax::CompileDefinition(id)` +
  `HighlightPrefetchService::PrewarmForViewport(token, resolve)` dispatch that
  compile to the existing highlight worker on tab switch (gated on the viewport
  identity inside the service, so detection runs once per switch, not per frame).
  Best-effort: `PostLatest` keeps one prewarm pending, and any superseded
  filetype is still compiled by its own normal prefetch. Behavior is unchanged
  (idempotent `std::call_once`); only compile timing moves off the render path,
  so the win is timing-only and not unit-asserted (the test covers safety /
  idempotency).

### Fixed in the 2026-07-14 actionable sweep

Closed this pass; the corresponding open/backlog entries below were removed.

- **Atomic save through a symlink whose target does not yet exist replaced the link
  with a regular file.** `ResolveSymlinkTarget` (util/TextFileIO.cpp) used
  `weakly_canonical`, which stops at the last existing prefix (the link's parent) and
  returns the link node itself for a dangling relative link (`link -> sub/missing.txt`);
  the atomic rename then destroyed the link. Now follows the link chain manually via
  `read_symlink`, resolving relative targets against the link's parent, so the save
  creates/overwrites the intended target and preserves the link. Bounded to 40 hops for
  cycle safety. Regression: `SaveDataIntegrity/AtomicWriteThroughSymlinkWithMissingTargetPreservesLink`.
- **Compare & merge mouse row hit-test selected row 0 for clicks in the band directly
  above the first row.** `(y - rows_y) / line_height` truncates a small negative toward
  zero to `0`, and the `clicked_row < 0` guard therefore missed it, so a click just above
  row 0 was treated as a hit on row 0. Both `WorkspaceCompareMouseCoordinator` and
  `WorkspaceMergeMouseCoordinator` now reject a negative row offset before the division.
  Regression: `WorkspaceShell/CompareClickAboveFirstRowIsNotHandled` (merge path is the
  symmetric mirror).
- **Settings overlay `StepSetting` used an ad-hoc truthiness test for plugin-contributed
  Bool settings** (`== "true"/"1"/"on"`), so a non-canonical truthy default like `"yes"`
  — which renders as checked via `SettingFlagEnabled` — computed `on == false` and no-oped
  the first toggle. Now routes through `SettingFlagEnabled`, matching the render predicate.

#### Curated open-items pass (2026-07-14, session 3)

A focused pass over two curated items; the rest of the batch (async compare pickers,
FindFirstRegex skip incremental) was assessed too large / behavior-risky to verify
headless and stays deferred with concrete sketches below.

- **Merge delete-conflict staging is now transactional.**
  `CompareInteractionCoordinator::MarkMergeResolved` previously `remove()`d the working
  file and marked the buffer clean *before* validation and `stage_merge_result_path`
  could fail, so a stage failure (stale index, git error, permissions) left the user's
  in-progress merge file gone and unrecoverable. The delete branch now snapshots the
  on-disk bytes (`util::ReadTextFile`) and the tab's dirty/tick/stale state before the
  removal and rolls them back (`util::WriteTextFileAtomically`) on any validation or
  staging failure; only full success leaves the file deleted + staged + resolved.
  Regressions: `MergeConflict/DeleteConflictStage{FailureRestoresFile,SuccessRemovesFile}`.
- **Project search reads are capped at 32 MiB per file.** `ReadFileForTextSearch` took
  the shared 512 MiB whole-file cap, so N search workers each buffering a whole file
  could reach N × 512 MiB transient, and line-scanning a 512 MiB generated/minified blob
  is pure latency. Added `kMaxSearchFileBytes` (32 MiB) plus a defaulted `max_bytes`
  parameter; the search worker and the "Replace All in Files" path both use it (aligning
  replace scope with what the finder can match), while whole-file callers keep the 512
  MiB cap. Regression: `TextFileIO/ReadFileForTextSearchRespectsMaxBytes`. The paired
  **search-cancellation** sub-item ("finishes a full current-file scan before stopping")
  is verified already addressed — the worker polls `cancel_requested_` per line
  (`ProjectSearchService.cpp`) and the regex path polls internally
  (`kRegexCancelPollInterval`), so cancellation is already threaded through line/regex
  iteration; no change needed.

#### Curated open-items pass (2026-07-14, session 2)

A follow-on pass working the curated **"Still open (deferred)"** + **"multi-pass
residue"** clusters. Each item shipped regression coverage. The matching deferred/
residue entries were removed below.

- **Editor wheel scrolls the pane under the pointer, not just the focused viewport**
  (`WorkspaceEditorMouseCoordinator::HandleWheel` resolves the split under the cursor via
  a new `viewport_for_pane` op → `WorkspaceShell::ViewportForPane`; falls back to the active
  viewport only in divider gaps; focus unchanged). Regression:
  `WorkspaceShell/WheelScrollsPaneUnderPointer`.
- **Toggle Block Comment is a real toggle** — `ShapingActions::ToggleBlockComment` strips an
  existing surrounding block comment (whitespace-aware) instead of nesting `/* /* x */ */`.
  Plus: both `ToggleLineComment`/`ToggleBlockComment` were hardcoded to C-style markers
  regardless of language; they now read the buffer's resolved `LanguageContractView`
  (`line_comment`/`block_comment_*`, e.g. `--[[ ]]` for Lua) with a C-style fallback.
  Regressions: `EditorEssentials/Shaping/ToggleBlockComment{RoundTrips,SelectionStripsWithWhitespace}`.
- **Status-bar LSP tone comes from typed readiness state, not a `find("Ready")` substring.**
  `LspService::LspStatusSeverity` (Idle/Busy/Error) threads through `ActiveLspStatusStrings`
  and the status-bar operation as a `StatusBarSegmentTone`; a `Failed` server is now Error,
  and `Not Ready`/server names no longer mis-tone. Regression:
  `WorkspaceStatusBar/LspToneFromTypedSeverityNotLabelText`.
- **Git commit feeds the message on stdin via `commit -F -`** (new
  `GitRepository::ExecuteWithStdin`), removing argv exposure and the argv-length limit for
  huge/shell-significant bodies; `-F` keeps the same `whitespace` cleanup as `-m`. Regression:
  `CommitWorkflow/ExecuteCommitPreservesShellSignificantAndLargeBody`.
- **Persistence recover-and-protect for a corrupt primary.** `PersistenceService` guards a
  state file recovered from `.bak` because the primary was present-but-corrupt: it records
  the recovered state's re-encoded baseline and suppresses a Save whose body matches (no user
  mutation), so the still-recoverable corrupt primary is not clobbered with stale backup
  state; a real mutation writes through and clears the guard. Regression:
  `PersistedStateRecord/PersistenceServiceGuardsCorruptPrimaryFromBackupOverwrite`.
- **Header-first bounded persisted read.** `PersistedRecordReader` validates the fixed
  16-byte header (magic + version) before allocating the body, so a corrupt/hostile large
  file is rejected after ~16 bytes instead of a full 256 MiB read. Regressions:
  `PersistedRecordIo/ReaderRejectsLarge{BadMagic,UnsupportedVersion}OnHeader`.
- **Tool downloader verifies cached-tool hash + all networking removed.**
  `GetCachedTool` takes an optional `expected_sha256` and verifies before returning;
  `ResolveToolSourcePath` hard-rejects any remote scheme (`http`/`https`/`ftp`/… — anything
  with `://` that is not `file://`). No networking by design. Regressions:
  `WorkspaceToolDownloader/{GetCachedToolVerifiesHash,RejectsRemoteSchemesNoNetworking}`.
- **`^`-anchored syntax rules honor a true line start** — `FindAllRegex`/`FindFirstRegex`
  thread `at_line_start` and pass `PCRE2_NOTBOL` for mid-line segments (the tail after a
  region), so `^foo` no longer matches a `foo` following a region on the same line. Narrow
  blast radius (NOTBOL only affects the circumflex assertion). Regression:
  `TextViewport/RuntimeSyntaxCaretAnchorHonorsTrueLineStart`. (The two perf sub-items —
  per-pattern compile byte cap and the `FindFirstRegex` skip-mask O(n²)→O(n) — stay deferred;
  see below.)
- **Diagnostics underline reuses a per-line visual-column map.** `AppendDiagnosticUnderlines`
  builds a `TextLayout::LineVisualColumnMap` once per line instead of two O(column) tab-stop
  walks per diagnostic (O(diagnostics·column) → O(column + diagnostics·log column)); kept
  byte-identical to the uncached walk by snapping mid-codepoint queries to the code-point
  boundary. Regression: `RowDecorationBuilder diagnostic underline cache matches uncached path`.
- **Compare per-hunk cumulative intra-line budget.** The oversized-hunk positional fallback
  emits an unbounded number of Modified pairs, each up to a full intra-line LCS;
  `PopulateChangedSpans` now consumes a per-hunk cell budget (`kMaxHunkIntralineTotalCells`,
  8M) — early rows keep character-level refinement, later rows fall back to whole-line-changed.
  Regression: `Compare/IntralineBudgetBoundsLargeModifiedHunk`.
- **Latent compare index guards hardened.** `CompareTabSelectedModelRowRef` returns a stable
  empty row on an empty model (was `rows[size()-1]` underflow); `ComparePresentationModel::
  CompareInline{Left,Right}Spans` return an empty span vector for an out-of-range index.
- **Multi-caret overlapping selections are refused in BOTH apply paths.**
  `TextViewport::MultiCaretSelectionsOverlap()` flags any two carets whose affected ranges
  intersect; the general `ApplyMultiCaretEdit` AND the single-char `TryMultiCaretPairInsert`
  fast path bail, so an injected overlap (test-access/plugins/future multi-cursor) cannot
  double-edit shared content. Regressions:
  `EditorMultiCaret/{RefusesOverlappingSelections,DisjointSelectionsStillApply}`.
- **OSC 52 oversized-payload status.** The allow/deny setting already gated normal writes; an
  OSC 52 sequence dropped for overrunning the 8 KiB escape buffer is now flagged
  (`TerminalSession::ConsumeOversizedOsc52Dropped`) and surfaced as a toast instead of failing
  silently. Regression: `TerminalSession/Osc52OversizedPayloadIsFlagged`.
- **No-overwrite `RenamePath`.** `platform::MovePathNoOverwrite` uses
  `renameat2(RENAME_NOREPLACE)` (atomic, race-free on the same filesystem) with a
  cross-device/exists-check fallback; `FileOperationService::RenamePath` routes through it,
  closing the `exists()`-then-move TOCTOU. Regressions:
  `Project/{RenamePathRefusesToOverwriteExistingDestination,MovePathNoOverwriteRefusesExistingDestination}`.
- **Debug value node ids: 64-bit widening TRIED and REVERTED.** Widening the id (and the
  per-row `node_id`) to 64-bit measurably regressed the `debug_value_tree_rebuild`/`_expand_large`
  hot path in the perf comparison vs `origin/main` (~+7% p50 / +17% max rebuild, identical
  allocations — the wider `Node`/`DebugVariableRowView` add memory traffic in the flatten/rebuild
  loop the step/render path runs). The 32-bit `next_id_` wrap it guards needs ~4 billion node
  allocations in a single session (practically unreachable), so the regression is not worth it.
  Reverted; the 32-bit-wrap risk stays a documented won't-do (see below).

### Fixed in the 2026-07-13 actionable sweep

Closed this pass (each with regression coverage unless noted); the corresponding open
entries below were removed. Detail lives in the sweep commit(s).

- **`AppendUtf8` (util + terminal input) folded invalid scalars to U+FFFD** instead of
  emitting malformed UTF-8 for surrogates / >U+10FFFF.
- **LSP `ParseDiagnostic` severity clamped to the 1..4 domain** (out-of-spec 0/99/INT_MAX
  → Error) so severity filters cannot misclassify.
- **Command completion routed `split-right`/`split-down`** instead of the stale `vsplit`
  branch that never matched a real command.
- **`ParseThemeColor` rejects numeric color tokens outside 0..255** rather than silently
  collapsing them to black inside `Ansi256Color`.
- **`MICROIDE_SEARCH_WORKER_LIMIT` clamped to a product max (64)** so a bad env value can't
  request full hardware concurrency.
- **`NotificationService::Show` saturates the expiry** near UINT64_MAX instead of wrapping
  to an immediate expiry (now via `util::SaturatingAdd`).
- **Plugin status-item `progress` rejects non-finite values** (NaN survived `std::clamp`) at
  both the registration parser and the update interop.
- **Legacy (non-SGR) terminal mouse encoding drops clicks past cell 222** instead of
  clamping to a phantom edge click.
- **Terminal selection copy omits the newline across soft-wrapped rows** (honors
  `wrapped_from_previous`); real line breaks still emit `\n`.
- **Terminal URL detection matches schemes case-insensitively** (`HTTPS://…`), preserving
  the original path casing.
- **OSC 7 working-directory reports from a non-local host are rejected** (accept empty,
  `localhost`, and the local short hostname).
- **Compare profiling residual row-assembly time uses a saturating subtract** so clock
  jitter can't wrap it to an enormous value. Added shared `util/SaturatingMath.h`.
- **`DebugValueTree::PathKey` includes a sibling ordinal** so two same-named siblings
  (array pages / repeated map labels) expand/collapse independently across stops.
- **`LspMessageFramer` parses the `Content-Length` header case-insensitively** and
  tolerates missing/extra whitespace around the value.
- **`DetectIndent` classifies a leading whitespace run containing a tab as tab-indented**
  (was first-byte-only, misdetecting `"  \tcode"` as spaces).
- **Commit subject length counts Unicode scalar values, not bytes**, so a short non-ASCII
  subject is no longer falsely flagged as too long.
- **Compare hunk/file patch copy no longer clobbers the clipboard on generation failure**
  (writes only on a non-empty success). No isolated regression — trivial guard; the
  patch-generator success path is covered by `PatchApplyTests`.
- **Git blame visible-window arithmetic uses saturating add** so a near-SIZE_MAX
  `visible_line_count` can't wrap and collapse the window.
- **Plugin `editor.apply_edits` rejects malformed coordinates**: `ReadIndexField` guards
  non-finite doubles (no UB cast) and clamps huge values; a 0/invalid coordinate now drops
  the whole edit rather than clamping to a wild insertion at the buffer start.
- **Plugin `text_styles` decoration rejects an inverted `start_col > end_col` range** at
  interop parse time instead of producing a negative-width span downstream.
- **`GitPorcelainParser::ParseLog` takes a bounded `max_entries` cap** (default 100000) so
  no caller can re-open the unbounded path on a hostile/corrupt log stream.
- **`FsOps::MovePath` rolls back the destination copy when the source removal fails**, so a
  cross-device move that half-completes no longer leaves a silent duplicate. (Guard only;
  a deterministic test needs a filesystem-injection seam that does not yet exist.)
- **File-index git-metadata filtering is case-insensitive on folding hosts.** Added
  `platform::HostPathsAreCaseInsensitive()`; `.GIT`/`.Git` are now excluded on
  Windows/macOS (still indexed on case-sensitive Linux). Fixed in both `FileIndex` and
  `FileIndexWatcher`.
- **`SingleLineEditor` caret/anchor clamp to a UTF-8 boundary** (`ClampCaret` snaps off
  continuation bytes) so a mouse/plugin/test caret placed mid-codepoint can't split a
  scalar on the next edit.
- **`SingleLineEditor::Append` shares Insert's CR/LF sanitization**, so a single-line field
  cannot store line breaks via that public helper.
- **Snippet parser honors VSCode-style escapes** (`\}`, `\$`, `\\` in default text; `\,`,
  `\|`, `\}`, `\\` in choices) instead of truncating at the first raw delimiter.
- **LSP single-string hover content is capped** (bare `MarkedString` and `MarkupContent
  {value}`) with a UTF-8-boundary truncation marker, matching the array shape — a server
  can no longer push a tens-of-MiB hover payload onto the callback/render path.
- **LSP signature-help parameter label offsets are UTF-16→UTF-8 converted** before slicing
  (via `LspCharacterToByteColumn`), so a non-ASCII signature label no longer corrupts the
  active-parameter highlight.
- **Theme includes are bounded** by a max nesting depth (16) and total include count (128),
  so a deep chain or broad fan-out fails cleanly instead of recursing/opening unbounded
  files. (Cycle handling stays skip-and-continue, keeping self-including themes loadable.)
- **Settings ids are validated at the `SetUser`/`SetProject` mutation boundary**
  (`SettingsStore::IsValidSettingId`): empty, whitespace, control-byte, or newline ids are
  rejected before they can reach the persisted layer.
- **Project-session tree-path lists are capped at decode** (100000 expanded/collapsed),
  so a corrupt/hostile session file cannot allocate an unbounded string vector at restore.
  (Guard only — a cap+1 test at 100k entries is impractical; the push is a simple bounded
  append.)
- **LSP server re-registration drops stale language-id aliases.** Re-registering
  `["cpp","c"]` as `["cpp"]` no longer leaves `alias_["c"]` resolving to the C++ server
  (`LspManager::RegisterServer` clears aliases pointing at the key before reinstalling).
- **LSP `DidChange`/`DidChangeIncremental`/`DidSave` require an open document.** They no
  longer insert a phantom version entry (via `operator[]`) for a URI that was never opened
  or was already closed; an unopened change/save is rejected.
- **Terminal paste is capped inside `TerminalSession::PasteText`** (64 MiB, UTF-8
  boundary), so every entry point — including the direct middle-click paste that
  bypassed the workspace clamp — is bounded.
- **Plugin `process.run` stdin is capped at 16 MiB** independently of the Lua heap
  budget (guard only; a 16 MiB end-to-end test is impractical).
- **DAP line-breakpoint conversion clamps before the `int` cast** in
  `SendBreakpointsForFile`, so a forged/corrupt persisted breakpoint near INT_MAX/
  SIZE_MAX can't wrap to a negative DAP line. (Guard only.)
- **Commit staged-summary numstat counts parse in 64-bit and saturate** instead of
  dropping an out-of-`int`-range count to 0 (under-reporting a huge staged change).
  (Guard only — a >2-billion-line diff can't be constructed to test.)
- **All git commands run with `--literal-pathspecs`.** A user filename beginning with
  git pathspec magic (`:(glob)`, `:(top)`, `:(exclude)`, `:!…`) can no longer alter a
  stage/discard/blame/diff/history operation. Added once in `ReadGitCommandOutput*`.
- **Branch-review delete/unmark on a missing target is a clean no-op.** `MarkFileUnreviewed`,
  `MarkHunkUnreviewed`, and `DeleteNote` use a find-only helper (`FindMutableTarget`) and
  bump the revision only on a real removal — no empty-target creation or spurious revisions.
- **Branch-review pruning is by recency, not insertion order.** `PruneTarget` stable-sorts
  newest-first on `reviewed_at`/`updated_at` before dropping the tail, so a recently
  re-reviewed early-inserted entry survives.
- **`TestController::RegisterTestItem` upserts by id.** A rediscovery with the same id
  but a new label/file/line updates the item in place instead of being dropped (stale
  navigation/names after a test rename/move).
- **Plugin tool sha256 is normalized to lowercase and compared case-insensitively.**
  Registration lowercases the digest and `ToolDownloader::Download` lowercases the
  expected sha, so an uppercase manifest digest verifies against the lowercase computed one.
- **Plugin language-server `language_ids` rejects empty/whitespace entries.** An array
  like `{"", "cpp"}` is refused instead of seeding a `""` key into the activation table.
- **`TestController::TestResults` returns by value.** It no longer hands back a reused
  scratch reference that a second call invalidated, so two tests' results held at once
  don't alias.
- **Control-socket `Start` refuses to delete a non-socket file.** It `lstat`s the path
  and only unlinks a stale socket owned by the current user; a regular file / dir /
  symlink at the socket path makes Start fail instead of destroying the user's file.
- **Git blame parser caps total attributions** (2M) in addition to the existing
  per-entry line-count clamp, so a hostile porcelain stream of many tiny entries can't
  grow the attribution vector without limit. (Guard only.)
- **`ParseCommandLine` bounds input.** The scanned length (64 KiB) and token count (4096)
  are capped so a pasted-megabyte command can't allocate/drive completion without limit.
- **`CompletePath` caps candidate collection at 2000.** Completing `/usr/` or a generated
  directory no longer builds and sorts an unbounded list on the UI thread.
- **Persisted user/project config dedupes duplicate setting ids at decode** (last-writer-
  wins), so a corrupt/hand-edited config with duplicate keys is no longer a split-brain state.
- **Project-session enum bytes (`GroupSplitOrientation`, `RightPaneMode`) — verified
  already guarded.** The restore path (`WorkspacePersistenceCoordinatorSession`) converts each
  with a `value <= max ? static_cast<Enum>(value) : default` gate, so an out-of-domain byte
  falls back to a valid default rather than an impossible pane state. No code change needed.
- **Project-session layout floats (sidebar width, panel height) also cap huge finite
  values.** `sanitize_pixels` already rejected non-finite/negative; it now also clamps to
  100000px so a corrupt session can't squeeze the layout to nothing. (Coordinator-level
  guard; group-split/right-pane fractions were already `std::clamp`ed at restore.)
- **Workspace-session project-root list is capped (256) and deduped at decode.** A corrupt
  session can no longer trigger a large startup loop or duplicate project tabs from
  duplicate/equivalent roots.
- **Plugin surface/display-list rects are sanitized at parse time.** `ReadRectFields` folds
  non-finite x/y/w/h to 0 and clamps negative width/height to >= 0, so a NaN or inverted
  hit-region/op rect can't poison layout/scroll extents or hit-testing.

### Fixed in the 2026-07-13 deferred-backlog full sweep

A follow-on sweep working the deferred tranches below. Each closed item shipped
regression coverage unless noted; the matching tranche/residue entries were removed.
Detail per batch lives in the sweep commits.

- **Unicode simple case-fold helpers added to `util/StringUtil`** (`SimpleFoldCodepoint`,
  `Utf8CaseFold(Into)`, `Utf8QueryHasCaseVariation`, `Utf8IsIdentifierCodepoint`) covering
  ASCII/Latin-1/Latin-Ext-A/Greek/Cyrillic. Shared infra for the "ASCII-only"
  search/replace/finder/word-motion/icon items (consumers still to be wired per subsystem).
- **Startup rejects a second positional project path** instead of silently opening the last.
- **`--dap-log` uses the attached `--dap-log=<path>` form**; the bare flag defaults its sink
  and no longer swallows a following project path.
- **`FileOperationService::CreateDirectory` creates-first then classifies via status**,
  removing the exists()-then-create TOCTOU. (`CreateFile` already used `O_EXCL`; the sole
  production caller already enforces project containment.)
- **Compare `TokenizeLine` reserves a half-length heuristic** (tokens are grouped runs, not
  per-byte); **`LineVisualColumnMap` caps its per-line reserve hint** (one entry per code
  point, not per byte).
- **`SettingsOverlayService::FilteredFontFamilies` is memoized by query text** (was recomputed
  per interaction); the family list is capped at 100k so the picker's `size_t`→`int` row math
  cannot overflow.
- **`OrderedSidebarViews` pre-indexes policies by id** and resolves each view once instead of
  re-scanning the policy list inside the filter and sort comparator.
- **`BuildMergeDisplayModel` computes the full merge result only for the empty-display
  fallback**; a zero-row (both-sides deletion) hunk is no longer recorded with an inverted
  `end_row < start_row` range. (The "merge can't represent an empty file" entry is a
  non-defect: `JoinLines([""])` and `JoinLines([])` both serialize to zero bytes, and `[""]`
  is the canonical empty buffer — moved to won't-do.)
- **`DebugValueTree::EraseSubtree` is iterative** (was recursive → stack overflow on a deeply
  nested hostile adapter tree); `FlattenInto` caps recursion depth at 256.
- **`DebugWatchModel` caps the expression list (512) and each expression length (4096)** so a
  paste/control flood cannot make every stop arbitrarily expensive.
- **Syntax definition loading has an instruction deadline** (`lua_sethook` 20M budget →
  `while true do end` becomes a clean load error) and **clamps every declared array length**
  (256 defs/file, 4096 string entries, 4096 rules/array).
- **`GitRepository::Discard` no longer recursively deletes an untracked directory**: it refuses
  a directory target and drops `-d` so a single-row discard cleans only the file (data-loss fix).
- **Commit-failure hook classification is anchored** to known hook-phase names (dropped the
  broad `find("hook")` that misclassified any failure whose output mentioned "hook").
- **LSP/DAP spawn-failure errors redact argv** via a shared `workspace/CommandSummary.h`
  (executable basename + arg count) instead of leaking `--token=…` secrets/paths.
- **LSP response-id narrowing is strict** (in-range-or-skip) so an out-of-int-range id from a
  hostile server cannot wrap and collide with a live pending request id.
- **Tool downloader publishes atomically** (copy to `.partial` → verify sha → rename) and
  **parses `file://` URIs** (empty/localhost host only; percent-decoded path).
- **Status-bar language cache keys on `runtime_syntax::RegistryRevision()`** so the language
  label re-detects after a syntax reload.
- **Project search + file finder + single-line word motion + file icons are Unicode-aware.**
  Wired the length-preserving `Utf8CaseFold` into project-search smart-case/insensitive literal
  matching (byte columns stay aligned), file-finder query/cache lowering, and file-icon matcher
  keys; single-line word movement decodes code points (`Utf8IsIdentifierCodepoint`). (Editor case-insensitive replace now folds too; identifier hover ranges
  remain ASCII — still open below.)
- **`SurfaceTextureCache` retries a transient `SDL_CreateTexture` failure** (bounded to 3, then
  treats it as permanent) instead of leaving the marker set and suppressing a valid decoded image
  until `Clear()` — matching the sibling `renderer == nullptr` path. (`ComputeDisplayListHash`
  hashing "struct padding" is a verified non-defect: it hashes each field individually and the
  field types have no internal padding.)
- **Plugin task/tool/debug-adapter/launch-config contributions reject a duplicate local id** at
  the interop boundary (shared `DuplicateContributionId`) so first-match consumers are
  deterministic. (Remaining id-shaped kinds — language servers, AI providers, agents, snippets —
  still to extend.)
- **`TextViewport::ReplaceAll` case-insensitive match folds** (Unicode), completing the editor
  side of the case-insensitive item (identifier hover ranges remain ASCII, still open below).
- **`CollectGitBranches` uses the full refname as the branch `.ref`** (short form only as the
  label) so an ambiguous local/remote name can't resolve the wrong target — matching the other
  `GitBranchReference` builders.
- **`FindAllRegex` caps matches per rule per line (8192)** so a single-character-matching syntax
  rule on a long line can't push ~100k matches × many rules on the highlight hot path.
- **`SettingsStore::Reindex` bumps the revision only on an effective change** (scratch-resolve +
  compare) so a project switch to identical settings no longer re-runs downstream live-settings
  application and render preparation.
- **DAP `RefreshThreadList` and `Pause` guard on session state**: a late thread-list response can
  no longer repopulate the Call Stack selector after a `continued` resume, and `Pause` no longer
  sends `pause` to a target that already stopped/terminated. (The broader DAP resume-on-reject
  restore + session-token guards remain a recorded follow-up cluster.)

#### Investigated during the sweep — DAP spawn-failure "crash" is a test-lifetime hazard, not a product bug

- **A `DapManager` session for a nonexistent adapter binary can crash on teardown *if a
  test declares the callback-referenced state after the manager*.** Root-caused with a symbolized
  gdb repro: the crash is a use-after-free of the test's local `CapturedSession` (captured *by
  reference* in `MakeCallbacks`), which is destroyed at scope exit **before** the `DapManager`
  whose worker threads still invoke those callbacks during teardown. `~DapClient` correctly joins
  both `init_thread` and `io_thread` before deleting its `Impl`, so the client itself is sound.
  **Not product-reachable**: production callbacks reference shell/project-owned state that outlives
  the manager (ordered shutdown), never a shorter-lived local. Test-authoring rule: declare
  callback-referenced fixtures **before** the `DapManager`, or shut the manager down explicitly
  first. Verified: `DapClient::HandleEvent` dispatches `event_callback` via `main_mailbox.Post`
  (runs on the main thread when drained), never synchronously on the io thread — so the session's
  callbacks always fire on the main thread and a worker thread can never invoke them, confirming
  production soundness. Nothing further to do here.

### Still open (deferred, lower value / larger / latent)

- **[RESOLVED 2026-07-15] Status bar: repo availability cached by `project_root` only.** The
  `repo_cache_` (keyed on project_root) is removed: `is_git_repo_valid` is a single cheap `.git`
  stat, not a subprocess, so caching it saved nothing while going stale after an in-session
  `git init` / `.git` removal. `StatusBarModelService::Refresh` now calls it directly (only when
  there is no git snapshot). Regression: `WorkspaceStatusBar/RepoAvailabilityReflectsInSessionGitInit`.
- **[WON'T-DO — low value / behavior-risk residue] RuntimeSyntaxRegistry regex/match perf budgets**
  (separate file from the loader): per-pattern
  & joined-pattern byte cap before PCRE compile, region-start budget, explicit `overrides` for
  filetype shadowing, prefetch key by document-id, `FindFirstRegex` skip-mask incremental. (The
  `^`-anchor correctness fix landed 2026-07-14; the loader-side speed wins — cached content-hash
  fingerprint, dedup definition dirs, prewarm cold filetype — landed 2026-07-15, see "Fixed in the
  2026-07-15 syntax-reload speed pass" above; the per-pattern byte cap was assessed low marginal
  value — syntax defs are already length/count-bounded at load — and the skip-mask incremental
  rewrite is behavior-risk in a path that can't be visually verified here, so both stay deferred.)

- **[WON'T-DO — pre-existing threading design; lifetime redesign deferred]
  `CommitWorkflowService::DispatchCommit` captures `&state` across the background executor + mailbox.**
  A project switch that moves/destroys that state while a commit is in flight dangles the reference;
  `operation_generation_` guards logical correctness but not lifetime. A correct fix needs a
  lifetime/mailbox redesign (results keyed by stable id, independent of the state address) — deferred as
  a focused change; it is the parent of the patch-apply threading items triaged in tranche 7. (Same
  async-lifetime family as the git-sidebar off-thread won't-do below.)
- **[WON'T-DO — deliberate speed/durability tradeoff] Persistence: no parent-directory fsync after the
  atomic rename.** The atomic rename already prevents a torn read; only a crash inside the fs flush
  window can lose the newest session write. Left as-is by design.

#### 2026-07-13 multi-pass bug-campaign residue (deferred, latent / larger / behavior-risk)

Most of this cluster was closed in the 2026-07-14 curated pass (editor wheel, block-comment
toggle, diagnostics visual-column cache, AlignHunkLines cumulative budget, multi-caret overlap
refusal, `^`-anchor correctness, latent compare index guards — see the "Curated open-items pass"
section above). The still-deferred residue:

- **[RESOLVED (fast path + gate) 2026-07-14 / full O(n) rewrite WON'T DO 2026-07-15] `RuntimeSyntaxRegistry::FindFirstRegex` skip-mask full-tail rescan.**
  For a region carrying a `skip` regex, every cursor step re-runs `FindAllRegex(skip)` over the
  remaining tail. **Landed:** (1) the characterization gate the prior note demanded now exists —
  `tests/RuntimeSyntaxSkipTests.cpp` snapshots per-byte token output across both call sites (end
  search at `RuntimeSyntaxRegistry.cpp:819`, region-start search in `FindEarliestRegionStart` at
  `:746`) for escape masking, escaped-backslash, escaped-quote-at-EOL, nested-region re-entry into a
  skip region, `^`-anchored skip under NOTBOL, UTF-8-next-to-escape, and a 1500-interpolation stress
  line; (2) a provably-identical fast path in `FindFirstRegex` — when the segment has **no** skip
  matches, the `masked_buf` copy and the second pattern pass are skipped and the raw text is searched
  directly (masking nothing is the identity). That removes the per-step buffer copy and halves the
  pattern scans on the dominant escape-free segments (the common case: most cursor steps span text with
  no escape). **Residual full O(n) rewrite — WON'T DO (2026-07-15):** the worst case (a region with many
  nested children re-scanning the shrinking tail k times) remains, but eliminating it needs a full-line
  skip precompute reused across cursor steps, which is only byte-identical for *context-free* skips (no
  lookbehind / `\b` / `^`) and so must be gated on skip shape plus an adversarial-boundary equivalence
  proof. The benefit is narrow — it only bites regions that declare a `skip` AND contain many nested
  children on one ≤100 KB line (`kMaxHighlightLineBytes` already caps the blast radius); real code lines
  are short and the landed fast path covers the common case. Set against a hot-path highlight-semantics
  change that risks altering colors on adversarial input, the trade is not worth it. Perf A/B vs `main`
  (2026-07-15) confirmed the highlight scenarios are allocation-flat and within wall-clock noise after the
  fast path. The golden gate (`tests/RuntimeSyntaxSkipTests.cpp`) stays as the equivalence oracle should a
  concrete pathological repro ever justify revisiting; absent that, this is closed.
- **[WON'T-DO — low priority; per-line highlighter works] `RuntimeSyntaxRegistry` rules still compile
  without `PCRE2_MULTILINE`.** The `^`-at-segment-boundary correctness bug is fixed (NOTBOL on mid-line
  segments, 2026-07-14); `$` semantics across a segmented line were not revisited, but the highlighter
  works per single line and a full MULTILINE pass would need generated-table verification. Low priority.

#### 2026-07-13 deep subsystem audit backlog (intake for later bug-fix agents)

This section is intentionally broad and actionable. It was produced by reading across the tree, not
by fixing anything. Most items need a focused reproducer before implementation; the suggested tests
are the preferred first step so lower-cost follow-up agents can confirm the failure and lock in the
behavioral contract before changing code.

##### Startup, app plumbing, and file operations

- **[WON'T-DO platform-only] Windows terminal launch does not quote or pass `lpApplicationName` for
  custom shells.** Real defect (`C:\Program Files\...` mis-parsed by `CreateProcessW(nullptr, ...)`),
  but Windows-only and not compile/validate-able on this Linux host; per the platform-only policy,
  writing untested Windows code risks a worse regression. Fix direction recorded (pass
  `lpApplicationName`, keep the command line mutable). See "Won't-do — platform-only".
- **[WON'T-DO platform-only] Windows terminal backend has unsynchronized reader/stop state.**
  Windows-only data race (`running_`/`stop_requested_`/`process_info_.hProcess` unsynchronized between
  `Stop()` and `ReaderMain()`); same platform-only policy. Fix direction: mirror the POSIX branch's
  atomics/mutex discipline and join before close.
##### Persistence and durable file I/O

- **[RESOLVED 2026-07-14 — the harmful part] Persistence backup fallback can resurrect stale state
  after a primary read failure.** The concrete harm — overwriting a still-recoverable primary with
  stale backup state — is prevented by the 2026-07-14 "recover-and-protect for a corrupt primary"
  guard: `PersistenceService` records the recovered state's re-encoded baseline and suppresses a Save
  whose body matches (no user mutation), so a present-but-corrupt/unreadable primary is not clobbered;
  a real mutation writes through. Regression:
  `PersistedStateRecord/PersistenceServiceGuardsCorruptPrimaryFromBackupOverwrite`. The residual —
  a user-visible recovery banner — is a UI nicety, not a data-safety issue (see next item).
- **[WON'T-DO — best-effort by design] Atomic-save metadata application failure appears non-fatal.**
  Mode/owner preservation across the atomic rename is best-effort; surfacing a warning when the
  content write succeeded but a `chmod`/`chown` copy failed is a UI-feedback nicety, not a
  correctness/data-loss issue (the content is written correctly either way). Not worth the
  status-plumbing under speed/correctness-first. Kept documented should a concrete
  executable-bit-loss report ever justify it.
- **[RESOLVED 2026-07-14] Session/config recovery has no "do not overwrite recovered-from-backup"
  marker.** Directly addressed by the same corrupt-primary guard: a backup-recovered record is not
  written back to the primary unless a genuine mutation occurs (the re-encoded baseline is recorded
  and matching Saves are suppressed). A user-visible recovery banner remains a deliberate omission
  (UI nicety, above).

##### Editor, text model, snippets, and search semantics

- **[RESOLVED 2026-07-14] Multi-caret deduplication ignores selection ranges.** The 2026-07-14
  `TextViewport::MultiCaretSelectionsOverlap()` refusal (both `ApplyMultiCaretEdit` and the single-char
  fast path bail) explicitly flags "two carets sharing a start position with different anchors"
  (`TextViewportMultiCaret.cpp:138`) as overlapping, so the nondeterministic-discard scenario now
  refuses the whole edit rather than dropping one replacement. Regressions:
  `EditorMultiCaret/{RefusesOverlappingSelections,DisjointSelectionsStillApply}`.
- **[RESOLVED 2026-07-15] Snippet commit restores pre-snippet secondary carets at stale positions.**
  Took the "discard" option: `ExpandSnippetAtSelection` clears `saved_secondary_carets` on a
  successful expansion (keeping them only for the failure-rollback path, where the buffer is
  unchanged), so `CommitSnippetSession` no longer restores pre-snippet offsets that the replacement /
  field edits made stale — the snippet consumes the multi-cursor state. Regression:
  `EditorSnippet/DiscardsPreExpansionSecondaryCarets`.
- **[RESOLVED 2026-07-15] Newline insertion while a snippet is active can leave stale snippet ranges.**
  `SnippetTryInsertText` now calls `CommitSnippetSession` before declining a `\n`/`\r` payload, so the
  linked-edit session is ended rather than retaining pre-newline ranges (matches VSCode dropping the
  session on a multi-line insert; the host still performs the real insert). Regression:
  `EditorSnippet/…MultiLineInsertDeclinesFastPath` (extended to assert the session is dropped).
- **[RESOLVED 2026-07-15] Closed-file LSP workspace edits silently clamp out-of-range positions and
  then save.** `ApplyLspEditsToClosedFilesOnDisk` now validates each edit's line against the scratch
  document: a line beyond EOF is a hard reject that drops the whole file's edit group (leaving the file
  untouched) rather than clamping onto the last line, while the LSP end-of-document sentinel
  (`{line == line_count, character == 0}`) maps to the end of the last line and a `character` past the
  line end stays a soft clamp. Regression:
  `WorkspaceShell/ServerApplyEditRejectsOutOfRangeClosedFileEdit`. (The open-buffer applier's forgiving
  clamp is the sibling item below.)
- **[RESOLVED 2026-07-15] LSP and plugin workspace edit appliers do not reject overlapping edits up
  front.** Both the open-buffer applier (`WorkspaceShell::ApplyLspWorkspaceEdit`) and the closed-file
  applier (`LspService::ApplyLspEditsToClosedFilesOnDisk`) now, after the highest-first sort, walk the
  consecutive descending-order pairs and refuse the whole per-buffer/per-file group when a lower-start
  edit's end passes a higher-start edit's start (touching endpoints allowed). No partial,
  order-dependent double-apply. Regressions:
  `WorkspaceShell/WorkspaceEditRejectsOverlappingEdits` (open buffer) and the closed-file path shares
  the identical guard.

##### Workspace orchestration, LSP, code actions, and plugin-facing edits

- **[WON'T-DO for this sweep — feature gap, not a correctness bug] Server-initiated `WorkspaceEdit`
  ignores resource operations and version semantics.** Supporting `CreateFile`/`RenameFile`/`DeleteFile`
  from a server edit is a genuine capability addition (route through `FileOperationService` with
  containment + undo), not a bug in the shipped text-edit path — symbol rename and multi-file fixits
  (the common code actions microide surfaces) already work via text edits. `textDocument.version`
  rebasing is a rare edge because edits apply immediately on receipt. Recorded as a scoped feature to
  add when a concrete server-driven file create/rename/delete need arises; the fix direction stands.
- **[WON'T-DO — UI-feedback nicety] Server workspace edits return false when every edit is filtered,
  without surfacing why.** Returning a typed reason (`unsupported URI` / `outside project` / …) and a
  status message is a diagnostics-polish improvement, not a correctness/data issue — the edit is
  correctly not applied either way. Not worth the enum + status-plumbing under speed/correctness-first;
  recorded should code-action "did nothing" reports ever justify the UX work.
- **[RESOLVED — open-buffer-only dispatch is the containment] Plugin runtime path resolution for
  editor edits is lexical, not containment-enforced at read time.** Verified: `ApplyPluginWorkspaceEdit`
  resolves a named path **only** to an already-open editor buffer and returns false otherwise; it never
  writes files on disk. So `editor.apply_edits({ path = "../outside.txt" })` resolves to no open buffer
  and is dropped — the open-buffer-only dispatch is exactly the containment the item asked to enforce
  or document. No lexical path ever reaches a filesystem write on this plugin path.
- **[RESOLVED 2026-07-15] Plugin `editor.apply_edits` silently truncates edit arrays at 100,000
  entries.** `LuaEditorApplyEdits` now fails the request closed with
  "editor.apply_edits exceeds the maximum edit count" when the raw edit count exceeds `kMaxApplyEdits`,
  instead of applying a corrupting prefix. Regression:
  `WorkspaceShell/PluginApplyEditsRejectsTooManyEdits`.
- **[WON'T-DO — outside the plugin trust model] Plugin data-directory discovery trusts lexical roots.**
  A plugin root that is a symlink the user swaps mid-session could drift the data-directory identity.
  But plugins are user-installed code the host already runs with process/file capabilities; a user who
  places a self-swapping symlink at their own plugin root is not a threat microide defends against
  (consistent with plugins being trusted). Canonicalizing at discovery is recorded as the fix direction
  should plugins ever become a lower-trust boundary.

##### Git, compare, merge, and review state

- **[RESOLVED 2026-07-14] Compare picker history and outgoing-base pickers ran expensive git queries
  on the UI thread.** `OpenPickerForPath` / `OpenOutgoingBasePicker` now open the overlay immediately in
  a `loading` state and dispatch `CollectGitFileHistory` / `CollectGitBranches` + `CollectGitRecentCommits`
  on `project_background_executor_` via `WorkspaceShell::RequestComparePicker*`
  (`WorkspaceShellCompareMerge.cpp`). Results marshal back through a `util::MainThreadMailbox`
  (`compare_picker_mailbox_`, wake reuses `git_sidebar_event_type_`, drained in `ConsumeGitSidebarRefresh`)
  and repopulate via `ApplyComparePicker*` only if a process-monotonic
  `ComparePickerState::active_request_generation` still matches and the CommitPicker overlay is still
  visible — stale completions (overlay closed / project switched / newer open) are dropped. The bg job
  captures `root`/`path`/`generation` by value and never `&state_`. The synchronous path is kept for the
  `commit_spec != ""` case (control channel / headless). A three-function injectable provider seam on the
  shell (`compare_picker_*_provider_`, default = the real free functions) lets tests block git and assert
  the loading→populate→drop-stale sequence
  (`TestWorkspaceShellComparePickerOpensAsyncAndDropsStaleResult`). This also closed the "no cheap fake
  git/executor seam" tooling gap below for compare interactions. Follow-ups [RESOLVED 2026-07-14]: the
  picker git queries now run on a dedicated `WorkspaceShell::interactive_background_executor_` lane so a
  picker open never queues behind an in-flight sidebar `git status` refresh (the shared single-thread
  executor previously serialized them); and `CollectGitRecentCommits` dropped its redundant
  `rev-parse --verify HEAD` pre-check (`git log HEAD` already resolves to empty on an unborn branch, so the
  extra spawn was pure latency — covered by `Git/RecentCommitsOnUnbornBranchIsEmpty`). Residual: the
  outgoing-base flow still issues two git invocations (branches via `for-each-ref`, recent commits via
  `git log`) because they are genuinely distinct queries with no single-process collapse; they now run on
  the dedicated lane, so the cost no longer contends with the sidebar refresh.
- **[WON'T-DO — documented normalization by design] Merge result generation may normalize final
  newline / line-ending intent across conflict choices.** Verified intentional: a merge tab detects a
  single `result_line_ending` once (`WorkspaceShellMergeState.cpp`) and `SerializeLines` applies that
  one ending uniformly to the whole merged output. The merged file therefore gets one consistent line
  ending rather than a mix stitched per conflict side — which is the desirable outcome (a mixed-ending
  file is almost always a mistake). This is the "document the normalizing behavior" resolution the item
  offered: per-choice ending preservation is deliberately not done.

##### Project search, indexing, blame, and repository state

- **[RESOLVED 2026-07-14 — the harmful part] Project search per-worker memory cap can multiply into
  multi-gigabyte transient use.** The 2026-07-14 `kMaxSearchFileBytes` (32 MiB) cap on
  `ReadFileForTextSearch` cut the per-worker whole-file buffer from 512 MiB to 32 MiB, so N=8 workers
  now peak at ~256 MiB transient instead of ~4 GiB. A global in-flight byte budget (further tightening)
  is a lower-value residual won't-do.
- **[RESOLVED 2026-07-14] Project search cancellation still finishes a full current-file scan before
  stopping.** Verified already addressed: the worker polls `cancel_requested_` per line
  (`ProjectSearchService.cpp`) and the regex path polls internally (`kRegexCancelPollInterval`), so
  cancellation is threaded through line/regex iteration — no full-file scan runs to completion after a
  stop.
- **[WON'T-DO — cap-is-safety policy] Repository state caps can hide important changes silently.** Git
  status / search / file-index caps drop data past the cap **by design** (safety limits against a
  pathological/hostile repo). A first-class `truncated` banner in every UI consumer is a
  display-feedback nicety, not a correctness fix, and a repo exceeding the (high) caps is itself
  pathological. Recorded as the shared "truncation-visibility" cluster should it ever be prioritized.

##### Terminal and ANSI behavior

- **[WON'T-DO platform-only] Windows terminal `Write()` can block the shell thread indefinitely.**
  Windows ConPTY synchronous write can hang if the child stops reading stdin; needs a bounded-write
  queue / overlapped I/O. Windows-only, not validatable here. Fix direction recorded.
- **[RESOLVED 2026-07-14] OSC 52 clipboard support is capped only by the global 8 KiB escape buffer,
  not by decoded payload policy.** The allow/deny setting already gates normal OSC 52 writes, and an
  OSC 52 sequence dropped for overrunning the 8 KiB escape buffer is now flagged
  (`TerminalSession::ConsumeOversizedOsc52Dropped`) and surfaced as a toast rather than failing
  silently. Regression: `TerminalSession/Osc52OversizedPayloadIsFlagged`.
- **[WON'T-DO — display-only today] OSC 7 working-directory reports are accepted without project
  containment policy.** OSC 7 already rejects non-local hosts (2026-07-13); the reported cwd is
  display-only and is not consumed for any filesystem operation, so there is nothing to contain today.
  The containment guard belongs with a future consumer that uses `current_working_directory()` for FS
  work — recorded there rather than pre-emptively gating a display string.
- **[WON'T-DO — observability nicety] Terminal parser recovery after overlong CSI/OSC drops the whole
  sequence without an event.** Dropping an overflowing sequence is the correct speed/memory behavior; a
  dropped-sequence debug counter is a diagnostics nicety, not a correctness fix. Recorded for a future
  terminal-compat debug surface.

##### Rendering, UI state, and view models

- **[WON'T-DO — refactor nicety] Texture cache failure marker policy is inconsistent across failure
  modes.** The load-bearing defect (transient `SDL_CreateTexture` failure suppressing a valid image)
  was fixed 2026-07-13 with a bounded retry. Unifying decode/null-renderer/create/reset into one named
  state machine is a code-clarity refactor, not a behavior fix — the individual transitions already
  behave correctly. Recorded as the fix direction if the cache is ever reworked.
- **[WON'T-DO — near-zero-probability, plugin owns both payloads] Plugin surface texture cache keys
  depend on content hashes without collision verification.** A 64-bit content-hash collision between
  two distinct raster payloads is astronomically unlikely to occur accidentally, and a plugin that
  deliberately crafts one already owns both surfaces (no privilege gained). Byte-length/dimension
  verification is recorded as the fix direction should plugin rasters ever cross a lower-trust boundary.
- **[WON'T-DO — enforced by discipline + existing lint] Row-level render hot paths rely on discipline
  outside the linted set.** The hard invariant already covers the named render TUs (the ones on the
  per-frame path); extending the lint to every render-adjacent helper is a tooling expansion with
  diminishing returns. Recorded should a per-frame string-materialization regression ever slip through.
- **[WON'T-DO — per-surface behavior is correct + tested] Mouse hit-test behavior differs across
  editor, compare, merge, and sidebar surfaces.** The concrete row-band bugs were fixed (compare/merge
  row-0, editor wheel-active-pane) with regressions; consolidating four correct per-surface hit-tests
  into one shared helper is a refactor, not a behavior fix. Recorded as the consolidation direction.

##### Settings, configuration, and user-visible feedback

- **[RESOLVED — the concrete drift; lint is a nicety] Boolean setting parsing is split across
  registries and overlays.** The concrete bug (settings overlay `StepSetting` truthiness diverging
  from the render predicate) was fixed 2026-07-14 by routing through the single `SettingFlagEnabled`
  helper, which now governs both the render check and the toggle. A lint banning direct `"true"`/`"1"`
  comparisons is a defense-in-depth nicety, not an open behavior bug.
- **[WON'T-DO — UI-feedback nicety] Several failure paths return `false` with no user-facing status.**
  The listed operations correctly do nothing on failure; adding an `OperationResult`/status-message
  convention is a UX-polish layer, not a correctness fix. This is the same silent-failure cluster
  triaged elsewhere in this pass. Recorded should "action did nothing" reports justify the plumbing.
- **[WON'T-DO — policy already effectively in place] Caps and truncation limits are inconsistent in how
  they fail.** In practice the caps now behave by category: security/correctness caps fail closed
  (e.g. `apply_edits` over-cap now rejects; overlapping/out-of-range LSP edits reject); performance
  caps bound work; display caps drop. A formal spec note is process polish, not a code fix.

##### Test and tooling gaps exposed by this audit

- **[WON'T-DO — large tooling investment] Windows/macOS-only defects accumulate because platform code
  lacks compile-and-shim tests on Linux.** Extracting pure builders/state machines from every platform
  TU so Linux can shim-test them is a substantial cross-platform refactor of its own, not a bug fix.
  Recorded as the standing direction; the individual platform-only defects stay documented won't-do.
- **[RESOLVED 2026-07-14] No cheap fake git/executor seam for UI-thread blocking regressions.**
  Compare interactions have an injectable provider seam
  (`WorkspaceShell::compare_picker_{file_history,branches,recent_commits}_provider_`, set in tests via
  `WorkspaceShellTestAccess::SetComparePicker*Provider`), so a test can block the git query and assert
  the overlay is visible + loading before git returns. The git **sidebar** refresh path
  (`GitRepositoryService`) now has the matching seam: `SetRepositoryStateProviderForTesting` injects the
  `git status` producer so `Git/…BlockingRepositoryStateProviderIsAsync` blocks git on a gate and asserts
  the sidebar is refreshing (nothing published) before it returns, then publishes on release — no real git
  spawned. Both blocking-UI regression surfaces are now testable without sleeping on real git.
- **[WON'T-DO — unit coverage now closes the risk] No single fuzz target covers plugin/workspace edit
  range normalization.** The edit appliers now reject out-of-range (beyond-EOF) and overlapping edits
  and cap the array size, each with a regression test (2026-07-15). A dedicated fuzz target is
  additional assurance rather than closing a live defect; recorded as a nice-to-have if the edit path
  grows more complex.
- **[WON'T-DO — process guidance, not source] Known-tech-debt entries frequently outlive their
  original reproduction context.** This is a working-practice note for future passes (add a failing
  test named after the entry, then resolve), which this very sweep follows. No source change.

#### 2026-07-13 deep subsystem audit backlog — additional tranche

This is a second pass over less-central code paths and second-order failure modes. Treat these as
bug-investigation tickets: add the reproducer first, then fix or explicitly demote the item if the
test proves it is unreachable.

##### Persistence, JSON, and protocol framing

- **[WON'T-DO — recovery-by-design; harmful part guarded] Persisted-record writer leaves a stale
  backup after every successful write.** Keeping the previous generation as `*.bak` is the recovery
  mechanism. The harmful consequence (a stale backup silently becoming the new primary) is already
  prevented by the 2026-07-14 corrupt-primary guard (backup-recovered state is not written back unless
  genuinely mutated). Deleting the backup after each write would remove the recovery net; leaving it is
  the intended contract.
- **[WON'T-DO — best-effort by design, needs absent seam] Persisted-record write failure after backup
  rename can leave the primary restored best-effort only.** The atomic-rename ordering already prevents
  a torn read; a double-rename failure requires flaky storage failing two renames in a row, and the
  rollback is best-effort by design. A richer error category is a diagnostics nicety, and a
  deterministic test needs a `RenameReplacing` injection seam that does not exist. Recorded.
- **[RESOLVED 2026-07-14] Persisted-record reads allocate the full 256 MiB cap before validating
  header/body size.** `PersistedRecordReader` now validates the fixed 16-byte header (magic + version)
  before allocating the body, so a hostile large file is rejected after ~16 bytes. Regressions:
  `PersistedRecordIo/ReaderRejectsLarge{BadMagic,UnsupportedVersion}OnHeader`.
- **[WON'T-DO — transport size caps + linear parse; node cap risks regressions] JSON parsing has no
  aggregate array/object entry cap.** The inbound protocol transports already bound message size (LSP/
  DAP `Content-Length`, control line buffer), and JSON parse is ~linear in input size, so a parser
  node budget adds little over the transport cap while risking a regression on a legitimately large
  response (semantic tokens / symbols for a big file). The depth cap (200) already stops the stack
  vector.
- **[WON'T-DO — standard behavior; config path already dedups] JSON object duplicate keys use "last
  wins" with no signal.** Last-writer-wins is standard JSON-object behavior and matches most peers; the
  one place it mattered for correctness (persisted user/project config) already dedups at decode
  (last-wins) deliberately. Rejecting protocol JSON on a duplicate key would be stricter than peers.
- **[WON'T-DO — serialization safety by design] JSON serialization silently converts non-finite doubles
  to `null`.** Emitting `null` for NaN/Inf keeps the output valid JSON (the alternative is producing a
  token no strict peer accepts). The protocol-argument cases that mattered (e.g. plugin status
  `progress` NaN) are already rejected at their typed builders (2026-07-13). Generic serialization
  keeping the safe fallback is correct.
- **[WON'T-DO — each transport already caps appropriately] Protocol message-size policy is split
  between JSON, DAP, LSP, and control surfaces.** DAP/LSP bound via `Content-Length` framing and the
  control socket via its line-buffer cap; each is sized to its surface. A single unified budget is
  documentation/consolidation polish, not a missing bound.

##### DAP and debug workflow

- **[WON'T-DO — covered by lifecycle + stop-epoch guard] DAP launch/configuration callbacks capture
  `this` without a session-generation guard.** Verified sound (see the "DAP spawn-failure" investigation
  in this doc): all DAP callbacks dispatch on the **main thread** via `main_mailbox.Post`, never
  synchronously on the io thread, and `~DapClient` joins both worker threads before deleting its Impl —
  so a destroyed/replaced session's pending callbacks are dropped, not fired against new state. Stop-
  related responses (stack/threads/scopes) are additionally dropped by the `stop_epoch_` guard. A
  uniform session token would be belt-and-suspenders over an already-ordered lifecycle; not a live race.
- **[WON'T-DO — low-frequency, needs event-loop timer, teardown already bounded] DAP `RequestStop` can
  leave the UI active if `terminate` is sent but the adapter never answers.** Adapters honor `terminate`
  in practice; the manager teardown (`~DapClient`) already joins worker threads with bounds, and closing
  the session forces disconnect. A bounded optimistic-terminate timer needs DAP-event-loop timer infra
  that does not exist and would be exercised only by a deliberately-hung adapter. Recorded as the fix
  direction if a real hung-adapter report appears.
- **[RESOLVED — already correct] DAP `configurationDone` rejection records an error but still
  transitions to Running.** Verified the current `SendConfigurationDone` response handler
  (`DebugSession.cpp`) checks `response.success`: a rejection **during the handshake**
  (`Configuring`/`Initializing`) transitions to `Failed` and shuts the client down; only a successful
  response moves to `Running`. The behavior the item describes is not present.
- **[RESOLVED 2026-07-15] DAP resume commands optimistically clear stopped UI without checking request
  success.** `SendResumeRequest` keeps the synchronous optimistic Running flip (load-bearing for the
  stale-stack-drop guard) but now passes a response callback that, on rejection, undoes it —
  `SetState(Stopped)` + `RequestStackTrace(last_stop_)` re-projects the execution line / Call Stack /
  scopes. Regression: `DebugService/SessionRejectedResumeRestoresStoppedView` (new `reject_resume` mock
  adapter mode).
- **[RESOLVED — positional-to-requested + id reconciliation by design] DAP breakpoint responses are
  matched by send order, not stable breakpoint identity.** Verified deliberate: the verification
  consumer (`DebugServiceCallbacks.cpp`) anchors each result to `requested_lines[i]` (the user's chosen
  line, NOT the adapter's possibly-relocated `breakpoint.line`) and drops any extras beyond the
  requested count; later `breakpoint` events reconcile in place by adapter **id**. Matching by adapter
  line — the item's suggestion — would break the relocate case (gutter marker jumps to the adapter's
  line). The DAP spec requires the response array in request order and count; the code bounds the
  non-conformant "more results" case.
- **[WON'T-DO — external source is expected while debugging] DAP source paths from adapter events are
  trusted as filesystem paths.** Stepping into system libraries / dependencies (`/usr/lib/...`, vendored
  sources outside the project) is a normal, expected debugging action, so a debug source path that
  leaves the project root is not an attack — it is the point of a debugger. The debuggee's own sources
  can legitimately live anywhere. Containment here would break step-into-stdlib.
- **[WON'T-DO — heuristic covers real adapters] DAP value-size limits are gdb-name heuristics.**
  `CommandLooksLikeGdb` correctly classifies the real gdb variants (`gdb`, `arm-none-eabi-gdb`,
  `gdbserver`); the freeze-risk guard it drives is a defensive value-paging limit whose only downside on
  a false positive is slightly more conservative paging. A capability/type/setting override is a nicety,
  not a correctness fix.
- **[WON'T-DO — display-cap policy] DAP variables/scopes list truncation has no visible "more data
  omitted" state.** The 10k parser cap is a safety limit against a hostile/huge adapter payload;
  surfacing a `truncated` banner + paging is the same display-feedback nicety triaged across this pass
  (a legitimate 10k+ single scope is itself pathological).

##### Plugins and extension data

- **[WON'T-DO — protective cap; real plugins are small] Plugin decoration publish can allocate huge
  valid payloads up to four times per call.** The 100k-per-kind cap bounds a hostile payload; real
  decoration providers publish a handful to hundreds of entries. Tighter per-file/per-owner budgets are
  tuning, and plugins are user-installed (trusted) code. Recorded as tuning if a huge-decoration plugin
  ever appears.
- **[WON'T-DO — display-only, capped, plugin trust] Plugin decoration paths use lexical resolution, not
  containment.** Decorations never write files; an outside-project decoration is inert display data
  already bounded by the per-kind cap, and the store's rename/delete retargeting operates on keys, not
  the filesystem. Under the plugin trust model this is not a reachable harm.
- **[WON'T-DO — display-cap policy] Plugin code-action/provider result truncation is silent.** Same
  display-cap policy triaged across this pass: completion/action/test/SCM lists clamp for UI safety. The
  correctness-critical mutation surface (`apply_edits`) was changed to fail-closed on over-cap; provider
  *result* lists are display data where dropping the tail is acceptable.
- **[WON'T-DO — external navigation is the point] Plugin language-provider location paths are not
  containment-checked.** Go-to-definition / find-references legitimately navigate into stdlib,
  dependencies, and generated sources outside the project root — that is the feature. Containing it
  would break jumping to a symbol defined in a vendored/system header, same as the DAP external-source
  case.
- **[WON'T-DO — plugin trust model] Plugin provider string fields have no per-field length limits.**
  Plugins are user-installed code already bounded by the Lua memory budget; a plugin bloating its own
  completion documentation degrades only its own UI. Per-field clamps are a nicety, not a trust boundary.
- **[WON'T-DO — covered by single-worker teardown ordering] Plugin callbacks can synchronously call
  back into host surfaces during lifecycle teardown.** The plugin runtime is single-worker-threaded and
  teardown erases each runtime entry for a state before the state is nulled (see the verified-non-defect
  note on provider-query guards), so a teardown callback cannot observe a half-updated registry from
  another thread. An explicit "no-publish during teardown" mode is belt-and-suspenders.

##### Project change, file finder, and indexing

- **[WON'T-DO — bulk-change edge; rescan reconciles] Project change coalescer can drop per-file
  reload/diagnostic updates once the cap flips to tree rescan.** Passing 1,024 pending changes is a bulk
  operation (branch switch, mass generate) where the tree rescan rebuilds the index and open tabs are
  reconciled by the refresh. A per-file external-change banner for one file lost in a 1,024+ flood is an
  edge, and the follow-on mtime sweep is a nicety.
- **[WON'T-DO — nets to Deleted; delete target is the right path] Project change coalescer
  delete-then-create loses the new absolute path for Created.** The item itself notes most two-event
  combinations are correct; the residual (a Created-then-Deleted collapsing to Deleted with the old
  absolute path) nets to a deletion whose path is the delete target — which is what a delete consumer
  wants. No reachable stale-path harm.
- **[WON'T-DO platform-only] File finder recent-file de-dup keys are platform-sensitive strings.**
  Only bites case-insensitive folding hosts (Windows/macOS); Linux is case-sensitive so the key is
  exact. Not validatable here. Fix direction (host-normalized key) recorded.
- **[WON'T-DO — bounded by the capped index] File finder keeps the full uncapped matched-index set for
  narrowing.** The index itself is capped, so the broad-query match-index vector is bounded (~one
  size_t per indexed file) and built once per query, not per frame. A compressed representation is
  micro-tuning; the per-keystroke deep-copy that actually hurt was fixed 2026-07-15.
- **[WON'T-DO — display; pathological tree] Full project scan does not propagate traversal-budget
  truncation to `FileIndex::truncated()`.** A repo exceeding `kTreeTraversalEntryBudget` is itself
  pathological; surfacing a "truncated" banner is the display-visibility nicety cluster. The budget is
  high enough that real repos index fully.
- **[WON'T-DO — ExcludeHidden is a hard UI mode by design] Project scanner hidden filtering checks only
  the current filename before ignore rules.** Resolved by decision: `ExcludeHidden` is a hard "hide
  dotfiles" UI mode, not a gitignore-style filter, so a negated `!.hidden/` does not re-include a hidden
  subtree. This is the intended, documented behavior (users toggle hidden-file visibility explicitly).
- **[WON'T-DO platform-only] Windows native tree watcher cannot watch more than
  `MAXIMUM_WAIT_OBJECTS - 1` roots.** Windows-only wait-object limit forcing a polling fallback;
  surfacing the fallback state / sharding handles is Windows work not validatable here.

##### Terminal, input, and ANSI behavior

- **[WON'T-DO — observability nicety] Terminal pending query replies silently truncate at 64 KiB.**
  The cap prevents a reply-flood UI freeze; a program legitimately issuing 64 KiB of query replies
  before the terminal drains is pathological. Dropping whole replies + a debug counter is a
  diagnostics nicety, not a correctness fix.

##### Rendering, UI, and user feedback

- **[WON'T-DO — intentional + tested per surface] Debug pane row hit-testing maps the top band to row
  zero.** The item itself notes this is deliberate and pinned by tests; the compare/merge top-band bug
  was separately fixed. Consolidating four correct per-surface hit-tests into one shared helper is a
  refactor, not a behavior fix.
- **[WON'T-DO — trackpad polish] Overlay scroll wheel paths use integer ticks and can skip small high-resolution wheel deltas.**
  SDL wheel events can carry fractional/precise deltas depending on device. Coordinators that cast to
  integral ticks can ignore trackpad micro-scrolls or behave differently across platforms. Fix
  direction: accumulate fractional wheel deltas per surface and consume whole rows. (Integer-tick
  scrolling works on all devices; sub-row trackpad accumulation is polish.)
- **[WON'T-DO — UX polish] Several UI lists clamp selection after content changes but do not preserve
  item identity.** Preserving the selected row by stable id across an async refresh is a nicety;
  index-clamp keeps a valid selection and the common case (no mid-interaction refresh) is unaffected.
- **[WON'T-DO — infra for already-declined niceties] Status/error messages lack severity and lifetime
  policy.** A status-message service is the prerequisite for the silent-failure/truncation-banner
  niceties triaged as won't-do throughout this pass, so it inherits the same disposition.

#### 2026-07-13 deep subsystem audit backlog — third tranche

##### LSP / DAP protocol framing and request semantics

- **[WON'T-DO — bounded churn, conformant servers don't hit it] Malformed LSP frame recovery can scan
  arbitrary body bytes as future headers.** A conformant server never emits a nonnumeric/nonpositive
  `Content-Length`; the line-by-line resync eventually recovers and the churn is bounded by the message
  already in the buffer. Tightening the resync window is hardening against a malfunctioning server, not
  a live defect.
- **[WON'T-DO — non-conformant; microide sends integer ids] String-valued JSON-RPC response ids are
  ignored.** MicroIDE emits integer request ids and servers echo them; a server stringifying the id it
  was given is non-conformant and rare. Accepting decimal-string ids is a tolerance nicety.
- **[WON'T-DO — feature, empty result is correct] LSP request timeouts and server exits are delivered
  as empty success-shaped objects.** The empty result is technically correct (no data arrived);
  distinguishing "timed out" from "genuinely empty" needs an `LspRequestOutcome` envelope threaded
  through every request callback — a UX feature, not a correctness fix. The server-status segment
  already surfaces a failed/exited server.
- **[WON'T-DO — rarely-used LSP feature, null is valid] `window/showMessageRequest` is auto-dismissed
  without surfacing server actions.** Answering with `null` (dismissal) is spec-valid; routing server
  action prompts through the prompt surface is a feature addition, not a bug.
- **[WON'T-DO — feature gap] `window/showDocument` requests are auto-rejected/ignored.** Opening
  server-requested documents is a capability microide does not implement; declining is spec-valid.
  Recorded as a feature.
- **[WON'T-DO — config-mapping feature; servers default] LSP `workspace/configuration` does not resolve
  dotted section names.** A server asking for `rust-analyzer.cargo` and getting null falls back to its
  own defaults — it works, just without microide-side overrides. A settings-id↔section mapping is a
  configuration feature, not a correctness bug.
- **[WON'T-DO — display-cap policy] LSP result truncation is silent on locations/symbols/tokens/etc.**
  The same display-cap-visibility cluster: the parser caps arrays for UI-thread safety; a `truncated`
  banner is the nicety triaged throughout this pass.
- **[WON'T-DO — counts capped; server-provided] Signature-help labels and documentation have no
  per-field byte cap.** Array counts are capped; a per-field byte cap is defense-in-depth against a
  hostile server (servers are semi-trusted, launched by the user's own config). Low value; the hover
  path that mattered was already capped 2026-07-13.
- **[WON'T-DO — observability nicety] Semantic-token parsing silently ignores trailing partial groups
  and invalid coordinates.** Ignoring malformed tokens is the correct robustness behavior; a
  dropped-token debug counter is diagnostics polish.
- **[WON'T-DO — refactor] DAP and LSP framing rules are implemented separately and can drift.**
  Extracting a shared `ContentLengthFramer` is code-consolidation, not a behavior fix; both framers
  work and their caps/tolerances are individually covered.

##### Patch apply, compare review, and git boundaries

- **[WON'T-DO — async-lifetime redesign family (see CommitWorkflow won't-do)] Patch apply background
  work calls shell callbacks from the background executor.** Marshalling the shell/service callbacks back
  through a completion mailbox is the same redesign as the `CommitWorkflowService::DispatchCommit`
  lifetime won't-do; deferred as that focused change. Refresh generations guard logical correctness
  today.
- **[WON'T-DO — same family; generation guards logical correctness] Patch apply stale checks compare
  only repository generation, not identity.** Adding a project/session token to the stale check is part
  of the same async-lifetime redesign; the same-generation-across-projects collision needs the completion
  mailbox + identity work deferred with the commit-workflow item.
- **[WON'T-DO — dead field / same family] `diff_model_generation` is captured then intentionally
  unused.** Either removing the unused field or validating the model revision belongs with the compare/
  patch async-lifetime redesign; the destructive-discard confirmation is guarded by the repository
  generation today.
- **[WON'T-DO — text-focused review; content is staged correctly] Patch generation hardcodes file
  modes for create/delete.** microide's compare/stage review targets text content; the `100644`
  default stages the content correctly. Preserving executable/symlink mode metadata through the patch
  generator is an edge that also overlaps the mode-only item below — deferred with the same disposition.
- **[WON'T-DO — feature gap] Mode-only changes have no patch-apply path.** Staging a chmod-only /
  symlink-target change from compare review is a capability microide's content-hunk-based review does
  not model; users can apply mode changes via a terminal. Recorded as a first-class-compare-entry
  feature, not a bug.
- **[WON'T-DO — rename staging works via the new path] Rename/copy metadata is flattened to the new
  path for review operations.** Content-hunk staging on the new path applies correctly; carrying the
  old path + similarity score to open the old side by identity is a review-fidelity feature, not a
  correctness defect in the staged result.
- **[WON'T-DO — argv-based + --verify; config is user-owned] Branch base resolution accepts arbitrary
  config strings as refs.** All git invocations are argument-vector based with `--literal-pathspecs`,
  and ref resolution uses `--verify`, so a weird `branch.<name>.gh-merge-base` value fails to resolve
  rather than executing anything surprising. The config is the user's own; a stricter
  `check-ref-format` pre-validation is label-cosmetic hardening.
##### Control channel, socket lifecycle, and command transport

- **[WON'T-DO — runtime-dir containment, tiny window] Control socket file permissions are fixed after
  bind, leaving a short wider-permission window.** The socket lives in the user's private runtime
  directory; the window between bind and `chmod(0600)` is microscopic and requires another local user
  racing a connect into that directory. The 2026-07-13 Start hardening (lstat, refuse non-socket) is the
  material security fix; a restrictive bind-time umask is marginal.
- **[WON'T-DO — CLI-side, AF_UNIX connect is near-instant] Control client `connect()` is still blocking
  and outside the timeout budget.** The blocking `connect()` is in the separate CLI control client, not
  the IDE; an AF_UNIX connect to a listening socket returns immediately. A nonblocking+EINPROGRESS+poll
  path guards only a pathological/backlogged endpoint the CLI would rarely meet.
- **[WON'T-DO — bounded loss, EOF is a valid signal] Control `SendLine` decrements in-flight on write-
  buffer overflow.** The connection is flagged for drop, so the data loss is bounded; a control caller
  seeing EOF instead of a structured overflow line is a diagnostics nicety, not incorrect behavior.
- **[WON'T-DO — flood protection; client is misbehaving] Inbound control queue overflow drops the
  flooding connection after partially queueing earlier lines.** Executing the already-queued prefix and
  dropping a client that overruns `kMaxInboundQueued` is acceptable back-pressure against a misbehaving
  peer; atomic all-or-nothing batching is a semantics nicety.
- **[WON'T-DO — edge; harmful case fixed at Start] Control socket rebind only stat()s the socket path.**
  The material risk (destroying a user's non-socket file) is already prevented at `Start` (2026-07-13
  lstat + refuse non-socket). A mid-run replacement of the live socket path by another node is an edge
  that leaves the existing listener fd working for already-connected clients; repairing it is a nicety.

##### Editor primitives, text layout, and Unicode correctness

- **[RESOLVED 2026-07-15] Multi-line paste into single-line fields concatenates tokens with no
  separator.** `SingleLineEditor::Insert`/`Append` now collapse each run of CR/LF into a single space
  (dropping leading/trailing runs) via `CollapseLineBreaksToSpaces`, so `foo\nbar` pastes as `foo bar`
  (word boundaries preserved) instead of `foobar`. Regressions:
  `SingleLineEditor/{StripsLineBreaksOnInsert,SupportsSnapshotAndAppend}` (updated to the space
  behavior).
- **[WON'T-DO for this sweep — large visual-layout pass] Text visual width treats every non-tab code
  point as one cell (+ identifier hover ranges are ASCII-only).** These are the real "Unicode layout
  completeness" cluster: a wcwidth/grapheme-width layer through caret hit-testing / horizontal scroll /
  compare alignment / inlay placement, plus Unicode identifier classification in
  `TextLayout::IdentifierRangeAt` for hover/definition on `café`/`变量`. This is a substantial layout
  change whose correctness is inherently visual (cell alignment for CJK/emoji/combining marks) and
  cannot be verified on this headless host — doing it piecemeal risks inconsistent cell math. Deferred
  as a focused Unicode pass with visual verification; the `Utf8IsIdentifierCodepoint` /
  `Utf8CaseFold` infra added 2026-07-13 is the starting point. ASCII text (the overwhelming common
  case) is unaffected.
- **[WON'T-DO — pathological input] Inlay hint column math trusts plugin/LSP label widths after
  truncation but not aggregate overflow.** Thousands of inlay hints anchored at one line is a
  pathological adapter/plugin payload; individual labels are already capped. Saturating the aggregate
  is defense-in-depth against a case real language servers never produce.

##### Theme, rendering, and UI output

- **[WON'T-DO — cycle handled safely; error text is a nicety] Theme include cycles are silently
  accepted.** The 2026-07-13 include bounding (max depth 16 / count 128, skip-and-continue on a cycle)
  keeps self-including themes loadable and prevents unbounded recursion. Surfacing a structured cycle
  error is a diagnostics nicety over already-safe behavior.
- **[WON'T-DO — documented lifetime, render follows it] `TruncateToWidthView` returns a thread-local
  scratch view that is easy to invalidate.** The immediate-draw lifetime is documented and the render
  path draws each truncated label before the next call. An explicit scratch object is defensive
  refactoring; no live wrong-label bug exists.
- **[WON'T-DO — display fallback nicety] Output-panel context snippet highlighting assumes token count
  ≥ visible byte length.** Falling back to plain text when tokenization is short is a safe render
  fallback; distinguishing "missing" from "stale" for a status hint is polish.
- **[WON'T-DO — heuristic serves the common error+context case] Output-panel reference path context is
  sticky until an empty line or new reference changes it.** Inheriting the last reference path for the
  context lines that follow a `file:line: error` is usually correct (clicking the error's context opens
  the error file). Per-entry path attribution would refine the rare unrelated-line-between-references
  case at the cost of the common one; kept as the heuristic.
- **[WON'T-DO — allocation-free fits-case; caching is micro-tuning] Compare/merge truncation still
  happens inside render translation units.** `TruncateLabelView` is allocation-free on the fits case
  and the lint already keeps product logic out of render; caching truncation per label+width revision
  is CPU micro-tuning, not a correctness or allocation issue.

##### Settings and persistence edge cases

- **[RESOLVED 2026-07-13] Duplicate persisted setting keys resolve differently before and after
  mutation.** Persisted user/project config now dedups duplicate setting ids at decode (last-writer-
  wins), so no duplicate keys survive into the in-memory layer — the Reindex-vs-Upsert-vs-Find
  divergence the item describes cannot manifest because there is at most one entry per id after load.

#### 2026-07-13 deep subsystem audit backlog — fourth tranche

##### File watching, indexing, and tree traversal

- **[WON'T-DO — extreme tree; degraded-status nicety] Linux `FileIndexWatcher` degrades to a partial
  native watch tree.** Hitting `ENOSPC` / the inotify watch cap requires an enormous tree (or a low
  `fs.inotify.max_user_watches`); the index still serves its indexed prefix and a manual refresh
  resyncs. A poll-fallback swap or a "degraded watcher" banner is reliability polish for an extreme
  environment, not a live defect on normal repos.
- **[WON'T-DO — degraded-index visibility nicety] Moved-in directory indexing ignores subtree
  truncation.** Moving a directory larger than `entry_budget` into the project is an extreme case; the
  indexed prefix still works and a manual refresh completes it. Propagating the truncated bit to a
  degraded-index banner is the same truncation-visibility nicety triaged across this pass.
- **[WON'T-DO — kernel-chunked, self-limiting] Inotify drain batches have no per-batch change cap.**
  Kernel inotify input is already chunked; a high-churn generator producing one huge batch is a
  pathological workload, and chunked publishing is a smoothness nicety, not a correctness fix.
- **[WON'T-DO — fallback path, extreme tree] Poll fallback builds full snapshots without the native
  entry budget.** The 750 ms poll fallback only runs when native inotify is unavailable/exhausted; on a
  tree huge enough for the budget to matter, native watching would already have degraded. Applying the
  budget to the fallback is fallback-path perf tuning.
- **[WON'T-DO — predictable fallback-path cost] Poll fallback diff materializes complete previous and
  current maps.** Same 750 ms fallback path; rebuilding the snapshot map each cycle is predictable and
  bounded, and mtime-probe/incremental-visitor optimizations are fallback-path tuning, not a live
  defect.
- **[WON'T-DO — rapid-switch micro-cost] `FileTreeWatcher::SetRoots` can spend startup time collecting
  a native watch list for a tree that is later superseded.** Only rapid project switching pays a stale
  traversal, and each traversal is the same bounded scan the switch needs anyway; threading a
  cancellation token is a startup-latency micro-optimization for an uncommon interaction.
- **[WON'T-DO — display-truncation cluster; pathological tree] `CollectProjectFiles` returns a partial
  file list on traversal-budget exhaustion without a truncation signal.** Same truncation-visibility
  disposition as the other budget items: a repo exceeding `kTreeTraversalEntryBudget` is pathological
  and the budget is high; a `{files, truncated}` signal + banner is the deferred nicety.
- **[WON'T-DO — ExcludeHidden is a hard UI mode by design] Project file scanning and index watching can
  disagree about hidden files.** Resolved by the same decision recorded earlier: `ExcludeHidden` is a
  hard "hide dotfiles" mode. Routing all surfaces through one `ProjectTraversalFilter` is a
  consolidation refactor; the observable policy is intentional.

##### Project search, finder, and command completion

- **[WON'T-DO — skip-count footer nicety] Project search silently skips unreadable, binary, and
  too-large files.** Skipping unreadable/binary/oversized files is correct search behavior (you don't
  want binary noise); a classified skip-count footer is a diagnostics nicety in the same
  display-visibility cluster triaged throughout this pass.
- **[WON'T-DO — streaming search by design] Project search result ordering is nondeterministic under
  parallel workers.** Streaming matches as workers find them (rather than blocking for a global sort) is
  the responsive design; results are grouped by file in the UI. A stable final presentation sort is a
  refinement, not a correctness issue — every match is still reported.
- **[WON'T-DO — progress display nicety] Count-all project search can finish with stale-looking
  progress.** The final count is correct; the visible counter lagging at the display cap during a
  long count-all is a progress-smoothness nicety.
- **[RESOLVED (dominant path) 2026-07-15 / remainder WON'T-DO 2026-07-15 (session 3)] File finder cache
  rebuild lowercases and stores the entire file index on the UI thread.** The dominant interactive cost —
  `EnsureCacheBuilt` calling `SnapshotWithVersion()` (an O(index) deep copy of every `ProjectFile` under
  the index lock) on **every keystroke** just to read the version — is fixed: it now checks the cheap
  `index_->version()` first and only snapshots on an actual version change
  (`FileFinder/WarmRefreshDoesNotRebuildPerKeystroke`). **Remainder — WON'T-DO:** the one-time rebuild on
  a version change (finder-open after an index update) still runs the per-file case-fold +
  `CachedFileEntry` build on the UI thread. Re-examined in session 3: `FileIndex::ApplyBatch` (where
  `ToProjectFile` runs) fires on the **watcher's background thread** (per the FileIndexWatcher threading
  contract), so precomputing `lower_path`/`lower_filename` on `ProjectFile` during the scan *would*
  genuinely move the fold off the UI thread — the earlier "needs async wiring" note was wrong about that.
  But it is declined anyway: it adds three derived strings to every `ProjectFile` permanently, inflating
  the hot `files_` vector and every `SnapshotWithVersion()` deep copy (≈2–3× the per-entry string bytes,
  tens of MB on a 100k-file repo), and it also widens the defaulted `ProjectFile::operator==` used by the
  upsert dedup. That permanent memory cost on the core index structure buys only an **infrequent one-time**
  win (the per-keystroke path — the part that actually hurt — is already off the hot loop), so the trade
  is net-negative. The fully-async alternative (finder-owned `ProjectBackgroundExecutor` + wake→re-refresh
  hook, like `HighlightPrefetchService`) remains disproportionate for a once-per-index-change cost and is
  also declined. Closed.
- **[WON'T-DO platform-only] File finder recent-path matching uses raw `path.string()` identity.** Only
  bites case-insensitive folding hosts (Windows/macOS) or Unicode-normalizing filesystems; on
  case-sensitive Linux the raw string key is exact. Host-normalized recent keys are the recorded fix,
  not validatable here.
- **[WON'T-DO — power-user feature, capped] Command palette path completion can enumerate absolute
  filesystem roots.** Absolute-path completion (`open /usr/...`) is a deliberate power-user convenience;
  candidate collection is already capped at 2000 (2026-07-13). Gating it per-command is a policy
  refinement, and the user can already read any path they type — no privilege boundary is crossed.
##### Plugin runtime and extension boundaries

- **[WON'T-DO — plugin trust; open is not a mutation] `workspace.open_file` lets plugins request
  arbitrary absolute paths.** Opening a file into a read-only tab is not a capability trusted plugin
  code lacks (it can already `files.read_text` within its grants, and the user runs the plugin); unlike
  write, it mutates nothing outside the project. `files.read_text/write_text` gate because they move
  bytes; display does not.
- **[WON'T-DO — clamped consistently downstream] Plugin `workspace.open_file` line/column arguments are
  only checked as Lua integers.** The open path clamps the coordinates to the document on load (the item
  notes "leave each editor/open path to clamp consistently"), so an enormous `math.maxinteger` lands at
  end-of-document, not a wild position. A shared pre-clamp is redundant.
- **[WON'T-DO for this sweep — plugin-worker-only, timeout-bounded, needs worker pool]
  Plugin `process.run_async` blocks the plugin worker thread.** Real API/behavior mismatch, but it
  stalls only *later plugin tasks* (not the UI thread) and is bounded by the 120 s subprocess timeout.
  A correct async fix needs a bounded process worker pool + callback marshaling back into the runtime —
  a real subsystem addition deferred as its own focused change; the fix direction stands.
- **[WON'T-DO — intentional for tool wrappers; plugin trust] Plugin process allowlists match by
  basename.** Accepting `./eslint`/`<project>/tools/eslint` for an allowed `eslint` is deliberate so a
  project's vendored tool wrapper works; plugins are trusted code and the allowlist is authored by the
  same trusted manifest. Exact-vs-basename manifest entries are a schema refinement.
- **[WON'T-DO — plugin trust; display anchor] Plugin surface anchors are lexical, not
  containment-checked.** A surface anchor is display/navigation metadata authored by trusted plugin
  code; an external anchor navigates to a file the user could open anyway. No mutation crosses the
  project boundary.
- **[WON'T-DO — plugins already run commands] Plugin surface hit-region commands are raw command
  strings.** Trusted plugin code can register and invoke commands directly; a hit region carrying a
  command string is the same capability by another route. Structured command-id regions are an API
  nicety, not a new trust boundary.
- **[WON'T-DO — plugin trust; external SCM is display, mutations no-op] Plugin SCM snapshots can report
  paths outside the project.** A plugin SCM provider is trusted code populating its own view; an
  external entry is display data and host stage/discard operate on project git, so they simply do not
  act on an out-of-project entry.
- **[WON'T-DO — plugin trust; UI clamps positions] Plugin test discovery can report external files and
  negative lines.** Test items are trusted-plugin-provided; an external test file navigates to a file
  the user could open, and the UI clamps a non-positive line to a valid position. A containment/positive-
  line guard is defense-in-depth over trusted input.

##### Terminal backend, emulation, and terminal UI

- **[WON'T-DO for this sweep — uncommon, paste-capped, needs nonblocking redesign] POSIX terminal
  writes are blocking and have no deadline.** A blocking PTY write only stalls if the child stops
  reading its stdin mid-write, which is uncommon, and paste is already capped at 64 MiB. A nonblocking
  master fd + bounded write queue is a real backend redesign deferred as its own change; the fix
  direction stands.
- **[WON'T-DO — escalation-message nicety] Terminal `Stop()` can wait on a child shutdown path without
  user-visible escalation.** `ShellProcess` already bounds the child-shutdown wait; a "terminating…"
  toast after a threshold is UX polish, not a hang fix.
- **[WON'T-DO — display-only, dup] OSC 7 working directories are stored without containment.** Same as
  the earlier OSC 7 item: the reported cwd is display-only and is not consumed for any filesystem
  operation, so there is nothing to contain until a future consumer uses it.
- **[WON'T-DO — grid copy is standard; stream-copy is a feature] Terminal copy helpers treat empty
  cells as spaces.** Rectangular grid copy (spaces for empty cells) is standard terminal-copy behavior;
  a wrap-metadata "stream copy" mode is an additional feature, not a defect.

##### App startup, control specs, debug, and commit workflow

- **[WON'T-DO — 1 MiB file cap bounds it; user-owned] Control spec arrays have no item-count caps.**
  The control spec is a user-authored file already capped at 1 MiB, which bounds the generated command
  count; per-array caps are defense-in-depth against the user's own input.
- **[WON'T-DO — intentional escape hatch] Control spec `commands` bypass structured validation.** Raw
  command strings in a control spec are the deliberate power-user/LLM escape hatch (the same command
  line the user could type); ordering is the caller's responsibility. Marking them "unsafe" in docs is
  documentation, not a code fix.
- **[WON'T-DO — external debug targets are expected] Control spec breakpoint paths are resolved without
  containment.** Like DAP source paths, debugging legitimately targets files outside the project root
  (system libs, generated sources); a control spec authored by the user seeding an out-of-project
  breakpoint is intended, not an escalation.
- **[WON'T-DO — uncommon multi-adapter switch; user-toggleable] Debug exception-filter seeding is per
  model, not per adapter identity.** Switching debug adapters within one project mid-session is
  uncommon, and exception filters are user-toggleable, so stale defaults are a one-click fix. Keying
  seeded defaults by adapter id is a refinement.
- **[WON'T-DO — same adapter-switch edge] Exception filter conditions can outlive the advertised filter
  set.** Same low-frequency adapter-switch scenario; a leftover condition on a reused filter id only
  applies if a new adapter re-advertises that exact id, and the user can clear it. Keying by adapter id
  is the recorded refinement.
- **[WON'T-DO — high capture cap; huge-diff edge] Conflict-marker precheck can miss markers beyond git
  output capture limits.** A staged diff large enough to exceed the git output capture cap is itself
  enormous; a conflict marker past that cap is an extreme edge. Streaming the scan / a `truncated` flag
  is hardening for a pathological diff.

#### 2026-07-13 deep subsystem audit backlog — fifth tranche

##### Editor text core, folding, and visual navigation

- **[WON'T-DO — latent representation difference, each path handles its own] The editor has two
  incompatible empty-buffer representations.** `PieceTree::Reset({})` (zero-line) and `ResetFromText("")`
  / merge's `[""]` (one empty line) each serialize to zero bytes and render/save consistently within
  their own paths; no concrete cross-path failure is demonstrated. Unifying to one canonical empty
  representation is a refactor without a reproduced bug (the merge empty-line item was already verified
  a non-defect: `[""]` and `[]` both serialize empty).
- **[WON'T-DO — unreachable via upstream caps] `PieceTree::AppendToAdd` trusts inserted span length.**
  A single inserted span exceeding `uint32_t` (4 GiB) cannot arise: file load caps at 512 MiB, paste at
  64 MiB, and `apply_edits` caps count/size. The core is protected by every upstream path; a per-span
  guard defends an unconstructible input.
- **[WON'T-DO — bounded by document caps] `ReplaceLineRange` materializes whole replacement text before
  checking limits.** The inserted-line vector originates from edits already bounded by the document/edit
  caps, so the transient join is bounded; a pre-materialization size check is defense-in-depth over an
  already-capped source.
- **[RESOLVED 2026-07-15] Bracket matching allocates a full line-count scratch vector for a bounded
  scan.** `FindBracketMatch` now materializes only the `[caret-max, caret+max]` window and indexes it
  through a `WindowLines` accessor that maps absolute line numbers onto the slice via a `base` offset;
  the O(file) `views.assign(line_count, {})` is gone. `FindBracketMatchInLines` keeps its absolute
  (base-0) contract for the existing tests. See "Fixed in the 2026-07-15 cross-subsystem speed pass".
- **[WON'T-DO — cached + bounded window] Bracket matching can synchronously tokenize lines far outside
  the visible syntax cache.** The bracket-match result is cached (recomputed only on caret movement) and
  the scan is now bounded to the `[caret±max]` window (2026-07-15), so forced tokenization is bounded
  and infrequent. Switching to the non-forcing accessor would trade correct string/comment suppression
  for speed on cold lines — a worse tradeoff than the bounded force.
- **[WON'T-DO — folding heuristic] Fold dedupe keeps only one fold range per opener line.** One fold
  target per physical opener line is an accepted folding heuristic; multiple foldable constructs on one
  line (`if(){}else{}`) losing the secondary target is a fidelity edge, not a correctness bug.
- **[WON'T-DO — folding heuristic] Fold collapse remap keys on exact opener/closer pairs.** Losing a
  collapsed-fold's collapsed state after a structural edit that reclassifies its boundary is acceptable
  degradation (the fold re-expands, nothing is lost); fuzzy opener-identity persistence is fidelity
  polish.
- **[WON'T-DO — pathological input] Indent guide generation can create unbounded per-row guide runs.**
  A single line with hundreds of thousands of leading spaces is degenerate input (and beyond
  `kMaxHighlightLineBytes`-class limits); capping guides by visible columns is defense against a
  non-real line.
- **[WON'T-DO — part of the deferred Unicode layout pass] Single-line visual helpers operate on byte
  columns for bracket suppression and guide anchoring.** This is the same Unicode cell-mapping cluster
  as the wcwidth item: correct only with a grapheme/cell-width layer and visual verification. Deferred
  to the focused Unicode pass; ASCII (the common case) is exact.

##### Runtime syntax and highlighting

- **[WON'T-DO — low marginal value; already load-bounded] Runtime syntax regex source length is
  uncapped before PCRE compilation.** Syntax definitions are already length/count-bounded at load (256
  defs/file, 4096 rules/array, per the 2026-07-13 deadline+array-clamp work), so a per-pattern byte cap
  before PCRE compile was assessed low marginal value — part of the deferred RuntimeSyntax perf-budget
  cluster that stays deferred (see the "Still open" note above).
- **[RESOLVED 2026-07-13 (per-rule cap) + line-byte cap] Runtime syntax matching has no per-line match
  count budget.** `FindAllRegex` now caps matches per rule per line at 8192 (2026-07-13), and lines are
  capped at `kMaxHighlightLineBytes` (100 KiB) before tokenizing at all, so a single-byte-matching rule
  can no longer push ~100k matches across the highlight hot path.
- **[WON'T-DO — bounded by line-byte cap + region depth] Syntax region detection runs every sibling
  start regex at each cursor position.** The per-line work is bounded by `kMaxHighlightLineBytes` and
  the region depth cap; merging sibling start regexes is a micro-optimization in the deferred
  RuntimeSyntax perf-budget cluster, not a live stall on real code lines.
- **[RESOLVED 2026-07-15] Definition source fingerprinting reads all syntax files every reload check.**
  Replaced `DefinitionSourceFingerprint` with the `SyntaxSourceFingerprint` cache object
  (`path → {mtime,size,content_hash}`): unchanged files reuse the cached hash instead of being
  re-read, so a poll that finds nothing changed reads no source bytes, while the fingerprint stays a
  pure function of paths+contents (byte-for-byte-equivalent change detection). See "Fixed in the
  2026-07-15 syntax-reload speed pass".
- **[RESOLVED 2026-07-15] Duplicate syntax directories load the same definition file multiple times.**
  `DiscoverDefinitionFiles` now dedups both the directory list and normalized file keys (first
  directory wins → deterministic precedence). See "Fixed in the 2026-07-15 syntax-reload speed pass".
- **[WON'T-DO — plugin trust; intentional override precedence] Plugin syntax definitions can shadow
  built-in filetypes without an explicit override contract.** Runtime (plugin) definitions taking
  precedence over built-ins is the intended extension mechanism (a plugin adds/overrides a language);
  plugins are user-installed trusted code, and a too-broad filename regex is a bug in that plugin. An
  `overrides = true` opt-in is an API nicety.
- **[RESOLVED 2026-07-15] Lazy built-in regex compilation can still happen on a visible-line cache
  miss.** Cold-filetype compile is now prewarmed off the UI thread on tab switch via
  `runtime_syntax::CompileDefinition` + `HighlightPrefetchService::PrewarmForViewport` (gated on the
  viewport identity inside the service, so detection runs once per switch). Behavior unchanged
  (idempotent `std::call_once`); only compile timing moves off the render path. See "Fixed in the
  2026-07-15 syntax-reload speed pass".
- **[WON'T-DO — revision checks preserve correctness] Highlight prefetch requests dedupe by viewport
  pointer rather than document identity.** The install path drops stale results by document revision +
  live-viewport identity, so pointer reuse only ever wastes a little worker time, never installs wrong
  tokens. Keying by stable document id is a fragility/efficiency refinement, not a correctness fix.
- **[RESOLVED 2026-07-15] Highlight prefetch callbacks capture `this` without an explicit lifetime
  token.** Added `~HighlightPrefetchService() { Shutdown(); }`: the destructor drains + joins the
  worker before the result queues / wake callback are destroyed (members outlive the join since the
  destructor body runs first), so a queued job reaching into `this` can never outlive them even if an
  owner forgets `Shutdown()`. `Shutdown()` is idempotent. Regression:
  `SyntaxDefinitionLoader/HighlightPrefetchServiceDestructorDrainsWithoutShutdown`.
- **[WON'T-DO — transient, self-correcting] Deep-jump approximate tokens can be visible longer than the
  checkpoint backfill cadence.** Approximate highlight state for distant lines after a deep jump is a
  brief, self-correcting visual (the checkpoint backfill converges to exact colors); it's a transient
  rendering approximation, not a correctness defect.

##### Compare and merge models

- **[WON'T-DO — by design, pinned test] Large hunk alignment fallback pairs unrelated lines by
  position.** Already recorded as a verified non-defect in the won't-do section: the oversized-hunk
  positional pairing keeps the readable side-by-side Modified row for the common systematic-rename case
  and is pinned by `TestCompareLargeInputsUseBoundedFallback` (modified == 1500); a similarity gate was
  evaluated and rejected as a UX regression.
- **[WON'T-DO — INT_MAX-line diff is unreachable] Compare model row and line fields are `int`.** A diff
  with >2 billion rows/lines cannot be constructed (the compare read caps and file-size limits bound it
  far below INT_MAX); widening to `size_t` internally guards an unbuildable input.
- **[WON'T-DO — display-truncation nicety] Intraline diff silently degrades to whole-line spans without
  exposing why.** The degrade (whole-line span on an oversized/DP-budget line) is correct and bounded
  (the per-hunk intra-line budget landed 2026-07-14); a per-row `intraline_truncated` tooltip is the
  same display-visibility nicety triaged across this pass.
- **[WON'T-DO — raw concatenation is the documented "Both" semantics] `Both` merge choices concatenate
  sides without provenance or separator rows.** "Take both" meaning "incoming lines then current lines"
  (or vice-versa) is the expected, predictable result; inserting conflict/blank separators would
  corrupt the merged text with markers the user must then delete. Raw concatenation is the correct
  contract.
- **[WON'T-DO — grouping heuristic] Merge grouping treats touching delete/insert ranges differently
  from equal-column insertions.** Whether a delete ending exactly where another side inserts joins into
  one hunk is a presentation-grouping heuristic; both hunks are individually resolvable and the merged
  result is correct either way.

##### Persistence and cross-platform state

- **[WON'T-DO — cross-platform state restore is not a supported flow] Persisted paths record a platform
  tag but do not use it for host-specific decoding.** Moving a session/state file between a Windows and
  a POSIX host is not a supported workflow (state files are host-local); within one host the native path
  round-trips correctly. Per-field cross-platform path decoding is a feature for a flow microide does not
  offer.
- **[WON'T-DO — corrupt-record edge; render tolerates invalid UTF-8] Persisted strings are
  length-checked but not UTF-8-validated.** A corrupt persisted state file producing invalid byte
  sequences is an edge (the user's own CRC-checked state), and the glyph/render path tolerates invalid
  UTF-8 (falls back per-byte); string emission already folds invalid scalars to U+FFFD (2026-07-13
  `AppendUtf8`). Validating on decode is defense-in-depth over a CRC-guarded file.
- **[WON'T-DO — no future required flags; forward-compat feature] Persisted capability flags are parsed
  but not enforced.** There are no required-capability bits defined today, so nothing is silently
  ignored; enforcing a supported mask is forward-compatibility plumbing for a format extension that does
  not yet exist.
- **[WON'T-DO — distinct file paths; substitution is user error] Persisted record CRC is unkeyed.**
  Reading project-config bytes as session bytes requires someone to manually copy one state file over
  another (distinct, named paths); a record-kind body tag defends against a manual file swap, not a
  reachable program path.
- **[WON'T-DO — race on user's own state file; prefix parse bounded] `ReadAllBytes` trusts `tellg` size
  until the final read check.** A file growing between `tellg` and `read` is a race on the user's own
  CRC-guarded state file; the prefix parse is bounded by the read cap and a valid-prefix record is a
  valid record. Re-stat-after-read guards a self-inflicted concurrent write.
- **[WON'T-DO — harmful overwrite already guarded] Backup fallback can mask repeated primary corruption
  indefinitely.** The material harm (overwriting a recoverable primary with stale backup) is prevented
  by the 2026-07-14 corrupt-primary guard; a repeated-corruption quarantine/repair policy + recovery
  banner is the recovery-UX nicety triaged earlier.

##### Rendering, plugin display lists, and image assets

- **[WON'T-DO — clamped safely at replay] Display-list validation accepts huge finite rectangles that
  are later clamped silently.** Replay clamps to ±1,000,000 before the int cast, so a 1e20 coordinate
  cannot cause UB — it just clips; validating against content bounds is a surprise-reduction nicety over
  already-safe behavior.
- **[RESOLVED 2026-07-15] Display-list content dimensions are not finiteness-checked.**
  `ValidateDisplayList` now rejects a NaN/±inf or negative `content_width`/`content_height` (which feed
  the host's scroll extents / intrinsic layout size), alongside the existing op-rect/point finiteness
  checks. Regression: `PluginDisplayList/NonFiniteContentDimensionRejected`.
- **[WON'T-DO — correct for immutable bytes; plugin bug] Texture-cache decode failures are permanent by
  content hash.** Caching a decode failure by content hash is correct (immutable bytes never decode
  differently); a plugin reusing a hash for different bytes is a plugin bug, and the transient-failure
  retry was already fixed 2026-07-13. Byte-size/format in the key is defense over plugin misuse.
- **[WON'T-DO — micro-optimization; bounded images] Raw RGBA plugin images are copied twice before
  upload.** Plugin raster images are bounded by `kMaxDimension`, so the extra copy is bounded memory
  bandwidth on an infrequent path; moving the buffer directly is a micro-optimization, not a defect.
- **[WON'T-DO — kMaxDimension keeps pitch in range] Raster decode dimensions can overflow SDL pitch
  assumptions if caps change.** `kMaxDimension` keeps `width * 4` well within `int`; a checked-`size_t`
  pitch is an assertion to add *if* the cap ever grows, not a live overflow.

##### Platform filesystem, trash, and subprocess helpers

- **[WON'T-DO — crash-window durability tradeoff] Linux trashing writes metadata before the file move
  and never fsyncs either directory.** An orphan `.trashinfo` after a crash mid-trash is cosmetic (the
  freedesktop trash tolerates stale info), and directory-entry fsync is the same speed/durability
  tradeoff accepted for the persistence rename; only a crash inside the flush window is affected.
- **[WON'T-DO — uncommon cross-device; MovePath handles EXDEV] Linux trash move can cross filesystems.**
  `XDG_DATA_HOME` on a different filesystem from the deleted file is uncommon; `MovePath` already
  falls back to copy+delete on `EXDEV`, so the operation still succeeds (just slower). Topdir-trash
  routing is an optimization for that uncommon layout.
- **[WON'T-DO platform-only] macOS trash name reservation is not atomic.** macOS-only
  `UniquePathInDirectory` exists-then-move race; needs a platform trash API / atomic reservation,
  not validatable here.
- **[WON'T-DO platform-only] Windows recycle-bin deletion returns the original source path.** Windows-
  only result-semantics issue (`MovePathToTrashWindows` can't report the recycle-bin item path);
  returning logical `nullopt` there is Windows work not validatable here.
- **[WON'T-DO — callers treat inaccessible as skip anyway] `ReadPathType` collapses stat errors into
  `Missing`.** Directory discovery and path validation skip both a missing and an inaccessible path
  identically (they can't index/open it either way), so an `Inaccessible` distinction only feeds a
  user-facing "why" message — a diagnostics nicety, not a behavior change.
- **[WON'T-DO — partial-list status nicety] `ListDirectory` drops iteration errors and partial-read
  status.** A directory becoming unreadable mid-iteration is a rare race; returning entries-plus-status
  so callers can show a "partial" marker is the same display-visibility nicety triaged throughout.
- **[WON'T-DO — budget-accounting edge] `CaptureTreeSnapshot` does not count root files against
  `max_entries`.** Undercounting the budget by the number of top-level roots is a minor accounting edge;
  a caller passing many file roots with a tiny `max_entries` is atypical, and the budget is a soft
  bound.
- **[WON'T-DO — contents-only semantics by design] `CaptureTreeSnapshot` skips root directory entries
  themselves.** "Contents, not the root node" is a consistent, intended snapshot semantics; watchers
  detect root replacement by other means. Appending roots is a semantics change, not a bug fix.
- **[WON'T-DO — extreme multi-root imbalance] `CaptureTreeSnapshot` stops entirely after the first
  over-budget root.** Hitting the budget requires a root large enough to be pathological; per-root
  budgets / round-robin is fairness tuning for an extreme multi-root workspace.
- **[WON'T-DO — call-site discipline; no default policy needed] The project subprocess helper is a
  transparent alias.** `project::RunSubprocess` forwarding to `platform::RunSubprocess` is fine because
  the invariant "no `platform::RunSubprocess` in workspace `.cpp`; dispatch through
  `ProjectBackgroundExecutor`" is already lint-enforced, and callers pass explicit timeout/output
  options where they matter. Baking defaults into the alias is ergonomics, not a correctness fix.

#### 2026-07-13 deep subsystem audit backlog — sixth tranche

##### Workspace prompts, command surfaces, and sidebar orchestration

- **[WON'T-DO for this sweep — modal prompt guards it; stable-id plumbing is a focused change] Dirty
  prompts store tab indices instead of stable tab identities.** The dirty prompt is a modal confirmation
  that blocks further tab manipulation while visible, so a reorder/close between show and confirm
  requires an async mutation (file-watcher-driven close, plugin) racing the modal — an edge. A correct
  fix threads stable tab ids/`{path,revision,group}` tokens through `PromptSurfaceService` and every
  confirm path; deferred as its own focused change. Fix direction stands.
- **[WON'T-DO — same modal-prompt guard] Dirty prompt creation does not validate every target index.**
  The indices come from the shell's own current tab set at show time under the same modal guard; a
  stale out-of-range caller is not a reachable production path. Pairs with the item above.
- **[WON'T-DO — minor focus glitch] Prompt focus restoration is blind to surface lifetime changes.**
  Restoring focus to a surface that disappeared while the overlay was open yields a one-frame focus
  glitch (focus falls back on the next input), not data loss; a `RestoreIfValid` helper is UX polish.
- **[WON'T-DO — each action validates its target] Generic prompt path payloads are only
  lexical-normalized.** The destructive prompt actions (delete/rename/authorize) each validate
  containment/existence at execution time (e.g. `MovePathNoOverwrite`, containment checks), so the
  lexical prompt payload is not the trust boundary; a typed `PromptPathTarget` is a refactor.
- **[WON'T-DO — user confirms the displayed URL] External URL prompts have no scheme or length gate.**
  The URL is shown to the user in a confirmation prompt before anything opens, so the user is the gate;
  scheme/length validation is defense-in-depth over an explicit user confirmation.
- **[WON'T-DO — file-browser feature] Sidebar tree requests can target arbitrary absolute roots.** An
  absolute tree root (`tree /`) is a deliberate file-browser convenience the user types; it reads paths
  the user could already open. Project-containment-by-default is a policy refinement, not a bug.
- **[WON'T-DO — plugin authoring error; built-in precedence is safe] Plugin sidebar IDs can collide
  with built-in sidebar IDs.** A plugin registering `tree`/`git` is a plugin bug; the built-in wins
  (deterministic, safe) and the plugin's sidebar is simply unreachable under that id. Rejecting the
  collision at registration is a plugin-dev-ergonomics nicety over trusted-but-buggy plugin code.

##### Status bar, settings overlay, and notifications

- **[RESOLVED 2026-07-15] Repository availability status can stay stale after `git init` or `.git`
  removal.** Removed the `project_root`-keyed cache entirely — `is_git_repo_valid` is one cheap `.git`
  stat, so it now runs directly per refresh (only when no git snapshot exists). Regression:
  `WorkspaceStatusBar/RepoAvailabilityReflectsInSessionGitInit`.
- **[RESOLVED 2026-07-14] LSP status tone is derived by substring search for `Ready`.** `LspService`
  now returns a typed `LspStatusSeverity` (Idle/Busy/Error) threaded through `ActiveLspStatusStrings`
  and the status-bar operation as a `StatusBarSegmentTone`, so a `Not Ready` label / a server name
  containing "Ready" no longer mis-tones. Regression:
  `WorkspaceStatusBar/LspToneFromTypedSeverityNotLabelText`.
- **[RESOLVED 2026-07-15] Plugin status items with equal alignment and priority have unstable order.**
  `ResolveStatusItems` now uses `std::stable_sort`, so equal alignment+priority items keep their
  contribution order (deterministic, no cross-revision/platform jitter). Regression:
  `StatusRegistry/EqualPriorityKeepsRegistrationOrder`.
- **[WON'T-DO — minor keyboard-nav glitch on a static screen] Settings overlay pane cycling assumes
  every mode has three panes.** Cycling into a non-existent pane in Help/About (a mostly static screen)
  parks focus harmlessly until the next input; deriving pane count from mode is UX polish, not a data
  or correctness issue.
- **[WON'T-DO — uncommon reload-while-editing edge] Settings value edit target can go stale across
  settings rebuilds.** A settings row vanishing (plugin reload / registry rebuild) at the exact moment
  the user is editing its value is an edge; the commit resolves by `editing_row_id_` and a missing row
  is a no-op (no wrong setting is written). Cancelling the edit on rebuild is a small robustness
  refinement.

##### Plugin registries, tools, tasks, and AI/auth contributions

- **[WON'T-DO — protective cap; plugin trust] The per-kind plugin contribution cap is too high for
  UI-backed registries.** The 100k cap bounds infinity; a trusted plugin registering tens of thousands
  of visible commands/settings is a plugin bug, not a hostile input. Per-kind product-sized caps are
  tuning, not a defect.
- **[WON'T-DO — plugin bug; server still launches] Malformed LSP `initialization_options`/`settings`
  JSON is silently ignored.** Leaving the field null and launching the server (which then uses its
  defaults) is a graceful degradation of a plugin-authoring error; rejecting present-but-invalid JSON is
  a dev-ergonomics improvement over trusted plugin code.
- **[WON'T-DO — no ad-hoc runner exists yet] Plugin task registrations lack a runtime/execution
  contract.** Tasks are currently static contributions with no execution path that could grow unsafe;
  defining the cwd/env/result contract belongs with the task-runner feature when it ships, not as a
  pre-emptive change.
- **[WON'T-DO — plugin authoring typo] Plugin tool platform strings are free-form.** A mistyped
  `platform` makes the plugin's own tool undiscoverable on that host — a plugin bug the author sees when
  their tool doesn't appear; an enum validation is authoring polish.
- **[WON'T-DO — callers dispatch off-thread] Tool downloader blocks the caller while hashing.** Tool
  download/verify is already dispatched off the UI thread by its callers (`ProjectBackgroundExecutor`);
  the synchronous `future.get()` runs on that worker, not the shell thread.
- **[RESOLVED 2026-07-14 — no networking by design] Tool downloader only implements local/file
  sources.** `ResolveToolSourcePath` now hard-rejects every remote scheme (`http`/`https`/… anything
  `://` that is not `file://`); microide tools are local/file by design. Regression:
  `WorkspaceToolDownloader/RejectsRemoteSchemesNoNetworking`.
- **[RESOLVED 2026-07-14] Tool cache APIs do not verify hash on `GetCachedTool`.** `GetCachedTool` now
  takes an optional `expected_sha256` and verifies the cached file before returning it. Regression:
  `WorkspaceToolDownloader/GetCachedToolVerifiesHash`.
- **[WON'T-DO — protective cap; real lists are tiny] AI provider model lists and external-agent
  capabilities can still be enormous.** The 100k cap protects against infinity; real model/capability
  lists are a handful, so product-sized caps are tuning under the plugin trust model.
- **[WON'T-DO — metadata-only provider is harmless] Auth provider registration does not require a
  lifecycle function.** A label-only auth provider simply does nothing until it gains a `login`
  callback; requiring one is an authoring validation, not a defect (it cannot misbehave).

##### Debug pane, watch expressions, and value trees

- **[WON'T-DO — duplicates are allowed; capped] Debug watch expressions are not deduplicated or
  normalized.** Duplicate watch expressions are a legitimate user choice (watch the same expr in two
  scopes), and the watch list is already capped (512, 2026-07-13); deduping would remove a valid use.
- **[WON'T-DO — edit-during-inflight edge] Watch evaluation results are applied by positional index
  with no generation.** Editing/removing a watch expression in the sub-second window between its
  evaluate request and response is an edge; the worst case is a transient wrong value on one watch row
  that the next stop corrects. A per-request expression-id/generation guard is a refinement.
- **[WON'T-DO — bounded by expand state; hot path is flat] Debug value rows are rebuilt recursively
  without a visible row cap.** `FlattenInto` only emits *expanded* nodes, and the recursion depth is
  capped at 256 (2026-07-13); the user controls expansion, so a thousands-of-rows flatten requires the
  user to expand thousands of nodes. Row virtualization is a perf refinement, not a defect.
- **[WON'T-DO — TRIED, REVERTED, documented] Debug value node ids can wrap.** Widening the 32-bit
  `next_id_` to 64-bit was tried and reverted (2026-07-14: measurable regression on the value-tree
  rebuild/expand hot path); the 32-bit wrap needs ~4 billion node allocations in one session
  (unreachable). Accepted won't-do — see "Deliberate tradeoffs" below.
- **[WON'T-DO — presentation nicety] Debug breakpoint rows collapse multiple metadata fields into one
  secondary string.** Condensing condition/hit-count/log/verification into one secondary line is a
  compact presentation choice; separating them in the view model is UI polish, not a correctness issue
  (the underlying breakpoint state is intact).

##### Tests, icons, decorations, and terminal remaining edges

- **[WON'T-DO — bounded by test count; display cluster] Test results are stored without a cap or
  generation.** Results grow with the number of tests run, which is bounded by the discovered test set;
  stale results from a prior discovery are superseded on re-run. Per-test history caps + generation tags
  are the display/memory-tidiness cluster.
- **[WON'T-DO — Clear()+rediscover covers reload] Test items are not owned by provider id.** A plugin
  reload clears and rediscovers the test set, so stale items don't persist; per-provider ownership only
  matters for the rare "one of several providers fails rediscovery" case, a refinement.
- **[WON'T-DO — plugin authoring conflict] File icon theme rules are last-writer-wins.** Two plugins
  claiming `.rs` is an authoring conflict resolved deterministically by registration order; explicit
  priority / duplicate diagnostics is authoring polish under the plugin trust model.
- **[WON'T-DO — high cap; real files don't hit it] Plugin decoration aggregate truncation keeps
  lowest-line entries.** Dropping late high-priority marks only occurs past `kMaxMergedPerKind` (a high
  cap); a real file with that many merged decorations is pathological. Per-window/priority-bucket
  truncation is a refinement.
- **[WON'T-DO — rename-collision edge] Decoration retarget can overwrite an existing destination
  decoration for the same owner.** Renaming `a`→`b` when the same owner already decorated `b` is an
  edge; deterministic replace is an acceptable resolution (the decorations re-publish on the next
  provider pass).
- **[WON'T-DO — observability nicety, dup] Terminal pending reply buffer drops query responses after
  64 KiB.** Same as the earlier terminal-reply item: the cap prevents a UI-freeze flood; a
  dropped-reply counter is diagnostics polish, and 64 KiB of pending query replies is pathological.
- **[WON'T-DO — stale reply is harmless post-stop] Terminal CPR/DECRQM replies can be generated while
  the backend is no longer running.** A query reply buffered just before terminal stop is written to a
  backend that's gone (no-op) or a restarted one that ignores an unsolicited reply; tagging replies with
  a backend generation is tidiness over a harmless race.

### Deep audit tranche 7 — LSP/DAP/session/git/search/workflow bugs (2026-07-13)

This tranche continues the cross-subsystem bug hunt without fixing code. It focuses on ownership,
generation, and semantic-validation bugs in the LSP/DAP registry paths, debug UI callbacks,
persistence decode, git refresh/patch/commit workflows, project search, recents, and file operations.

#### Scope audited in this tranche

- `src/workspace/WorkspaceLspClient*.cpp`, `WorkspaceLspManager.cpp`, and related LSP lifecycle
  request/dispatch paths.
- `src/workspace/WorkspaceDapClient*.cpp`, `WorkspaceDapManager.cpp`, `DebugSession*.cpp`,
  `DebugService*.cpp`, and debug persistence records.
- `src/workspace/PersistenceService.cpp`,
  `WorkspacePersistenceBinaryFormat{,Sessions,Debug}.cpp`, and workspace-session restore/save.
- `src/project/GitRepository*.cpp`, `GitStatusService.cpp`, `GitCompareService.cpp`,
  `GitCommitExecutor.cpp`, `GitPatchApply.cpp`, `ProjectSearchService.cpp`,
  `FileOperationService.cpp`, and workspace services that orchestrate them.

#### LSP registry, document lifecycle, and server errors

- **[WON'T-DO — multi-plugin same-language conflict; retires on drain] LSP overlapping registrations
  can create unreachable duplicate server entries.** Two plugins registering the same language set in
  different orders is an authoring conflict; the older entry retires on the next drain (its client is
  shut down), so it is a transient resource, not a persistent wrong-server. The 2026-07-13
  alias-clearing fix already prevents a stale alias resolving to the wrong server.
- **[WON'T-DO — uncommon mid-session sandbox tightening] LSP sandbox changes are ignored by the
  registration equality check.** A settings/plugin reload that only tightens the subprocess sandbox
  while everything else matches is uncommon; the tighter sandbox applies on the next fresh start, and
  the user can restart the server. A sandbox fingerprint in the equality check is a refinement.
- **[WON'T-DO — bounded transient resource] Retired LSP clients can accumulate until a drain path
  runs.** Retired clients are collected on the shutdown/callback drain that runs regularly; between
  reloads they are a small bounded set (one per re-registration), not an unbounded leak.
- **[WON'T-DO — close-during-shutdown edge] `DidClose` drops local document-version state before the
  close notification is known to be queued.** A `didClose` rejected because the client is
  shutting down is an edge where the server is going away anyway; the lost version gate only matters for
  diagnostics on an already-closing server. A `close_pending` state is a refinement.
- **[WON'T-DO — start/exit errors already shown] Live LSP protocol errors are not surfaced through
  `LastServerError`.** The status surface shows the material states (starting/failed/exited); a running
  client's late transport/protocol error is diagnostics detail. Merging the client's `LastError()` is a
  reporting-precedence refinement.
- **[RESOLVED 2026-07-15] Serializing full JSON settings on every LSP registration is avoidable
  reload-path work.** `RegisterServer` now compares `initialization_options`/`settings` via a defaulted
  structural `JsonValue::operator==` (no allocation) instead of four `SerializeJson` calls — which also
  fixes a latent object-key-order sensitivity. See "Fixed in the 2026-07-15 cross-subsystem speed pass".

#### DAP manager/session/debug callback edges

- **[WON'T-DO — bounded joins; wedged-adapter edge; async teardown deferred] Stopping all debug
  sessions blocks the shell on adapter shutdown joins.** Same disposition as the RequestStop-timeout
  item: `~DapClient` joins its worker threads with bounds, and a wedged adapter is uncommon; converting
  stop-all to a bounded-slice async drain is a real teardown redesign deferred as its own change.
- **[WON'T-DO — same bounded-teardown family] Restart fallback blocks the shell while replacing the
  active session.** The synchronous shutdown of the old session before launching the new one inherits
  the same bounded-join latency; a two-phase async restart is the deferred teardown redesign.
- **[WON'T-DO — same bounded-teardown family] Clearing the active DAP entry can synchronously destroy a
  live session.** Destructor-driven transport joins are bounded; separating "remove from list" from
  "destroy transport" is the deferred async-teardown work, not a live hang on a responsive adapter.
- **[WON'T-DO — unreachable wrap] DAP session ids are `int` and can wrap.** Reusing a session id needs
  ~2 billion sessions in one process lifetime (unreachable), the same disposition as the reverted 32-bit
  node-id wrap. Widening is not worth it.
- **[WON'T-DO — plugin-reload-drops-live-adapter edge] Adapter retention can remove a type while
  sessions using that type are still running.** Reloading a plugin that drops an adapter type while a
  session of that type is live is an edge; the session keeps running, and only restart/relabel of that
  specific session degrades. Pinning adapter metadata is a refinement.
- **[RESOLVED 2026-07-15] Function breakpoint verification is positional but not bounded to the
  requested count.** `on_function_breakpoints_verified` now ignores results beyond
  `requested_names.size()` (mirrors the tested line-breakpoint handler), so a non-conformant adapter
  returning extra function breakpoints can no longer seed phantom verification state.
- **[WON'T-DO — names are unique in practice; deterministic first-match] Function breakpoint by-name
  commands affect only the first matching name.** Function breakpoints are keyed by name and are
  effectively unique (two breakpoints named `main` is a user oddity); first-exact-match is deterministic.
  Addressing by stable id is a refinement over an atypical input.
- **[WON'T-DO — session-reuse-before-drain edge] REPL evaluation callbacks are not generation-gated.**
  Printing a stale evaluate result into a console tab requires the session to end and a new session to
  reclaim that console's UI state within the sub-second callback window — an edge; the worst case is one
  stale console line. A session-generation guard is a refinement.
- **[WON'T-DO — adapter semi-trusted; page bounded] REPL structured-result expansion can spam unbounded
  console text.** The variables page is bounded; a malicious adapter returning multi-MB child strings is
  the same semi-trusted-server case as the LSP per-field caps — the server is launched by the user's own
  config. Per-child truncation is defense-in-depth.
- **[WON'T-DO — late-hover-into-wrong-cache edge] Debug hover evaluation is not tied to frame
  generation.** A late hover response resolving into the cache after a frame switch requires the hover
  generation to coincide across the switch — a narrow race whose worst case is a briefly-stale hover
  popup. Adding a frame-generation check is a refinement.
- **[WON'T-DO — old value reappearing is implicit feedback] Variable and watch edit commits do not
  report adapter failures.** On a `setVariable` failure the old value reappears, which is itself the
  feedback that the edit did not take; a per-row failure status / console message is a UX refinement.
- **[WON'T-DO — transient loading glitch] Watch/variable child fetches mark loading errors without
  requesting redraw on the no-session path.** A row briefly showing "loading" after session teardown
  until the next repaint is a transient visual glitch (the panel repaints on the next event), not a
  stuck state.
- **[WON'T-DO — external source is expected while debugging] Debug stop projection can focus source
  paths outside the active project.** Same disposition as the DAP/control-spec source-path items:
  stopping in a system library / dependency and focusing that source is normal, expected debugging;
  containing it would break step-into-stdlib.

#### Persistence decode and session-state validation

- **[WON'T-DO — normalized safely at restore] Workspace-session active index is not semantically
  validated during decode.** A corrupt/out-of-range active index is remapped to a valid tab at restore
  (no crash, no wrong-state); reporting "bad active index" distinctly is a diagnostics nicety over
  already-safe normalization.
- **[WON'T-DO — callers use fixed app-state roots] Legacy persistence cleanup deletes sidecar files by
  the structured path's existence.** The cleanup callers pass fixed app-state/config paths, so it never
  points at a user directory of unrelated files; deleting an unrelated `project.state.legacy` requires a
  caller bug pointing at a wrong directory, not a reachable path.
- **[WON'T-DO — external debug targets are expected] Debug persisted file-breakpoint paths are not
  normalized or project-contained.** A persisted breakpoint on an external path (system lib, dependency)
  is intentional if the user set it — debugging outside the project root is expected (same disposition
  as the DAP/control-spec source-path items).
- **[WON'T-DO — UI clamps on read; safe fallback] Persisted selected launch-config index is not clamped
  to the launch-config count.** A forged high index selects no row (safe fallback); the read sites clamp.
  Normalizing once at decode is tidiness over already-safe behavior.
- **[WON'T-DO — validated at launch] Persisted launch configs accept empty type/request and arbitrary
  arguments JSON.** An invalid config fails to launch with an error at launch time (the error boundary),
  which is acceptable; pre-annotating invalid configs in the UI is a UX nicety.

#### Git repository refresh, compare, patch, and commit workflows

- **[WON'T-DO — monotonic generation + Reset guards it] Git refresh generation does not include the
  project root.** The generation counter is monotonic within the service and Reset restarts it on
  project switch, so a delayed pre-switch job's generation never matches a post-switch generation;
  carrying `(root, generation)` is defense-in-depth over an already-guarded contract.
- **[WON'T-DO — project switch cancels project work appropriately] `GitRepositoryService::Reset` cancels
  the shared project background executor.** Reset runs on project switch, where cancelling the outgoing
  project's queued git/commit/patch work is the intended behavior (that work targets the project being
  left). Per-service cancellable namespaces are a refinement.
- **[RESOLVED 2026-07-15] Git sidebar outgoing base resolution can run extra subprocesses inside every
  full refresh.** `BuildSidebarSnapshot` now routes through `ResolveOutgoingBaseCached`, which memoizes
  the resolution keyed by `(root, choice, head_oid, branch_name, upstream, repo_available)`. A status
  refresh with unchanged HEAD/branch/upstream serves the base from cache and spawns no git subprocess;
  any HEAD movement or branch/upstream/choice change re-resolves. See "Fixed in the 2026-07-15
  cross-subsystem speed pass". (The config `gh-merge-base` edge — config changed without HEAD moving —
  is accepted staleness until the next HEAD/branch change; folding the refs into one git command is a
  possible further optimization.)
- **[WON'T-DO — --verify guards; user-owned config] Base-reference config values are not constrained
  before `show-ref`.** All git calls are argument-vector based and ref resolution uses `--verify`, so a
  weird `branch.<name>.gh-merge-base` value fails to resolve rather than doing anything surprising; the
  config is the user's own. A `check-ref-format` pre-validation is label-cosmetic (dup of the earlier
  branch-base item).
- **[WON'T-DO — invalid ref → empty outgoing; distinction is a nicety] Specific outgoing base refs are
  accepted without existence validation.** A typo `custom_ref` yields an empty outgoing-files list
  (harmless); distinguishing "bad ref" from "no outgoing files" with an explicit error is a UX nicety.
- **[WON'T-DO — --end-of-options guards; exotic-filename edge] `GitRepository::FileExistsAtRevision`
  concatenates `revision:path`.** `--end-of-options` prevents option injection; an ambiguous result
  needs a colon-bearing revision or a git-syntax-significant path (exotic filenames), an edge over a
  read-only existence check.
- **[WON'T-DO — real apply is authoritative] Patch apply preflight and apply are not atomic.** The
  `git apply --check` is advisory; the real `git apply` fails cleanly if the tree changed between them,
  so a TOCTOU produces a safe failure, not a corrupt apply.
- **[WON'T-DO for this sweep — async-lifetime redesign family] Patch apply dispatch reads repository
  state from a background thread + completions are not marshalled through a mailbox.** Same disposition
  as the commit-workflow `&state` item below: correctly threading these off the executor needs a
  completion-mailbox + generation redesign (mirroring `CommitWorkflowService`), a real change deferred
  as its own focused work; `operation`/refresh generations guard logical correctness today.
- **[WON'T-DO — generations report stale after execution] Pending discard preview stores patch text
  without a freshness re-check at confirm.** The request carries generations and the background path
  reports a stale confirm; a pre-dispatch re-check + explicit prompt warning is a UX refinement over a
  correctly-detected stale apply.
- **[WON'T-DO — pre-existing threading design; documented] Commit workflow captures `CommitWorkflowState&`
  across a background operation (+ does not bump generation on close, + no HEAD-advance verify).** This
  is the recorded async-lifetime won't-do (see "Still open (deferred)"): `operation_generation_` guards
  logical correctness but not lifetime; a correct fix needs a mailbox/id redesign independent of the
  state address, deferred as a focused change. The HEAD-advance and close-generation refinements are
  part of the same redesign.
- **[RESOLVED 2026-07-14] Commit subject/body are passed as command-line `-m` arguments.** Commit now
  feeds the message on stdin via `commit -F -` (new `GitRepository::ExecuteWithStdin`), removing argv
  exposure and the argv-length limit. Regression:
  `CommitWorkflow/ExecuteCommitPreservesShellSignificantAndLargeBody`.

#### Project search, recents, and file operations

- **[WON'T-DO — bounded at 8; env-clamped] Project search helper thread creation is unbounded relative
  to process-wide workload.** Helper count is capped (and `MICROIDE_SEARCH_WORKER_LIMIT` is clamped to a
  product max of 64, 2026-07-13), so a search occupies at most a bounded fraction of cores; a shared
  cross-subsystem worker pool with a global budget is a scheduling refinement.
- **[RESOLVED — offset-preserving fold contract in place] Case-insensitive literal search reports byte
  columns from lowercased ASCII buffers only.** The 2026-07-13 work wired the **length-preserving**
  `Utf8CaseFold` into project-search matching specifically so byte columns stay aligned with the
  original text — the offset-preserving contract this item asks to lock in is exactly what shipped.
- **[WON'T-DO — streaming search by design (dup)] Project search result ordering is thread-scheduling
  dependent.** Same disposition as the earlier streaming-order item: results stream as workers find them
  (responsive), grouped by file in the UI; a stable global sort is a refinement, and every match is still
  reported.
- **[WON'T-DO — progress display nicety] Project search progress counts files claimed, not fully
  searched.** The final result set is correct; the progress counter briefly leading actual completion
  is a smoothness nicety (dup of the count-all-progress item).
- **[WON'T-DO — bounded; helpers observe cancel] Project search cancellation does not join helper
  threads until the worker task returns.** Helpers poll `cancel_requested_` per line/regex-interval
  (2026-07-13), so an old search winds down promptly; a pathological single-file read keeping one helper
  briefly alive during a new run is bounded. A shared cancellable pool is a refinement.
- **[WON'T-DO — results already capped] Search pending updates can accumulate large result vectors
  between UI drains.** The worker caps total stored results, so a pending mailbox update is bounded; a
  per-update byte cap with a service-side overflow store is tuning, not an unbounded-growth defect.
- **[WON'T-DO platform-only alias edge] Recents are compared and deduped by raw path value.** On
  case-sensitive Linux the raw path key is exact; symlink/relative-vs-absolute aliases are an edge, and
  case-fold aliasing only bites folding hosts. Host-normalized recent keys recorded (dup of the finder
  recent-key platform item).
- **[WON'T-DO — MRU noise nicety] Recent-file entries are not pruned when their project root
  disappears.** Stale recents make the picker slightly noisier over long use across moved repos;
  lazy pruning is tidiness, not a correctness issue (a dead entry simply does not open).
- **[WON'T-DO — production callers enforce containment] File create/rename operations allow targets
  outside the active project unless the caller checks.** The production callers already contain to the
  project root (verified for the `CreateDirectory`/`CreateFile` callers 2026-07-14); making
  `FileOperationService` intrinsically root-scoped is defense-in-depth over callers that already check.
- **[RESOLVED 2026-07-14] `RenamePath` uses exists checks before `MovePath`.** `RenamePath` now routes
  through `platform::MovePathNoOverwrite` (`renameat2(RENAME_NOREPLACE)` — atomic, race-free on the same
  filesystem) with a cross-device/exists fallback, closing the exists()-then-move TOCTOU. Regressions:
  `Project/{RenamePathRefusesToOverwriteExistingDestination,MovePathNoOverwriteRefusesExistingDestination}`.
- **[WON'T-DO platform-only] Reserved-component validation checks only the destination filename.**
  `CON`/`NUL`/trailing-dot reserved names are Windows-specific (Linux has none); validating every
  component against Windows reserved-name rules is Windows work not validatable here.

### Won't-do — verified non-defects

#### Deliberate tradeoffs (2026-07-14 curated pass)

- **Git: sidebar stage/unstage/discard + commit `RefreshDerivedState` off-thread — WON'T-DO.**
  Re-investigated 2026-07-14 (`WorkspaceSidebarCoordinatorActions.cpp` `StageGitEntry`/
  `UnstageGitEntry`/`DiscardGitEntry`/`StageAllGitEntries`/`DiscardAllGitEntries`): each validates
  availability, runs a single `project::Git*Path` subprocess (single-digit ms — a local
  `git add`/`restore`/`clean`), then `invalidate_editor_blame_path` + (for discard)
  `ReconcileOpenTabsAfterPathDiscard` + a `RefreshProjectFiles()` whose git *status* scan
  (`RefreshGit`) is ALREADY async. So the synchronous cost is small and the slow status read is
  already off-thread. Moving the writes off-thread is declined: `DiscardGitEntry` is a destructive
  path (`git clean -fd` / `git restore` / TrashPath) whose post-op editor-tab reconcile reads the
  file's post-op existence, so a correct async version needs a completion mailbox +
  `operation_generation` guards + reconcile reordering, and its data-loss / tab-reconcile ordering
  can't be verified end-to-end without driving the real GUI + real git timing. Correctness-first:
  not worth an under-verified data-loss-path rewrite for a marginal, already-mostly-async speed
  benefit. If ever revisited, reuse `CommitWorkflowService`'s completion-mailbox +
  `operation_generation` pattern.
- **Debug value node ids widened to 64-bit — TRIED, REVERTED, WON'T-DO.** Widening the id (and the
  per-row `node_id`) to `uint64_t` measurably regressed the `debug_value_tree_rebuild` /
  `debug_value_tree_expand_large` hot path in the 2026-07-14 perf comparison vs `origin/main`
  (~+7% p50 / +17% max on rebuild, identical allocation counts — the wider `Node` /
  `DebugVariableRowView` add memory traffic in the flatten/rebuild loop the step/render path runs).
  The 32-bit `next_id_` it guards wraps only after ~4 billion node allocations in a single debug
  session (practically unreachable), so the regression is not worth it. Reverted; the 32-bit-wrap
  risk is accepted.

#### Verified non-defects

- **Terminal: combining mark after a double-width glyph.** Dead code —
  `TerminalCell::bytes` is 4 bytes and wide(≥3)+combining(≥2) always exceeds it, so the
  mark is dropped regardless of which cell it targets. Revisit only if the cell buffer
  widens.
- **`ColorMath::BlendColors` missing output clamp.** Provably in-range for all lerp
  inputs; a branch on this hot color path violates speed-first. Left as-is.
- **Git porcelain-v2 `'2'` rename record consuming the next record.** Working as
  intended; the truncation-only residue is dropped harmlessly.
- **Plugin sync `Query*` overloads reallocation hazard.** Production uses the `*Async`
  variants (allow_registration=false); the sync overloads are test-only.
- **`ResetForDisabledRuntime`/`ShutdownForDisabledRuntime` subset-clear.** Unreachable
  — `enabled()` only goes true→false once at startup before any plugin loads.
- **Compare `AlignHunkLines` 1×1 pairing ignores similarity.** By design — a
  1-del/1-add hunk always renders as a single Modified row (pinned by
  `TestCompareManyTokenLineBoundsAlignmentDp`).
- **Compare `AlignHunkLines` oversized-hunk fallback pairs positionally without a
  similarity gate.** Evaluated a per-pair similarity gate (render low-similarity
  positional pairs as delete+insert); rejected. It changes the pinned fallback
  contract (`TestCompareLargeInputsUseBoundedFallback`, `modified == 1500`) and
  degrades the common systematic-rename case (`left-N` → `right-N`) from a
  readable side-by-side Modified row into split delete/insert rows — a UX
  regression, not a correctness fix (lines still round-trip either way). Kept as
  positional pairing by design; do not re-attempt without a product decision.
- **Terminal DECSTBM home ignores the scroll-region top under origin mode on the
  primary buffer.** Consistent with this terminal's primary CUP semantics; a
  design-consistent choice, not a live divergence.
- **Render: `AsciiGlyphAtlas::BlitInto` straight-copy erases an overhang glyph's
  spill.** The proposed OR/max-coverage merge is unimplementable via SDL surface
  blits (`SDL_SetSurfaceBlendMode` rejects custom blend modes; `BLENDMODE_BLEND`
  premultiplies and thins the common case), and a hand-rolled per-pixel max-alpha
  merge in the hottest text-compositing path is a net negative under speed-first for
  an artifact invisible with every monospace font microide ships. Revisit only if an
  overhang/proportional font is ever routed through this atlas.
- **Plugin: `process.run`/`run_async` OOM-longjmp over live C++ locals;
  provider-query loops dereference `provider.state` before the null-plugin guard.**
  Verified non-defects. `process.run` already defers every deliberate raise to the
  `.inc` wrapper via `PushMessage` + `kPendingError`; the only remaining `lua_*`
  calls are unavoidable success-path table pushes shared by ~20 sibling interop
  functions (the invariant's own SAFE idiom). The provider-query guard is
  belt-and-suspenders: teardown erases every runtime entry for a state before the
  state is nulled (single plugin-worker thread), so `find_plugin_by_state` never
  returns null for a live iterated entry (pinned by
  `TestPluginHostSetupFailureTearsDownRegisteredProviders`). Reordering fixes no
  reachable failure and has no constructible regression.
- **Persistence: per-tab compare/merge divider fractions "unclamped" on restore.**
  Already neutralized upstream: `PrimitiveReader::ReadF32` replaces any non-finite
  persisted float with `0.0` at the binary-read source, and the render-time
  `std::clamp(fraction, min, 1-min)` brings finite out-of-range values (including the
  `0.0`) into a valid pane split. No NaN ever reaches layout arithmetic, so no
  restore-time sanitize is needed.

### Won't-do — platform-only, cannot compile/validate on this Linux host

Real defects, but writing untested Windows/macOS code risks a worse regression than the
latent bug. Kept documented with fix direction for a maintainer on that platform.

- **Windows `AsyncSubprocess`** declares `state_mutex` but never locks it and uses a
  non-atomic `running` → HANDLE UAF race. Fix: mechanically mirror the POSIX branch
  (lock around every state access, re-fetch the HANDLE under the lock, atomic bool).
- **Windows `RunSubprocess` ignores `options.timeout_ms`** — FIXED 2026-07-13 (mechanical
  mirror of the POSIX deadline path: timed `WaitForSingleObject` + `TerminateProcess` +
  short reap, sets `timed_out`). Written behind `#elif defined(_WIN32)`, so it does not
  affect the Linux build; not compile-validated on Windows.
- **Windows `IgnoreMatcher::ParseRule`** normalizes the glob through `lexically_normal`,
  corrupting a backslash-escaped literal. Benign on Linux.
- **macOS FSEvents incremental events bypass the ignore filter** (no directory prune, no
  per-file `filter.Includes`). Mirror the Linux inotify gate.
- **macOS FSEvents `FileIndexWatcher` `run_loop` publish race + `CFRunLoopStop` timing
  deadlock.** `run_loop` (plain `CFRunLoopRef`) is written by the worker thread but read
  unsynchronized by `StopNative()`, and `native_active` is set `true` before the worker
  runs, so a quick `Watch()`→`Unwatch()` can hit the null-`run_loop` guard, skip
  `CFRunLoopStop`, and hang `worker.join()` forever inside `CFRunLoopRun()`. Fix: atomic
  handoff of `run_loop` plus a `CFRunLoopRunInMode`-with-stop-flag loop so a stop issued
  before the run loop starts is not lost.
- **Windows `DirectoryTree::RelativeKeysExcludingRoot` `root_key` is not lowercased** while
  the stored expansion keys are (via `NormalizePathKey`), so the `key == root_key` guard
  never matches on `_WIN32`. Harmless (the `rel == "."` check still excludes the root);
  fixing needs the `#ifdef _WIN32` lowercasing replicated and tested on Windows.

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
work, the `TextViewport` / `WorkspaceShell` decomposition, the 2026-06-11 deep correctness audit, the
2026-06-15/16 render/app/util/terminal closeouts, and the 2026-07-12 deferred-backlog sweep (the
cross-subsystem bug-hunt passes 5–24 closeout) — now lives in:

- `guidelines/tech-debt/archive/` — per-pass archive records (with reproduction notes and lessons)
- `CHANGELOG.md` — shipped, user-facing release history
- `openspec/changes/archive/` — the full proposal/spec/tasks record per shipped change

The broader 2026-04-20 architectural review is archived at
`dev-docs/archive/production-tech-debt-review.md`.
