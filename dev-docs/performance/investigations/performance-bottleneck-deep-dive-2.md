# Performance Bottleneck Deep Dive — Round 2

Date: 2026-05-15

## Status snapshot (2026-05-15, end of pass)

| # | Finding                                                | Status |
| - | ------------------------------------------------------ | ------ |
| 1 | View-model surfaces rebuilt 2-3×/frame                  | done  (prepare owns sidebar/bottom/text-input VMs once/frame; clip cache shares frame+overlay across `RenderClip`; render TUs reuse) |
| 2 | Per-row paint loops over flat run vectors              | done  (whitespace_row_offsets CSR + occurrence binary search) |
| 3 | RefreshStatusBar synchronous git                       | done  (cached project segment + invariant lint) |
| 4 | std::stol with try/catch                                | done  (util::ParseInt rewrite) |
| 5 | Wrapped-row layout rebuild per edit                    | done  (trivial-layout fast path skips the vector build) |
| 6 | FoldingModel O(N²) remap + FindPair linear scan        | done  (indexed remap + 256-entry bracket table) |
| 7 | Render-TU string materializations                      | done  (string_view + thread_local scratches + lint guard) |
| 8 | TerminalCell carries std::string per cell              | deferred — std::string SSO makes the heap-pressure cost small; full rewrite to inline storage belongs with the scrollback ring/page refactor (round-1 Finding 6) |
| 9 | Width-cache LRU duplicates key strings                  | done  (deque of string_view into map keys) |
| 10 | secondary_carets() allocates vectors per call         | done  (span accessor + cache stability) |
| 11 | sticky/occurrence vectors copied on cache hit        | done  (std::span<const T> view models) |
| 12 | Unordered_set in occurrence scan refresh             | done  (sorted thread_local vector) |
| 13 | VisualColumnFromLayout binary search                 | partial (editor row loop + secondary carets; compare/diagnostics paths still walk bytes where noted in round 3) |
| 14 | IndentGuides per-row segments                        | done  (coalesce into multi-row runs) |
| 15 | Glyph atlas for SDL_ttf cache misses                  | deferred — separate text-rendering pass |
| 16 | Single layout_revision cascade                       | partial (Findings 5/6 closed the visible-cost paths; full revision split is the next pass) |
| 17 | Per-frame scratch vectors that should be members     | done  (ComputeSingleLineViewMetrics thread_local scratch) |
| 18 | PrepareFrameOnce queue consumers                     | deferred — needs event-revision accounting infrastructure |
| 19 | Lint coverage gaps                                   | done  (5 new ArchitectureInvariants rules + targeted fixtures) |

Deferred items are tracked individually and will be tackled in follow-up passes.

---

This is a follow-up investigation after the round-1 fixes (`dev-docs/performance/investigations/performance-bottleneck-deep-dive.md`)
were implemented. The first deep dive removed the worst frame-prep regressions; this round digs
deeper into render-path translation units, edit invalidation, terminal text storage, and several
architectural-invariant violations that were missed.

Findings are grouped by load-bearing impact rather than file location.

---

## Finding 1: `RenderViewModelBuilder` Surfaces Are Rebuilt 2-3× Per Frame

Relevant code:

- `src/workspace/WorkspaceShellRenderFrame.cpp:69-71`, `154`, `225`, `227`
- `src/workspace/WorkspaceShellRenderSidebar.cpp:37`
- `src/workspace/WorkspaceShellRenderTextInput.cpp:149-152`
- `src/workspace/WorkspaceShellRenderOverlay.cpp:12`
- `src/workspace/WorkspaceShellRenderBottomPanel.cpp:35`
- `src/workspace/WorkspaceShellHoverTargets.cpp:165, 396`
- `src/workspace/RenderViewModelBuilder.cpp:412-447`

Each `Build…Surface` constructor was meant to be the single, frame-stable boundary between shell
state and render TUs. In practice, several surfaces are rebuilt multiple times per frame:

| Surface | Frame call sites |
| --- | ---: |
| `BuildSidebarSurface` | 3 (PrepareFrameOnce + Sidebar TU + TextInput TU) |
| `BuildBottomPanelSurface` | 2 (PrepareFrameOnce + BottomPanel TU) |
| `BuildFrameSurface` | 2 (RenderFrameBase + RenderEditor) |
| `BuildOverlaySurface` | 2 (Frame + Overlay TU, plus TextInput) |
| `BuildHoverTargets` | 2 (HoverTargets + HoverPopup paths) |
| `BuildTextInputSurface` | 2 (TextInput + Sidebar) |

`BuildSidebarSurface()` itself materializes two `std::string` fallback labels on every call
(`query_fallback_text`, `replace_fallback_text` at lines 423-435). Three calls per frame ⇒ six
fallback strings allocated/destroyed per frame even when the search query is unchanged.

### Rewrite plan

1. Materialize each `*SurfaceViewModel` exactly once per frame and stash it in a
   `PreparedFrameViewModels` owned by `PrepareFrameOnce()`.
2. Pass the prepared surfaces by `const&` to each render TU; remove redundant builder construction
   in render TUs entirely.
3. Convert `query_fallback_text`/`replace_fallback_text` to compile-time `std::string_view` constants
   (the fallback is invariant) and pick at render time.
4. Add a perf counter `RenderViewModelBuildsPerFrame` and assert in debug builds that no surface is
   built more than once per `PrepareFrameOnce` call.

### Expected impact

- Removes 6-10 redundant view-model materializations and ~6 `std::string` allocations per frame.
- Establishes the boundary intended by the original "one builder, one consumer" design.

---

## Finding 2: Editor Per-Row Paint Loops Over O(N) View-Model Vectors

Relevant code:

- `src/editor/EditorViewRenderer.cpp:632-664` (occurrence loop)
- `src/editor/EditorViewRenderer.cpp:745-787` (whitespace glyph loop)

`Render()` walks each visible row and, inside the row loop, iterates the entire
`view_model->occurrence_ranges` / `view_model->whitespace_glyph_runs` to find matches for that
specific row:

```cpp
for (const OccurrenceRange& occ : view_model->occurrence_ranges) {
  if (occ.line_index != line_index || …) continue;
  …
}
```

Cost is O(visible_rows × total_runs) per frame. With 50 visible rows and ~10 whitespace glyphs per
row on a heavily-indented file, that is 25 000 comparisons just for whitespace, plus the same shape
for occurrences when a symbol is selected. The earlier deep dive named this in Finding 5 ("row-
indexed spans"), but the data structures are still flat.

### Rewrite plan

1. Change `EditorViewModel.whitespace_glyph_runs` to a `std::vector<RowSpan>` where each entry
   `(row_offset, count)` indexes the contiguous range in a flat run array — already how
   `RenderViewModelBuilder::CollectWhitespaceGlyphRuns` produces them (rows are emitted in order).
   Add a parallel `row_start_offsets` of size `visible_rows + 1`.
2. Apply the same layout to `occurrence_ranges`: sort by `line_index` and store an offset table
   keyed by row index.
3. Drop the `if (glyph.visual_row_index != …)` filter from the row paint hot loop.

### Expected impact

- `editor_render_whitespace_paint` and `editor_indent_guides_paint` paint loops drop from O(R·G)
  to O(G), where G is the visible whitespace glyph count.
- Same shape benefits the occurrence-highlight paint when many in-view matches exist.

---

## Finding 3: `RefreshStatusBar()` Still Allocates Per Frame, And Synchronously Runs `git`

Relevant code:

- `src/workspace/WorkspaceShellChrome.cpp:641-700`
- `src/workspace/WorkspaceShellChrome.cpp:31-47` (`ResolveBranchLabel`)

Round 1 added caches for line/column, indent, and the repo-validity probe. The branch + cleanliness
segment, however, is still recomputed every frame:

```cpp
project_segment.text = branch_label == "no-scm" && cleanliness == "no-scm"
                         ? std::string("no-scm")
                         : branch_label + " [" + cleanliness + "]";
project_segment.tooltip = "Open Source Control (" + cleanliness + ")";
```

Both `branch_label + " [" + cleanliness + "]"` and the tooltip concatenation allocate every frame,
regardless of whether anything changed.

Worse, when `sidebar.git.branch_label` is empty, `RefreshStatusBar` calls `ResolveBranchLabel()`
which runs `git symbolic-ref --short HEAD` **synchronously on the shell thread**. This is a frame-
path subprocess spawn — exactly the failure mode Finding 8 of the round-1 doc was meant to remove,
and it conflicts with the documented invariant: *"No `platform::RunSubprocess(...)` calls in
workspace `.cpp` units; dispatch through `ProjectBackgroundExecutor`."*

### Rewrite plan

1. Cache the project/branch/cleanliness segment by `(branch_label, cleanliness, layout_mode)`. Only
   rebuild the strings when one of those changes.
2. Delete `ResolveBranchLabel()` from `WorkspaceShellChrome.cpp`. The branch label is already
   produced asynchronously by `WorkspaceSidebarCoordinatorRefresh`; let the status bar read a
   missing label as "…" or empty until the async path lands.
3. Update the architectural lint to also flag `git symbolic-ref` and other `repo.Execute(...)`
   calls from frame-prep TUs.

### Expected impact

- Status-bar prep stops allocating ~4 strings/frame and never blocks on a child process.
- The "git just locked the UI for 50 ms" hazard goes away.

---

## Finding 4: `ParseStickyScrollMaxDepthSetting` Violates The `std::sto*` Invariant

Relevant code:

- `src/workspace/RenderViewModelBuilder.cpp:297-314`

```cpp
try {
  const long parsed = std::stol(*value);
  …
} catch (...) {
  return kDefault;
}
```

CLAUDE.md / hard architectural invariants: *"Numeric token parsing uses `util/Parse.h` (`ParseInt`,
`ParseInt64`, `ParseSize`, `ParseFloat`). No `try`/`catch` around `std::sto*`."* This is the **only**
remaining `try` block in `src/` and it's on a render-adjacent code path that runs every editor
view-model build for a tab with sticky scroll enabled.

### Rewrite plan

1. Replace with `util::ParseInt(*value).value_or(kDefault)` followed by the existing clamp.
2. Move parsing out of the render-time view-model builder entirely — sticky-depth is a setting,
   parse it once when settings change and cache the typed value on `EffectiveEditorSettings`.

### Expected impact

- Restores the invariant the architectural lint enforces elsewhere.
- Removes the exception-construction tax on the cold path.

---

## Finding 5: `EnsureWrappedRowLayouts` Still Rebuilds A `O(line_count)` Vector Per Edit

Relevant code:

- `src/editor/TextViewport.cpp:2996-3082`
- `src/editor/TextViewport.cpp:1497-1573` (`InvalidateDerivedCaches`)

Round 1 documented that small edits trigger full document-scale invalidation. The newly-added "no-
soft-wrap fast path" (line 3030) does avoid the per-line `VisualColumnForTextColumn` walk, but it
still:

- Allocates `wrapped_row_layouts_` with `lines.size()` entries.
- Allocates `wrapped_line_row_offsets_` with `lines.size()` entries.
- Walks `folding_model_->collapsed_flags()` linearly to compute `has_any_collapsed_fold`.

`InvalidateVisualColumnCache` is called on every edit, clearing both vectors. Then the next render
re-fills them. For a 50 000-line file, that is two 50 000-element vector reallocations per
keystroke even when the file has no soft-wrap and no folds.

### Rewrite plan

1. Add a "trivial wrapped layout" mode: when soft-wrap is off and no folds are collapsed, do not
   build `wrapped_row_layouts_` / `wrapped_line_row_offsets_` at all. `VisualRowLineIndex(r)` and
   `VisualRowCount()` can return `r` and `lines.size()` directly.
2. Cache `FoldingModel::has_any_collapsed_fold()` as a counter inside the model; invalidate when
   a `Toggle*` mutates flags. No more O(N) scan to ask "are any folds collapsed?"
3. Split `document_->layout_revision` into a content revision (text bytes) and a layout-shape
   revision (soft-wrap width, fold visibility, tab size). Trivial edits should only bump the
   content revision; the layout vectors should keep their identity unless shape changes.
4. When the layout is non-trivial, support incremental updates: `ApplyHistoryEntryToLines` already
   knows `start_line`, `removed_count`, and the inserted slice; rebuild only the affected slice of
   `wrapped_row_layouts_` and shift the tail offsets.

### Expected impact

- `editor_smart_indent_typing` / `editor_auto_close_typing` allocations should drop from millions
  to dozens per keystroke when no soft-wrap or folds are active (the common case).
- `editor_fold_recompute` benefits because `has_any_collapsed_fold` no longer rescans flags.

---

## Finding 6: `FoldingModel::Compute` Snapshots Previous Ranges And Remaps O(N²)

Relevant code:

- `src/editor/FoldingModel.cpp:227-242` (`RemapCollapsedFlags`)
- `src/editor/FoldingModel.cpp:271-360` (`ComputeWithBudget`)
- `src/editor/FoldingModel.cpp:49-55` (`FindPair`)

Each `ComputeWithBudget` call:

1. Full-copies `ranges_` into `previous_ranges` and `collapsed_` into `previous_collapsed` (line
   277-278) — even on fresh recomputes where no collapsed state is preserved.
2. Runs `RemapCollapsedFlags`, an O(N · M) loop where N = new ranges and M = previous ranges.
3. For every character of every scanned line, calls `FindPair`, which is a linear scan of the
   `bracket_pairs` vector (typically `()`, `{}`, `[]`).

For a large source file with ~20 000 fold ranges (deeply nested JSON / minified JS), `RemapCollapsedFlags`
runs ~4×10⁸ comparisons. `FindPair` is called once per source byte during the bracket scan; on a
5 MB source that is 5 million linear-scan-of-3 calls.

### Rewrite plan

1. Replace `FindPair` with a 256-byte `bracket_class[]` lookup table populated at scan start
   (matching `'('`→0, `')'`→0, `'{'`→1, etc.). `IsBracketChar` becomes a single load.
2. Build a `std::unordered_map<OpenerLine, bool>` of currently-collapsed openers once; remap is then
   O(N) instead of O(N·M). Even better: store `collapsed_` as a `std::unordered_set<std::size_t>` of
   collapsed opener lines so it survives a `ranges_` rebuild without needing a remap at all.
3. Avoid copying `previous_ranges` when no remap is needed (e.g., first compute on a fresh document
   or after `Clear()`).
4. When the bracket-prefix incremental path is valid, the indent ranges produced by `ScanIndentRanges`
   still rescan the whole `[0, scan_end)` range using `bracket_opener[]` to suppress lines that have
   bracket folds. Make this incremental too — `bracket_opener[]` for the kept prefix is already
   known.

### Expected impact

- Folding recompute on a 20 k-range file becomes essentially O(edited-region + N).
- The bracket-scan inner loop runs a `[]`-indexed table lookup instead of a 3-element linear scan
  for every source byte. Expect ~3× speedup on the bracket scan alone.

---

## Finding 7: Render TUs Still Materialize Strings In Hot Paths

Relevant code:

- `src/workspace/WorkspaceShellRenderSidebar.cpp:20-31` (`BuildProjectSearchResultLabel`)
- `src/workspace/WorkspaceShellRenderSidebar.cpp:337` (`std::string(1, git_marker)`)
- `src/workspace/WorkspaceShellRenderSidebar.cpp:479` (same pattern)
- `src/workspace/WorkspaceShellRenderBottomPanel.cpp:140-153` (per-run `std::string run_text`)
- `src/workspace/WorkspaceShellRenderBottomPanel.cpp:191-200` (`std::string header_label = "Command"` rebuilt per frame)
- `src/workspace/WorkspaceShellRenderBottomPanel.cpp:207` (`ActiveLspStatusText(...)` returns `std::string` each frame)
- `src/workspace/WorkspaceShellRenderBottomPanel.cpp:365` (`std::string(display_text)` for cursor cell)
- `src/workspace/WorkspaceShellRenderBottomPanel.cpp:383-396` (`status_text`, `panel_fallback` rebuilt per frame)
- `src/workspace/WorkspaceShellRenderTextInput.cpp:78` (`std::string full_text = std::string(prefix) + state.text();`)
- `src/workspace/WorkspaceShellRenderTextInput.cpp:140` (returns `std::string displayed_text`)
- `src/workspace/WorkspaceShellRenderMenus.cpp:64` (`spec ? std::string(spec->label) : std::string{}`)
- `src/editor/EditorViewRenderer.cpp:245` (`"text renderer: " + std::string(text_renderer.BackendName())`)
- `src/editor/EditorViewRenderer.cpp:52-58` (`ToLower(...)` allocates per call)

The hard architectural invariant says: *"Render translation units must not materialize new strings in
hot paths (`std::string(...)`, string `+`/`+=`, `to_string`, or `std::format`/`fmt::format`); compute
render text in `RenderViewModelBuilder` instead."* The lint must not currently be catching these.

The terminal foreground-run rebuild (`run_text` at line 140) is particularly bad: it allocates one
`std::string` per same-color run per visible terminal row per frame. A 50×100 terminal with mixed
colors easily produces 50-200 run-string allocations per frame, all destroyed immediately.

### Rewrite plan

1. Tighten the architectural lint to detect `std::string(` constructions and string `+/+=` in render
   TUs (currently regex-based — extend or move to a Clang AST matcher).
2. Move per-row terminal run assembly into a `TerminalSession` snapshot that produces
   `std::span<const TerminalRunDescriptor>` keyed by `(line_generation, selection_generation)`. The
   render TU only reads spans.
3. For `BuildProjectSearchResultLabel` and similar formatters: pre-format into the sidebar
   view-model when the project-search results change, not per render.
4. For `git_marker == ' ' ? "" : std::string(1, git_marker)`: store as `char marker;` in the view
   model and use `text_renderer_.DrawString(..., std::string_view(&marker, 1))` (already what
   `DisplayText` does for terminal cells).
5. Convert `ComputeSingleLineViewMetrics` to operate on `std::string_view` over the live editor
   state; have it return a `(view_start, view_end)` byte range plus cursor x instead of an owning
   `std::string displayed_text`. Hand `displayed_text` to render TUs as `std::string_view`.

### Expected impact

- Eliminates 20-50 hot-path string allocations per frame across the chrome surfaces.
- Makes the lint actually enforce the invariant.

---

## Finding 8: `TerminalCell` Carries A `std::string` Per Cell

Relevant code:

- `src/terminal/TerminalSession.h:30-41`

```cpp
struct TerminalCell {
  char character = '\0';
  std::string text;
  TerminalStyle style;
};
```

Every cell holds a `std::string` (24 bytes empty on libstdc++) for non-ASCII glyphs. For a 200×100
terminal with mixed UTF-8 output, that is potentially 20 000 small-string allocations and 24×20 000
bytes of headers, plus per-cell `style` (~16 bytes). On scrolling, `TrimScrollbackLocked` destroys
all these strings; `SnapshotLineRangeIfChanged` copies them.

For pure-ASCII content the `text` is empty and the inline `char character` is used, but the storage
overhead is paid unconditionally.

The deep-dive Finding 6 already mentioned per-cell UTF-8 storage, but did not propose collapsing
the representation.

### Rewrite plan

1. Replace `TerminalCell::text` with a `std::array<char, 7>` (covers up to 4-byte UTF-8 plus a
   length byte) and a `uint8_t length`. No allocation per cell; the cell becomes trivially
   copyable.
2. For genuinely wide grapheme clusters (rare), promote to a sidecar
   `std::vector<std::pair<RowCol, std::string>>` per line and reference it from the cell via an
   index.
3. Update `DisplayText()` to return `std::string_view` from the inline storage.
4. Combined with Finding 7's run aggregator, snapshotting becomes a bulk memcpy of trivially
   copyable cells.

### Expected impact

- Terminal allocation count during heavy output should fall by an order of magnitude (mostly cell
  storage churn).
- Scrollback snapshot copies become cheap memcpys; the round-1 doc's recommended ring/paged store
  becomes easier to implement.

---

## Finding 9: `TextRenderer` Width Cache Stores Each Key String Twice

Relevant code:

- `src/render/TextRenderer.h:67-68`
- `src/render/TextRenderer.cpp:182-199`

```cpp
mutable std::unordered_map<std::string, float, …> width_cache_;
mutable std::deque<std::string> width_cache_order_;
```

The LRU order deque holds full `std::string` copies of every cache key, mirroring the keys already
present in the map. With `kWidthCacheCapacity = 4096` entries and an average key length of 20 bytes
that's ~80 KB duplicated plus ~96 KB of `std::string` headers. Worse, every cache insertion does an
extra heap allocation to copy the key into the deque, and every eviction destroys it.

`unordered_map` does not invalidate references to keys on rehash, so the order deque can hold
`std::string_view`s referencing the map's keys.

### Rewrite plan

1. Change `width_cache_order_` to `std::deque<std::string_view>` (or `std::list<std::string_view>`
   for splice-on-touch promotion).
2. On eviction, look up the map by the view (already done) and erase.
3. As a follow-up, replace the LRU-on-deque with a single `std::list<CacheNode>` plus a map of
   `std::string_view` → list iterator (the pattern used by `SdlTtfTextBackend::cache_order_`),
   which makes touch-promotion O(1) and avoids the deque churn on cache hit.
4. Add a perf counter `RenderTextWidthCacheEvictions` to mirror the texture cache.

### Expected impact

- ~80 KB resident size reduction in steady state.
- One less heap allocation per cache insertion; eviction becomes pointer-only work.

---

## Finding 10: `secondary_carets()` And Friends Allocate Vectors Per Call

Relevant code:

- `src/editor/TextViewport.cpp:1014-1021`
- `src/editor/EditorViewRenderer.cpp:368` (binds to a returned-by-value temporary)
- `src/editor/ShapingActions.cpp:39, 171`
- `src/editor/SnippetEngine.cpp:230`
- `src/workspace/WorkspaceEditActionExecutor.cpp:259`

`TextViewport::secondary_carets()` returns `std::vector<TextPosition>` by value. The render path
binds it to `const auto& secondary_carets = …`, which extends the temporary's lifetime but still
allocates a fresh vector every frame.

### Rewrite plan

1. Add `secondary_caret_positions()` returning `std::span<const TextPosition>` backed by a small
   `std::vector<TextPosition>` member kept in sync with `secondary_carets_`. Update when
   `secondary_carets_` mutates rather than per query.
2. Replace the existing `secondary_carets()` callers in render with the span-returning method.
3. The non-render callers (snippet engine, edit executor) that need an owning copy can call a
   renamed `secondary_caret_positions_copy()`.

### Expected impact

- One allocation per render frame removed; same per call in edit action paths.

---

## Finding 11: `RenderViewModelBuilder` Caches Are Copied Out On Every Cache Hit

Relevant code:

- `src/workspace/RenderViewModelBuilder.cpp:511`
- `src/workspace/RenderViewModelBuilder.cpp:556-557`

```cpp
out.sticky_lines.assign(g_sticky_scroll_cache.lines.begin(),
                        g_sticky_scroll_cache.lines.end());
…
out.occurrence_ranges.assign(g_occurrence_scan_cache.ranges.begin(),
                             g_occurrence_scan_cache.ranges.end());
```

The whole point of these caches is to avoid recomputing the lines/ranges, yet on every cache hit
both vectors are copied into the editor view model. For a busy occurrence highlight with hundreds
of matches, that is a fresh vector allocation each frame.

### Rewrite plan

1. Change `EditorViewModel.sticky_lines` and `EditorViewModel.occurrence_ranges` to
   `std::span<const std::size_t>` and `std::span<const editor::OccurrenceRange>` respectively,
   pointing at the cache's storage.
2. The cache vectors are `thread_local` and stable for the duration of a frame; spans are safe.
3. Add lifetime documentation to `EditorViewModel`.

### Expected impact

- Removes 1-2 vector copies per editor pane per frame.

---

## Finding 12: `RefillOccurrenceScanCache` Builds A Hash Set Per Refresh

Relevant code:

- `src/workspace/RenderViewModelBuilder.cpp:150-160`

```cpp
std::unordered_set<std::size_t> visible_line_indices;
visible_line_indices.reserve(visible_rows);
…
for (std::size_t row = 0; row < visible_rows; ++row) {
  …
  visible_line_indices.insert(row_meta.line_index);
}
```

The set dedupes wrapped-row visits that share a buffer line. With ~50 visible rows the set
allocates 64-128 nodes, runs hash lookups, and then we *iterate it unordered* — meaning occurrence
ranges are appended in arbitrary order.

### Rewrite plan

1. Collect indices into a `thread_local std::vector<std::size_t>` (reuses capacity across frames),
   then `std::sort` + `std::unique`. For ~50 entries that beats a hash set on both allocation and
   constant factor, and produces sorted output.
2. Sorted indices let downstream consumers (Finding 2) use binary search instead of `find_if`.

### Expected impact

- Removes one hash set allocation per cache refill.
- Produces sorted occurrence ranges, which makes the per-row paint loop fix in Finding 2 cleaner.

---

## Finding 13: Editor `VisualColumnForTextColumn` Walks Each Line Multiple Times Per Frame

Relevant code:

- `src/editor/TextLayout.cpp:9-19`
- `src/editor/EditorViewRenderer.cpp:602-720` (search matches, occurrences, selection, brackets)

Per visible row, `EditorViewRenderer::Render` calls `VisualColumnForTextColumn` *up to ten times*:
- 2 per search match (start/end), times match count.
- 2 per occurrence match.
- 2 per selection (start/end).
- 1 per bracket cell × 2 brackets.

Each call walks the line from byte 0 up to the target column, repeating UTF-8 length decoding and
tab-stop math. On long lines this multiplies the per-row cost.

The row's `LayoutLine` already contains `source_columns[]` and `text_offsets[]`, which lets you map
between source column and visual column in O(log row_width) by binary search.

### Rewrite plan

1. Add `TextLayout::VisualColumnFromLayout(const LayoutLine&, std::size_t source_column)` that does
   a `std::lower_bound` on `source_columns`. Use this in the per-row paint instead of walking the
   line again.
2. Where the column is past the layout's right edge (off-screen), fall back to a single
   `VisualColumnForTextColumn` per line, cached for the frame.
3. Alternative: cache `(line_index, source_column) → visual_column` for the visible window inside
   the renderer per frame.

### Expected impact

- Per-row paint becomes O(active_decorations × log row_width) instead of O(active_decorations × line_length).
- Especially helpful for very long lines (minified JS, JSON) and dense selections.

---

## Finding 14: `IndentGuides` Re-Walks Leading Whitespace Per Row, Emits Per-Row Segments

Relevant code:

- `src/editor/IndentGuides.cpp:27-92`

`ComputeIndentGuides` runs `LeadingVisualIndent` on every visible row and then emits one guide *per
indent step per row*. For a file with depth-8 indentation at `indent_width=2` and 50 visible rows,
that is 400 `IndentGuideRun` entries — each a 1-row segment — pushed into `out`.

The render-side paint then iterates that flat vector per row (`if (guide.start_row != row) continue;`),
same shape as Finding 2.

### Rewrite plan

1. Compute leading indent once per *buffer line* (not per visible row), cache by `(line_index,
   layout_revision, tab_size)`.
2. Coalesce vertical runs of identical indent depth into a single `IndentGuideRun` spanning
   `[start_row, end_row]`. The renderer already paints rectangles; coalescing reduces the inner
   loop from per-row segments to per-block segments.
3. Make the output row-indexed (Finding 2 again) so the renderer doesn't need the per-row filter.

### Expected impact

- `editor_indent_guides_paint` should drop from 700 ms p50 to roughly the visible-cell paint cost.

---

## Finding 15: `SdlTtfTextBackend` Cache Misses Are Code-Editor Hostile

Relevant code:

- `src/render/SdlTtfTextBackend.cpp:429-503`
- `kMaxCacheEntries = 4096`

The texture cache keys by `(text, color, [background])`. Editor content with syntax highlighting
produces unique color-per-token *strings*, so scrolling a large file rapidly evicts entries and
re-builds composite surfaces.

Each cache miss for ASCII runs `BuildAsciiCompositeSurface`, which does N individual
`TTF_RenderText_Blended` calls and N `SDL_BlitSurface` calls, then `SDL_CreateTextureFromSurface`.
That is roughly N+2 SDL allocations and one GPU upload per unique (token-string, color) pair.

Round 1 (Finding 5) flagged this; no glyph atlas exists yet.

### Rewrite plan

1. Build a glyph atlas: pre-rasterize ASCII (0x20..0x7E) into a single SDL texture per color hash;
   render runs by `SDL_RenderTextures` on the atlas. Color is applied via `SDL_SetTextureColorMod`,
   so the atlas itself is one alpha mask per font size, reused across themes.
2. Keep the existing whole-string composite as a fallback for non-ASCII shaped runs.
3. Add perf counters: glyph-atlas hits, atlas evictions (never if sized correctly), composite-path
   strings rendered.
4. Sample-based perf gate: scroll a 5 000-line C++ file and assert the steady-state texture-cache
   miss rate is <1 % once the atlas is warm.

### Expected impact

- Scrolling through unique code lines becomes glyph-bound, not texture-creation-bound.
- Editor `editor_render_whitespace_paint` and similar paint scenarios should benefit even without
  the row-indexing changes.

---

## Finding 16: `RemapCollapsedFlags`, `wrapped_row_layout` Rebuilds, And Settings Recompute Are All Triggered By A Single Revision

Relevant code:

- `src/editor/TextViewport.cpp:1497-1573` (`InvalidateDerivedCaches`)
- `src/editor/TextViewport.cpp:1581-1595` (`InvalidateVisualColumnCache`)
- `src/editor/TextViewport.cpp:1597-1600` (`InvalidateLayoutCaches`)

Every edit currently runs:

1. `++document_->layout_revision` (single global revision).
2. Clears visible-line cache, highlight cache, line-highlight states, checkpoints — all from
   `safe_start` to EOF when `safe_start > 0`, all entirely when `safe_start == 0`.
3. `InvalidateVisualColumnCache` clears `wrapped_row_layouts_` (Finding 5), the visual-column cache,
   and the `cached_visual_line_columns_` map.

So a one-character insertion at line 5 invalidates **all** wrapped rows ≥ start (which is the whole
document past line 5), **all** visible-line layout entries ≥ start, **all** highlight cache entries
≥ start, **and** the entire visual-column cache. The visual-column cache flush has no `start_line`
parameter at all; it's an unconditional clear.

### Rewrite plan

1. Split revisions as proposed in round-1 Finding 2: `content_revision`, `syntax_revision`,
   `layout_shape_revision`, `presentation_revision`. Trivial edits bump only the content revision.
2. Make `InvalidateVisualColumnCache` range-based: accept `start_line` and only erase
   `cached_visual_line_columns_` entries with `key.line_index >= start_line`.
3. For non-soft-wrap, the visual-column cache is keyed by `(line_index, tab_size)`; it does not
   need to be cleared when an unrelated line changes.
4. `line_highlight_states_` should be resized only when the line count changes; clearing back to
   `SyntaxState{}` for the tail is fine, but allocating a fresh vector is not.

### Expected impact

- The cascade of caches that drop on every keystroke shrinks to "the changed line and downstream
  syntax checkpoint". Caret motion, scrolling, and bracket matching all stop paying the per-edit
  invalidation tax.

---

## Finding 17: Per-Frame Scratch Vectors That Could Be Members Are Locals

Relevant code:

- `src/workspace/WorkspaceShellRenderTextInput.cpp:88-95` (`std::vector<CharEntry> before_cursor`)
- `src/workspace/WorkspaceShellRenderSidebar.cpp:69-82` (`BuildVisibleStripTabs` returns a fresh
  vector — already in `WorkspaceShellChrome.cpp:69`)
- `src/editor/TextViewport.cpp:780-790` (`std::vector<std::string> before_changed_lines` for
  replace-all — accepted, since it is structural)

`ComputeSingleLineViewMetrics` runs on every render where a text-input surface is visible, and each
call constructs a fresh `std::vector<CharEntry>`. With 4-5 prompts/inputs on screen (file finder,
project search, buffer search, command, sidebar query/replace) plus refresh-on-every-keystroke,
this is dozens of small allocations per second.

`BuildVisibleStripTabs` returns a `std::vector<VisibleStripTab>` by value on every chrome refresh,
even when the tab strip is stable across frames.

### Rewrite plan

1. Convert `before_cursor` to a `mutable thread_local std::vector<CharEntry>` (cleared, not
   reallocated) inside `ComputeSingleLineViewMetrics`.
2. For `BuildVisibleStripTabs`, accept an output `std::vector<VisibleStripTab>*` and reuse a
   scratch member on `WorkspaceShell`. Many of these helpers already follow this pattern (see
   `lowered_search_query_scratch_`, `scratch_row_`, `last_fold_gutter_marks_`).
3. Audit `src/workspace/*Render*.cpp` for `std::vector<...>` locals in per-frame helpers and lift
   them to `thread_local` or shell-owned scratch.

### Expected impact

- A handful of allocations per frame removed; cache-friendlier in cold-start frames.

---

## Finding 18: `PrepareFrameOnce` Still Drives Multiple Subsystems Synchronously

Relevant code:

- `src/workspace/WorkspaceShellRenderFrame.cpp:55-147`

`PrepareFrameOnce` runs in this order each frame:

1. `ConsumePendingProjectOpenDialogResult()` — reads a queue, can mutate state.
2. `ConsumeProjectSearchUpdates()` — same.
3. `text_renderer_.EnsureInitialized(...)`.
4. `BuildSidebarSurface()` (allocates strings, Finding 1).
5. `BuildBottomPanelSurface()`.
6. `ApplyLiveSettings()` (round-1 fixes gated this, but the call is still unconditional).
7. `ComputeLayout(...)` *unless* `layout_dirty_` is unset.
8. `RefreshStatusBar()` (Finding 3).
9. `RefreshSettingsOverlayCatalog()` — rebuilds the settings list?
10. `MakeTextInputCoordinator().SyncTextInputSurface(render_window)`.
11. `NormalizeEditorSplitTree(*editor_tab)` if active.
12. `ResizeTerminalToPanel(panel)` if panel changed.

`RefreshSettingsOverlayCatalog()` looks like a candidate for "only when settings or overlay
visibility changes". Let me check.

### Rewrite plan

1. Track an "input revision" (events consumed since last frame). When zero, skip steps that only
   react to user input (queue consumers, settings refresh, status-bar refresh).
2. Audit each `Refresh*` for an early-exit condition.
3. Define a "steady frame" state (no events, no cursor blink, no animation) and assert in debug
   that `PrepareFrameOnce` does zero allocations in that state.

### Expected impact

- Idle frames become almost free; this is the long tail of CPU/battery cost when MicroIDE is
  focused but the user is reading.

---

## Finding 19: Architectural Lint Coverage Gaps

The architectural lint (`tests/ArchitectureInvariantsTests.cpp`) hard-fails on a documented set of
violations, but several of the round-1 invariants are not currently enforced by it:

1. **Hot-path string materialization** in render TUs (Findings 1, 3, 7, 17) — the invariant is
   documented but the test does not catch all the offending constructs (e.g., concatenation with
   `+`, `std::string(1, ch)`, `std::string(std::string_view)`).
2. **`std::sto*` with `try/catch`** is forbidden but the test does not scan for `std::stol|stoi|stoul|stoll`
   call sites — Finding 4 slipped through.
3. **Synchronous `git` subprocess via `repo.Execute(...)`** in workspace render/frame-prep TUs is
   not enforced; only `platform::RunSubprocess` is.
4. **`RenderViewModelBuilder` surfaces being built more than once per frame** (Finding 1) is not
   asserted anywhere.

### Rewrite plan

1. Add regex-based lint patterns for `std::string\s*\(`, ` + std::string`, `std::stol|stoi|stoul|stoll`
   restricted to `src/workspace/*Render*.cpp` and `src/editor/*Render*.cpp`.
2. Extend the lint to forbid `repo\.Execute\(` in `WorkspaceShell*.cpp` files (other than the git
   coordinator that owns the async path).
3. Add a debug-only counter in `PrepareFrameOnce` that increments per surface build, and have the
   perf harness fail if any surface counter exceeds 1 in a steady frame.

### Expected impact

- The fixes from this and the round-1 doc stay fixed.
- Future contributors get an enforced boundary instead of advisory text.

---

## Prioritized Plan

### P0: Re-establish lint coverage

Finding 19 first: tighten the architectural lint so that fixes for Findings 1, 3, 4, 7, 17, 18 stay
in. Otherwise this work decays the next time someone adds a "small" feature.

### P1: Frame-prep cleanups

Findings **1** (done: prepare-scoped sidebar/bottom/text-input VMs + per-clip frame/overlay cache), **3, 4, 11, 12, 17** (done). Finding **18** remains deferred (event-revision / steady-frame infrastructure).

### P2: Editor invalidation surgery

Findings 5, 6, 10, 13, 14, 16 — convert wrapped-row layout, folding remap, indent guides, visual
column mapping, and per-edit invalidation to incremental/range-based form.

### P3: Render hot-path text

Findings 2, 7, 9, 15 — row-indexed view models, glyph atlas, and string-view-friendly width cache.

### P4: Terminal correctness-plus-perf

Findings 7 (terminal subset) + 8 — collapse per-cell `std::string`, replace per-row run-string
allocation with snapshot-stable spans.

---

## What This Doc Does Not Cover

- Compare/merge large-input perf (round-1 Finding 9 still open — confirmed unchanged).
- Plugin hover debounce/caching (round-1 Finding 10 still open).
- File index / project search large-repo work (round-1 Finding 7 still open).

These were re-checked and have not regressed but also have not been improved since round 1; they
remain valid follow-ups.
