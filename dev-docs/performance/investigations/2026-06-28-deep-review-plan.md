# Deep Review Plan — 2026-06-28 (speed/correctness/tech-debt)

Source: multi-agent deep review (7 finder dimensions × adversarial verification).
21 findings raised, **21 survived independent verification**, de-duplicated into 13
work items. Ranked speed-first, then correctness, CPU, memory, then tech-debt/dedup.

This codebase is already heavily optimized (revision-keyed thread-local render
caches, layout caches, retained-scene partial redraws). The wins below are the
residual ones that survived skeptical re-reading of the actual code. Where no
committed perf scenario covers a path, **add a focused harness scenario first**
so the win is observable and regression-guarded.

## Status legend
- ✅ done in this pass
- ☐ remaining

## Implementation status — all 13 work items shipped (2026-06-28)

All Tier A/B/C items landed on branch `perf/deep-review-tier-a` across several
commits, each built clean with the full `ctest` suite (incl. the
ArchitectureInvariants lint) passing. One deliberate scope cut: **B1** shipped
only the correctness-safe visibility guard, not the visible-but-partial-redraw
input-keyed VM memoization — a mis-specified memo key would stale-render the
sidebar, and correctness outranks the modest per-frame cpu saving (see B1 below).
Likewise the B7 ":544 second-copy" and the ParseExtendedSgrColor `seq` alloc were
left as-is as marginal/risky; noted in their sections.

Perf wins here are allocation/copy removals on measured hot paths; per the
perf-harness discipline they should still be confirmed before/after on the named
scenarios (sustained-scroll for A1, etc.) and new scenarios added where none
exists (Git sidebar, FileFinder, review comments, terminal, blame).

---

## Tier A — do first (high leverage, low risk)

### ✅ A1 (+#5). Eliminate the per-row `LayoutLine` copy-out from the layout cache
- Tier: **speed** (editor render hot path)
- Files: `src/editor/TextLayoutCache.{h,cpp}`, `src/editor/TextViewport.{h,cpp}`, `src/editor/EditorViewRenderer.cpp`
- Change: cache now exposes `VisibleLineLayoutRefCached` returning `const LayoutLine&`;
  `TextViewport::VisibleLineLayoutRef` + `CaretForLine` split the per-call caret out of
  the cached object. Renderer hot loop binds `row_layout` by reference (owned scratch only
  on the soft-wrap branch) and resolves caret separately. #5: redundant full-line rebuild at
  the EOL-decoration anchor now reuses `row_layout.visual_columns` on the non-wrap branch.
- Effect: removes up to 3 heap allocs (string + 2 vectors) per visible row per painted
  frame on the dominant soft-wrap-off path, even on cache hits. Verify on the sustained-scroll lane.

### ✅ A2. Lock-protect the `stdin_fd` close on the `Write()` error path
- Tier: **correctness** (data race + double-close)
- File: `src/platform/AsyncSubprocess.cpp:258`
- Change: `impl_->CloseStdin()` → locked `CloseStdin()`, matching sibling error branches.
- Effect: removes a real race + double-`close()` (fd reuse) during LSP/DAP teardown. TSAN-observable.

### ✅ A3. Stop deep-copying the undo `HistoryEntry` per keystroke
- Tier: **cpu** (typing hot path)
- File: `src/editor/TextViewportEditEngine.cpp` (`BuildRangeHistoryEntry`, `ApplyRangeEdit`, `ApplyLineEdit`)
- Change: `std::move` `before_lines` into the entry; `std::move(*entry)` into the saved entry.
- Effect: removes 2 per-keystroke vector-of-string deep copies. Zero behavior change.

### ✅ A4. Hoist the review-comment URI and kill per-row URI map lookups
- Tier: **memory/cpu** (editor render, code-review context)
- Files: `src/workspace/render/WorkspaceShellRenderFrame.cpp:351-386`, `src/workspace/WorkspaceReviewComments.h`
- Change: resolve the URI index once per pane (not `IndexForUri(uri)` twice per visible row);
  prefer resolving in `RenderViewModelBuilder` and threading down (WorkspaceShellRenderFrame is
  a lint-covered render TU — `generic_string()` slips the textual lint but violates its spirit).
- Effect: removes 2 full-string-hash lookups per visible row + 1 alloc/pane/frame when review
  comments are loaded. Add a review-comments-loaded scenario to measure.

### ✅ A5. Bind compare inline changed-spans by const reference
- Tier: **memory** (compare render hot path)
- File: `src/workspace/WorkspaceShellCompareRender.cpp:404,489`
- Change: by-value `const std::vector<compare::CompareTextSpan>` → `const auto&` (targets
  outlive the frame; downstream `std::span` unchanged).
- Effect: removes a small-vector heap copy per modified visible row per side per frame.
  Shows on `compare_scroll_large_fixture` / `diff_next_hunk_large_file`.

---

## Tier B — worthwhile

### ✅ B1. Guard + memoize the Git sidebar view model
- Tier: **cpu** (per-frame, Git-sidebar users)
- Files: `src/workspace/render/RenderViewModelBuilder.cpp:459-498`, `WorkspaceShellRenderFrame.cpp:88-89`, `GitSidebarCommandCenter.cpp:408-497`
- Change: (1) only build when `sidebar.visible`; (2) memoize keyed on `snapshot_generation`,
  commit-workflow state, `selected_index`, `collapsed_directory_keys`, `branch_review` revision.
  Build stays in `RenderViewModelBuilder` (the layer allowed to read `current_project_state`).
- Effect: removes O(changed-files) per-entry string allocs discarded every partial frame while
  Git view is selected. Add a dirty-repo + Git-view-selected scenario.

### ✅ B6. Highlight prefetch — copy lines once via `SliceLines`
- Tier: **memory** (scroll into cold region)
- File: `src/editor/TextViewportHighlightCache.cpp:277-279`
- Change: replace the `push_back(document_->lines[line])` loop with `SliceLines(start_line, end)`.
- Effect: removes the double-copy and stops polluting `TextBuffer::line_cache_`.

### ✅ B2. FileFinder — narrow candidate set on forward typing (+ riders)
- Tier: **speed** (per-keystroke finder)
- Files: `src/project/FileFinder.{cpp,h}`
- Change: when new query is a prefix of the last, re-rank only prior matches (re-score, don't
  reuse scores); fall back to full scan on backspace/shrink. Riders: size `ranked.reserve` to
  matches (not blindly `cached_entries_.size()`); replace recents O(R·N) `find_if` with a map.
- Effect: per-keystroke O(N)→O(prior-matches) on the common forward-typing case. Add a
  type-into-finder scenario. (100k-file blowup is beyond tuned scale — interactive, not measured.)

### ✅ B3. Terminal CSI/SGR parsing — stop heap-allocating per escape sequence
- Tier: **cpu** (terminal reader thread, colored-output bursts)
- Files: `src/terminal/TerminalCsiParser.cpp:33-85`, `TerminalSessionCsi.cpp:38`, `TerminalSessionSgr.cpp:23-63`
- Change: parse ints inline from the `string_view`; fill caller-owned reusable scratch (`.clear()`'d
  per call); SGR fixed inline-capacity for the common ≤3-group case. Also stop computing
  `ParseCsiParameters` unconditionally before the switch (discarded for SGR `m`).
- Effect: ~3-4 short-lived allocs per SGR removed, under `mutex_` on the reader thread.
  Note: `ParseExtendedSgrColor` depends on nested-group shape — preserve it.

### ✅ B4. Buffer-search match cache — transparent (allocation-free) lookup key
- Tier: **memory** (editor render while find is open)
- Files: `src/editor/EditorViewRenderer.{cpp,h}` (~594-601 / 114-142)
- Change: heterogeneous lookup with a borrowed `CacheKeyView` (`string_view query` + fields),
  materialize the owning `std::string` only on insert. Mirror `SdlTtfTextBackend` (cpp:427-446).
- Effect: removes one string copy + full-query re-hash per visible row per frame while find open.
  Allocation win bounded to queries longer than ~15-byte SSO.

### ✅ B5. Git blame `Snapshot` — return only the caret window
- Tier: **memory** (per-frame when inline blame eligible)
- Files: `src/project/GitBlameService.cpp:367,396-401`, `src/workspace/services/EditorBlameOverlayService.cpp`
- Change: pass caret line range; add `result_start_line`/`result_line_count` (keep wider
  `visible_*` for prefetch/cache), or `SnapshotLines(file_key,start,end)`. Cache stays keyed
  on the visible window; only the returned vector shrinks to caret±`kCaretBlameRadius`.
- Effect: stops copying ~visible_rows `GitBlameLine`s (4 strings + 40-char commit_id each) per
  frame when only ~3 are consumed (~94% discarded). Concentrates on editor/compare scroll with blame.

### ✅ B7. `UpdateMergeMaxVisualColumns` — take a span, drop the owned copy
- Tier: **cpu** (per merge-result keystroke)
- File: `src/workspace/shell/WorkspaceShellMergeState.cpp:19-25,413-423`
- Change: `MaxVisualColumnsForLines` takes `std::span<const std::string>`; drop `owned_lines`.
  Optionally span the changed range at the :544 callsite too.
- Effect: removes a per-keystroke copy over the changed line range. Clean signature improvement.

### ✅ B8. ProjectSearchService — dedup SDL wake events with a pending flag
- Tier: **cpu** (during active project search)
- Files: `src/project/ProjectSearchService.{cpp,h}` (~521-531 / 121-133)
- Change: `std::atomic<bool> wake_pending_`; `SDL_PushEvent` only when `exchange(true)` was false;
  clear under `mutex_` in `TakePendingUpdate`. Reuses the watcher's `*_pending_.exchange` pattern.
- Effect: collapses ~total_files/64 + per-batch wakes into one pending wake; all state coalesced
  in `pending_update_` under `mutex_`, so the final `finished` flag can't be lost.

---

## Tier C — nice-to-have / dedup

### ✅ C1. De-duplicate the compare right-line → model-row lookup
- Tier: **tech-debt/dedup** (+ real speed rider on the blame path)
- Files: `src/workspace/shell/WorkspaceShellCompare.cpp:477`, `CompareTabReview.cpp:231`
- Change: drop the byte-identical `WorkspaceShell::CompareRowIndexForRightLine`; delegate to the
  free `CompareTabModelRowForRightLine`. Optionally back with a right-line→model-row table cached
  on `CompareTabState`, rebuilt on `model_revision` (binary search is unsafe: `right_line==0`
  deleted rows break monotonicity; a lookup table is correct).
- Effect: removes a duplicated O(model.rows) scan; cache matters for the compare blame-overlay
  callback (O(lines × model.rows) per frame while blame active).

### ✅ C2. Unify ASCII-lowercasing onto `util::ToLowerAsciiInto` / `util::ToLowerAscii`
- Tier: **tech-debt/dedup** (+ bounded worst-case safety)
- Files: `src/editor/EditorViewRenderer.cpp:347-352,604-606`; `RenderViewModelBuilder.cpp:122-126,208-214`;
  `WorkspaceTextSearch.cpp:110-122`; `WorkspaceShellLsp.cpp:330-337`; `TextViewportEditEngine.cpp:261`
- Change: replace `resize`+`std::transform(std::tolower)` blocks with `util::ToLowerAsciiInto`
  against existing scratch; delete the local `ToLowerAscii` in `WorkspaceShellLsp.cpp`; hoist a
  reusable scratch in ReplaceAll; route `FindLiteralNeedleInLine`'s case-insensitive branch through
  the shared lower-once-into-thread_local + `string_view::find` shape.
- Effect: one canonical branchless/locale-free ASCII fold + small constant-factor win. NOTE: the
  quadratic fix only helps bounded single-gesture paths (F3/Enter, select-all-occurrences), **not**
  find-as-you-type (already optimized). Treat as convergence + worst-case safety, not per-keystroke.

---

## Dropped / deprioritized
- **#10 standalone** (FileFinder `ranked` reserve): `FileFinderResult` is nothrow-movable, so growth
  moves not copies; blind `reserve(cached_entries_.size())` over-allocates. Folded into B2 as a sized rider.
- **#12 standalone** (recents `find_if`): empty-query full O(N log N) sort dominates. Folded into B2.
- **#19 as a per-keystroke quadratic**: refuted — find-as-you-type already uses `FindLiteralMatchesImpl`.
  Kept only as the C2 dedup/safety convergence.

## Execution notes
- Ship Tier A together; A1 is the one real speed win on a measured lane. A2–A5 are small/near-zero risk.
- Measure each perf item before/after with the named harness scenario. Where none exists
  (B1 Git sidebar, B2 FileFinder, A4 review comments, B3 terminal, B5 blame), add one first.
- Respect hard invariants in `CLAUDE.md`: render TUs stay view-model-only; no new string
  materialization in render hot paths; persistence routes through `PersistenceService`.
