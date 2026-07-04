# Deep Review Plan — 2026-06-29 (speed/correctness/tech-debt, pass 3)

Source: multi-agent deep review (8 finder dimensions × adversarial verification),
deliberately aimed at subsystems the two prior passes (2026-06-28) barely touched:
persistence, syntax replay, the diff/compare model, the plugin/Lua runtime, file
watchers / idle-CPU, LSP/DAP internals, core editor buffer, and util-layer dedup.

35 raw findings → **30 survived independent verification** → synthesized into 25
ranked items (several merged). Ranked speed-first, then correctness, CPU, memory,
then tech-debt/dedup.

This codebase is already heavily optimized. The wins below are the residual ones
that survived skeptical re-reading. Where no committed perf scenario covers a path,
**add a focused harness scenario first** so the win is observable and regression-guarded
(per the `performance-budgets` measured-before-merged policy).

## Status legend
- ✅ shipped in this pass (mechanical, behavior-preserving, full `ctest` green incl. ArchitectureInvariants)
- ☐ remaining — needs a regression test and/or before/after harness measurement before landing

---

## Update 2026-07-04 — re-audit + measured follow-up (perf-runner-v1)

Re-audited every remaining ☐ item against live code and measured the tempting ones on this host.
Most of the "remaining speed" list had already shipped in **v2.5.1** (the multi-caret / path-dedup
speed pass). Current status:

- ✅ **A1** (multi-caret one-pass rebuild) — shipped: `TextViewport::SetSecondaryCarets` is a single
  clamp/sort/dedup pass; the O(k²·log k) `AddSecondaryCaret` loop is gone.
- ✅ **A2** (compare `lines_equal` pre-scan) — removed and documented at `CompareModel.cpp` ~:1134.
- ✅ **A4-dedup** (LSP URI once/keystroke) — `EnsureLspDocumentOpen(precomputed_uri)`; computed once
  in `SyncLspForActiveEditableLastChange`.
- ✅ **B-Dirty** (`TextViewport::LoadLines`) — exists; session restore loads the split snapshot straight in.
- ✅ **C-PieceTree** (single-pass extractors) — `ToVector`/`SliceLines` delegate to one pruned treap
  walk (`ExtractLineRange`).
- ✅ **B-DapPoll** — already relaxed 16→64 ms with a documented rationale (`WorkspaceShellRedraw.cpp`).
  Accept as-is; the plan's ~250 ms was a judgment call, 64 ms is the shipped judgment.
- ✅ **B-PluginEvt** — **shipped 2026-07-04.** Added `buffer_open`/`buffer_save` interest flags
  (excluded from `any()`; consulted only at lifecycle-dispatch time) and gated all three
  `OnBuffer{Open,Save,Close}` sites so no `CaptureSnapshot()` + worker task fires with no subscriber.
  Test: `PluginHost/BufferLifecycleInterestGate`.
- ~ **B-Fanout** — the O(doc_lines)-per-caret rebuild is already gone (per-caret invalidation is now
  `start_line`-anchored/incremental). Only a marginal "defer to one post-loop invalidation" remainder
  is left; not worth the parity risk without a multi-caret profile flagging it.
- ✗ **B-Hist** (single-line `BuildRangeHistoryEntry` fast path) — **implemented, measured, reverted.**
  `perf-compare` on typing/multi-caret/edit scenarios showed **zero allocation delta and wall within
  the 2σ noise band** (the dominant per-edit allocations are highlight/view-model, not the history
  entry). Not worth the extra branch. Do not re-attempt without a scenario that isolates it.
- ☐ **B-Body** (persistence reader body deep-copy + `AppendRecord` throwaway vector) — still open;
  medium/structural, touches the persisted-record read/write layer. Startup-path allocation only.

Also measured and **rejected** in this pass: a **width-cache ASCII fast path**
(`TextRenderer::MeasureWidth` → `backend->FastMonospaceWidth`, skipping the `unordered_map<string,float>`
for printable ASCII). The render hot path does ~400 `MeasureWidth`/frame in compare/merge scroll, but
the small hot map's hit cost ≈ the ASCII-scan cost, and adding a virtual call before the cache lookup
is a slight pessimization for non-ASCII. `perf-compare` across the compare/merge/editor scroll
scenarios: all deltas within 2σ noise. Reverted.

**Environment note:** this pass had **no CPU sampler** (`perf_event_paranoid=4`, no passwordless sudo,
no valgrind), so hotspot localization relied on `perf_counters` + `perf-compare` wall/alloc deltas
rather than flamegraphs. The ranking confirms the top interactive scenarios (compare/merge scroll
bursts, ~4.5 ms/frame, low-alloc) are **render-bound under the software renderer** (cached-glyph
texture blitting) with no cheap CPU win available. See `performance-findings.md` → "2026-07-04
measurement pass".

---

## Shipped in this pass (mechanical / behavior-preserving)

### ✅ A3. Hoist a reusable DP scratch buffer in hunk-alignment similarity
- Tier: **memory** (compare per-keystroke). File: `src/compare/CompareModel.cpp`.
- `CommonSignificantTokenBytes` / `LineSimilarity` now take a caller-owned
  `std::vector<std::size_t>&` and `assign()` it (re-zeroes the boundary value-init gave);
  `AlignHunkLines` declares one `common_token_scratch` and threads it through `similarity_at`.
  A dense modified hunk calls this up to `left*right` times (capped 65536) — was one malloc
  per pair, now first-call-only. Output byte-identical.

### ✅ A5. Move (not copy) token vectors in highlight-cache install + forced-miss insert
- Tier: **memory** (per-frame forced miss / per prefetch landing). Files:
  `src/editor/TextViewportHighlightCache.cpp`, `TextViewport.h`, `WorkspaceShellEditor.cpp`.
- Cache-miss `emplace(line_index, std::move(highlighted.tokens))` (local dead after).
  `InstallPrefetchedHighlights` now takes `HighlightPrefetchResult` by value and
  `std::move`s each `result.tokens[offset]` in; the drain loop moves the result in.
  (By-value, not `&&`, so the lvalue test callsites keep compiling.)

### ✅ A6. Persistence primitive cleanups
- Tier: **speed** (startup/save). Files: `src/persistence/PersistedRecord.cpp`,
  `src/workspace/WorkspacePersistenceBinaryInternal.h`.
- `ReadString` → single `value->assign(reinterpret_cast<const char*>(input_.data())+offset_, size)`
  (bounds already checked); `WriteString` → bulk `out_->insert(...)` instead of per-byte
  `push_back`. `ParseRecordStream`'s callback is now a deduced template param (mirroring the
  sibling `AppendRecord`), dropping the `std::function` type-erasure on the decode tree.

### ✅ A4 (partial). Allocation-free file-URI percent-encode
- Tier: **speed** (per-keystroke when an LSP server is active). Files:
  `src/workspace/FileUri.cpp`, `src/util/Hex.h`.
- `FileUriForPath` rewritten off `std::ostringstream` + `<iomanip>` onto a plain `std::string`
  + new `util::AppendHexByte` (symmetric with the existing decode helpers in `Hex.h`).
  Behavior-identical (uppercase, zero-padded). **Remaining:** the URI is still computed twice
  per keystroke (`LspService.cpp` ~:305 and ~:454) — see ☐ A4-dedup below.

### ✅ B-Fold. FoldingModel indents vector sized to the scan window
- Tier: **memory** (fold recompute). File: `src/editor/FoldingModel.cpp:235`.
- `indents(scan_end, ...)` instead of `lines.size()`; all reads were already bounded by
  `scan_end` (matching the adjacent `bracket_opener`). One-liner, stops zero-initing the
  unread tail of a big file per budgeted recompute.

### ✅ B-Poll. Move the poll-fallback file-index snapshot
- Tier: **cpu** (idle-wake, poll fallback only). File: `src/platform/FileIndexWatcher.cpp` (×4).
- `const auto current` → `auto current` + `snapshot = std::move(current)` at all four poll
  workers (`current` is dead after assignment). Was deep-copying the whole RB-tree snapshot a
  second time each 750 ms cycle.

### ✅ C-Overlay. De-duplicate the triplicated overlay-surface rect math
- Tier: **dedup** (cold). File: `src/workspace/WorkspaceLayout.cpp`.
- Three byte-identical helpers collapsed into `ComputeOverlaySurfaceRectImpl(area, params)`;
  the three public functions are now one-line forwarders carrying their fraction/pad constants.

### ✅ C-Guard. Drop the provably-dead `|| ActiveTabIsCompare()` term
- Tier: **tech-debt** (cold). File: `src/workspace/WorkspaceShellRedraw.cpp` (both overloads).
- Compare is fully handled by the early return above; the second check was dead and misleading.

---

## Remaining — speed (needs test/measurement)

### ☐ A1. Multi-caret secondary-caret rebuild is O(k²·log k); build in one pass
- **cpu**, per-keystroke (multi-caret only). `src/editor/TextViewportMultiCaret.cpp:167,288,406`.
- The `secondary_carets_.clear(); for(...) AddSecondaryCaret(...)` tail re-sorts + linear-scans
  per insert over an already sorted/unique/primary-stripped vector. Replace with one
  reserve+push_back loop (compute `PreferredColumnForCaret`/`ClampTextColumn` inline) factored
  into a single `RebuildSecondaryCaretsFromSorted` helper (avoids 3-way drift). Add a trailing
  adjacent-dedup only if clamp can collapse two positions — **that edge case is why this needs a
  large-doc many-caret regression test, not a blind apply.**

### ☐ A2. Delete the redundant `lines_equal` pre-scan in compare rebuild
- **speed**, per-keystroke (compare). `src/compare/CompareModel.cpp:1069-1104`.
- Typing inside a line keeps line counts equal, so the all-equal fast path walks the whole common
  prefix only to return false, then the general prefix loop re-scans the identical prefix. The
  general path already emits an identical all-Unchanged model. Delete the branch; **verify
  row/hunk parity against `CompareModelTests` first.**

### ☐ A4-dedup. Compute the LSP document URI once per keystroke
- **speed**, per-keystroke. `src/workspace/LspService.cpp` ~:305 / ~:454.
- `SyncLspForActiveEditableLastChange` should compute the URI once and pass it into
  `EnsureLspDocumentOpen` instead of re-normalizing+encoding twice. Small flow change; pair with
  a quick re-read of the open/change call paths.

### ☐ B-Dirty. Dirty-tab session restore round-trips lines through join + re-split
- **speed**, startup (dirty tabs only). `src/workspace/WorkspacePersistenceCoordinatorSession.cpp:379`.
- Add `TextViewport::LoadLines(std::vector<std::string>, path, line_ending)` (→ `ResetState(std::move,...)`)
  and use it instead of `SerializeLines(...) → LoadContent(...)`, which joins already-split lines
  then re-splits them (two extra full passes). Best case `std::move` the discarded
  `PersistedSession::buffer_lines`. New API ⇒ verify.

### ☐ C-PieceTree. Single-pass `ToVector()` / `SliceLines()` bulk extractors
- **cpu**, background. `src/editor/PieceTree.cpp:318-330,339-357`.
- Rewrite only these two bulk extractors as a single in-order treap walk (O(N+p) vs N double
  descents); naturally guarded by `PieceTreeEquivalenceFuzz`. **Rejected** the broader stateful
  `LineIterator` cursor proposal: freshly-loaded large files are single-piece and the bracket
  scanners' per-column loop dominates `log p` — not justified without measurement.

---

## Correctness defects — all four shipped (2026-06-29)

### ✅ B-WS. `ignore_whitespace` silently dropped inside changed hunks
- `src/compare/CompareModel.cpp`, `CompareModel.h`. Commit: "fix(compare): honor ignore_whitespace
  inside changed hunks".
- Threaded `CompareBuildOptions` through `BuildExactLineOps`/`AppendAnchoredFallbackOps`, replaced
  raw `==` with `LinesEqualForDiff`, and extended `DiffOp` with `right_text` so Equal ops that pair
  two whitespace-different lines keep each column's own text (the `AppendEqualPairs` helper emits
  both sides for every matched run). Default (ignore_whitespace=false) path is unchanged.
- Test: `Compare/IgnoreWhitespaceInteriorHunkLine`.

### ✅ B-Enc. LSP position encoding — common case fixed; utf-16 conversion remains
- `src/workspace/WorkspaceLspClientInternal.h`, `WorkspaceLspClient.{h,cpp}`. Commit: "fix(lsp):
  negotiate utf-8 position encoding".
- Now advertises `general.positionEncodings = ["utf-8","utf-16"]` (utf-8 first). UTF-8 LSP positions
  are byte offsets == the editor's columns, so a server that honors it (clangd/rust-analyzer/gopls/
  pyright) is correct with zero conversion. Captures `capabilities.positionEncoding`, exposes
  `LspClient::ServerPositionEncoding()`, and logs once when a server falls back to non-utf-8.
- Tests: initialize advertises utf-8; utf-8 negotiation captured; default → utf-16.
- ☐ **Remaining:** full byte↔UTF-16 conversion at every position boundary (outbound completion/hover/
  definition/references/codeAction/signatureHelp/rename + incremental-sync ranges; inbound locations/
  hover-range/diagnostics/completion-edits) for servers that only support utf-16. Each needs the
  relevant line text to convert; gate behind `ServerPositionEncoding() != "utf-8"`. Test with a 2-byte
  `é` and a 4-byte emoji before the edit point.

### ✅ B-Stale. Stale LSP completion/code-action responses overwrite the current session
- `src/workspace/AssistService.{h,cpp}`. Commit: "fix(assist): drop stale LSP completion/code-action
  responses".
- Extracted the drop decision into pure `AssistService::ResultIsStale` and routed all three callbacks
  (plugin completion, LSP completion, LSP code action) through it; the LSP paths capture `request_path`
  and bail after `finish_tracked_lsp_request` so the in-flight counter is not leaked.
- Tests: `tests/AssistServiceTests.cpp` (closed-buffer, switched-path, matching-path).

### ✅ B-Harvest. Plugin provider-query harvest loops were unbounded + metamethod-invoking
- `src/plugin/PluginProviderQueryInterop.cpp`. Commit: "fix(plugin): bound provider-query harvest
  loops to the raw array spine".
- Converted all eight array-spine walks to `lua_rawlen`+`lua_rawgeti` (bounded, no metamethods),
  matching the accepted decoration/diagnostics parsers. Named-field reads unchanged.
- Test: `PluginHost/ProviderQueryBoundsAdversarialMetatable` (phantom `__index` + 1e9 `__len`).

---

## Remaining — CPU / memory (smaller, measure or scope first)

- ☐ **B-Fanout** (`TextViewportMultiCaret.cpp:137,255,373`): defer the per-caret highlight /
  visual-column cache fan-out to one post-loop invalidation; the O(doc_lines) `UpdateVisualColumnCacheAfterEdit`
  memmove per caret dominates. `RestoreViewState` must stay per-caret for correct clamping ⇒ parity test. **medium.**
- ☐ **B-Hist** (`TextViewportEditEngine.cpp:381-413`): single-line fast path for `BuildRangeHistoryEntry` —
  skip `NormalizeLineEndings`/`SplitLines`, read via `LineView` (zero-copy) not `operator[]` (full line copy),
  build the after-line with one reserve+appends. Human-paced ⇒ allocator-churn cleanup, not latency. **low.**
- ☐ **B-Body** (`PersistedRecordReader.cpp:76` + `WorkspacePersistenceBinaryInternal.h:272-283`):
  reader deep-copies the whole record body that consumers only read as a span; `AppendRecord` allocates a
  throwaway vector per field. Make the result own the file bytes + a `body()` span; rewrite `AppendRecord`
  to write a length placeholder then `PatchU32` (with `resize(record_start)` rollback on failure). **medium, structural.**
- ☐ **B-PluginEvt** (`PluginHostPublicApi.inc:115,132,251`): buffer open/save/close events `CaptureSnapshot()`
  + post a worker task even with no subscriber. Add `buffer_open`/`buffer_save` interest flags (a dead
  `buffer_close` flag already exists) and early-return like the per-keystroke events do. **low.**
- ☐ **B-DapPoll** (`WorkspaceShellRedraw.cpp:742-744`): the in-flight DAP 16 ms idle clamp re-introduces
  ~62 wakes/sec during stepping/expansion. Responses already deliver via mailbox→PushWake, so widen
  `kDapPollMs` 16→~250 ms (keep a backstop, don't remove). **low, judgment call.**

---

## Remaining — dedup / tech-debt (cold, maintenance wins)

- ☐ **C-PollWorker** (`FileIndexWatcher.cpp` ×4, ~270 lines): hoist `BuildFileIndexSnapshot` +
  `RunFileIndexPollLoop` anonymous-namespace helpers; the four poll workers are byte-identical.
  Coordinate with the shipped B-Poll move. **low, mechanical-but-large.**
- ☐ **C-Transport** (`WorkspaceLspClientInternal.h:80` + `WorkspaceDapClientInternal.h:89`, ~400 lines 1:1):
  extract a header-only `FramedJsonRpcChannel` (AsyncSubprocess + self-pipe wake + framing + queue +
  timeout sweep); inject protocol dispatch via callbacks. Preserve exact write/mutex ordering and
  `memory_order_release`; add partial-frame + stdin-write-failure fixtures; gate under TSAN. The prior
  stdin_fd close-race ("fix twice") is the canonical motivation. **medium.**
- ☐ **C-PathNorm** (`PluginHost.cpp:629,641,646,305`): provider queries re-normalize the input twice and
  re-normalize the stable project root per call. Store `current_project_root` normalized at both assignment
  sites; add a `RelativePathFromNormalized` overload — **keep** the public `RelativePathString` normalizing
  for the non-normalized `PushBufferTable` caller. **low, immeasurable (worker thread).**
- ☐ **C-Tok** (`CompareModel.cpp:262-277,501-503`): paired Modified lines tokenize once in alignment
  (discarded) then again for intraline spans — but both are dwarfed by the O(left·right) DP. Only worth the
  plumbing if `CompareBuildProfile` shows tokenization material. The codepoint-fallback `BuildUtf8Offsets`
  double-build is a tiny self-contained dedup. **Do not** touch `TokenizeLine`'s `reserve(text.size())`.

---

## Dropped (verified not worth it)
- **Region-start skip-mask removal** (`RuntimeSyntaxRegistry.cpp:651-652`): passing `nullptr` for the START
  search changes tokenization semantics (an escaped start char could open a region); the cost is on the
  cached per-line-on-edit path, not per-frame. Correctness-first ⇒ don't trade highlight semantics.
- **`TokenizeLine` over-reserve** (`CompareModel.cpp:93`): short-lived, immeasurable next to the co-located
  O(n²) DP; shrinking it trades away a guaranteed-no-realloc property. Revisit only via an arena rework.

## Execution notes
- The shipped batch is allocation/copy/type-erasure removals + provably-dead/lockstep dedup — no algorithm
  change, full `ctest` green. Per `performance-budgets`, confirm the per-keystroke ones (A3 compare DP, A4
  FileUri) on the relevant harness lanes (`compare_scroll_large_fixture`, an LSP-active typing scenario).
- Every remaining ☐ item either adds a code branch / API / equivalence claim (needs a regression test) or
  needs before/after harness numbers — none should land on code-inspection alone.
- Respect the hard invariants in `CLAUDE.md`: render TUs stay view-model-only, no string materialization in
  render hot paths, persistence routes through `PersistenceService`, no `luaL_error` in `src/plugin`.
