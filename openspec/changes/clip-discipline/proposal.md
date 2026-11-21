# Proposal

## Why

Of the soak's open stale-paint signatures, 340 of 720 (47%) are `clip-edge` or
`in-clip`, not `outside` (ledger, 2026-11-21): 154 of them seen in the last 72 h,
403 sightings. These are the pixels `derived-damage` does not touch -- the frame
asked for the right region, and the partial frame still differs from the full
one. Measured on that 72 h window (`notes.md`):

- 61% of the sightings are at scale 1, so fractional-scale rounding is a minority
  cause (5be0561ac fixed one class of it); most are paint that does not stay
  inside the rect its damage covers.
- By surface: editor surface 130, bottom panel 95, sidebar 60, tab strips 53,
  right pane 33, the rest 36.
- A sample of twelve, classified from their stale line against their clips and,
  for three, from the retained/reference crops, falls into four causes:
  1. **overhang**: text or a glyph painted past its element's rect -- a tab label's
     last glyph into the gap between tabs (053365e38e57), a wide emoji past the one
     terminal cell the cursor blink damages (bfa3565938ee);
  2. **edge rounding**: paint one pixel past the right or bottom edge of the rect
     its damage asked for, at scale 1 and fractional alike (b4be0b6bdba5,
     050407e2649e, d6c26d0b9495, 2a3dfdcfdf2d);
  3. **unowned gaps**: pixels between two adjacent elements' damage rects that one
     of them paints (tab separators, dfaecf57f6db);
  4. **missing fill**: inside a near-full-window clip, an element paints text over
     what was there without filling its background first, so a partial frame
     leaves the old text under the new (eab90dac1acf, 0cb638ffcb39).

Each finding is fixed one at a time today, and new elements add new ways to break
the same rules.

## What Changes

- **Element paint scopes**: a painted element (a row, a tab, a divider, a cell, a
  button) is drawn inside a `PaintElement` scope on its layout rect. The scope
  clips to the rect (the full frame then shows exactly what a partial one can),
  fills the element's background when the element is opaque, and -- under the QA
  redraw verifier's flag -- records any draw call that falls outside the rect.
- **One snapping function**: the device-pixel rect of a fill, a clip and a damage
  request comes from one function (`SnapToDevicePixels`), so the rect damage
  covers is the rect paint touches at every scale.
- **Overhang is a layout decision, not a paint accident**: text is truncated or
  elided to its element before it is drawn; a glyph wider than its cell widens the
  cell's damage (and its scope) instead of spilling.
- **Mechanical guard, two layers**: the runtime overrun recorder makes every unit
  test that paints (`RetainedRedraw`, render TU tests) and every QA run report an
  overrun with the element's name, without waiting for the soak to catch a stale
  frame; and an architecture lint requires migrated render TUs to draw only inside
  a `PaintElement` scope (draw calls at function scope are rejected).
- **Staged by surface**, ordered by sightings: editor surface, bottom panel,
  sidebar, tab strips, right pane, then the rest. A performance gate per stage
  (base vs head interleaved, as in `derived-damage/notes.md`).

## Capabilities

### New Capabilities
- `paint-clip-discipline`: how an element is painted -- inside its own scope on
  its layout rect, with one device-pixel snapping function shared by clips, fills
  and damage, opaque elements owning their background, and draws outside the rect
  reported as defects.

### Modified Capabilities
- `redraw-verification`: the verifier also reports element overruns recorded by
  paint scopes, naming the element and the overrun, alongside stale frames.

## Impact

- `src/render/ScopedRenderClip.h` (snapping function shared with fills and damage),
  a new `src/render/PaintElement.h` scope, the render TUs per stage, `src/editor/
  EditorViewRenderer` (the editor stage: coordinated with `derived-damage`'s
  owner, whose row records are where a row's scope belongs).
- `tests/architecture/` (the lint), `tests/support/RetainedRedraw.h` (overruns fail
  a redraw test), `tools/qa` (overrun lines become findings).
- No user-visible behaviour change beyond text that used to spill now eliding at
  its element's edge; no persisted-state or protocol change.
- Not in scope: `outside` stale paint (`derived-damage`), and stale paint whose
  cause is state the partial frame read differently from the full one (in-clip
  findings that a fill does not explain are triaged one by one).
