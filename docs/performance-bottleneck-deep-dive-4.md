# Performance Bottleneck Deep Dive — Round 4

Date: 2026-05-15

## Status snapshot

| # | Finding                                                | Status |
| - | ------------------------------------------------------ | ------ |
| 1 | Fold bracket scan thrashes syntax-highlight LRU        | **done** — `HighlightedLineTokensIfCached` (non-forcing) + per-line hoist in `FoldingModel.cpp`. `editor.highlight_cache_forced_misses` over 5 fold scenarios drops from ~24 k/iter (when slow) to ~200/iter. `editor_fold_recompute` now passes its 60 ms per-sample p95 budget. |
| 2 | `EnsureWrappedRowLayouts` 50 k visits/keystroke         | **done** — trivial-cache hit re-keyed on `(wrapped_row_layouts_trivial_ && trivial_now)` only; `editor.ensure_wrapped_row_layouts_line_visits` drops from 36.7 M to **0** across all typing/multi-tab scenarios. |
| 3 | Terminal scrollback trim is O(N) front-erase             | **done** — trim coalesced (only act at 1.25× max), `lines_` switched to `std::deque<TerminalLine>` for O(1) pop-front, OOB guard added in `RenderBottomPanelSurface` for the count/snapshot skew. `terminal.trim_scrollback_calls` over 3 iterations: 20 182 → 1 735 (**11.6× reduction**). |
| 4 | `editor_sticky_scroll_scroll` paints 1.07 s for 96 frames | **partial** — `InvalidateDerivedCaches` no longer eagerly resets `line_highlight_states_` / `highlight_checkpoints_`; replaced with `*_valid_through_` cursors gated at each read site. Smoke wall-time unchanged because the reset loop was already trivial (struct assignments), but the change removes the round-2 #16 cascade pre-condition for future layout-revision splitting. |
| 5 | `idle_soak_30s` runs 51 prepares during a 27 s idle window | **measured, deferred** — `workspace.scheduled_wakes` counter now in place; on this branch only 17 partial renders fire across 30 s of soak (≈ caret-blink cadence). The remaining 7.5 k allocs/sec are spread across normal partial-render work; further reductions need surgery on `PrepareFrameOnce` not unlocked by this pass. |
| 6 | `menu_hover_switch` 29 k width-cache queries per frame  | **done** — menu-bar top-level spec widths now cached in a `thread_local` map keyed by static label pointer. `menu_hover_switch` queries 176 k → 127 k (~28 %); `menu_popup_hover_rows` queries 29 k → 14 k (~50 %); wall p50 −17 % and −28 % respectively. |
| 7 | `multi_tab_cycle` does 1 710 wrapped-row rebuilds        | **done** — closed by Finding 2; `multi_tab_cycle` wall p50 309 → 293 ms (−5 %), line visits 8 495 → **0**. |

### Tooling counters added in this pass

The doc's "Tooling" section listed 5 counters that would have caught the
landed and remaining findings earlier. All 5 are now wired:

| Counter | Hook |
| ------- | ---- |
| `editor.highlight_cache_forced_misses` | `TextViewport::HighlightedLineTokens` cache-miss branch — keeps fold-scan-style thrash visible |
| `editor.highlight_cache_evictions`     | Eviction during cache miss — surfaces LRU pressure separately from miss rate |
| `render.clip_invocations`              | `WorkspaceShell::RenderClip` entry — divide by `frame.prepare_calls` for round-3 Finding 1's "RenderClipInvocationsPerFrame" |
| `workspace.scheduled_wakes`            | `WorkspaceWakeController::HandleScheduledWake` — counts idle-tax wakes for Finding 5 |
| `terminal.scrollback_lines_allocated`  | `TerminalSession::EnsureCursorLineExistsLocked` line growth — separates "trim freed" vs "trim reused" once Finding 3 lands |

## Method

Round 4 is **measurement-led**: I ran `microide_perf` across smoke + advisory
scenarios, captured per-iteration `perf_counters` from the JSON reports, and
used `MICROIDE_PERF_TRACE` with a temporary instrumentation scope inside
`EnsureFoldingModelFresh` to pin down where a 1.6 s/render scope was actually
going. Findings are ordered by measured impact, not file location.

## Headline numbers (this branch, dummy SDL, 3 iterations)

| Scenario                          | wall p50 | wall p95 | allocations p50 |
| --------------------------------- | -------: | -------: | --------------: |
| `editor_fold_recompute`           | **fails** | **fails** | n/a (asserts before report) |
| `editor_sticky_scroll_scroll`     | 1 072 ms | 1 096 ms | 850 776 |
| `editor_render_whitespace_paint`  | 807 ms   | 812 ms   | 637 543 |
| `editor_smart_indent_typing`      | 838 ms   | 852 ms   | 3 669 245 |
| `editor_auto_close_typing`        | 727 ms   | 729 ms   | 327 702 |
| `editor_indent_guides_paint`      | 644 ms   | 649 ms   | 572 991 |
| `multi_tab_cycle`                 | 309 ms   | 314 ms   | 879 972 |
| `terminal_scroll_long_output`     | 109 ms   | 112 ms   | 1 939 912 |
| `idle_soak_30s` (30 s soak)       | 30 432 ms | 30 475 ms | 58 529 |

The catastrophic line is `editor_fold_recompute`: it has a **hardcoded
`EnforceP95Microseconds(... 60'000.0)` budget**, and we measure
**p95 = 1 372 528 – 1 716 275 µs** (23–28× over budget). The scenario was added
in `d8a8a0d` together with the assert; it has likely never passed on this code.

---

## Finding 1: Fold bracket scan thrashes the syntax-highlight LRU (the headline)

**Code:** `src/editor/FoldingModel.cpp:84-96, 127-128, 167-168` (calls into
`viewport->HighlightedLineTokens(line_index)` once per bracket byte) and
`src/editor/TextViewport.cpp:990-1028, 1021` (cache limit `kHighlightCacheLimit = 256`).

**What the trace shows.** With a temporary `PerformanceTrace::Scope` inside
`EnsureFoldingModelFresh`, `editor_fold_recompute` (insert at line 25 000 of a
50 000-line C++ file, then one render frame, then undo) yields:

```
EnsureFoldingModelFresh  286.5 ms   (first iteration, after-edit fold rebuild)
EnsureFoldingModelFresh  168.1 ms   (second iteration, after-undo fold rebuild)
EnsureFoldingModelFresh  <1 ms      (iterations 3..24, incremental prefix hits)
```

p95 of 24 samples ≈ second-worst sample ≈ 168 ms. The fold model's
`ComputeWithBudget(2000)` is asked for `target_end = scroll_line + visible_rows
+ 32`, and `work_budget = max(2000, target_end) ≈ 25 093` — so the bracket
scanner walks ~25 000 lines. For every bracket byte it hits, it calls

```cpp
const auto& tokens = viewport->HighlightedLineTokens(line_index);
```

`HighlightedLineTokens` is backed by an unordered-map LRU with
`kHighlightCacheLimit = 256` entries. With 25 000 distinct line indices and a
256-line cap, **24 744 cold misses** are forced; each miss runs the syntax
highlighter for that line plus `HighlightStateBeforeLine(...)`. The bracket
scanner ends up paying the cost of full syntax highlighting for ~25 000 lines
on every fold recompute that targets a far-from-warmed region.

This is also why the second iteration drops to 168 ms (vs. 286 ms): the prior
warming left a thin sliver in the LRU that survives the next pass, but with
25 000 ≫ 256 it still thrashes.

**Fix sketch.**

1. Add `TextViewport::HighlightedLineTokensIfCached(line)` that returns a
   `std::span<const SyntaxTokenKind>` and a `bool` for "cached", **without**
   triggering `EnsureHighlightCaches` or `HighlightLine`.
2. In `FoldingModel.cpp::IsSuppressedBracketToken`, switch to the
   non-forcing call. When tokens are not cached, treat as "not in a
   string/comment" (i.e., do not suppress). Far-from-viewport scans then
   over-emit a handful of fold ranges for brackets inside strings — a much
   smaller correctness cost than locking the UI for 0.5–2 s.
3. Hoist the per-line lookup **out of the per-byte loop**: take the span once
   per line and index it by column. With the non-forcing call this is also
   cheaper for the hot path (one map lookup per scanned line instead of one per
   bracket byte).
4. Add a perf counter `EditorHighlightCacheForcedMisses` so future regressions
   of "fold scan forces syntax highlight of N k lines" are visible.

**Expected impact.** Fold recompute time drops from 168–286 ms to single-digit
ms on the 50 k-line fixture. `editor_fold_recompute` returns inside its 60 ms
budget. `editor_sticky_scroll_scroll` and `editor_indent_guides_paint` also
benefit if their fold recomputes were paying the same tax (worth confirming
with the same counter).

---

## Finding 2: `EnsureWrappedRowLayouts` non-trivial branch runs 50 k visits / keystroke

**Code:** `src/editor/TextViewport.cpp:3051-3170`.

`editor_auto_close_typing` and `editor_smart_indent_typing` both report:

```
editor.ensure_wrapped_row_layouts_rebuilds   ≈ 1 per keystroke
editor.ensure_wrapped_row_layouts_line_visits ≈ 50 000 per keystroke
editor.invalidate_derived_caches_lines       ≈ 50 000 per keystroke
```

Round-2 Finding 5 added a trivial fast path that returns 0 visits when
soft-wrap is off and no fold is collapsed. The counter shows that path is
**not** being hit — so something is keeping `trivial_now` false or the cache
keys mismatch every call. Suspects:

- `folding_model_->revision()` bumps on every edit (FoldingModel rev was
  added in round 2 Finding 6 follow-up). The trivial branch is keyed on this
  revision, so it invalidates each edit even when no fold is actually
  collapsed.
- After the rebuild, line 3097's `!soft_wrap_ && !has_any_collapsed_fold`
  decides between trivial-write (0 visits) and full-write (line visits =
  `lines.size()`). The 50 k visits per keystroke imply we are taking the full
  path; either `soft_wrap_` is true or `has_any_collapsed_fold` is true for
  this fixture, but the auto-close fixture has no user-collapsed folds.

Either path is wrong: this is supposed to be the "common case" cheap path.

**Fix sketch.**

1. Re-key the trivial cache check on `(soft_wrap_, has_any_collapsed_fold,
   tab_size_, visible_columns_, folding_model_)` only — drop
   `wrapped_row_layouts_fold_revision_`. As long as the model has nothing
   collapsed, its revision is irrelevant for the trivial layout.
2. Confirm via a regression test that
   `editor.ensure_wrapped_row_layouts_line_visits` stays ≈ 0 per keystroke in
   `editor_auto_close_typing`. Today, 36.7 M visits over a 700-keystroke run
   is unambiguously wrong.

**Expected impact.** Removes ~50 k vector-population steps per keystroke on
large files. The 3.6 M allocations/iteration in `editor_smart_indent_typing`
should also drop sharply.

---

## Finding 3: Terminal scrollback trim is O(N) front-erase, called 6 700×/iter

**Code:** `src/terminal/TerminalSession.cpp:1828-1845`.

`terminal_scroll_long_output` counters:

```
terminal.trim_scrollback_calls: 20 182  (3 iters; ≈ 6 727 per iter)
terminal.trim_scrollback_lines: 973 372  (≈ 48 trimmed lines per call)
allocations p50: 1 939 912 / iter
```

Each call does `lines_.erase(lines_.begin(), lines_.begin()+trim_count)` on a
`std::vector<TerminalLine>`. The erase tears down `trim_count` `TerminalLine`s
(each owns a `std::vector<TerminalCell>` — round-2 Finding 8 made cells
trivially copyable, but the **inner vector still allocates per line**) and
then moves the remaining N entries down. With 30 000 lines in the ring and
6 700 trims, this is the alloc-churn source.

**Fix sketch.**

1. Replace `std::vector<TerminalLine> lines_` with a fixed-capacity
   `std::deque<TerminalLine>` or a ring-buffer (`std::vector` + head index).
   `pop_front()` becomes O(1) without moves and without freeing the inner
   cell vectors on every trim.
2. Reuse a free-list of cell vectors when trimming so each trim becomes a
   cell-storage reset (`vector::clear()` reuses capacity) instead of a free.
3. Coalesce trims: trim down to `target = max_scrollback` only when the buffer
   reaches `1.5 × max_scrollback`, so each trim drops a meaningful chunk
   rather than one or two lines. Cuts trim calls by ~3×.

**Expected impact.** `terminal_scroll_long_output` allocations should fall
roughly an order of magnitude. Wall time follows.

---

## Finding 4: `editor_sticky_scroll_scroll` paints 1.07 s for 96 scroll frames

**Code:** scenario at `tests/perf/PerfMain.cpp:778-803`; sticky scroll lines
flow through `RenderViewModelBuilder` and the row paint loop.

The counter `editor.invalidate_derived_caches_lines: 250 035` (over 3 iters,
96 frames each → 250 035/(3·96) ≈ 868 lines invalidated per frame) tells the
same story as round-2 Finding 16: each scroll bumps `layout_revision` and
cascades into a derived-cache wipe across a wide line range.

This is **not** a new finding — round-2 16 / round-3 cascade is exactly this
shape — but it's still the biggest paint-side cost on this branch.

**Fix sketch.** Split `document_->layout_revision` into the four-tier scheme
proposed by round-2 Finding 16 (content / syntax / layout-shape /
presentation). Scrolling should bump nothing in the layout/content tiers.

---

## Finding 5: `idle_soak_30s` runs 51 `PrepareFrameOnce` calls during a 27-second idle window

**Code:** `src/workspace/WorkspaceShellRenderFrame.cpp:55-147`.

Counters per 30-second iteration:

```
frame.prepare_calls:           51   (≈ 1.7 / sec during idle)
frame.refresh_status_bar_calls: 51
render.build_editor_view_model_calls: 102
editor.invalidate_derived_caches_lines: 57 / 90 / 110 (iterations differ)
```

The idle assert (no SDL wake events from watcher/git executor) passes, so the
shell *thread* sleeps correctly. The 1.7 prepares/sec must come from animated
cursor blink, hover dwell timers, or scheduled wake callbacks. Idle should be
zero `PrepareFrameOnce` calls past warm-up; today it's ~50.

This matches round-2 Finding 18 / round-3 P4 ("interaction idle tax"), still
deferred.

**Fix sketch.** Add a per-scope wake reason counter to
`WorkspaceWakeController` and have `idle_soak_30s` assert
`frame.prepare_calls <= K` (where `K` is the cursor-blink count expected over
30 s). When the assert breaks, the counter names the responsible wake.

---

## Finding 6: `menu_hover_switch` issues 29 k width-cache queries per frame

**Code:** `src/render/TextRenderer.cpp` (width cache) + chrome menu paint.

`menu_hover_switch` counters per 6-frame iteration:

```
render.text_width_cache_queries: 176 170 / 3 iters → 29 361 per frame
render.text_width_cache_hits:    176 003  (99.9 % hit rate)
```

The hit rate is excellent — caching works. But 29 k queries/frame for a hover
animation means we are **re-measuring the same strings** thousands of times
per frame because the call shape walks every visible glyph through the width
function. Even at near-perfect hit rate, the unordered-map lookup cost adds
up; `menu_hover_switch` p95 is 25 ms with this churn.

**Fix sketch.** Cache `MeasureText(...)` results at the *menu item* level
inside the menu surface view model — items are small (≤ ~50 entries) and stable
across hover frames. Drop per-glyph width queries to one-per-item-per-rebuild.

---

## Finding 7: `multi_tab_cycle` does 1 710 wrapped-row rebuilds across 3 iterations

This shows up as ~570 rebuilds per iteration of "open 20 tabs + cycle". Same
shape as Finding 2 — tab switches bump `layout_revision`/fold revision and
force the wrapped-row layout cache to invalidate even when nothing structural
changed about the tab's layout state.

Closing Finding 2 likely closes most of this. Re-measure after.

---

## Carry-over from rounds 2 & 3 (status check)

| Round-3 # | Theme | Status now |
| --------- | ----- | ---------- |
| 1 | Per-clip view-model multiplication | **done** — clip-scoped cache lands; `BuildFrameSurfaceCalls` counter is the gate to keep |
| 5 | Secondary caret column walks | **done** — `VisualColumnFromLayoutClipped` is used (`5ce650d` lineage) |
| 6 | LSP `didOpen` synchronous serialization | **open** — still worth fixing for huge buffer first-interaction |
| 7 | Merge conflict grouping O(N²) | **open** — no measurable improvement |
| 8 | `FileIndex::Snapshot` copies | **open** |
| 9 | Search lowering allocates per line | **open** — `SearchProjectLowerLineCalls` counter exists; no callers reduced |
| 10 | `ProjectSearchService::SnapshotResults` | **open** |
| 11 | Compare surface `VisualColumnForTextColumn` | **open** |
| 12 | Plugin hover debounce | **open** |
| 13 | `UpdateMouseCursor` per frame | **open** |

| Round-2 # | Theme | Status now |
| --------- | ----- | ---------- |
| 8 | `TerminalCell` `std::string` per cell | **done** (inline UTF-8, `37f58bb`) — but per-line cell vector still allocates; see Finding 3 |
| 15 | Glyph atlas | **rejected** — prototyped, measured a +48–83 % wall-time regression on the software renderer; see "Rejected experiment: ASCII glyph atlas" below for the preconditions any future revisit MUST meet |
| 16 | Single `layout_revision` cascade | **done** (2026-05-15) — openspec change `split-layout-revision-tiers` introduced four-tier revisions (`content` / `syntax` / `layout_shape` / `presentation`) plus typed `InvalidationReason`; every derived cache now keys on its minimum tier set, arch-lint guards the regression, and per-tier perf counters are in place. Scroll-only perf scenario + baseline updates tracked as follow-ups in the change's `tasks.md`. |
| 18 | `PrepareFrameOnce` idle | **open** — Finding 5 |

---

## Tooling

Round 3 listed three counters as "recommended before the next slice"; two
landed (`RenderViewModelBuildFrameSurfaceCalls`,
`RenderViewModelBuildOverlaySurfaceCalls`). The remaining gaps that would have
caught Findings 1, 2, and 5 earlier:

| Proposed counter | Hooks |
| ---------------- | ----- |
| `EditorHighlightCacheForcedMisses` | `TextViewport::HighlightedLineTokens` cache-miss branch; bracket scan should keep this ≈ 0 |
| `EditorHighlightCacheSize` (max LRU occupancy) | Cheap probe, lets us size `kHighlightCacheLimit` against fold-scan footprint |
| `WorkspaceWakeReason` (idle counter) | `WorkspaceWakeController::HandleScheduledWake` |
| `RenderClipInvocationsPerFrame` | Round-3 punted; still missing |
| `TerminalScrollbackLinesAllocated` | Distinguish "trim freed" vs "trim reused" once the ring buffer lands |

No new top-level tool is needed; the perf harness already produces the JSON
that `python3` can drill into. The most useful one-off addition would be a
small `tools/perf_diff.py` that aggregates counters across iterations and
prints "per-frame" derivations (e.g., `width_cache_queries /
build_editor_view_model_calls`) — currently I am doing this by hand with
inline Python.

---

## Prioritized plan (round 4)

- **P0 — Finding 1.** Highest ROI by a wide margin: removes a 1.6 s/render
  spike, fixes a hard-asserting perf scenario, restores correctness of the
  `editor_fold_recompute` budget.
- **P1 — Finding 2.** Restores the round-2 trivial-layout fast path; cuts
  ~50 k visits/keystroke on large files. Likely also closes Finding 7.
- **P2 — Finding 3.** Terminal scrollback ring/deque + trim coalescing. Large
  allocation drop in `terminal_scroll_long_output` and any heavy-output
  session.
- **P3 — Finding 6.** Menu/hover width-query cache at the item level.
- **P4 — Findings 4 + 5.** The round-2/3 deferred items behind layout-revision
  splitting and idle-tax instrumentation.

## What this doc does not cover

- Compare-surface and large-merge work (round-3 7, 11) — re-confirmed open,
  not measured here.
- Glyph atlas (round-2 15) — **rejected after measurement**; see
  the "Rejected experiment" section below.
- LSP `didOpen` (round-3 6) — still the right move, but blocked on the LSP
  bridge refactor pencilled in elsewhere.

---

## Rejected experiment: ASCII glyph atlas

Date: 2026-05-15.

### What was tried

Following the round-2 Finding 15 / round-3 P5 carry-over, an ASCII glyph
atlas was prototyped in `SdlTtfTextBackend`:

- One alpha-only `SDL_Texture` keyed by `(font face, font size)` covering
  codepoints `0x20..0x7E`, built once at `TextRenderer::EnsureInitialized`
  (rebuilt only on font reload).
- `DrawString` fast path: for every same-color ASCII run, build a
  per-glyph vertex/index pair in `thread_local` scratch buffers and submit
  the whole run via a single `SDL_RenderGeometry` call against the atlas
  texture. Foreground color applied per-vertex (`SDL_FColor`), so color
  stays out of the texture-cache key entirely.
- Three perf counters added to make the experiment measurable:
  `render.glyph_atlas_hits`, `render.glyph_atlas_fallbacks`,
  `render.glyph_atlas_evictions`.
- Behind `MICROIDE_RENDER_GLYPH_ATLAS=1` opt-in flag for the trial.

The implementation worked end-to-end: counters fired correctly
(~2 M `glyph_atlas_hits` per `editor_sticky_scroll_scroll` iteration),
`glyph_atlas_evictions` stayed at 0, and `text_texture_cache_misses`
dropped to ~1 per iteration.

### What happened

The atlas **regressed** every editor paint scenario on the software renderer:

| Scenario                          | atlas-off p50 | atlas-on p50 | Δ         |
| --------------------------------- | ------------: | -----------: | --------: |
| `editor_render_whitespace_paint`  | 803 ms        | 1 453 ms     | **+81 %** |
| `editor_sticky_scroll_scroll`     | 1 020 ms      | 1 869 ms     | **+83 %** |
| `editor_indent_guides_paint`      | 645 ms        | 954 ms       | **+48 %** |

The atlas, flag, counters, and supporting infrastructure were reverted in
full — none of it remains in `src/`. `MICROIDE_RENDER_GLYPH_ATLAS` is gone
and the three counters were dropped from `src/util/PerformanceCounters.{h,cpp}`.

### Why the design didn't work

The hypothesis in round-2 Finding 15 was *"editor content with syntax
highlighting produces unique color-per-token strings, so scrolling a large
file rapidly evicts entries and re-builds composite surfaces."* On
measurement, that premise turned out to be wrong on real workloads:

1. **`render.text_texture_cache_hits` is already > 99 % on every paint
   scenario.** The composite cache is healthy; it isn't thrashing.
2. **`DrawString` is called at the *run* level, not the *cell* level.**
   `EditorViewRenderer` and `DecoratedTextGridRenderer` batch same-color
   cells into a single `run.text` string and call `DrawString` once for
   the whole run. The composite cache keys on that whole string. With
   < 1 % miss rate, nearly every run is served by **one**
   `SDL_RenderTexture` call against a pre-baked string texture.
3. **The atlas un-batches that work.** Each cell becomes its own quad
   with 4 vertices and 6 indices. For a 50-row × 80-column visible region
   with mixed coloring, the atlas path emits roughly 4 000 quads per frame
   where the composite path emits ~50 textured rectangles.
4. **`SDL_RenderGeometry` is not a free batched primitive on the software
   renderer.** Per-vertex color modulation forces per-pixel attribute
   interpolation (color × sampled-texel), and total per-pixel work scales
   with quad count × quad area. The "one call per run" win is real on a
   GPU pipeline; the software backend ends up doing strictly more
   rasterization work than the composite blit.

In other words: the atlas would have helped if cache-miss thrash had been
the bottleneck. It isn't. The composite cache is already doing the
equivalent of what an atlas does, at a strictly coarser (and therefore
cheaper) granularity for the software renderer.

### Do not revisit this without these preconditions

Do not propose another glyph-atlas variant for the editor text path
unless **all** of the following preconditions hold:

- **MicroIDE is rendering through a GPU backend, not software.** The
  software path has been the perf-gated path since the harness landed and
  remains the dominant cost reported on `perf-runner-v1`.
  `SDL_RenderGeometry` in the software renderer rasterizes per-pixel just
  like `SDL_RenderTexture`, so the "batched submission" win does not
  exist there.
- **A measured fixture exists where `render.text_texture_cache_misses /
  cells_visited` exceeds ~10 % in steady state.** If that miss rate is
  below ~1 % (today's number), the composite cache is already doing the
  heavy lifting and any atlas-style replacement will pay more, not less,
  per pixel.
- **A profile of the composite path shows
  `BuildAsciiCompositeSurface` / `SDL_CreateTextureFromSurface` as a
  top-3 hotspot** on the perf-runner trace. Today it is far below the
  per-pixel blit cost.

If those preconditions ever hold, the right starting point is **not**
the per-quad-geometry approach attempted here — it is "reuse atlas data
to *build* a composite string texture on cache miss" so the hot path
still draws one texture per cached string. That preserves the per-string
draw-call shape the software renderer is happy with and is closer in
shape to the existing `ResolveAsciiGlyphSurface` per-glyph cache than to
`SDL_RenderGeometry`.

The corresponding OpenSpec proposal at
`openspec/changes/text-renderer-glyph-atlas/` is kept as a record of the
experiment (proposal, design, specs, tasks). **It SHALL NOT be applied.**
This section is the official status of round-2 Finding 15 and round-3
P5: **rejected on measurement, do not retry without the preconditions
above**.
