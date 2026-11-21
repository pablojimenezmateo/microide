# Proposal

## Why

The soak's largest finding class is stale pixels: 768 open signatures, ~1,100
sightings in the last 36 h alone. 65-70% are regions *never damaged*: some state a
surface paints changed, and no code path asked for that region to be repainted.
Damage today is imperative -- every transition must remember to request the rect it
affects -- so each new feature adds new ways to forget, and fixing findings one at a
time never converges. `PreparedFrameState` already holds ~40 hand-picked "as painted"
fingerprints, each added after a soak bug; that is this change done piecemeal and
incompletely. A full-window repaint per frame would end the class but is ruled out:
speed and low CPU lead the priority order (`AGENTS.md`), and partial redraw is what
keeps typing and the caret blink cheap.

## What Changes

- Damage becomes **derived**: at frame preparation each surface produces a paint
  snapshot (the view model it paints from) at a defined grain -- per surface, per
  strip, per editor row -- and frame prep damages exactly the units whose snapshot
  differs from the one last painted. Hand-written damage requests stay legal but are
  no longer what correctness depends on.
- **Completeness by construction**: a surface's paint reads only its snapshot, so
  hashing the snapshot covers every input it paints. The existing render-surface
  architecture lint (`CheckRenderSurfaceStateAccess`) is extended so a migrated
  surface's paint cannot read live state behind its snapshot.
- **Staged by surface**, editor surface first (60-65% of recent sightings), then the
  bottom panel, right pane, tab strips, sidebar and the rest. Each stage removes the
  hand-written "painted_*" fingerprints its snapshot subsumes.
- **Performance is a gate, not an afterthought**: every stage is measured base vs
  head with `perf-tests` (allocation counters) and the perf harness's paint
  scenarios (`editor_typing_paint`, caret blink, scroll), interleaved on the same
  disk. A stage that costs more than its budget is redesigned before it lands.
- Clip-edge and in-clip stale paint (the other ~35%: drawing outside one's own
  rect, fractional-scale rounding) are **not** this change; they need a clip
  discipline of their own and are listed as a follow-up.

## Capabilities

### New Capabilities
- `frame-damage-derivation`: how a prepared frame decides what to repaint -- paint
  snapshots per surface unit, the comparison against what was painted, the
  completeness rule tying paint to its snapshot, and the performance budget that
  bounds snapshot cost.

### Modified Capabilities
- `workspace-architecture`: the "Render Functions Take View Models" requirement is
  tightened for migrated surfaces -- their paint reads only the frame's snapshot,
  enforced by the render-surface lint.

## Impact

- `src/workspace/render/WorkspaceShellRenderFrame.cpp` (`PrepareFrameOnce`),
  `PreparedFrameState.h`, `RenderViewModelBuilder`, the per-surface render TUs.
- `src/editor/EditorViewRenderer` (per-row paint inputs built at prep, consumed by
  paint) -- the stage with the real performance risk: partial frames skip rows
  outside the clip today because building every row cost typing ~40%.
- `tests/architecture/` (lint extension), `tests/support/RetainedRedraw.h` tests,
  the QA redraw verifier (unchanged; it is the oracle that proves each stage).
- No user-visible behaviour change; no persisted-state or protocol change.
