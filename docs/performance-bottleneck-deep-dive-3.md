# Performance Bottleneck Deep Dive — Round 3

Date: 2026-05-15

## Scope

This pass assumes the round-2 workstream in `docs/performance-bottleneck-deep-dive-2.md` is **in progress or partially landed**: many render-path and editor-structure fixes there are real wins, but several items remain **partial** or **deferred**, and new hotspots appear once the obvious per-row paint and folding costs shrink.

Measurements below mix **implemented counter evidence** (local `microide-perf` runs) with static analysis. For authoritative regression gating, use `docs/perf-harness.md` with `perf-runner-v1` provenance.

## Status snapshot (carry-over from round 2)

| # | Round-2 theme | Round-3 status |
| - | ------------- | -------------- |
| 1 | `RenderViewModelBuilder` surfaces built 2–3×/frame | **done** — clip-scoped cache for `BuildFrameSurface`/`BuildOverlaySurface`; prepare-scoped sidebar/bottom/text-input VMs (see round-2 doc) |
| 8 | `TerminalCell` `std::string` | **deferred** — still the right long-term move with scrollback ring |
| 13 | `VisualColumnFromLayout` everywhere | **partial** — primary decorations + secondary carets use `VisualColumnFromLayoutClipped`; compare/diagnostics may still walk bytes |
| 15 | Glyph atlas for SDL_ttf | **deferred** — still the dominant scrolling cost on software renderer |
| 16 | Single `layout_revision` cascade | **partial** — trivial wrapped-row fast paths help; broad invalidation remains |
| 18 | `PrepareFrameOnce` queue consumers / steady-frame | **open** — still runs a fixed pipeline every frame |

---

## Finding 1: Partial redraw multiplies `BuildFrameSurface` (and friends) per SDL clip

**Implemented on this branch:** `RenderClip` now calls `EnsureClipFrameAndOverlayViewModels(layout)` once per clip pass; `RenderFrameBase`, `RenderActiveWorkspaceSurface`, and `RenderOverlaySurface` consume the cached `FrameSurfaceViewModel` / `OverlaySurfaceViewModel` instead of rebuilding. `PrepareFrameOnce` also snapshots sidebar, bottom panel, and text-input view models once per frame for render TUs (round-2 Finding 1 completion).

Relevant code (historical / remaining clip-loop context):

- `src/app/Application.cpp` — partial path loops `merged_clip_rects` and calls `workspace_shell_.RenderClip(...)` once **per** clip (`Application::WorkspaceRender(partial-loop …)`).
- `src/workspace/WorkspaceShellRender.cpp` — `RenderClip` always calls `RenderFrameBase`, then conditional passes.
- `src/workspace/WorkspaceShellRenderFrame.cpp` — `RenderFrameBase` / `RenderActiveWorkspaceSurface` / `RenderOverlaySurface` consume cached view models built in `EnsureClipFrameAndOverlayViewModels` (see `WorkspaceShellRender.cpp`); per clip, `BuildFrameSurface` / `BuildOverlaySurface` run at most when the layout key changes.

**Cost model (before the cache):** For a frame with `C` coalesced clips where the dirty region still intersects the editor surface, a single user-visible frame could execute:

- `C` × `RenderFrameBase` → `C` × `BuildFrameSurface`
- `C` × `RenderActiveWorkspaceSurface` → up to `C` × (`BuildFrameSurface` + `BuildOverlaySurface`)

So **up to `2C` frame-surface builds and `C` overlay builds** in one `Application::Render(partial)` — on top of `PrepareFrameOnce`, which still materializes sidebar + bottom + text-input view models once for layout clamping and render reuse.

**After the cache:** view-model CPU for frame/overlay scales with **layout changes** and **once per `PrepareFrameOnce`**, not with clip count.

This directly fights the goal of cheap menu-hover / partial updates (historical evidence in `openspec/changes/archive/2026-05-04-address-render-and-plugin-reload-hotspots/`).

### Rewrite plan

1. After `PrepareFrameOnce`, materialize **once per frame**: `FrameSurfaceViewModel`, `OverlaySurfaceViewModel`, and any other surfaces that do not depend on the SDL clip rect. Thread `const&` into `RenderFrameBase` / `RenderActiveWorkspaceSurface` / overlay render helpers.
2. Alternatively, split `RenderClip` into **clip-only draw** (assume view models prepared) and keep a single “prepare view models” step in `Application::Render` immediately after `PrepareFrameOnce`.
3. Add perf counters (see “Tooling” below): `BuildFrameSurfaceCallsPerFrame`, `BuildOverlaySurfaceCallsPerFrame`, `RenderClipInvocationsPerFrame` — debug builds can `SDL_assert` “steady single-clip frame ≤ 1” for each build kind.

### Expected impact

- Partial frames with fragmented dirty regions stop scaling linearly with clip count for **CPU** (GPU upload / fill is still per clip, but view-model work drops to O(1) per frame).

---

## Finding 2: `PrepareFrameOnce` still constructs view models only to read two floats

Relevant code:

- `src/workspace/WorkspaceShellRenderFrame.cpp` — `PrepareFrameOnce` builds `SidebarSurfaceViewModel` and `BottomPanelSurfaceViewModel` primarily to reach `project_state` for `ClampSidebarWidth` / `ClampBottomPanelHeight`.

Even when nothing in the sidebar or panel **semantic** state changed, the builder runs. Round 2 narrowed some **string** allocations inside `BuildSidebarSurface`, but the structural issue remains: **layout clamping is coupled to full view-model construction**.

### Rewrite plan

1. Expose a minimal `SidebarPanelLayoutInputs` (visible flags, raw widths, project pointer) built without walking the full sidebar scene, **or** cache clamp outputs keyed by `(width,height,sidebar_visible,panel_visible,raw_sidebar_w,raw_panel_h,layout_mode_inputs…)`.
2. Reserve `RenderViewModelBuilder` for the render pass that actually paints those surfaces.

### Expected impact

- `multi_tab_cycle` and idle frames stop paying builder walks when only window size or scroll changed.

---

## Finding 3: `RenderActiveWorkspaceSurface` rebuilds frame + overlay view models inside the editor pass

Relevant code:

- `src/workspace/WorkspaceShellRenderFrame.cpp:225-227` — second `BuildFrameSurface` + `BuildOverlaySurface` in the same `RenderClip` invocation.

`RenderFrameBase` already computed `FrameSurfaceViewModel` for chrome rectangles. The editor/compare/merge branch recomputes overlapping data. Even on **full** frames this duplicates work; on **partial** frames it duplicates per clip (Finding 1).

### Rewrite plan

1. Pass the `FrameSurfaceViewModel` from `RenderFrameBase` into `RenderActiveWorkspaceSurface` (or store last-built frame VM on a `FrameRenderCache` cleared at frame boundary).
2. Ensure overlay VM is shared between chrome, editor, and text-input paths.

---

## Finding 4: `WorkspaceRootView::RenderPrepared` recomputes layout vs `PrepareFrameOnce` layout

Relevant code:

- `src/workspace/WorkspaceRootView.cpp` — `RenderPrepared` calls `compute_layout(width,height)` after `prepare_render_frame`.
- `src/workspace/WorkspaceShellBootstrapper.cpp` — `compute_layout` lambda calls `ComputeLayout(...)` directly from shell project state.
- Production `Application` uses `PrepareFrameOnce` + `RenderClip` with `prepared_frame_layout_` (`src/workspace/WorkspaceShellRender.cpp`).

The **Application** path is consistent. Any code path still calling `WorkspaceRootView::Render` / `RenderPrepared` (tests, tools, future refactors) risks **double layout** and subtle divergence from clamped sidebar/panel widths applied during `PrepareFrameOnce`.

### Rewrite plan

1. Deprecate `compute_layout` from `WorkspaceRootView::FrameOperations` for the hot path; inject `const WorkspaceLayout&` from the shell’s prepared layout.
2. Keep `ComputeLayout` as the single implementation, but ensure **one authority per frame**.

---

## Finding 5: Secondary caret columns still use `VisualColumnForTextColumn` per caret per row

Relevant code:

- `src/editor/EditorViewRenderer.cpp:937-938` — `TextLayout::VisualColumnForTextColumn(lines[line_index], secondary_carets[idx].column, …)` inside the visible-row loop.

Round 2 moved search/selection/occurrence/bracket columns onto `VisualColumnFromLayoutClipped` (binary search on `LayoutLine::source_columns`). Secondary carets reintroduce an **O(line length)** walk for each drawn caret.

### Rewrite plan

1. Reuse the row’s `LayoutLine` / `row_layout` and map secondary caret columns through `VisualColumnFromLayoutClipped` (with the same “past end of layout” fallback policy as other decorations).
2. If the caret column can lie past the soft-wrapped segment, keep a **single** fallback walk per line per frame, not per caret.

### Expected impact

- Long-line files with multi-caret sessions stop multiplying UTF-8/tab walks by caret count.

---

## Finding 6: `EnsureLspDocumentOpen` still serializes the full buffer and sends `didOpen` synchronously on the calling thread

Relevant code:

- `src/workspace/WorkspaceShellLsp.cpp:308-321` — `SerializeViewportText(viewport)` then `client.DidOpen(...)`.
- Callers include assist / plugin / sync paths (`WorkspaceShellAssist.cpp`, `WorkspaceShellPlugins.cpp`, `SyncLspForActiveEditable*`).

`AGENTS.md` (2026-05-02 invariant) requires **`textDocument/didOpen` / `didChange` not on the `ActivateTab` call path**; even if tab activation itself no longer calls this directly, **first interaction** on a large buffer can still block the shell thread on **full-document serialization + JSON construction**.

### Rewrite plan

1. Post “hydrate document `{uri}`” work to the existing LSP / background executor queue; apply `HasOpenDocument` optimistically or show “LSP: syncing…” in status until ack.
2. Split “open empty / version handshake” vs “send content chunks” if the protocol stack allows; at minimum, never call `SerializeViewportText` on the UI thread for multi-MB buffers.
3. Architectural lint: flag `SerializeViewportText` + `DidOpen`/`DidChange` in `WorkspaceShell*.cpp` outside the dedicated async bridge.

### Expected impact

- Tab switches and first completions on huge files remain interactive; LSP throughput moves to a worker.

---

## Finding 7: Merge conflict grouping remains worst-case quadratic

Relevant code:

- `src/compare/MergeModel.cpp:147-176` — expand transitive `ChangesInteract` groups with nested `for (candidate)` × `std::any_of` over the growing `group`.

This is unchanged from round 1 Finding 9 / round 2 “not covered”. Large generated conflicts (rebases, vendor merges) can blow up CPU.

### Rewrite plan

1. Replace with **union-find** or **interval sweep** after sorting tagged changes by `base_start`.
2. Add a perf fixture + `microide_diff_bench`-style CLI slice for “many tiny adjacent hunks”.

---

## Finding 8: `FileIndex::Snapshot` + `FileFinder::EnsureCacheBuilt` still copy the entire project file list

Relevant code:

- `src/project/FileIndex.cpp:172-175` — `Snapshot()` returns `std::vector<ProjectFile>` by value under `shared_lock`.
- `src/project/FileFinder.cpp:151-163` — snapshot + per-file `path_string`, `ToLower(path)`, `ToLower(filename)`.

Round 1 Finding 7 remains accurate: this is **O(N files)** memory bandwidth whenever the finder cache cold-builds or refreshes.

### Rewrite plan

1. Immutable generation handle + `std::span<const ProjectFile>` or refcounted block.
2. Store lowercase filename once in the index ingest path, not in the finder cache rebuild.

---

## Finding 9: Project search case-insensitive literal path allocates a lowered line string per match scan

Relevant code:

- `src/project/ProjectSearchService.cpp:125-129` — `LowerLine` builds a new `std::string` for each `(line, lowered_line)` pair; counters `SearchProjectLowerLineCalls` / `SearchProjectLowerLineBytes` already exist (`src/util/PerformanceCounters.h`).

Workers are async (good), but **CPU + allocator churn** on huge trees still scales with bytes processed, not results returned.

### Rewrite plan

1. ASCII fast path: scan `line` in-place with `tolower` rules without allocating when all codepoints are ASCII.
2. Optional: mmap / memory-map large files or reuse a per-thread `std::string` buffer cleared between lines.

---

## Finding 10: `ProjectSearchService::SnapshotResults` copies the full result vector for UI consumers

Relevant code:

- `src/project/ProjectSearchService.cpp:236-245` — returns `std::vector<ProjectSearchResult>` copy under `shared_lock`.

Every poll from the shell that snapshots results duplicates all `ProjectSearchResult` strings. Round 1 called for delta delivery; implementation is still snapshot-oriented.

### Rewrite plan

1. `span` + generation, or ring buffer of appended results with “read cursor” for UI.

---

## Finding 11: Compare surface still maps columns with `VisualColumnForTextColumn`

Relevant code:

- `src/workspace/WorkspaceShellCompareRender.cpp` — multiple `VisualColumnForTextColumn` calls per row (`~412` region).

Compare rows can be extremely wide (generated JSON). Same fix family as Finding 5: reuse precomputed `LayoutLine` maps.

---

## Finding 12: Plugin hover debounce / caching still absent at the boundary

Round 1 Finding 10 / round 2 “not covered” — still valid. Pointer-driven work should be **budgeted + cached** so providers cannot pin the main thread.

---

## Finding 13: `UpdateMouseCursor` runs inside every `PrepareFrameOnce`

Relevant code:

- `src/workspace/WorkspaceShellRenderFrame.cpp:142-145` — `UpdateMouseCursor(mouse_x, mouse_y, …)` with `PerformanceTrace::Scope`.

Even when the pointer did not move and no surface changed cursor policy, the shell pays the full cursor resolution path.

### Rewrite plan

1. Track last `(x,y,hover_generation,cursor_shape)` and early-out when unchanged.
2. Only call SDL cursor APIs when the resolved cursor kind actually changes.

---

## Tooling gaps (recommended before the next optimization slice)

Round 1–2 already added several `PerfCounterId` values. The next high-signal additions:

| Counter / trace | Purpose |
| --- | --- |
| `RenderClipInvocationsPerFrame` | Quantify partial-loop amplification (Finding 1) |
| `RenderViewModelBuild*` family | Count each `BuildFrameSurface` / `BuildOverlay` / `BuildSidebar` / `BuildBottomPanel` per frame |
| `PrepareFrameOnce` phase scopes | Already partially traced; add explicit scope for “view model build for clamp only” vs paint |
| Perf scenario: `partial_red_menu_hover` | Deterministic multi-clip frame with assert on build counters |

Hook these into `microide_perf` JSON (`perf_counters` / `phase_metrics`) so local advisory runs can prove “partial menu hover no longer scales with clip count”.

---

## Prioritized plan (round 3)

### P0 — Frame and partial-render architecture

Findings **1–3**: single authoritative view-model build per frame; `RenderClip` becomes paint-only; eliminate duplicate `BuildFrameSurface` / `BuildOverlaySurface`.

### P1 — Main-thread LSP hydration

Finding **6**: async `didOpen` / full-buffer serialization off the shell thread; tighten lint.

### P2 — Remaining O(line) decoration walks

Findings **5** and **11**: unify on layout-based column mapping.

### P3 — Large-repo and merge pathologies

Findings **7–10**: merge grouping, file index sharing, search line lowering, result deltas.

### P4 — Interaction idle tax

Finding **13** + round-2 **18**: cursor + steady-frame early exits.

### P5 — Renderer backlog (unchanged product direction)

Round-2 **8** + **15**: terminal storage + glyph atlas — still the right sequel once frame CPU is no longer dominated by redundant view-model builds.

---

## What this document does not repeat

Detailed re-derivation of folding `RemapCollapsedFlags` improvements, width-cache `string_view` LRU, CSR whitespace/occurrence indexing, and indent-guide coalescing — those are covered as **done / partial** in round 2; this round focuses on **per-clip multiplication**, **LSP hydration threading**, and **remaining index/search snapshot copies**.
