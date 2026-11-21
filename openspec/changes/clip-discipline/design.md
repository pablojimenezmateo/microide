# Design

## Context

The redraw verifier (`MICROIDE_QA_VERIFY_REDRAW`) repaints every partial frame in
full and compares. `tools/qa/findings.py::stale_clip_relation` files each stale
frame by where its first stale pixel sits against the frame's clips: `outside`
(nothing repainted it -- `derived-damage`'s class), `clip-edge` (one pixel past a
clip's edge), `in-clip` (inside a repainted clip). This change is about the last
two. Today 19 `ScopedRenderClip` scopes exist in `src/`; most elements paint with
no clip of their own and rely on the frame's damage clip, so whatever an element
draws past its rect is visible in a full frame and cut in a partial one.

## Goals / Non-Goals

Goals: partial and full frames agree by construction for paint that stays inside
its element; a draw that does not is reported where it happens (test or QA run),
naming the element; the device-pixel rect of a fill, a clip and a damage request
is one computation.

Non-goals: deciding what to damage (`derived-damage`); visual redesign; GPU-only
paths.

## Decisions

### D1. `PaintElement` scope on the element's layout rect

```cpp
PaintElement scope(canvas, rect, "tab:0/main.cpp", PaintElement::Opaque{background});
```

- Pushes a clip on the snapped rect (nested: intersected with the frame's clip).
- `Opaque{color}` fills the rect first: an element that paints text over its
  parent's background now owns that background, so a partial frame cannot leave
  older text under it (cause 4).
- Under the verifier flag, the canvas wrapper compares each draw call's device
  rect with the scope's and records `element, rect, overrun` once per element per
  frame. Off the flag it costs the clip push/pop only.

Alternative considered: one clip per surface. Rejected: it does not stop one
element spilling into its neighbour inside the same surface (the tab-gap case),
and gives the overrun no name.

### D2. One snapping function

`SnapToDevicePixels(rect, scale) -> SDL_Rect` (the alignment rule of
`AlignClipToDevicePixels`, generalized) is the only way a logical rect becomes
device pixels for clips, fills under a scope, and damage requests. Edge rounding
(cause 2) is a clip and a fill disagreeing by one pixel; with one function they
cannot. A fill at fractional coordinates outside any scope is the lint's concern.

### D3. Overhang is decided at layout

Text is fitted to its element before drawing (`TruncateLabelView` and the
existing elision), never clipped visibly by the scope. A glyph wider than its cell
(emoji, CJK under a terminal cursor) widens the cell's rect -- and so its damage
-- to the glyph's advance. The scope's clip is the backstop, the recorder the
alarm.

### D4. Unowned gaps

Every pixel of a strip belongs to exactly one element: separators and gaps are
either part of an adjacent element's rect or an element of their own. The strip's
layout function asserts (in tests) that its elements tile it.

### D5. Guards

- Runtime: the overrun recorder above; `RetainedRedraw::PaintAndDiffWithFullRepaint`
  fails a test on any recorded overrun, and `tools/qa` turns an overrun line into a
  `paint-overrun` finding keyed on the element kind.
- Lint: in a migrated render TU, draw calls (`FillRect`, `DrawText`, line and
  border helpers) must sit lexically inside a `PaintElement` scope. Feasible as a
  brace-depth scan like the existing render lints; it cannot follow helpers across
  TUs, which is why the runtime recorder is the primary guard and the lint the
  cheap early one.

Could the rule close the class mechanically, as `derived-damage` aims to? For
causes 1-3, yes, given D1-D4 on every element: a clip on the snapped rect makes
full and partial frames draw the same pixels, and the recorder makes a new
violation fail the first test that paints it. For cause 4, only where the element
is declared opaque; an element that is genuinely translucent over its parent keeps
the parent in its damage. In-clip findings caused by paint reading different state
in partial and full frames are outside any clip rule (non-goal).

## Risks / Trade-offs

- **Cost**: a clip push/pop per element. Rows are the volume (editor ~40 visible,
  sidebar, panel); measured per stage against the budget below. If per-row clips
  cost too much on the software renderer, the editor row scope records overruns
  without clipping (the recorder still guards) and clips only at the pane.
- **Visible clipping**: text that spilled now stops at its edge; D3 makes it elide
  instead of being cut, which is the intended look anyway.
- **Coordination**: the editor stage touches `EditorViewRenderer`, which
  `derived-damage` is restructuring; that stage goes after derived-damage stage 2
  and on its row records.

## Migration Plan

Staged by surface in sighting order (tasks.md). Each stage: wrap the surface's
elements, route its fills and damage through D2, enable the recorder for its
tests, fix what the recorder reports, perf A/B (base vs head, three interleaved
runs, `perf-tests` allocation counters and the paint scenarios; budget: no p50
> +5% beyond noise, no new steady-state allocations), then a week of soak
measuring that surface's clip-edge and in-clip sightings.

## Open Questions

- Does SDL's software renderer clip cost enough per row to matter (D1's fallback)?
  Answered by the stage-1 spike.
- Should `paint-overrun` findings be severe? Proposed: no -- they are the cause
  of stale frames, not user-visible on their own.
