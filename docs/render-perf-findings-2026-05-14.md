# Render Performance Deep-Dive — 2026-05-14

Reviewer: deep-dive after user reported subjective sluggishness during scrolling and
window resize. This document records concrete, prioritized findings against the
current tree. Numbers below are static-analysis estimates unless explicitly
captured with `MICROIDE_PERF_TRACE`; live measurements are noted inline.

The prior `docs/performance-findings.md` is largely about historical fixes and
backlog items. This file is the new “queue of things I’d attack next”.

## Live trace summary (SDL dummy driver, this repo as the project)

`env SDL_VIDEODRIVER=dummy MICROIDE_PERF_TRACE=1 MICROIDE_PERF_TRACE_MIN_MS=2 MICROIDE_TRACE_REDRAW=1 ./build/microide/microide`

Representative steady-state frames (no UI interaction, scene already rendered once):

- `Application::WorkspaceRender(fallback-full)` ~13–17 ms per full repaint
- `EditorViewRenderer::Render` ~2 ms per pane
- `WorkspaceShell::PrepareFrameOnce` ~2.5–3 ms
- One-off startup costs that still dominate first frame:
  - `RuntimeSyntaxRegistry::EnsureInitialized` ~88 ms
  - `WorkspaceProjectFileMonitor::ArmPendingWatch` ~256 ms
  - first window resize triggers full-pass: ~17 ms

The dummy driver removes the GPU/vsync cost, so these are CPU-only times. With
a real GPU the present is bounded by vsync (~16.6 ms at 60 Hz, ~8.3 ms at
120 Hz), so anything ≥10 ms of CPU work per frame is already at the limit before
vsync arrives.

## P0 — Text rendering: per-glyph cache breaks GPU batching (highest leverage)

`src/render/SdlTtfTextBackend.cpp:171,201,256`

`DrawString` / `DrawStringOn` currently dispatch *any* ASCII-only text through
`DrawFastAsciiString`, which loops one character at a time:

```cpp
for (std::size_t index = 0; index < text.size(); ++index) {
  ...
  CacheEntry* entry = ResolveEntry(glyph_text, color, nullptr);
  ...
  SDL_RenderTexture(renderer_, entry->texture, nullptr, &destination);
}
```

Consequences:

- For a populated editor view (50 rows × ~80 visible chars) this is ~4 000
  `SDL_RenderTexture` calls per editor pane per frame. The sidebar, terminal
  panel, status bar, chrome, breadcrumbs, line numbers, search results add
  hundreds more.
- Each call uses a different texture (one cache entry per glyph+color), so
  SDL3’s internal batcher cannot fold them into a single draw call.
- Every call goes through a hashmap lookup (`ResolveEntry`) — even cache hits.
- `DrawStringOn`’s `background` parameter is silently dropped (`(void) background;`),
  so callers paying to compute a background colour get nothing for it on ASCII
  paths. That’s a latent correctness issue, not just performance.

Why this matters most: rendering text dominates the editor surface, and the
batching loss compounds with every text-heavy panel that is currently visible.
This is the single largest CPU cost in the steady-state full-redraw path.

Fix options, in increasing scope:

1. **Cheap win:** for ASCII strings up to N chars, cache the *whole-string*
   blended texture (already done for non-ASCII). 1 cache entry + 1 draw call
   per (string, color). Cache thrash worry is already handled by the existing
   `kMaxCacheEntries = 2048` LRU.
2. **Proper fix:** build a glyph atlas once, then issue text via
   `SDL_RenderGeometry` with per-vertex UVs and per-vertex colour. One draw
   call per atlas page covers every visible glyph regardless of colour.
   `docs/performance-findings.md` already lists this as “Lower-cost text
   rendering backend”; it should be P0, not a footnote.
3. While here, switch `DrawFastAsciiString` to actually honour the background
   parameter on `DrawStringOn` or document that ASCII paths intentionally
   render on transparent.

## P0 — Resize churn: scene texture destroyed/recreated on every WINDOW_RESIZED

`src/app/Application.cpp:303-315,485-519`

On `SDL_EVENT_WINDOW_RESIZED` / `SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED`,
`UpdateRendererPresentation()` runs, then `EnsureSceneTexture()` runs in the
next render. `EnsureSceneTexture` compares the GPU texture dimensions with the
new render output and, on any mismatch, calls `DestroySceneTexture()` followed
by `SDL_CreateTexture(...)` for a full-window RGBA8888 render target.

During a live drag, X11/Wayland fire dozens of these events per second. Every
event:

- destroys the GPU scene texture (forces driver-side cleanup)
- allocates a new full-window render target
- triggers a forced full redraw of the entire shell
- runs `SDL_SetRenderLogicalPresentation` (also non-trivial)

Static analysis suggests this is the source of the “slow resize drag” feel,
because each frame is ~13–17 ms of redraw + a GPU texture realloc + vsync
wait. The resize-trace docs even call out that during outer-layout resize,
partial replay is intentionally disabled, so we pay a full repaint each event.

Fix: throttle. Either:

- ignore resize-driven scene-texture rebuilds when the new size is within a
  small delta of the cached size, or
- defer scene-texture realloc until the resize stream has been quiet for
  ~50 ms (track last resize timestamp; rebuild lazily on first render after
  the silence window), or
- keep a slightly oversized texture (next-power-of-two or +N pixels each
  axis) and reuse it across small size deltas — only re-allocate when the
  window crosses a real size bucket.

Bonus: the per-frame `UpdateRendererPresentation()` call (which queries
`SDL_GetRenderOutputSize` + `SDL_GetWindowDisplayScale` + sets logical
presentation) runs every frame even when nothing has changed. Cache the
presentation state and only re-publish it when a window event invalidates it.

## P0 — Mouse wheel scroll is chunky on smooth-scroll devices

`src/workspace/WorkspaceEditorMouseCoordinator.cpp:445-466`

```cpp
viewport->ScrollVertical(-vertical_ticks * 3);
```

Where `vertical_ticks` is `event.wheel.integer_y` (with `lround` fallback).
On a high-resolution trackpad (libinput, macOS, Wayland), the wheel events
come in as small fractional `y` values per frame. With `integer_y` rounding
to zero for small magnitudes, several frames of scroll input produce no
movement, then a single tick produces a 3-line jump. That is the classic
sensation users describe as “sluggish”/“laggy” scrolling.

Combined with the line-precision `ScrollVertical` (no sub-line pixel scroll),
the visible result is a stair-step.

Fix sketch:

1. Accumulate `event.wheel.y` (float) into a sub-line accumulator; convert
   to whole-line scroll when |accumulated| ≥ 1.
2. Optionally support pixel-precision scroll in `TextViewport` (track a
   `scroll_offset_px` and shift the row paint origin by it). This is the
   real fix — line-precision feels wrong with modern input devices. Requires
   adjusting the render-row loop start position and growing the visible-row
   loop by one row to cover the partial top/bottom rows.
3. Honour `event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED` (it’s ignored
   today).

The `* 3` multiplier should also be a per-user setting; some users want 1
line, some 6. Three is arbitrary and locked in.

## P1 — Per-frame allocations in `EditorViewRenderer::Render`

`src/editor/EditorViewRenderer.cpp:289-930`

The `editor_mouse_selection_drag` perf baseline shows **8.77 M allocations**
for the scenario p50. That points squarely at this file. Concrete hotspots:

- Line 371: `const std::string lowered_search_query = ToLower(search_query);`
  runs every render frame even when `search_query` is empty (`ToLower` of an
  empty view still constructs a `std::string`). Skip when empty; otherwise
  cache by (query, layout_revision).
- Lines 376–380: `last_fold_gutter_marks_` is rebuilt by copy each frame.
  Replace with `std::span<const FoldGutterMark>` borrowed from the view
  model and a small revision counter when the source actually changes.
- Line 388: `std::vector<std::size_t> visible_rows_for_guides;` is fresh
  per frame. Make it a member scratch vector, `clear()` then `reserve()`.
- Lines 547+: `DecoratedTextRow row_desc;` is constructed and destructed per
  visible row × per frame. Each row contains three `std::vector`s
  (`fills`, `runs`, `underlines`), each of which gets `push_back`ed
  repeatedly. With ~50 rows per pane this is ~150 vector allocations per
  frame, plus growth churn. Two cheap fixes:
  - hoist a single `DecoratedTextRow scratch_row_;` member, `clear()` between
    rows, `reserve()` once based on `metrics.visible_columns`;
  - or batch all rows into one `DecoratedTextRow` and have
    `RenderRow` emit them all in one pass (lets fills be sorted by colour
    for fewer `SDL_SetRenderDrawColor` flips — see P1 batching below).
- Line 374: `std::string lowered_line_scratch;` is the right idea (scratch
  buffer reused) — extend the same pattern to the other transients above.
- `AppendLayoutSyntaxTextRuns` and `AppendDiagnosticUnderlines` push into
  `row.runs` / `row.underlines` per segment with no `reserve()` hint.

These changes won’t move steady-state CPU enormously on their own (per-row
allocators are typically fast), but combined they will materially reduce GC
pressure during a long scroll/drag.

## P1 — Fill-rect batching across the shell

Every `DecoratedTextRow` paint in `DecoratedTextGridRenderer::RenderRow`
(`src/editor/DecoratedTextGridRenderer.cpp:156-189`) does:

```cpp
for (const DecoratedTextFill& fill : row.fills) {
  SDL_SetRenderDrawColor(renderer, ...);  // state change → batch flush
  SDL_RenderFillRect(renderer, &fill.rect);
}
```

Each `SDL_SetRenderDrawColor` of a different colour forces a flush of SDL3’s
internal batcher. A typical row has interleaved colours (selection +
search-match + bracket-match + indent guide + whitespace dot/tab). Grouping
fills by colour before draw, or using `SDL_RenderFillRects` with same-colour
runs, will collapse many flushes.

Same for `SDL_RenderFillRect` for the four-rect fold marker outline
(`EditorViewRenderer.cpp:869-877`) and the multi-rect underlines.

`grep -rn SDL_RenderGeometry src` returns nothing — the codebase has not
adopted any batched primitive yet. Even one `SDL_RenderGeometry` call for the
visible fills would close the gap.

## P1 — Per-frame layout/state churn in `PrepareFrameOnce`

`src/workspace/WorkspaceShellRenderFrame.cpp:54-136`

Each `PrepareFrameOnce` does, *unconditionally*, in this order:

1. `ConsumePendingProjectOpenDialogResult()`
2. `ConsumeProjectSearchUpdates()`
3. `RenderViewModelBuilder(context_).BuildSidebarSurface()` — constructs a
   view model object every frame
4. `RenderViewModelBuilder(context_).BuildBottomPanelSurface()` — ditto
5. `ApplyLiveSettings()` — re-reads settings every frame
6. `RefreshStatusBar()` and `RefreshSettingsOverlayCatalog()` — even when
   the status bar / overlay haven’t changed
7. `NormalizeEditorSplitTree(*editor_tab)` for the active editor tab
8. `ResizeTerminalToPanel(...)` whenever the bottom panel is a terminal
9. `UpdateMouseCursor(...)` (which can chain into `UpdateEditorHover`,
   which calls plugin hover providers)

Each of these is bounded but together they form the ~2–3 ms `PrepareFrameOnce`
slice. None of them gate on a “did anything change?” flag.

Fix: tag the cheap things with a revision counter so steady-state hover
ticks (caret blink, idle mouse motion) skip them. `BuildSidebarSurface` and
`BuildBottomPanelSurface` in particular should memoize on
`(context_revision, layout_revision)`.

`ResizeTerminalToPanel(layout.bottom_panel)` is called every frame that the
terminal is the bottom-panel content. Internally it likely no-ops when the
size hasn’t changed, but the call itself isn’t free — guard at the caller
on a `last_panel_rect` compare.

## P1 — Mouse motion path is allocation-heavy

`src/workspace/WorkspaceShellMouseMotion.cpp:280-413`

A single mouse-motion event triggers, per event:

- `HoveredProjectTabTooltipLabel(...)` — returns `std::string` by value
- `HoveredProjectTabTooltipRect(...)`
- `HoveredTabTooltipLabel(...)` / `HoveredTabTooltipRect(...)`
- `HoveredStatusTooltip(...)` / `HoveredStatusTooltipRect(...)`
- `UpdateMouseCursor(...)` → optionally `UpdateEditorHover(...)` → which
  walks diagnostics / plugin hover providers
- Sidebar hover-button rect comparisons
- Tooltip change detection + redraw requests
- Various coordinator dispatch chains

With smooth pointing hardware reporting up to 1000 Hz, SDL coalesces but the
remaining events are still 60–120 Hz. Each of these queries allocates a
`std::string` when it could return `std::string_view` into an already-owned
buffer.

Fix is mostly mechanical: change the `Hovered*Label` helpers to return
`std::string_view`, cache previous-rect computations on the motion path,
and skip the full hover refresh when the pointer hasn’t crossed any
hit-region boundary (a cheap rect-equal compare against the last hit).

## P2 — `RuntimeSyntaxRegistry::EnsureInitialized` startup cost

`docs/performance-findings.md` already calls this out (~44 ms in the old
trace, observed ~88 ms here). The recommended directions (disk-cached
compiled definitions, defer-after-first-render, parallel parse) still apply
and are still unimplemented in the tree. Quoting the existing doc to confirm
this is not stale advice.

## P2 — `WorkspaceProjectFileMonitor::ArmPendingWatch` 256 ms at project open

This is on the project-open hot path; it isn’t per-frame but it does delay
the first paint enough to read as “the app is slow when I open a project.”
Worth tracing whether the watcher arming can run on
`ProjectBackgroundExecutor` instead of the main thread.

## P3 — Lower-hanging hygiene

- `Application::Render` does `SDL_RenderTexture(scene_texture, ...)` +
  `SDL_RenderPresent` even when the scene texture is unchanged and there
  was nothing dirty. Presenting an unchanged frame still costs a vsync
  wait. The outer loop already guards `if (full_redraw_pending || !dirty_rects.empty())`,
  so steady idle is fine — but caret blink rebuilds the editor pane every
  blink interval just to flip a single 1×lineheight cursor rect. Worth
  measuring if that present can be merged with the caret-only partial
  invalidation that already exists.
- `Application::UpdateRendererPresentation` runs `CaptureWindowPresentationState`
  every frame (queries `SDL_GetRenderOutputSize` + `SDL_GetWindowDisplayScale`
  every frame). Cache the result, invalidate on window/display-scale events.
- The `MICROIDE_TRACE_REDRAW` env vars are wired up but there is no
  in-app FPS / frame-time HUD. A tiny on-screen counter behind a flag would
  make it much easier for users (and us) to spot regressions without
  re-running with env vars.

## Suggested order of attack

1. **P0 glyph atlas / whole-string ASCII cache** — biggest CPU/GPU win in
   steady-state rendering, also fixes the latent dropped-background issue.
2. **P0 resize coalescing** — directly addresses the user’s “sluggish during
   resize” report. Cheap to implement (timer-based defer + size-bucket).
3. **P0 pixel-precision scroll** — addresses “sluggish during scroll” on
   smooth-input devices. Larger surface change but well-bounded to
   `TextViewport` + the wheel handler.
4. **P1 EditorViewRenderer allocations** — knock the 8.7 M-allocation
   scenario down to something closer to baseline; reduces tail-latency
   spikes during long selection drags.
5. **P1 fill batching** — easy follow-on once allocations are out of the way.
6. **P1 PrepareFrameOnce memoization** — small per-frame win but compounds.
7. **P2/P3 cleanup.**

## How to verify

- Per-finding: add or extend a `tests/perf/baselines/*.json` scenario
  covering the touched path. `editor_mouse_selection_drag` is already a
  good harness for allocator pressure; we should add an
  `editor_scroll_wheel_burst` scenario for P0 #3 and a synthetic
  `window_resize_drag` scenario for P0 #2.
- For the user-visible feel: run with
  `env MICROIDE_PERF_TRACE=1 MICROIDE_PERF_TRACE_MIN_MS=1 MICROIDE_TRACE_REDRAW=1 MICROIDE_TRACE_REDRAW_VERBOSE=1 ./build/microide/microide`
  before and after each change and compare the rolling-average frame time
  reported every 120 frames. Target: < 6 ms steady-state full-frame at
  60 Hz vsync so partial-redraw bursts have headroom.

## Out of scope here

- Plugin host churn (cross-process IPC, LSP wakeups) — the live trace
  showed those as one-shot, not per-frame.
- Persistence I/O — already routed through `PersistenceService`.
- Compare/merge surfaces — separate, less-visited paths; deferred until
  the editor render path is fixed since they share primitives.
