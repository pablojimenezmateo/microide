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
- **RuntimeSyntaxRegistry regex/match perf budgets** (separate file from the loader): per-pattern
  & joined-pattern byte cap before PCRE compile, region-start budget, explicit `overrides` for
  filetype shadowing, prefetch key by document-id, `FindFirstRegex` skip-mask incremental. (The
  `^`-anchor correctness fix landed 2026-07-14; the loader-side speed wins — cached content-hash
  fingerprint, dedup definition dirs, prewarm cold filetype — landed 2026-07-15, see "Fixed in the
  2026-07-15 syntax-reload speed pass" above; the per-pattern byte cap was assessed low marginal
  value — syntax defs are already length/count-bounded at load — and the skip-mask incremental
  rewrite is behavior-risk in a path that can't be visually verified here, so both stay deferred.)

- **`CommitWorkflowService::DispatchCommit` captures `&state` (a `CommitWorkflowState` inside
  `current_project_state`) across the background executor + mailbox.** A project switch that
  moves/destroys that state while a commit is in flight dangles the reference;
  `operation_generation_` guards logical correctness but not lifetime. Pre-existing threading
  design; a correct fix needs a lifetime redesign beyond a local edit. (Same async-lifetime
  family as the git-sidebar off-thread item, now a won't-do — see below.)
- **Persistence: no parent-directory fsync after the atomic rename.** A deliberate
  speed/durability tradeoff: the atomic rename already prevents a torn read; only a
  crash inside the fs flush window can lose the newest session write. Left as-is.

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
- **`RuntimeSyntaxRegistry` rules still compile without `PCRE2_MULTILINE`.** The `^`-at-segment-
  boundary correctness bug is fixed (NOTBOL on mid-line segments, 2026-07-14), but `$` semantics
  across a segmented line were not revisited; a full MULTILINE pass would need generated-table
  verification. Low priority — the highlighter works per single line.

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

- **Persisted-record writer leaves a stale backup after every successful write.** The writer renames
  the old primary to `*.bak`, renames the new temp into place, then returns success without deleting
  or refreshing the backup. That is useful for recovery, but it means backup contents can be many
  sessions old and still get trusted by `ReadFile` after a future primary failure. Fix direction:
  define the backup contract explicitly: either keep a previous-good generation with metadata and
  visible recovery, or delete backup after successful primary write if backup is only for rollback.
  Add tests that write A, B, C, corrupt C, and verify whether recovery is allowed to return B or must
  fail closed.
- **Persisted-record write failure after backup rename can leave the primary restored best-effort
  only.** If `RenameReplacing(temp, path)` fails after the primary was moved to `*.bak`, the rollback
  call `RenameReplacing(backup_path, path)` is best effort and its failure is ignored. A rename
  failure on flaky storage can leave no primary and only a backup, with no surfaced "manual recovery
  required" state. Fix direction: return a richer error category for rollback failure and preserve
  enough context for the caller/status bar. Add an injected `RenameReplacing` seam so the test can
  force both rename and rollback failure deterministically.
- **Persisted-record reads allocate the full 256 MiB cap before validating header/body size.**
  `ReadAllBytes` reads the whole file into memory, then `DecodeRecordFile` checks magic, version, CRC,
  and body. A hostile 256 MiB file in the config/state path can allocate the cap on startup just to be
  rejected. Fix direction: read and validate the fixed header first, reject impossible body lengths,
  then stream or bounded-read only the declared body. Add a test with a large bogus file that proves
  startup/read memory stays below a small threshold using an injected reader.
- **JSON parsing has no aggregate array/object entry cap.** The parser bounds recursion depth, and DAP
  list parsers cap selected arrays after parsing, but `ParseJson` can still materialize a flat
  millions-entry array/object inside one message before any domain cap runs. A DAP/LSP peer can spend
  memory and CPU in the generic JSON layer even if the later parser truncates. Fix direction: add a
  parser-level node/byte budget or message-size-specific parse context, and make LSP/DAP clients pass
  protocol frame caps. Add JSON parser tests for a huge flat array and huge object.
- **JSON object duplicate keys use "last wins" with no signal.** `ParseObject` assigns
  `obj[key->AsString()] = value`, silently replacing earlier keys. That is legal-ish in many parsers,
  but DAP/LSP messages with duplicate `seq`, `command`, `success`, or `body` fields can be interpreted
  differently from peers. Fix direction: decide whether duplicate object keys should reject protocol
  JSON or be recorded as a diagnostic. Add a DAP/LSP parser test with duplicate `success` and `body`
  keys.
- **JSON serialization silently converts non-finite doubles to `null`.** This prevents invalid JSON,
  but protocol callers may believe they sent a numeric value. A malformed internal value could turn a
  DAP/LSP numeric request field into `null` without an error path. Fix direction: keep this fallback
  for generic serialization if needed, but reject non-finite numbers at protocol argument builders
  where the caller expects a number. Add unit tests for `Make*Arguments` with bad numeric inputs once
  those builders expose typed validation.
- **Protocol message-size policy is split between JSON, DAP, LSP, and control surfaces.** JSON can
  parse a payload as long as it reaches it; DAP/LSP framing has separate Content-Length handling; the
  control socket has its own line buffer cap. Fix direction: document and enforce a single inbound
  message budget per protocol, with parse budgets below transport caps. Add tests that verify
  over-budget DAP/LSP/control messages fail before JSON allocation.

##### DAP and debug workflow

- **DAP launch/configuration callbacks capture `this` without a session-generation guard.**
  `DebugSession` async callbacks check state in some places, but launch, configurationDone, pause,
  restart, evaluate, variables, scopes, setVariable, and breakpoint response callbacks all capture the
  session object directly. If a session is stopped/replaced while responses remain queued, a stale
  callback can mutate the new state or call host callbacks after termination. Fix direction: add a
  monotonically increasing session token bumped on `Start`/terminal transition and validate it in
  every DAP response callback. Add a fake DAP client test that queues a response, terminates/restarts,
  then drains the old callback.
- **DAP `RequestStop` can leave the UI active if `terminate` is sent but the adapter never answers.**
  For adapters advertising `supportsTerminateRequest`, `RequestStop` sends `terminate` and waits for
  `terminated`/process exit; it does not appear to set a timeout or transition optimistically. A hung
  adapter can leave the session in Running/Stopped with stop requested. Fix direction: start a bounded
  terminate timer, then disconnect/kill the adapter and transition to Terminated or Failed. Add a fake
  adapter test that accepts `terminate` and never emits `terminated`.
- **DAP `configurationDone` rejection records an error but still transitions to Running.** The
  callback sets `last_error_` when the response fails, then moves `Configuring`/`Initializing` to
  `Running` regardless. Some adapters reject configurationDone because launch/configuration is invalid;
  the UI can show a running session that will not run. Fix direction: classify configurationDone
  failure as terminal unless a known adapter requires tolerance. Add a DAP client test with a failed
  configurationDone response.
- **DAP resume commands optimistically clear stopped UI without checking request success.** `Continue`,
  step commands, reverse commands, and the deferred no-thread resume path send a request and set state
  to Running immediately. If the adapter rejects `continue`/`next`/`stepIn`, the execution highlight
  and variable state are cleared while the target remains stopped. Fix direction: either wait for
  response success before clearing, or restore stopped state on failure. Add fake adapter tests for
  rejected continue and rejected step.
- **DAP breakpoint responses are matched by send order, not stable breakpoint identity.** The
  `setBreakpoints` response mirrors input order in the spec, but adapters that omit or reorder
  unverified breakpoints can mark the wrong local breakpoint if matching is positional only. Fix
  direction: match returned breakpoint line/source when present, fall back to order only for missing
  line data, and surface unmatched adapter breakpoints. Add a response fixture with reordered
  breakpoints.
- **DAP source paths from adapter events are trusted as filesystem paths.** Breakpoint/source events
  parse `source.path` directly into `std::filesystem::path`. If later callbacks open/focus the path
  without containment or existence checks, a debug adapter can navigate the editor outside the project.
  Fix direction: audit every DAP path consumer, then route through a "debug external path" policy:
  allow read-only focus with a banner, or require project containment. Add tests for a breakpoint event
  with `/etc/passwd` or a temp path outside the workspace.
- **DAP value-size limits are gdb-name heuristics.** `CommandLooksLikeGdb` matches any basename
  containing `gdb`, and false negatives restore the freeze risk while false positives send gdb-only
  REPL commands to another adapter. Fix direction: prefer adapter capability/type configuration or a
  user setting, then keep basename heuristic as fallback. Add tests for `arm-none-eabi-gdb`,
  `gdbserver-adapter`, and `notgdb-but-name-contains-gdb`.
- **DAP variables/scopes list truncation has no visible "more data omitted" state.** Parsers cap at
  10,000 entries; the debug tree likely renders what arrives as complete. A hostile adapter is the
  security case, but a legitimate giant array can also be silently incomplete. Fix direction: propagate
  a `truncated` bit from protocol parsing into the debug pane and request paging when adapter counts
  are available. Add parser/UI tests for 10,001 variables.

##### Plugins and extension data

- **Plugin decoration publish can allocate and sort huge valid payloads up to four times per call.**
  Each decoration kind allows 100,000 entries, so one `decorations.set` can materialize up to 400,000
  host records before store merge/render costs. The cap is protective but still high for a
  shell-owned render path. Fix direction: set per-file and per-owner budgets based on visible use
  cases, fail closed with a plugin-visible error, and expose truncation/failure in plugin diagnostics.
  Add a plugin decoration stress test at cap and cap+1.
- **Plugin decoration paths use lexical resolution, not the stronger containment helper.** Like
  `editor.apply_edits`, `decorations.set` resolves `raw_path` with `ResolveRuntimePath`. Decorations
  do not write files, but publishing decorations for outside-project paths can consume store memory
  and later follow rename/delete retargeting logic. Fix direction: use `ContainPath` for file-bound
  plugin contributions or explicitly reject decorations for paths outside the active project. Add a
  plugin test with `../outside.cpp`.
- **Plugin code-action/provider result truncation is silent.** Completion, code action, test, SCM,
  annotation, and auth harvesters clamp arrays with `std::min(rawlen, cap)`, then return success.
  Providers that exceed a cap get partial results with no plugin-visible error, making generated code
  actions/tests appear missing. Fix direction: fail the provider result when raw length exceeds the
  cap for correctness-critical surfaces, or return a `truncated` flag to the UI. Add provider tests
  for cap+1 on completions and tests.
- **Plugin language-provider location paths are not containment-checked.** Definition/reference
  providers call `resolve_runtime_path` and accept any non-empty resolved path. A plugin can navigate
  the user outside the project, possibly intentionally, but the product boundary is not specified.
  Fix direction: decide whether plugin language locations may be external; if yes, mark them as
  external and open read-only; if no, use `ContainPath`. Add provider tests for absolute outside paths
  and `../` paths.
- **Plugin provider string fields have no per-field length limits.** Labels, documentation, detail,
  code action titles, test names, and SCM labels can be very large while still under Lua memory budget,
  then move into UI/render view models. Fix direction: clamp or reject plugin-provided display strings
  at interop boundaries, with separate limits for labels vs documentation. Add tests with multi-MB
  completion documentation and code-action title.
- **Plugin callbacks can synchronously call back into host surfaces during lifecycle teardown.**
  Lifecycle helper code invokes Lua callbacks while host registries are being updated elsewhere in the
  plugin lifecycle. If a callback publishes decorations/diagnostics or registers commands during
  teardown, ordering assumptions can break. Fix direction: audit lifecycle callback phases and set an
  explicit "no registration/no publish" mode during teardown callbacks. Add a plugin teardown test
  that attempts to publish from `on_project_close`.

##### Project change, file finder, and indexing

- **Project change coalescer can drop per-file reload/diagnostic updates once the cap flips to tree
  rescan.** Past 1,024 pending file changes it sets `tree_rescan_requested` and ignores later file
  changes. The rescan updates the index, but open-buffer external-change banners, diagnostics clearing,
  and other per-file side effects may not run for the dropped tail. Fix direction: when collapsing to
  a tree rescan, also mark "unknown file changes happened" so downstream open buffers perform a cheap
  mtime sweep. Add a coalescer/workspace test with cap+1 modified open files.
- **Project change coalescer delete-then-create semantics lose the new absolute path for Created.**
  A Deleted followed by Modified becomes Created and updates `absolute_path`; a Deleted followed by
  Created falls through to `existing->kind = change.kind` and updates the path, which is okay, but a
  Created followed by Deleted collapses to Deleted and leaves the old absolute path. Any downstream
  consumer that uses `absolute_path` for delete cleanup may see a stale path after rename-like floods.
  Fix direction: define per-kind payload validity and clear irrelevant absolute paths on delete. Add
  unit tests for all two-event combinations.
- **File finder recent-file de-dup keys are platform-sensitive strings.** Recents are de-duplicated
  and looked up by `recent.string()` while cached entries also use path strings. On Windows/macOS,
  case-insensitive paths and separator normalization can show duplicates or fail to match a recent
  whose casing changed. Fix direction: use the same normalized path key as editor tabs/decorations for
  recent lookup. Add host-platform-override tests with `Src/Main.cpp` vs `src/main.cpp`.
- **File finder keeps the full uncapped matched-index set for narrowing.** This preserves correctness
  for later typing, but an empty/one-character query over a huge index stores every match index. The
  index itself is capped elsewhere, yet this can still be a noticeable allocation on the shell thread.
  Fix direction: keep a compressed bitset/range representation for broad matches or disable narrowing
  cache until the query length passes a threshold. Add a benchmark with the maximum index size and
  one-character query.
- **Full project scan does not propagate traversal-budget truncation to `FileIndex::truncated()`.**
  `CollectProjectFiles` stops at `kTreeTraversalEntryBudget`, but returns only the collected vector.
  `FileIndex::Refresh` then clears `truncated_` because it has no scan metadata. A too-large tree can
  look complete after a full rescan. Fix direction: return `{files, truncated}` from
  `CollectProjectFiles` and surface it through `FileIndex`. Add a scanner test with a tiny injected
  budget.
- **Project scanner hidden filtering checks only the current filename before ignore rules.** In
  `ExcludeHidden` mode, a hidden directory is skipped before loading its child ignore file, which is
  good for speed, but a negated rule inside parent `.gitignore` cannot re-include a hidden subtree for
  file finder/search. Fix direction: decide whether `ExcludeHidden` is a hard UI mode or a
  gitignore-like filter; if hard, document it; if not, let negated ignore rules override hidden
  filtering. Add scanner tests for `.hidden/file.cpp` with `!.hidden/`.
- **Windows native tree watcher cannot watch more than `MAXIMUM_WAIT_OBJECTS - 1` roots and falls back
  coarsely.** Large root lists from recursive watch preparation can exceed the wait-object limit and
  force polling. This is acceptable but not surfaced clearly to users, and polling may be expensive on
  large trees. Fix direction: shard Windows watch handles across worker threads or surface a
  "polling fallback" state. Add a Windows seam test for 65 watch roots.

##### Terminal, input, and ANSI behavior

- **Terminal pending query replies silently truncate at 64 KiB.** The cap prevents a reply flood from
  freezing the UI, but a program issuing many legitimate color/DA/DSR queries can receive a partial
  reply stream with no reset marker. Fix direction: when the cap is hit, drop whole replies rather
  than partial bytes and increment a debug counter. Add tests that fill the buffer exactly to the cap
  and then issue one more query.

##### Rendering, UI, and user feedback

- **Debug pane row hit-testing intentionally maps the top band to row zero, unlike other miss
  behavior.** Tests pin this for debug pane, while compare/merge top-band behavior is logged as a
  residue. The inconsistency makes pointer bugs hard to reason about across panes. Fix direction:
  choose one shared policy for all row lists and migrate tests deliberately. Add a shared row-hit-test
  helper before changing behavior.
- **Overlay scroll wheel paths use integer ticks and can skip small high-resolution wheel deltas.**
  SDL wheel events can carry fractional/precise deltas depending on device. Coordinators that cast to
  integral ticks can ignore trackpad micro-scrolls or behave differently across platforms. Fix
  direction: accumulate fractional wheel deltas per surface and consume whole rows. Add coordinator
  tests with repeated 0.25-row deltas.
- **Several UI lists clamp selection after content changes but do not preserve item identity.** File
  finder, code actions, command palette, debug threads, and similar overlays reset/clamp by index.
  Async refreshes can move the selected item under the cursor/keyboard. Fix direction: preserve
  selection by stable id/path/command where available, falling back to index only when identity is
  absent. Add overlay tests where a refresh inserts an item before the selected one.
- **Status/error messages lack severity and lifetime policy.** Many subsystems need to surface
  "operation failed", "truncated", or "fallback used"; the status bar currently has ad hoc call sites.
  Fix direction: define a small status-message service with severity, source, expiry, and replace
  rules. This is a prerequisite for several silent-failure fixes above.

#### 2026-07-13 deep subsystem audit backlog — third tranche

##### LSP / DAP protocol framing and request semantics

- **Malformed LSP frame recovery can scan arbitrary body bytes as future headers.** When the
  `Content-Length` value is nonnumeric or nonpositive, `LspMessageFramer::Next` consumes only that
  line and then tries to resync line-by-line. If the malformed frame also has a blank line and a large
  body, the body is interpreted as candidate headers, causing avoidable CPU churn and occasionally a
  false resync if the payload contains a `Content-Length:` line. Fix direction: once a malformed
  length is seen inside a header block, drop through the next blank line and optionally require a
  bounded resync window before accepting another frame. Add a fixture with malformed header plus body
  containing a fake `Content-Length`.
- **String-valued JSON-RPC response ids are ignored.** The client only accepts integer or double ids.
  This matches requests emitted by MicroIDE today, but some intermediaries and test harnesses stringify
  ids. If a server echoes `"id":"5"` the pending request times out and then degrades as an empty
  response. Fix direction: decide whether to reject string ids explicitly with a trace entry or accept
  exact decimal strings that match in-flight integer ids. Add one LSP transport fixture either way.
- **LSP request timeouts and server exits are delivered as empty success-shaped objects.**
  `FailPendingRequests` invokes callbacks with `{}` and no error envelope, so completion, definition,
  semantic tokens, hover, and code-action handlers cannot distinguish "server did not answer" from a
  legitimate empty result. The UI then reports no matches/actions instead of "language server timed
  out/exited". Fix direction: introduce a small `LspRequestOutcome` or JSON-RPC error envelope and
  plumb it through request callbacks. Add tests for timeout and EOF on at least definition and code
  action surfaces.
- **`window/showMessageRequest` is auto-dismissed without surfacing server actions.** Server requests
  for user choice are answered with `null`. That is valid protocol-wise, but servers use this for
  "install component", "reload project", or "apply workspace setting" prompts; silently declining can
  make language features appear broken. Fix direction: route the request through the prompt surface
  with bounded action labels, then return the selected action or `null` on dismissal. Add a fake server
  that offers two actions and asserts the selected action is sent back.
- **`window/showDocument` requests are auto-rejected/ignored.** The dispatch path returns `null` for
  server document-open requests, so servers cannot open generated documentation, external build logs,
  or source files from server-side commands. Fix direction: support `file:` URIs inside the project
  through the normal open-file path and explicitly reject external/web URIs with visible feedback.
  Add containment tests for project file, outside-project file, and `https:` URI.
- **LSP `workspace/configuration` does not resolve dotted section names.** The handler checks
  `settings.HasKey(section)` directly. Servers commonly ask for sections such as
  `rust-analyzer.cargo`, `clangd.arguments`, or nested workspace keys, while persisted settings may be
  stored as hierarchical objects or prefixed ids. Fix direction: define one mapping between MicroIDE
  settings ids and LSP section keys, including dotted paths, and test common clangd/rust-analyzer
  requests against real configured values.
- **LSP result truncation is silent on locations, diagnostics, symbols, workspace edits, semantic
  tokens, inlay hints, and signature help.** The parser caps several arrays to protect the UI thread,
  but it returns the same type whether the result was complete or clipped. Users see an incomplete
  references list or outline with no indication. Fix direction: return a `truncated` bit alongside
  parsed results and surface it through status/messages and picker footers. Add parser tests that hit
  each cap and UI tests that assert visible truncation feedback.
- **Signature-help labels and documentation have no per-field byte cap.** Counts are capped, but each
  signature label, parameter label, and documentation string is copied as-is. A single hostile result
  can consume large memory and render time without exceeding count limits. Fix direction: cap each
  copied string on UTF-8 boundaries, with a truncation marker where user-facing. Add parser tests with
  one oversized label/documentation field.
- **Semantic-token parsing silently ignores trailing partial groups and invalid coordinates.** This is
  currently defensive, but it also hides a server/protocol bug and can shift token colors with no user
  feedback. Fix direction: keep ignoring malformed tokens for robustness, but trace and expose a
  per-response dropped-token count to diagnostics/status in debug builds. Add parser tests that assert
  the dropped count for partial groups, negative deltas, and overflow coordinates.
- **DAP and LSP framing rules are implemented separately and can drift.** DAP has an inline parser in
  `WorkspaceDapClientInternal.h` while LSP uses `LspMessageFramer`. Any header tolerance, cap, or
  malformed-frame recovery fix must be duplicated by hand. Fix direction: extract a shared
  `ContentLengthFramer` parameterized by protocol name and cap, then reuse it in both clients. Add
  shared framing tests plus one DAP integration smoke test.

##### Patch apply, compare review, and git boundaries

- **Patch apply background work calls shell callbacks from the background executor.**
  `PatchApplyService::DispatchApply` invokes `current_repository_state`, `request_git_refresh`,
  `invalidate_editor_blame_path`, `reload_clean_editor_tabs_for_path`, `refresh_compare_tab_for_path`,
  and `set_command_feedback` from the posted worker lambda. Several of those callbacks are shell/UI
  state operations and are not documented thread-safe. Fix direction: keep git apply on the worker but
  marshal all shell/service callbacks back to the main mailbox; make the callback type names reflect
  thread affinity. Add TSAN coverage around rapid stage/discard plus tab close/project switch.
- **Patch apply stale checks compare only repository generation, not repository identity.** A delayed
  apply request captures a repository root and generation, but the worker compares the generation
  against the current active repository state. If the user switches projects and the new state happens
  to have the same generation, the stale check can pass while callbacks refresh the wrong project and
  feedback is attributed to the wrong shell context. Fix direction: compare repository root and a
  project/session token as well as generation; drop or re-route completion when the originating project
  is no longer active. Add a two-repo fixture where both states start at generation 0.
- **`diff_model_generation` is captured then intentionally unused.** Patch text is generated
  synchronously before dispatch, but confirmation flows and async apply still carry a model revision
  that is never checked. If the compare tab is refreshed/rebased without a repository generation bump
  between request construction and destructive discard confirmation, the prompt can apply a patch the
  visible model no longer represents. Fix direction: either remove the field and document the invariant
  or validate the originating compare tab/model revision before dispatch and completion. Add tests for
  discard confirmation followed by compare refresh before confirm.
- **Patch generation hardcodes file modes for create/delete.** `PatchGenerator` emits
  `new file mode 100644` and `deleted file mode 100644` and has no representation for executable bits
  or symlink mode. Staging/discarding a created executable script, deleted executable, or symlink can
  produce an index/worktree state with wrong mode metadata. Fix direction: thread mode metadata from
  compare semantic metadata or git diff headers into the patch generator. Add git fixtures for
  executable create/delete and symlink create/delete.
- **Mode-only changes have no patch-apply path.** The patch-review UI gates on text semantic files and
  `PatchGenerator` only emits content hunks. A chmod-only change or symlink-target mode change can
  appear in git status but cannot be staged/discarded from compare review. Fix direction: model
  mode-only changes as first-class compare entries with explicit stage/discard commands, not fake text
  hunks. Add git fixtures for `chmod +x file` with no content change.
- **Rename/copy metadata is flattened to the new path for review operations.**
  `ParseGitBranchDiffNameStatusZ` reports only the new path for rename/copy records, and patch
  generation emits `diff --git a/<path> b/<path>` for a single path. Content hunk staging on renamed
  files can lose rename context, and branch/outgoing file lists cannot open the old side by identity.
  Fix direction: keep old path, new path, and similarity score in the semantic file model; teach patch
  generation about `rename from` / `rename to` when needed. Add fixtures for staged and unstaged
  renames with additional edits.
- **Branch base resolution accepts arbitrary config strings as refs.** `ResolveGitBaseReference` reads
  `branch.<name>.gh-merge-base` and `branch.<name>.remote`, concatenates them into refs, and falls
  back to checking the raw branch name. Commands are argument-vector based, but invalid refname bytes,
  path traversal-looking names, or names beginning with option-like text can still produce surprising
  resolution and labels. Fix direction: validate config-derived names with `git check-ref-format
  --branch` or stricter local rules before using them as refs/labels. Add fixtures with malformed
  config values and option-looking names.
##### Control channel, socket lifecycle, and command transport

- **Control socket file permissions are fixed after bind, leaving a short wider-permission window.**
  The server binds with the process umask, then calls `chmod(0600)`. On permissive umasks there is a
  small interval where another local user could connect before the chmod. Fix direction: set a
  restrictive temporary umask around bind or create the containing runtime directory with 0700 and
  verify it before binding. Add a test for parent directory permissions where practical.
- **Control client `connect()` is still blocking and outside the timeout budget.** After connecting,
  send/read paths are poll-deadline driven, but `ControlSocketClient::Connect` calls blocking
  `connect()` first. AF_UNIX connects are normally quick, yet a pathological socket/backlog or
  filesystem-backed endpoint can stall the CLI before `--timeout` applies. Fix direction: create the
  fd nonblocking, handle `EINPROGRESS`, and poll for writability within the caller's deadline. Add a
  test with a socket that does not accept promptly if the harness can make one deterministic.
- **Control `SendLine` decrements in-flight even when the write buffer overflowed and the reply was
  dropped.** This lets linger logic reap a half-closed connection as "answered" even though the
  specific response was discarded due to a stalled reader. The connection is flagged for drop, so the
  data loss is bounded, but control callers see EOF rather than a structured overflow error. Fix
  direction: send an explicit overflow/error line before dropping when possible, or keep accounting as
  failed-not-answered for diagnostics. Add a slow-reader broadcast/reply test.
- **Inbound control queue overflow drops the flooding connection after partially queueing earlier
  lines.** `IngestReadBuffer` can enqueue some requests, then hit `kMaxInboundQueued` and drop the
  peer. Those already-queued requests may still execute while later requests from the same client are
  lost. Fix direction: define overflow semantics: either reject the whole batch atomically before
  enqueueing, or send a clear overflow response for the cut point. Add a flood test that verifies
  exactly which requests execute.
- **Control socket rebind only checks that some filesystem node exists at the socket path.**
  `MaybeRebindSocket` calls `stat(path)` and returns when it succeeds. If the socket path is replaced
  by a regular file, directory, or symlink, the advertised descriptor remains present but clients can
  no longer connect to the live listener, and the server will not repair it. Fix direction: use
  `lstat`, verify the node is the expected socket type, and rebind or fail visibly when it is not.
  Add a test that replaces the socket path with a regular file after startup.

##### Editor primitives, text layout, and Unicode correctness

- **Multi-line paste into single-line fields concatenates tokens with no separator.** `Insert`
  removes CR/LF entirely, so pasting `foo\nbar` becomes `foobar` rather than `foo bar` or `foo`.
  That can silently change search needles, rename targets, branch names, or command args. Fix
  direction: choose a product policy per field: replace line breaks with spaces for search/commands,
  take first line for paths/renames, or reject with visible feedback. Add paste tests for search,
  command palette, and rename prompt.
- **Text visual width treats every non-tab code point as one cell.** `AdvanceVisualColumn` ignores
  East Asian wide characters, emoji width, zero-width joiners, and combining marks. This affects caret
  hit testing, horizontal scroll, hover target mapping, compare alignment, and inlay placement for
  valid UTF-8 documents. Fix direction: add a small wcwidth/grapheme-width layer with deterministic
  tests and keep a fast ASCII path. Add layout tests for CJK, emoji, and combining sequences.
- **Identifier hover ranges are ASCII-only.** `TextLayout::IdentifierRangeAt` refuses non-ASCII
  identifier bytes before LSP hover/definition lookup, so Rust, Python, JavaScript, and many language
  servers will not get hover requests for valid Unicode identifiers. Fix direction: use language
  server position under cursor even when local identifier extraction fails, or extend identifier
  classification to Unicode. Add hover-target tests for `café` and `变量`.
- **Inlay hint column math trusts plugin/LSP label widths after truncation but not aggregate overflow.**
  Individual inlay labels are capped, yet `InlayLineTotalCells` and displacement sums can still grow
  large when many hints target one line. Fix direction: saturate aggregate cell counts per visual row
  and surface truncation when inlays are dropped. Add a test with thousands of hints at one anchor.

##### Theme, rendering, and UI output

- **Theme include cycles are silently accepted.** If an included theme is already in `include_stack`,
  `LoadThemeStyles` returns true and continues. That avoids infinite recursion but leaves users with a
  partially applied theme and no explanation. Fix direction: return a structured cycle error or at
  least trace/status it. Add a cycle fixture that asserts the error text.
- **`TruncateToWidthView` returns a thread-local scratch view that is easy to invalidate.** The
  lifetime is documented in a member comment, but render code can accidentally store a returned view
  or call the truncator again before drawing the previous value, causing wrong labels. Fix direction:
  use a small explicit scratch object passed by the caller or return an owning string in paths that
  need more than immediate draw. Add a render unit test that exercises two truncations before drawing.
- **Output-panel context snippet highlighting assumes token count is at least visible byte length.**
  The render path falls back to plain text when `highlighted->tokens.size() < visible_code.size()`,
  but it does not distinguish "highlight missing" from "tokenization stale/truncated". Fix direction:
  carry a highlighted-range generation and a truncation reason so stale syntax data does not quietly
  disappear. Add tests for output snippet refresh after file edit.
- **Output-panel reference path context is sticky until an empty line or new reference changes it.**
  A context snippet updates `current_reference_path`; unrelated following lines inherit that path for
  click/reference handling until an empty line resets it. Build tools often emit dense logs without
  blank lines, so clicks can open the previous file for unrelated text. Fix direction: attach the
  resolved reference path to each parsed output entry instead of maintaining render-time mutable
  context. Add output-channel tests with reference, snippet, unrelated line, and click.
- **Compare/merge truncation still happens inside render translation units.** The current lint allows
  `TruncateLabelView` hot-path calls, but every call can measure multiple prefixes and touch the width
  cache during drawing. This keeps product logic out of render but still leaves variable CPU in the
  render pass. Fix direction: move stable truncated labels into view models where dimensions are known
  or cache truncation per label+width revision. Add perf counters for compare/merge truncation calls.

##### Settings and persistence edge cases

- **Duplicate persisted setting keys resolve differently before and after mutation.** Reindexing
  iterates the raw vector and lets later duplicates win, while `settings_layer::Upsert` updates the
  first duplicate and `Find` returns the first duplicate for overlay labels. A corrupted or manually
  edited config with duplicate keys can show one value in the UI and apply another until reset erases
  all copies. Fix direction: canonicalize duplicates during persistence load or make all operations
  consistently last-wins. Add persistence fixtures with duplicate user and project setting ids.

#### 2026-07-13 deep subsystem audit backlog — fourth tranche

##### File watching, indexing, and tree traversal

- **Linux `FileIndexWatcher` degrades to a partial native watch tree instead of a reliable fallback.**
  When `AddWatchRecursive` hits `ENOSPC` or the internal watch-entry cap, setup logs once and keeps
  the root plus whatever subdirectories were already registered. Files in unwatched subtrees then stop
  updating until a full rescan, but the UI still sees a live watcher. Fix direction: switch to poll
  fallback or mark the index as degraded with a visible status and periodic full resync. Add an
  injected watch-budget test that creates a deep tree and verifies changes below the cap boundary are
  not silently missed.
- **Moved-in directory indexing ignores subtree truncation.** `AppendSubtreeCreations` calls
  `CollectTrackedCreations` but discards the returned `truncated` bit. A large directory moved into
  the project can enqueue only the first `entry_budget` files with no follow-up full scan and no
  user-visible "index incomplete" state. Fix direction: propagate truncation into the batch and trigger
  an initial-style resync or degraded-index status. Add an inotify-oriented unit seam that moves in a
  directory above the cap.
- **Inotify drain batches have no per-batch change cap.** A single drain cycle can accumulate
  creations, recursive deletions, and moved-in subtree entries into one `changes` vector. Kernel input
  is chunked, but the loop drains repeatedly before publishing. A high-churn generator can build a very
  large batch on the watcher thread and then force one large consumer update. Fix direction: cap
  incremental batches, publish in chunks, or fall back to a full resync after the cap. Add a synthetic
  inotify-event builder test that asserts chunking.
- **Poll fallback builds full snapshots without the same entry budget as native setup.** The poll
  worker's `build_snapshot` walks the whole tree into a `std::map` every 750 ms and does not consult
  `entry_budget`. On a huge tree, the fallback path can consume more CPU/memory than the native path it
  replaced. Fix direction: apply the same budget/truncated state to poll snapshots and lengthen or
  disable polling when the tree is too large. Add a poll-mode fixture with a budgeted fake tree.
- **Poll fallback diff materializes complete previous and current maps before deciding nothing
  changed.** Even when a repo is stable, each cycle rebuilds `current`, compares it to `snapshot`, and
  replaces the old map. That is predictable but expensive for large projects, and it runs even when the
  file index is already known truncated/degraded. Fix direction: use mtime directory probes, a slower
  cadence for large trees, or an incremental snapshot visitor that can stop at the budget. Add perf
  coverage for poll fallback over a large synthetic tree.
- **`FileTreeWatcher::SetRoots` can spend startup time collecting a native watch list for a tree that
  is later superseded.** The method snapshots roots under lock, releases it, builds native watch paths
  and maybe a full snapshot, then discards the prepared result if roots changed. Rapid project switches
  can repeatedly pay the whole traversal cost for stale roots. Fix direction: thread a generation or
  cancellation token through `CollectRecursiveWatchPaths` / `CaptureTreeSnapshot`. Add a test that
  calls `SetRoots` repeatedly with slow injected traversal.
- **`CollectProjectFiles` returns a partial file list on traversal-budget exhaustion without a
  truncation signal.** The scanner stops when `platform::kTreeTraversalEntryBudget` is reached and
  returns whatever was collected. Callers cannot distinguish "project has N files" from "scanner gave
  up at N". Fix direction: return `{files, truncated}` or publish scanner status through the file index
  service. Add scanner tests for exactly-at-budget and over-budget trees.
- **Project file scanning and index watching can disagree about hidden files.** `CollectProjectFiles`
  has an explicit `ExcludeHidden` mode based on filename dot-prefixes, while the file index watcher
  relies on traversal filters and `.git` checks. Surfaces that use different sources can show hidden
  files in one picker and not another. Fix direction: route all tree surfaces through one
  `ProjectTraversalFilter` policy with an explicit hidden-file flag. Add fixtures for `.env`,
  `.config/file`, and non-hidden files inside hidden directories.

##### Project search, finder, and command completion

- **Project search silently skips unreadable, binary, and too-large files.** `ReadFileForTextSearch`
  returns false for all of those cases, and the search result only reports matches/truncation. Users
  can believe a term is absent even though files were skipped. Fix direction: return classified skip
  counts (`unreadable`, `binary`, `too_large`) and surface a compact status footer. Add tests with one
  matching unreadable file and one oversized text file.
- **Project search result ordering is nondeterministic under parallel workers.** Matches are published
  in worker-claim and batch timing order, not in stable path/line order. The same query can reorder
  results between runs depending on file sizes and scheduling. Fix direction: either sort by
  path/line before publishing final results or document streaming order while keeping a stable final
  presentation pass. Add a deterministic fixture with two workers and deliberately uneven files.
- **Count-all project search can finish with stale-looking progress.** Over-cap matches are counted in
  worker-local counters and folded into `matches_found` only at worker exit. During a long common-term
  count-all search, progress can advance by files while the visible match count remains stuck at the
  display cap. Fix direction: periodically fold local over-cap counts or expose "counting..." distinct
  from exact total. Add a common-literal count-all test that observes progress before completion.
- **[PARTIALLY RESOLVED 2026-07-15] File finder cache rebuild lowercases and stores the entire file
  index on the UI thread.** The dominant interactive cost — `EnsureCacheBuilt` calling
  `SnapshotWithVersion()` (an O(index) deep copy of every `ProjectFile` under the index lock) on
  **every keystroke** just to read the version — is fixed: it now checks the cheap `index_->version()`
  first and only snapshots on an actual version change (`FileFinder/WarmRefreshDoesNotRebuildPerKeystroke`).
  **Still deferred:** the one-time rebuild on a version change (finder-open after an index update) still
  runs the per-file case-fold + `CachedFileEntry` build on the UI thread. Moving it fully off-thread
  needs the finder to own a background executor + a wake→re-refresh hook wired through the shell (SDL
  event, like `HighlightPrefetchService`), plus a stale/rebuilding state while the finder is
  interactive; that async wiring is hard to verify headless, so it is left as a follow-up. Concrete
  design: finder-owned `ProjectBackgroundExecutor` builds the cache off the version-change path, posts a
  wake event, and swaps the new cache in on the UI drain; rank against the previous (or empty) cache
  meanwhile. Alternative: keep case-folded keys in `FileIndex` (built during the already-off-thread
  scan) so the finder never folds.
- **File finder recent-path matching uses raw `path.string()` identity.** Recents are compared to
  cached index paths without platform normalization beyond the stored string. Case-only renames on
  Windows/macOS, slash spelling differences, or Unicode normalization can make a recent file disappear
  from the empty-query lead section even though it is indexed. Fix direction: key recents through
  `editor::PathKey` / host-platform normalization. Add host-platform override tests.
- **Command palette path completion can enumerate absolute filesystem roots from any project.** A
  token beginning with `/` searches from the filesystem root rather than the project root. That may be
  useful for power users, but it is a broader read/enumeration surface than most project commands
  need. Fix direction: decide which commands allow absolute completion and gate it per command
  metadata. Add tests for `open /`, `debug-run /`, and project-relative-only commands.
##### Plugin runtime and extension boundaries

- **`workspace.open_file` lets plugins request arbitrary absolute paths without capability or
  containment checks.** The API resolves the path and calls the host `open_file` callback directly;
  unlike `files.read_text`/`write_text`, it does not consult `ContainPath` or filesystem capabilities.
  Fix direction: decide whether opening external files is a plugin capability; otherwise contain to the
  project root and mark external opens read-only. Add plugin tests for `/etc/passwd` and
  `../outside.txt`.
- **Plugin `workspace.open_file` line/column arguments are only checked as Lua integers.** Positive
  `lua_Integer` values are cast to `std::size_t` with no upper bound. A plugin can request enormous
  coordinates and leave each editor/open path to clamp consistently. Fix direction: clamp to a shared
  maximum or reject values outside the document range after load. Add plugin tests with
  `math.maxinteger`.
- **Plugin `process.run_async` still blocks the plugin worker thread.** The API name and callback
  shape suggest async behavior, but it calls `platform::RunSubprocess` synchronously and invokes the
  callback before returning from the Lua interop call. A long formatter/test command can stall every
  later plugin task for up to the 120 s timeout. Fix direction: route async process work through a
  bounded process worker pool and post the callback back into the plugin runtime. Add a test where a
  slow `run_async` does not delay an unrelated provider query.
- **Plugin process allowlists match by basename, enabling project-local binary shadowing.** If a plugin
  is allowed to run `eslint`, `ProgramAllowed` also accepts `./eslint` or
  `<project>/tools/eslint`. That may be wanted for tool wrappers, but it weakens the meaning of an
  allowlist entry that looked like a system binary. Fix direction: distinguish exact path entries from
  basename entries in the manifest schema. Add tests for `eslint`, `/usr/bin/eslint`, and
  `./eslint`.
- **Plugin surface anchors are lexical, not containment-checked.** `ReadAnchor` resolves
  `{path,line}` through `ResolveRuntimePath` and stores the result. A plugin surface can anchor itself
  to an external absolute path or `../` path, then later participate in navigation and refresh logic.
  Fix direction: use `ContainPath` for anchors or explicitly model external anchors. Add plugin
  surface tests for project, outside-project, and missing paths.
- **Plugin surface hit-region commands are raw command strings.** `ReadHitRegions` accepts arbitrary
  command text per region; clicking the region can execute the same parser surface as user commands.
  That is powerful, but there is no distinction between invoking a registered plugin command and
  synthesizing host command-line text. Fix direction: prefer structured command ids plus arguments, or
  mark raw command-line hit regions as privileged. Add a test that a hit region cannot smuggle a
  multi-command or malformed quoted string if raw commands are restricted.
- **Plugin SCM snapshots can report paths outside the project.** `SnapshotScm` resolves each entry path
  with `resolve_runtime_path` and accepts any non-empty result. A plugin SCM provider can populate the
  SCM view with external files, after which stage/discard/navigation semantics become ambiguous. Fix
  direction: contain paths or explicitly tag external SCM entries with disabled mutations. Add provider
  tests for absolute outside paths and relative `../` paths.
- **Plugin test discovery can report external files and negative lines.** `DiscoverTests` resolves
  test files lexically and stores `line` as whatever integer the plugin returns. External files and
  negative/zero lines then flow into test navigation. Fix direction: contain or mark external files,
  and require positive one-based lines before converting to UI positions. Add plugin test-provider
  fixtures.

##### Terminal backend, emulation, and terminal UI

- **POSIX terminal writes are blocking and have no deadline.** `PosixTerminalBackend::Write` loops on
  `write(master_fd, ...)` until the whole buffer is sent or an error occurs. Key input, focus events,
  query replies, and paste all call this path; if the child stops reading from the PTY input side, the
  UI thread or reader callback can block. Fix direction: make the master fd nonblocking and use
  poll-driven deadlines or a bounded write queue owned by the backend thread. Add a PTY fixture with a
  child that never reads stdin.
- **Terminal `Stop()` can wait on a child shutdown path without user-visible escalation.** POSIX stop
  asks `RequestTerminalChildShutdown(child_pid)` and then joins the reader. If shutdown takes a long
  time, closing a terminal tab or quitting can feel hung. Fix direction: surface "terminating
  terminal..." after a short threshold and hard-kill after the existing bounded policy if not already
  enforced in `ShellProcess`. Add tests with a child ignoring SIGTERM.
- **OSC 7 working directories are stored without containment or existence checks.** The decoded path is
  assigned to `reported_working_directory_` directly. Later terminal affordances can seed new terminals
  or labels from a path that no longer exists or points outside the active project. Fix direction:
  classify reported cwd as project-contained/external/missing and constrain commands that reuse it.
  Add tests for missing path and outside-project path.
- **Terminal copy helpers treat empty cells as spaces even inside wide-glyph sequences.** Wide trailing
  cells are skipped, but other empty cells in a selection become spaces. For applications that use
  absolute cursor moves, this can copy rectangular padding that was never text. Fix direction: offer a
  "stream copy" mode based on line dirty extents/wrap metadata in addition to grid copy. Add tests for
  cursor-positioned output with gaps.

##### App startup, control specs, debug, and commit workflow

- **Control spec arrays have no item-count caps.** The file size is capped at 1 MiB, but settings,
  breakpoints, open files, function breakpoints, and commands can still contain many small entries that
  expand into a much larger command list. Fix direction: cap each array and the generated command
  count, with parse errors before execution. Add `ParseControlSpec` tests for cap and cap+1.
- **Control spec `commands` bypass structured validation and ordering guarantees.** Raw command strings
  are appended after generated breakpoint/open/launch commands. A spec can include malformed quoting,
  commands that mutate the project before later commands, or commands that undo earlier generated
  setup. This is likely intended for power users, but it needs a product contract. Fix direction:
  either keep raw commands but mark them "unsafe escape hatch" in the spec, or add structured command
  entries with explicit arguments and validation. Add tests that raw command failures are surfaced and
  do not abort later entries unless requested.
- **Control spec breakpoint paths are resolved against the project root without containment.**
  `ResolveAgainstProject` normalizes `../outside.cpp` to an absolute outside path and the generated
  breakpoint command then targets it. If debugging external files is allowed, this should be explicit;
  otherwise specs can seed out-of-project breakpoints. Fix direction: contain spec paths by default and
  add an `external` opt-in if needed. Add spec tests for `../outside.cpp`.
- **Debug exception-filter seeding is per model, not per adapter identity.** Once `seeded_` is true,
  `SetAdvertisedFilters` no longer adopts defaults from a newly advertised filter set. Switching debug
  adapters in the same project can leave the old adapter's enabled filters applied to the new adapter.
  Fix direction: store the adapter id/fingerprint with seeded defaults and reseed when it changes.
  Add tests that switch from one fake adapter's exception filters to another's.
- **Exception filter conditions can outlive the advertised filter set.** `ClearAdvertisedFilters`
  clears `advertised_` but not `filter_conditions_`. If a later adapter advertises the same filter id,
  an old condition can silently apply. Fix direction: clear conditions for filters that are no longer
  advertised or key them by adapter id. Add tests for adapter switch with reused filter ids.
- **Conflict-marker precheck can miss markers beyond git output capture limits.** The staged diff scan
  asks git for the whole cached diff and then searches the captured output. If the command capture layer
  truncates large output, a marker after the cap is indistinguishable from "no marker". Fix direction:
  stream the diff scan or have `GitRepository::CommandResult` expose `truncated` and convert that into
  a warning/blocking precheck. Add a staged-diff fixture with marker after the capture cap using an
  injected command result.

#### 2026-07-13 deep subsystem audit backlog — fifth tranche

##### Editor text core, folding, and visual navigation

- **The editor has two incompatible empty-buffer representations.** `PieceTree::Reset({})` preserves
  a zero-line document, while `ResetFromText("")` creates one empty line; merge result helpers also
  force empty results to `[""]`. Empty new files, deleted merge outputs, and restored buffers can
  therefore disagree about line count, EOF rendering, save output, and cursor bounds. Fix direction:
  define one canonical empty document representation at the text-buffer boundary and convert all
  callers there. Add regression tests for new empty file, empty saved file restore, and a merge that
  resolves to no lines.
- **`PieceTree::AppendToAdd` still trusts inserted text length after compaction.** The cumulative add
  buffer is compacted before overflow, but a single inserted span larger than `uint32_t` can still be
  appended and cast to a 32-bit piece length. Current file-load caps make this rare, but plugin/control
  edit paths should not depend on a caller cap to protect the core text structure. Fix direction: make
  `InsertText` reject or chunk any span above the piece length limit before `AppendToAdd`. Add a
  direct `PieceTree` test with an injected oversized span seam or a lowered test limit.
- **`ReplaceLineRange` materializes whole replacement text before checking live-buffer limits.** A
  formatter or multi-file replace can pass a very large inserted-line vector; the method joins it into
  one string and only then delegates to `InsertText`. This can allocate far beyond the accepted live
  document size. Fix direction: pre-compute replacement byte size with overflow checks and fail before
  materialization, or append chunks through a bounded builder. Add a test using many inserted lines
  whose total crosses the text-core limit.
- **[RESOLVED 2026-07-15] Bracket matching allocates a full line-count scratch vector for a bounded
  scan.** `FindBracketMatch` now materializes only the `[caret-max, caret+max]` window and indexes it
  through a `WindowLines` accessor that maps absolute line numbers onto the slice via a `base` offset;
  the O(file) `views.assign(line_count, {})` is gone. `FindBracketMatchInLines` keeps its absolute
  (base-0) contract for the existing tests. See "Fixed in the 2026-07-15 cross-subsystem speed pass".
- **Bracket matching can synchronously tokenize lines far outside the visible syntax cache.** The
  suppression path calls `HighlightedLineTokens` for every probed bracket position, unlike folding's
  non-forcing `HighlightedLineTokensIfCached` approach. A bracket search across a cold window can force
  highlight work and evict visible-line tokens. Fix direction: hoist a cached-token span per scanned
  line and use the non-forcing accessor, accepting unsuppressed brackets when tokens are cold. Add a
  test with brackets inside comments beyond the highlight cache and a perf counter for forced misses.
- **Fold dedupe keeps only one fold range per opener line.** `SortDedupeRangesByOpener` drops every
  range after the first opener match, preferring bracket over indent by source order and then the
  widest closer. Languages with multiple foldable constructs on one physical line, or generated code
  with `if (...) { ... } else { ... }`, can lose a valid fold target. Fix direction: keep multiple
  ranges per opener when their closer/source differ and make toggle commands disambiguate by row or
  innermost range. Add folding fixtures with two bracket ranges sharing an opener line.
- **Fold collapse remap keys on exact opener/closer pairs, so nearby edits expand stable logical
  folds.** The remap shifts previous ranges by net line delta, but any edit inside a collapsed block
  that changes only the closer line, or a syntax/highlight change that changes source classification,
  loses the collapsed state. Fix direction: persist collapsed folds by opener identity plus a fuzzy
  range match, or explicitly mark edits inside collapsed ranges as preserving that collapsed opener.
  Add tests for inserting and deleting lines inside a collapsed fold.
- **Indent guide generation can create unbounded per-row guide runs for pathological leading
  whitespace.** `ComputeIndentGuides` emits one guide for every `indent_width` up to the visible
  line's leading indent. A single line with hundreds of thousands of spaces can allocate and sort a
  huge scratch vector during render preparation. Fix direction: cap guides by visible columns or editor
  horizontal extent. Add a viewport fixture with an overlong indented line.
- **Single-line visual helpers still operate on byte columns for bracket suppression and guide
  anchoring.** Several editor helpers pass byte offsets directly into token and line-layout data. That
  is fine for ASCII brackets, but adjacent multibyte text can make UI affordances appear offset when
  byte, codepoint, and cell columns diverge. Fix direction: centralize byte-to-cell mapping for all
  cursor-adjacent decorations and use it consistently. Add Unicode layout tests with brackets beside
  combining marks and wide glyphs.

##### Runtime syntax and highlighting

- **Runtime syntax regex source length is uncapped before PCRE compilation.** Plugin definitions can
  provide very large filename/header/signature/rule patterns; `JoinSyntaxPatterns` and
  `CompileSyntaxRegex` materialize and compile them on the main reload path. Fix direction: enforce a
  per-pattern and joined-pattern byte cap before compilation. Add tests with oversized pattern strings.
- **Runtime syntax matching has no per-line match count budget.** `FindAllRegex` collects every match
  for every pattern rule on lines up to 100 KiB. A rule matching single bytes can push 100k matches,
  and a definition with many such rules multiplies the work. Fix direction: stop after a per-rule and
  per-line match budget and mark the line as partially highlighted. Add a pathological syntax rule perf
  test.
- **Syntax region detection runs every sibling start regex at each cursor position.** `FindEarliestRegionStart`
  tries all region rules for the current parent, then the highlighter advances to the next delimiter
  and repeats. Definitions with many sibling regions can turn one line into O(region_count * matches)
  regex work. Fix direction: merge sibling start regexes where possible or add an ordered budget with
  instrumentation. Add a synthetic definition with many region starts and a long line.
- **[RESOLVED 2026-07-15] Definition source fingerprinting reads all syntax files every reload check.**
  Replaced `DefinitionSourceFingerprint` with the `SyntaxSourceFingerprint` cache object
  (`path → {mtime,size,content_hash}`): unchanged files reuse the cached hash instead of being
  re-read, so a poll that finds nothing changed reads no source bytes, while the fingerprint stays a
  pure function of paths+contents (byte-for-byte-equivalent change detection). See "Fixed in the
  2026-07-15 syntax-reload speed pass".
- **[RESOLVED 2026-07-15] Duplicate syntax directories load the same definition file multiple times.**
  `DiscoverDefinitionFiles` now dedups both the directory list and normalized file keys (first
  directory wins → deterministic precedence). See "Fixed in the 2026-07-15 syntax-reload speed pass".
- **Plugin syntax definitions can shadow built-in filetypes without an explicit override contract.**
  Runtime definitions are appended before built-ins and detection returns the first matching
  definition. A broad plugin filename regex can take over common filetypes accidentally. Fix direction:
  require explicit `overrides = true` metadata for a runtime definition that uses an existing
  filetype, or show a reload warning. Add detection tests for a broad `.*` plugin definition.
- **[RESOLVED 2026-07-15] Lazy built-in regex compilation can still happen on a visible-line cache
  miss.** Cold-filetype compile is now prewarmed off the UI thread on tab switch via
  `runtime_syntax::CompileDefinition` + `HighlightPrefetchService::PrewarmForViewport` (gated on the
  viewport identity inside the service, so detection runs once per switch). Behavior unchanged
  (idempotent `std::call_once`); only compile timing moves off the render path. See "Fixed in the
  2026-07-15 syntax-reload speed pass".
- **Highlight prefetch requests dedupe by viewport pointer rather than document identity.** Two splits
  over the same document submit separate background requests, while a destroyed viewport address can be
  reused later and collide with an old unstarted key. Revision checks drop stale results, but worker
  time is wasted and pointer reuse makes dedupe reasoning fragile. Fix direction: key by stable
  document id plus viewport generation. Add tests for split panes and closing/reopening a viewport
  while a prefetch is queued.
- **Highlight prefetch callbacks capture `this` without an explicit lifetime token.** Posted lambdas
  call `results_`, `checkpoint_results_`, and `wake_` through the service pointer. If a future owner
  destroys the service without a complete executor drain, this becomes a use-after-free. Fix direction:
  capture a shared cancellation state and make `Shutdown`/destructor invalidate it before draining.
  Add a lifecycle test that destroys the service with queued requests.
- **Deep-jump approximate tokens can be visible longer than the checkpoint backfill cadence.** When
  `HighlightStateBeforeLine` returns an approximate state, the token cache still stores the resulting
  line colors; it is only cleared when checkpoint install advances. If the executor is saturated, users
  can see stale lexical state for distant lines. Fix direction: tag approximate token-cache entries and
  prefer recomputation or visible "highlighting pending" state once the exact checkpoint arrives. Add a
  test with a multi-line comment beginning before a deep jump.

##### Compare and merge models

- **Large hunk alignment fallback pairs unrelated lines by position.** When the hunk matrix exceeds
  `kMaxHunkAlignmentMatrixCells`, `AlignHunkLines` pairs the first `min(deleted, inserted)` rows as
  modified without similarity checks. The exact path would often emit delete/insert rows instead. Fix
  direction: run a cheap similarity gate on fallback pairs or prefer delete+insert over low-confidence
  positional pairs. Add a large-hunk fixture with unrelated reordered blocks.
- **Compare model row and line fields are `int` even though inputs are `size_t`.** Very large generated
  diffs can overflow row indices, hunk row ranges, or line numbers when casting from vector sizes. Fix
  direction: keep model indices as `std::size_t` internally and clamp only at rendering boundaries.
  Add an injected model-builder test around `INT_MAX` using synthetic ops.
- **Intraline diff silently degrades to whole-line spans without exposing why.** Oversized lines and
  DP-budget fallback both mark broad spans, but the UI/profile only tracks some codepoint/token call
  counts. Users cannot tell whether a line is fully changed or just too expensive to refine. Fix
  direction: record a per-row `intraline_truncated` flag and show a subtle status/tooltip. Add tests
  for long-line and DP-budget fallback rows.
- **`Both` merge choices concatenate sides without provenance or separator rows.** For real conflicts,
  `BothIncomingFirst` and `BothCurrentFirst` append one side's lines directly after the other. Adjacent
  edits can become ambiguous or syntactically fused when saved. Fix direction: decide whether "both"
  should insert conflict-style separators, blank separators, or remain raw but show an explicit warning.
  Add tests for no-newline-at-boundary and adjacent one-line edits.
- **Merge grouping treats touching delete/insert ranges differently from equal-column insertions.**
  Groups join when `base_start < group_max_end`, with a special case only for insertions at the same
  base column. A delete ending exactly where another side inserts can split into separate hunks even
  though users may need to resolve the interaction together. Fix direction: review whether touching
  ranges should join when at least one side changes content at the boundary. Add merge fixtures for
  delete-at-line-N plus insert-at-line-N.

##### Persistence and cross-platform state

- **Persisted paths record a platform tag but do not use it for host-specific decoding.** `ReadPath`
  validates the tag and then constructs a native `std::filesystem::path` from the stored string. A
  Windows path restored on POSIX becomes a relative-looking `C:/...`, and a POSIX path restored on
  Windows can lose intended root semantics. Fix direction: decode into a typed persisted path first and
  decide per field whether cross-platform restore is supported. Add fixtures for Windows-on-Linux and
  POSIX-on-Windows path records.
- **Persisted strings are length-checked but not UTF-8-validated.** Settings ids, tab titles, paths,
  and prompt strings can be decoded from a corrupted record with invalid byte sequences and then flow
  into render/search/plugin surfaces. Fix direction: validate UTF-8 at typed-record boundaries or
  replace invalid sequences with U+FFFD before UI use. Add persistence fuzz cases with invalid string
  payloads.
- **Persisted capability flags are parsed but not enforced by the generic reader.** The header exposes
  `capability_flags`, but `PersistedRecordReader::DecodeRecordFile` only checks version and CRC.
  Callers can accidentally ignore "requires feature X" bits and partially load records they do not
  understand. Fix direction: pass supported capability masks into typed readers and fail unknown
  required bits. Add a fixture with a future required flag.
- **Persisted record CRC is unkeyed and only detects corruption, not wrong-file substitution.** Any
  valid record body with a matching CRC can be copied into another persisted slot if the typed reader
  does not include its own root tag. Fix direction: include a record-kind tag in the body and verify it
  at every typed reader entry. Add tests that try to read project config bytes as session bytes.
- **`ReadAllBytes` trusts `tellg` size until the final read check.** Files that shrink between
  `tellg` and `read` fail cleanly, but files that grow are read only to the old size and then parsed as
  a complete record if the prefix is valid. Fix direction: reopen or stat after read, or reject when
  EOF is not reached immediately after the expected byte count. Add a race-injected reader test.
- **Backup fallback can mask repeated primary corruption indefinitely.** A corrupt primary plus valid
  backup returns `used_backup`, but no automatic quarantine/repair policy is attached at the generic
  layer. Callers that ignore `used_backup` can keep loading stale backup state every launch. Fix
  direction: centralize backup-recovery reporting and repair/quarantine decisions in
  `PersistenceService`. Add an integration test that corrupts primary and verifies visible recovery
  status.

##### Rendering, plugin display lists, and image assets

- **Display-list validation accepts huge finite rectangles that are later clamped silently.** Replay
  clamps to +/-1,000,000 before int conversion, but validation does not report geometry far outside
  content bounds. A plugin can create impossible hit/paint extents and get clipped in surprising ways.
  Fix direction: validate against content size plus a small overscan margin during interop parsing.
  Add display-list tests for 1e20 coordinates and negative dimensions.
- **Display-list content dimensions are not finiteness-checked.** Validation checks op rects and point
  arenas, but `content_width`/`content_height` are hashed and may be consumed by layout surfaces. NaN or
  infinities there can poison scroll extents even if every op is valid. Fix direction: require finite
  non-negative content dimensions in `ValidateDisplayList`. Add tests for NaN/Inf content size.
- **Texture-cache decode failures are permanent by content hash.** `in_flight_or_failed_` keeps failed
  hashes forever until `Clear()`. That is correct for immutable bytes, but if a plugin reuses a hash
  incorrectly for corrected bytes, the image never retries and no diagnostic identifies the bad cache
  key. Fix direction: include declared byte size/format in the request key or reject plugin-supplied
  hash reuse with mismatched metadata. Add a test that requests same hash with bad then good bytes.
- **Raw RGBA plugin images are copied twice before upload.** `Request` takes `bytes` by value, the
  worker moves it into `WrapRgba8`, then `WrapRgba8` copies into `out.rgba`, and SDL copies again on
  upload. Large plugin images pay avoidable memory bandwidth. Fix direction: move the validated RGBA
  buffer directly into `Decoded::rgba`. Add a microbenchmark for max-size RGBA image request/upload.
- **Raster decode dimensions can overflow SDL pitch assumptions if caps change.** `SDL_UpdateTexture`
  receives `decoded.width * 4` as an `int` expression. Current `kMaxDimension` likely keeps this safe,
  but the invariant is local to the cache and not asserted at the pitch calculation. Fix direction:
  compute pitch with checked `std::size_t` and reject if it exceeds `INT_MAX`. Add a lowered-cap test
  around pitch overflow.

##### Platform filesystem, trash, and subprocess helpers

- **Linux trashing writes metadata before the file move and never fsyncs either directory.** A crash
  after `.trashinfo` write but before or during `MovePath` can leave orphan metadata, and a crash after
  move can lose directory-entry durability. Fix direction: move to a temporary trash name, fsync the
  files/info directories where supported, then publish metadata. Add a fault-injection test around move
  failure and crash-simulation hooks.
- **Linux trash move can cross filesystems through a generic move helper without preserving trash
  semantics.** If `XDG_DATA_HOME` is on a different filesystem, `MovePath` may copy/delete or fail
  depending on helper behavior. Copy/delete to trash changes failure modes and can be very expensive
  for large directories. Fix direction: detect cross-device moves and use the freedesktop topdir trash
  for the source mount, or surface a clear "permanent delete required" failure. Add a temp mount seam
  or mocked `EXDEV` test.
- **macOS trash name reservation is not atomic.** `UniquePathInDirectory` checks `exists` and returns a
  path, then `MovePath` uses it later. Two concurrent trash operations for same-named files can race
  into the same destination. Fix direction: use platform trash APIs or an atomic rename/link
  reservation strategy. Add a macOS-specific concurrency test.
- **Windows recycle-bin deletion normalizes to an absolute path but returns the original source path as
  the result.** `MovePathToTrashWindows` cannot report the recycle-bin item path, so callers may treat
  the source as still meaningful. Fix direction: make the result explicitly `std::nullopt`/logical on
  platforms that cannot return a destination, or query the shell item if needed. Add Windows UI tests
  that the success message does not offer to open the old path.
- **`ReadPathType` collapses stat errors into `Missing`.** Permission denied, symlink loops, and
  transient I/O errors all return `PathType::Missing`. Callers such as directory discovery and path
  validation can silently skip paths they should report as inaccessible. Fix direction: return
  `{type,error}` or add a distinct `Inaccessible` type for user-facing flows. Add filesystem tests for
  EACCES/ELOOP with a platform seam.
- **`ListDirectory` drops iteration errors and partial-read status.** If a directory becomes
  unreadable or mutates during iteration, the function returns whatever was collected with no
  truncation/error flag. File pickers and plugin loaders can show incomplete lists as complete. Fix
  direction: return entries plus status, and have callers surface partial results. Add a directory
  iterator fault-injection test.
- **`CaptureTreeSnapshot` does not count root files against `max_entries`.** The traversal budget is
  incremented only inside recursive directory iteration. A call with many root files can exceed the
  advertised entry cap before any budget check. Fix direction: count every appended root and child
  entry against the same budget. Add snapshot tests with many file roots and `max_entries = 1`.
- **`CaptureTreeSnapshot` skips root directory entries themselves.** Directory roots are traversed but
  not appended, while file roots are appended. Consumers that compare snapshots cannot tell whether a
  watched root directory was deleted/recreated versus only its children changing. Fix direction: decide
  whether snapshots are "contents only" or "roots plus contents" and make all callers explicit. Add
  watcher tests for root directory replacement.
- **`CaptureTreeSnapshot` stops entirely after the first over-budget root.** Once any root exhausts
  the budget, later roots are not scanned at all. Multi-root workspaces can therefore starve smaller
  roots behind one huge tree. Fix direction: allocate per-root budgets or round-robin roots until a
  global cap is reached, and return per-root truncation. Add a snapshot fixture with one huge and one
  tiny root.
- **The project subprocess helper is a transparent alias with no policy enforcement.** `project::RunSubprocess`
  simply calls `platform::RunSubprocess`, so project-level command timeouts, output caps, sandbox
  expectations, and logging have to be remembered by every caller. Fix direction: move default
  project-safe options into the helper and require opt-out for exceptional commands. Add tests that
  project helpers apply timeout/output defaults.

#### 2026-07-13 deep subsystem audit backlog — sixth tranche

##### Workspace prompts, command surfaces, and sidebar orchestration

- **Dirty prompts store tab indices instead of stable tab identities.** `PromptSurfaceService` records
  `tab_index`, `target_tabs`, and `dirty_tabs` as positional indices. If tabs are reordered, closed by
  another action, or moved between groups while the prompt is visible, confirming the prompt can save or
  close the wrong tab. Fix direction: store stable tab ids or `{path, content_revision, group_id}`
  tokens and resolve them at confirm time. Add tests for close-tab prompt followed by tab reorder and
  close-other-tabs prompt followed by a new tab insertion.
- **Dirty prompt creation does not validate every target index.** `ShowDirtyPromptForTabs` trusts the
  provided `target_tabs` and `dirty_tabs` vectors after only checking for emptiness. A stale caller can
  create a prompt with out-of-range indices, leaving the confirm path to clamp, skip, or hit the wrong
  tab later. Fix direction: validate and normalize all target/dirty indices at prompt creation, or make
  the confirm action re-resolve stable tab ids. Add unit tests with target `size()` and duplicate
  indices.
- **Prompt focus restoration is blind to surface lifetime changes.** Dirty and generic prompts capture
  `previous_focus` and restore it on dismiss. If the project closes, the editor group disappears, or the
  previous focus target is disabled while the overlay is open, dismiss can restore focus to an invalid
  surface. Fix direction: route focus restore through a `FocusService::RestoreIfValid` helper with a
  fallback priority. Add tests for prompt open, project close, and dismiss.
- **Generic prompt path payloads are only lexical-normalized.** `OpenPromptSurface` accepts a path and
  stores `path.lexically_normal()` with no existence, containment, or path-type classification. Prompt
  actions that delete, rename, or authorize tool access must each remember their own validation. Fix
  direction: store a typed `PromptPathTarget` with root containment and expected path type. Add prompt
  tests for missing, outside-project, symlink, and wrong-type paths.
- **External URL prompts have no scheme or length gate.** `OpenExternalUrlPrompt` accepts any non-empty
  string and puts it in `detail`. A plugin/sidebar/terminal source can present `file:`, `javascript:`,
  oversized, or control-character URLs to the user. Fix direction: validate allowed schemes and cap URL
  length before opening the confirmation prompt. Add tests for `https`, `file`, newline-containing, and
  cap+1 URLs.
- **Sidebar tree requests can target arbitrary absolute roots.** `ParseSidebarViewRequest` stores
  `args[1]` directly for the tree root. That may be a useful file-browser feature, but it bypasses the
  project traversal policy and can expose large or sensitive filesystem roots through the sidebar. Fix
  direction: decide whether tree roots are project-contained by default and require an explicit
  external-root mode if not. Add command tests for `tree /`, `sidebar-show tree ../outside`, and
  project-relative roots.
- **Plugin sidebar IDs can collide with built-in sidebar IDs.** `RegisterSidebar` validates uniqueness
  only inside the plugin sidebar map. `FindSidebarView` always checks built-ins first, and
  `SidebarViewIds` sorts/uniques the combined list, so a plugin registering `tree` or `git` becomes
  unreachable or ambiguously hidden. Fix direction: reject plugin sidebar IDs that match built-in view
  ids. Add plugin registration tests for `tree`, `search`, and a valid plugin id.

##### Status bar, settings overlay, and notifications

- **[RESOLVED 2026-07-15] Repository availability status can stay stale after `git init` or `.git`
  removal.** Removed the `project_root`-keyed cache entirely — `is_git_repo_valid` is one cheap `.git`
  stat, so it now runs directly per refresh (only when no git snapshot exists). Regression:
  `WorkspaceStatusBar/RepoAvailabilityReflectsInSessionGitInit`.
- **LSP status tone is derived by substring search for `Ready`.** `StatusBarModelService` treats any
  text containing `Ready` as default tone. Labels such as `Not Ready`, `Readying`, or a server name
  containing that word are misclassified. Fix direction: have the LSP service return a typed status
  severity instead of parsing display text. Add tests for ready, starting, errored, and "not ready"
  labels.
- **Plugin status items with equal alignment and priority have unstable order.** `ResolveStatusItems`
  uses `std::sort` and only compares alignment and descending priority. Equal-priority items can
  reorder between revisions or platforms, causing visual jitter. Fix direction: add stable tie-breakers
  (`plugin_id`, `id`) or use `stable_sort` over registration order. Add status-item ordering tests with
  equal priority contributions.
- **Settings overlay pane cycling assumes every mode has three panes.** `CycleFocusedPane` wraps over
  a fixed count of three even in Help/About mode, which has no filter/value editing panes. Keyboard
  navigation can focus invisible panes and make input handling mode-dependent in surprising ways. Fix
  direction: derive pane count from `mode_` and visible controls. Add navigation tests for Settings and
  Help/About modes.
- **Settings value edit target can go stale across settings rebuilds.** The service stores
  `editing_row_id_`, but settings rows can disappear after a query change, plugin reload, or settings
  registry rebuild. Commit paths must revalidate the row; otherwise an edit can apply to a hidden or
  removed setting id. Fix direction: cancel value edit when `editing_row_id_` is no longer present
  after `RebuildSettingsRows`. Add tests for plugin setting removed while editing.

##### Plugin registries, tools, tasks, and AI/auth contributions

- **The per-kind plugin contribution cap is too high for UI-backed registries.**
  `kMaxPluginContributionsPerKind` is 100,000 for every kind. That protects against infinity but still
  lets one plugin register enough commands, settings, snippets, models, or providers to make UI rebuilds
  and memory use impractical. Fix direction: use per-kind caps sized to product needs, e.g. low
  hundreds for visible UI contributions and separate caps for data-heavy providers. Add registration
  tests for each cap.
- **Malformed LSP `initialization_options` and `settings` JSON is silently ignored.** The parser leaves
  the fields null when JSON parsing fails, but registration still succeeds. Plugin authors get a server
  launched with missing configuration and no actionable error. Fix direction: reject present-but-invalid
  JSON like launch config parsing already does. Add plugin registration tests for malformed JSON.
- **Plugin task registrations lack a runtime/execution contract in the host registry.** Tasks are
  stored as static contributions, but there is no clear runner, cwd containment, env policy, or result
  channel at the registry boundary. Later execution code can easily grow ad hoc subprocess behavior.
  Fix direction: define task execution through `ProjectBackgroundExecutor` plus explicit cwd/env/input
  fields before exposing run commands broadly. Add spec/tests for task cwd, args, and cancellation.
- **Plugin tool platform strings are free-form.** `ParseToolRegistration` accepts any `platform`, while
  `FindTool` later exact-matches host strings such as `linux`, `macos`, or `windows`. Typos silently
  make tools undiscoverable. Fix direction: validate platform against a known enum and reject unknown
  values at registration. Add tests for `linux`, `darwin`, `macos`, and empty platform.
- **Tool downloader blocks the caller while hashing.** `Download` posts SHA computation to
  `background_executor_` and immediately calls `future.get()`. If `Download` runs on the UI thread, a
  large cached tool still freezes the shell until the digest subprocess finishes. Fix direction: make
  downloads fully async with progress/completion callbacks, or require callers to dispatch `Download`
  off-thread. Add a test with a fake slow hash worker.
- **Tool downloader only implements local/file sources despite storing `download_url`.** HTTP(S) URLs
  simply fail `ResolveToolSourcePath`, while the registry field name suggests network download support.
  Fix direction: either rename the field to `source_path` until network download exists, or implement a
  bounded HTTPS downloader with TLS and progress. Add tests that HTTP URLs produce an explicit
  unsupported-source error.
- **Tool cache APIs do not verify hash on `GetCachedTool`.** Once a caller has a tool id, `GetCachedTool`
  returns any existing cache file without checking the expected digest. A later launch path can use a
  tampered cache entry if it bypasses `Download`. Fix direction: require expected sha for cache lookup
  or make cached entries content-addressed by digest. Add tests for tampered cached files.
- **AI provider model lists and external-agent capabilities can still be enormous.** The parsers cap
  at 100,000 entries, which is far beyond any usable selector and can dominate memory/UI time. Fix
  direction: use product-sized caps and truncate with a registration warning or reject. Add parser tests
  for cap+1 models/capabilities.
- **Auth provider registration does not require any lifecycle function.** A provider with no login,
  refresh, or logout callback is accepted as a visible auth provider but cannot do useful work. Fix
  direction: require at least `login` or explicitly classify metadata-only auth providers. Add parser
  tests for label-only auth providers.

##### Debug pane, watch expressions, and value trees

- **Debug watch expressions are not deduplicated or normalized.** The same expression can be added many
  times and evaluated separately on every stop. That is sometimes intentional, but accidental repeated
  adds from keyboard/control flows waste DAP requests. Fix direction: decide whether duplicates are
  allowed; if allowed, show duplicate rows distinctly and cap them. Add tests for repeated `foo`.
- **Watch evaluation results are applied by positional index with no generation.** `ApplyEvaluate`
  writes to `expression_root_ids_[index]`. If expressions are edited/removed while evaluate requests
  are in flight, a late response can update a different expression now occupying that index. Fix
  direction: attach an evaluation generation and expression id to each request. Add tests for remove
  expression before response and edit expression before response.
- **Debug value rows are rebuilt recursively without a visible row cap.** `FlattenInto` recursively
  emits every expanded child and descendant. A large expanded tree can build thousands of rows on every
  variable update or selection change. Fix direction: virtualize rows or cap flattened rows with "more"
  at the view layer. Add a synthetic variables tree perf test.
- **Debug value node ids can wrap.** `next_id_` intentionally never resets, but it is a 32-bit id. Long
  sessions with repeated stops and large variable trees can eventually wrap and alias stale rows or
  async responses. Fix direction: use 64-bit ids or detect wrap and clear generations. Add a lowered-id
  wrap test.
- **Debug breakpoint rows collapse multiple metadata fields into one secondary string.** Conditions,
  hit counts, log messages, and verification failures overwrite or concatenate in priority order.
  Users can miss that a breakpoint has both a condition and log message, or a hit condition plus
  failure. Fix direction: model secondary fields separately in the row view model and let render choose
  concise presentation. Add view-model tests for all metadata combinations.

##### Tests, icons, decorations, and terminal remaining edges

- **Test results are stored without a cap or generation.** `RecordTestResult` appends forever, and
  results from old discovery sessions remain mixed with current tests. Long-running test sessions can
  grow memory and display stale failures. Fix direction: cap result history per test and tag results by
  run/discovery generation. Add tests for repeated runs and provider reload.
- **Test items are not owned by provider id.** The controller stores a flat item list and has only
  `Clear()`. Plugin reload or one provider failing rediscovery cannot remove just that provider's
  tests. Fix direction: store items by provider and discovery generation. Add tests with two providers
  where one reloads.
- **File icon theme rules are last-writer-wins without priority or diagnostics.** `Rebuild` overwrites
  `by_name_`/`by_extension_` entries as it walks contributed themes. Two plugins matching the same
  extension silently depend on registration order. Fix direction: add explicit priority or report
  duplicate icon rules. Add tests with two themes for `.rs`.
- **Plugin decoration aggregate truncation keeps lowest-line entries, not highest-priority entries.**
  After merging multi-owner decorations, vectors are resized to `kMaxMergedPerKind`. Gutter marks are
  sorted by line first, then priority, so high-priority marks on later lines are dropped before
  low-priority marks near the top. Fix direction: define truncation per visible window or priority
  bucket instead of whole-file first-N. Add tests with cap+1 marks where the highest priority is late.
- **Decoration retarget can overwrite an existing destination decoration for the same owner.** During
  `RetargetPathPrefix`, replacements are assigned with `owner_entries[new_key] = moved`. If the owner
  already had decorations at the new path, they are replaced rather than merged or conflict-reported.
  Fix direction: merge same-owner moved and existing decorations or clear/rebuild with a deterministic
  policy. Add tests for renaming `a` to `b` when `b/file` already has decorations.
- **Terminal pending reply buffer drops query responses silently after 64 KiB.** The cap prevents memory
  growth, but once full, DSR/DECRQM/color replies are discarded with no state reset or diagnostic. A
  query-heavy app can observe missing replies and behave incorrectly. Fix direction: coalesce repeated
  replies, expose a dropped-reply counter/status, or apply backpressure. Add a terminal test flooding
  private-mode queries past the cap.
- **Terminal CPR/DECRQM replies can be generated while the backend is no longer running.**
  `SendBytesLocked` buffers replies from parser paths without checking backend liveness; the later
  flush may write stale query responses after process shutdown/restart. Fix direction: tag pending
  replies with terminal backend generation and drop on stop/restart. Add tests for query immediately
  followed by terminal stop.

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

- **LSP overlapping registrations can create unreachable duplicate server entries.** The canonical
  key is `language_ids.front()`. Registering one plugin as `["cpp", "c"]` and another as
  `["c", "cpp"]` creates two `servers_` entries, then aliases both ids to whichever registration ran
  last. The older entry can remain in `servers_` with a live or retiring client but no alias path to
  reach it. Fix direction: canonicalize language-id sets, or reject/retire any existing entry whose
  alias set intersects the new registration. Add tests for swapped-order language ids and partial
  overlaps.
- **LSP sandbox changes are ignored by the registration equality check.** `RegisterServer` reuses an
  existing client when language ids, command, root URI, cwd, initialization options, and settings
  match; the `SubprocessSandbox` is not part of the comparison. A plugin or settings reload that
  tightens process limits or permissions can leave the old less-restricted server running. Fix
  direction: include a stable sandbox fingerprint in the equality check and force retirement when it
  changes. Add a test that changes only sandbox limits.
- **Retired LSP clients can accumulate until an unrelated drain path runs.** A changed registration
  calls `BeginShutdown()` and pushes the old client into `retiring_clients_`, but plain
  `RegisterServer` does not call `CollectRetiredClients`. If reloads happen without a subsequent
  `DrainCallbacks` or shutdown pass, joined clients stay resident longer than needed. Fix direction:
  collect after each retirement or enqueue a bounded cleanup pass. Add a test that repeatedly
  re-registers a server and asserts the retired list drains.
- **`DidClose` drops local document-version state before the close notification is known to be
  queued.** The local version entry is erased before the send path confirms it accepted the
  notification. If the client is shutting down or the queue rejects the send, the server can still
  think the document is open while the host has lost the version gate that would classify later
  diagnostics. Fix direction: erase only after queue acceptance, or store a `close_pending` state
  until shutdown/close completion. Add a test with a stopped transport that rejects `didClose`.
- **Live LSP protocol errors are not surfaced through `LastServerError`.** `LastServerError` returns
  the manager entry's cached `last_error`, which is mainly start/exited state. A running client can
  accumulate transport/protocol failure information in the client itself while status UI still reports
  an empty manager error. Fix direction: merge `entry.last_error` with `entry.client->LastError()` and
  define precedence. Add a test where a running fake client sets a late protocol error.
- **[RESOLVED 2026-07-15] Serializing full JSON settings on every LSP registration is avoidable
  reload-path work.** `RegisterServer` now compares `initialization_options`/`settings` via a defaulted
  structural `JsonValue::operator==` (no allocation) instead of four `SerializeJson` calls — which also
  fixes a latent object-key-order sensitivity. See "Fixed in the 2026-07-15 cross-subsystem speed pass".

#### DAP manager/session/debug callback edges

- **Stopping all debug sessions blocks the shell on adapter shutdown joins.** `DebugService::StopAllDebugging`
  calls `manager.BeginShutdownAll()` and then `manager.ShutdownAll()`, whose session/client teardown
  joins adapter I/O threads. A wedged adapter or slow pipe close can freeze the UI during the stop-all
  command. Fix direction: use the same bounded drain model as plugin teardown: request shutdown,
  return to the event loop, and prune/join in bounded slices. Add a fake adapter that delays I/O exit.
- **Restart fallback blocks the shell while replacing the active session.** When an adapter lacks
  `supportsRestartRequest`, `DebugService::Restart` calls `LaunchSession(..., replace=true)`;
  `ReplaceActiveSession` shuts down the old session synchronously before launching the new one. Fix
  direction: model fallback restart as an async two-phase operation: terminate old session, show
  restarting state, launch when the bounded drain completes. Add a slow-terminate adapter test.
- **Clearing the active DAP entry can synchronously destroy a live session.** Manager paths that
  replace or clear sessions can call destructors from UI-initiated commands. Destructors are allowed
  to join transport threads, so a seemingly cheap focus/replace/close action can inherit subprocess
  teardown latency. Fix direction: separate "remove from visible list" from "destroy transport" and
  drain on the existing callback tick. Add tests with a fake session whose destructor blocks until a
  latch is released.
- **DAP session ids are `int` and can wrap.** `next_session_id_` increments forever, while console
  channels, active-session ids, and control notifications use that id as an identity. Long-running
  debug-heavy sessions can eventually reuse a prior id and route output or termination events to the
  wrong console. Fix direction: use `std::uint64_t` session ids or detect wrap and refuse/reseed. Add
  a lowered-wrap test seam.
- **Adapter retention can remove a type while sessions using that type are still running.** Adapter
  registry refresh paths can retain only currently advertised adapter types; an active session that
  was launched from a removed type keeps running, but restart, relaunch, or UI labelling can no longer
  resolve the adapter entry. Fix direction: pin adapter metadata while any session of that type is
  active, or mark it retired-but-session-owned. Add a test with an active session and a plugin reload
  that drops its adapter.
- **Function breakpoint verification is positional but not bounded to the requested count.**
  `on_function_breakpoints_verified` pushes every returned breakpoint, then applies it against the
  requested names. A non-conformant adapter returning more results than requested can update extra
  store entries or leave confusing unmatched verification state, depending on store behavior. Fix
  direction: mirror line-breakpoint handling: ignore results beyond `requested_names.size()` and log a
  protocol warning. Add a fake adapter response with cap+1 function breakpoint results.
- **Function breakpoint by-name commands affect only the first matching name.** `RemoveFunctionBreakpointByName`,
  `ToggleFunctionBreakpointByName`, and `SetFunctionBreakpointConditionByName` search the first exact
  name. If duplicates are allowed, commands can mutate the wrong duplicate; if duplicates are not
  intended, the store should reject them earlier. Fix direction: either enforce unique names or address
  rows by stable id. Add tests for two function breakpoints named `main`.
- **REPL evaluation callbacks are not generation-gated.** `EvaluateRepl` captures the session id and
  console label, then appends results when async evaluate/variables callbacks arrive. If the session
  ends and a new session reuses or reclaims nearby UI state before the callback drains, stale results
  can be printed into an obsolete console tab. Fix direction: include session generation or verify
  `SessionById(session_id)` is still the same active/evaluating session before appending. Add a test
  that terminates the session before the evaluate response.
- **REPL structured-result expansion can spam unbounded console text.** A result with
  `variablesReference` triggers one eager page of variables and appends each child line to the console.
  The page size is bounded, but child names/values are not truncated at the console handoff, and a
  malicious adapter can return very large strings for every child. Fix direction: truncate REPL child
  output by line and aggregate byte budget before appending. Add a fake evaluate result with large
  child values.
- **Debug hover evaluation is not tied to frame generation.** `EvaluateHover` uses the hover model's
  own generation, but it does not compare against `frame_generation_`. A frame switch can clear the
  model, then a late hover response may still resolve into the hover cache if the hover generation
  happens to match the latest hover slot. Fix direction: capture both hover generation and frame
  generation/frame id, then drop on either mismatch. Add a test for hover request followed by frame
  focus before response.
- **Variable and watch edit commits do not report adapter failures.** `CommitVariableEdit` and
  `CommitWatchEdit` leave edit mode immediately; on `SetVariable` failure, the callbacks only redraw.
  Users see the old value reappear with no feedback, and automated control cannot distinguish failure
  from a no-op. Fix direction: keep a per-row failure/status field or append a debug-console message.
  Add fake adapter tests for failed `setVariable`.
- **Watch/variable child fetches mark loading errors without requesting redraw on the no-session path.**
  `FetchVariablesPage` and `FetchWatchChildren` call `MarkChildrenError` when no session exists, then
  return without `request_debug_pane_redraw`. The UI can leave the row showing "loading" until some
  unrelated event repaints. Fix direction: request a debug-pane redraw after all synchronous error
  transitions. Add tests for toggling a variable row after session teardown.
- **Debug stop projection can focus source paths outside the active project.** `BuildExecutionView`
  trusts `frame.source.path`, and `ProjectStop` calls `focus_source_location` on it. An adapter can
  send absolute paths outside the project root, causing the editor to open arbitrary files when a
  debuggee stops. That may be useful for system libraries, but it needs an explicit policy. Fix
  direction: define debug source-opening policy: project-only by default with prompt/setting for
  external files, or read-only external buffers with clear labelling. Add tests for `/etc/passwd` or a
  temp path outside root.

#### Persistence decode and session-state validation

- **Workspace-session active index is not semantically validated during decode.** The raw size value is
  accepted and remapped during restore. That is mostly safe, but it means a corrupt session can hide
  invalid data until restore and cannot report "bad active index" distinctly. Fix direction: validate
  after root decode and normalize with a warning/error state. Add decode tests with an active index far
  beyond root count.
- **Legacy persistence cleanup deletes sidecar files based only on the requested structured path's
  existence.** `RemoveLegacyPersistenceArtifactsFor` removes fixed legacy filenames from the parent
  directory when the structured target exists. If a caller points at an unexpected directory, the
  cleanup can delete unrelated same-named files in that directory. Fix direction: restrict cleanup to
  known app state/config roots or require the target filename to be one of the expected structured
  artifacts. Add tests using a temp directory with unrelated `project.state.legacy`.
- **Debug persisted file-breakpoint paths are not normalized or project-contained during decode.**
  `DecodeFileBreakpoints` accepts any path payload, including absolute external paths or `..`
  relative paths. Later breakpoint restore can display or send breakpoints for paths outside the
  project. Fix direction: normalize and contain-check when hydrating debug state into a project, and
  reject external persisted breakpoints. Add tests for absolute and parent-traversal paths.
- **Persisted selected launch-config index is not clamped to the decoded launch-config count.**
  `SelectedLaunchConfigIndex` decodes independently of `LaunchConfig` records. UI code must remember
  to clamp every time it reads it; a forged high index can select no row or hit fallback behaviour.
  Fix direction: normalize after decode once the launch list is known. Add tests for index == count and
  index >> count.
- **Persisted launch configs accept empty type/request and arbitrary arguments JSON.**
  The decoder stores strings without checking that `type` maps to a registered adapter, `request` is
  `launch`/`attach`, or `arguments_json` parses to an object. Later launch code becomes the error
  boundary. Fix direction: validate when loading into the debug model and annotate invalid configs in
  UI instead of letting them fail at launch time. Add tests for empty type, unknown request, and array
  JSON.

#### Git repository refresh, compare, patch, and commit workflows

- **Git refresh generation does not include the project root.** `RequestRefresh` increments a single
  generation counter and stores `active_project_root_`, but publish/supersede checks compare only the
  numeric generation. A delayed job from root A cannot normally match a newer generation for root B,
  but test/synchronous paths and reset/reopen sequences make the contract fragile. Fix direction:
  carry `(root, generation)` as the identity on every refresh and publish only if both match. Add a
  test switching projects while a refresh job is blocked.
- **`GitRepositoryService::Reset` cancels the shared project background executor.** The service owns no
  private queue; `background_executor_.Cancel()` can drop unrelated queued jobs from commit, patch, or
  other project services if reset runs during project switching. Fix direction: give git refresh a
  cancellable task namespace instead of cancelling the whole executor, or use per-service executors.
  Add an integration test with an in-flight commit/patch job followed by git reset/project switch.
- **[RESOLVED 2026-07-15] Git sidebar outgoing base resolution can run extra subprocesses inside every
  full refresh.** `BuildSidebarSnapshot` now routes through `ResolveOutgoingBaseCached`, which memoizes
  the resolution keyed by `(root, choice, head_oid, branch_name, upstream, repo_available)`. A status
  refresh with unchanged HEAD/branch/upstream serves the base from cache and spawns no git subprocess;
  any HEAD movement or branch/upstream/choice change re-resolves. See "Fixed in the 2026-07-15
  cross-subsystem speed pass". (The config `gh-merge-base` edge — config changed without HEAD moving —
  is accepted staleness until the next HEAD/branch change; folding the refs into one git command is a
  possible further optimization.)
- **Base-reference config values are not constrained before `show-ref`.** `ResolveNamedBranchReference`
  builds ref strings from `branch.<name>.gh-merge-base` and remote config. Weird values containing
  `..`, spaces, or ref metacharacters are passed to git commands. `--verify` protects option parsing,
  but the UI can still show and later diff surprising refs. Fix direction: validate configured base
  names against git refname rules or use `git check-ref-format --branch` in the background. Add tests
  for invalid base config values.
- **Specific outgoing base refs are accepted without existence validation.** `ResolveGitOutgoingBase`
  for `SpecificRef` copies `custom_ref` straight into sidebar state. Later outgoing-file collection
  fails to empty lists with little distinction between "no outgoing files" and "bad base ref". Fix
  direction: validate specific refs in the background refresh, surface an explicit invalid-base error,
  and avoid running outgoing diff when invalid. Add tests for typo refs and option-looking refs.
- **`GitRepository::FileExistsAtRevision` still concatenates `revision:path` into one argument.** The
  call uses `--end-of-options`, but `revision` and path are still a single revspec string. A path with
  syntax meaningful to git rev parsing, or a revision containing a colon from user input, can produce
  ambiguous results. Fix direction: validate revision strings before use or prefer `git cat-file`
  plumbing with separated object resolution. Add tests for paths with colon-like names on platforms
  that allow them and invalid custom revisions.
- **Patch apply preflight and apply are not atomic.** `ApplyGitPatch` runs `git apply --check` and then
  `git apply`. The working tree/index can change between the two commands, especially because the
  operation is on a background queue while other git operations can run. Fix direction: rely on the
  real apply result as authoritative, or serialize patch operations with other git mutators and refresh
  generation checks. Add tests where the file changes after preflight but before apply.
- **Patch apply dispatch reads current repository state from a background thread through callbacks.**
  `DispatchApply` calls `callbacks_.current_repository_state()` inside the executor job. If that
  callback is backed by UI-owned state or takes locks in UI order, it risks races/deadlocks and makes
  the project mutation boundary unclear. Fix direction: capture the repository root/generation needed
  for preflight at request build time and marshal all UI-state reads back through the completion
  mailbox. Add TSAN coverage for patch apply while refreshing git state.
- **Patch apply completions are not marshalled through a mailbox.** Unlike the commit workflow,
  `PatchApplyService::DispatchApply` calls `PublishResult`, `ReportResult`, and UI callbacks directly
  from the background executor job. That mutates command feedback, refreshes compare tabs, and touches
  blame/editor callbacks off the main thread. Fix direction: add a completion mailbox/wake event like
  `CommitWorkflowService`, and run all callbacks on the shell thread. Add TSAN tests for staged hunk
  apply.
- **Pending discard preview stores patch text without a repository/diff freshness re-check at confirm
  time.** `ConfirmPendingDiscard` dispatches the previously generated patch. The request carries
  generations, but there is no user-facing warning if the preview is old and the confirm happens much
  later; the background path reports stale only after queue execution. Fix direction: re-check current
  diff/repository generation before dispatching confirm and close the prompt with explicit stale
  feedback. Add tests for refresh between preview and confirm.
- **Commit workflow captures `CommitWorkflowState&` across a background operation.** `DispatchCommit`
  captures `&state` in the worker and completion lambda. If the overlay/project is closed, switched,
  or the state object is moved before completion drains, the completion can write to the wrong or dead
  state. Fix direction: identify the project and workflow instance by stable generation/id, store
  results in a mailbox independent of the state address, and apply only if the instance is still
  current. Add tests closing the commit overlay and switching projects before a fake commit returns.
- **Commit workflow does not cancel or invalidate an in-flight commit on close.** `Close` sets
  `operation_in_flight = false`, but it does not increment `operation_generation_`. A later completion
  with the same captured generation can still publish output, close/reset fields, or notify after the
  user intentionally closed the workflow. Fix direction: bump the generation on close/project switch
  and decide whether an in-flight commit remains visible in an output channel. Add a test closing while
  the executor job is blocked.
- **Commit success does not verify that HEAD advanced from the captured repository generation.**
  `PublishResult` ignores `repository_generation`; a successful `git commit` can race with a refresh or
  repository switch and still clear the current draft. Fix direction: compare captured root/head/generation
  on completion and only clear the matching workflow. Add tests for project switch and external commit
  during in-flight commit.
- **Commit subject/body are passed as command-line `-m` arguments.** This is functionally correct but
  exposes commit text to process listings on some platforms and can hit argv length limits for large
  bodies. Fix direction: write the message to a temporary file or pipe and use `git commit -F -`, with
  careful cleanup and tests for large bodies. Add a test near platform argv limits.

#### Project search, recents, and file operations

- **Project search helper thread creation is unbounded relative to process-wide workload.** Each search
  can spawn up to eight helper threads inside a background executor job. Multiple subsystems also use
  background work, so a search in a huge repo can occupy cores and delay git/plugin tasks. Fix
  direction: route search through a shared worker pool or expose a global concurrency budget. Add perf
  tests with concurrent search and git refresh.
- **Case-insensitive literal search reports byte columns from lowercased ASCII buffers only.** That is
  correct for ASCII lowercasing, but if Unicode case folding is later added naively, byte offsets will
  no longer line up with original text. Fix direction: lock in an offset-preserving search contract
  before adding Unicode folding. Add regression tests that assert reported columns are original byte
  offsets.
- **Project search result ordering is thread-scheduling dependent.** Results are published in batches
  from multiple helpers as files are claimed by an atomic cursor. Display order can vary between runs
  even with the same indexed file list, which hurts keyboard workflows and tests. Fix direction:
  collect stable `(file_index,line,column)` order before display or sort pending batches on the UI
  side with incremental merge. Add deterministic-order tests with worker limit > 1.
- **Project search progress counts files claimed, not files fully searched.** `files_visited` is
  incremented before file read/search work. Progress can show a file as searched while a slow read or
  regex scan is still running, and final progress can briefly reach total before results drain. Fix
  direction: track claimed and completed separately; display completed. Add tests with a blocking file
  read seam.
- **Project search cancellation does not join helper threads until the worker task returns.** `Stop`
  requests cancel and cancels the task queue, but if a running worker spawned helper threads, they join
  only inside `RunSearch` after each helper observes cancellation. Pathological file reads or regex
  calls can still keep the old search alive while a new run starts. Fix direction: use a shared
  cancellable pool with bounded join latency or make file reads interruptible. Add a stress test
  starting/stopping searches rapidly on a slow filesystem seam.
- **Search pending updates can accumulate large result vectors between UI drains.** The worker caps
  total stored results, but `pending_update_.results.insert` can still move a large number of preview
  strings into one mailbox update if the UI is busy. Fix direction: cap pending-update bytes and keep
  overflow in a service-side result store. Add tests where the UI does not drain until search finishes.
- **Recents are compared and deduped by raw path value.** `RecordProjectOpen`, `RecordFileOpen`, and
  `RecentFilesFor` use direct `std::filesystem::path` equality. The same path can appear multiple
  times via symlinks, relative-vs-absolute spelling, case differences on case-insensitive filesystems,
  or lexical `..`. Fix direction: normalize/canonicalize paths at record time with platform-aware
  case policy. Add tests for lexical aliases and symlink aliases.
- **Recent-file entries are not pruned when their project root changes or disappears.** `RecentFilesFor`
  filters by exact project root but never removes stale files/projects. Long use across moved repos can
  leave dead entries and make file pickers noisy. Fix direction: prune missing roots/files lazily with
  a budget and update MRU storage. Add tests for missing project root on startup.
- **File create/rename operations allow targets outside the active project unless the caller checks.**
  `FileOperationService` normalizes absolute paths and performs the operation; it has no root
  containment policy. If any UI/control/plugin caller passes an arbitrary absolute path, project file
  operations can write or move outside the workspace. Fix direction: make the service root-scoped or
  require an explicit `AllowExternalPath` mode. Add tests for `../outside` and absolute external paths
  through every caller.
- **`RenamePath` uses exists checks before `MovePath`, so destination races are reported late and
  unclearly.** Another process can create the destination after the check. Depending on `MovePath`
  implementation, the operation may fail, overwrite, or perform cross-device fallback work before
  discovering the race. Fix direction: make `MovePath` expose no-overwrite semantics end-to-end and
  return structured errors. Add tests with destination created between check and move via a seam.
- **Reserved-component validation checks only the destination filename after normalization.** For file
  create/rename, earlier path components can include reserved names before lexical normalization, and
  platform-specific reserved names (`CON`, `NUL`, trailing spaces/dots on Windows) are not checked
  here. Fix direction: validate every user-entered component before normalization and add
  platform-specific reserved-name rules. Add Windows tests for device names and trailing-dot paths.

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
