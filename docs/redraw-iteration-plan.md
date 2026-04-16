# Redraw Iteration Plan

## Why this plan exists

The retained redraw architecture is directionally correct, but the current policy and invalidation behavior still have two concrete problems:

1. Fragmented partial redraws can become more expensive than a full redraw.
2. Layout-changing interactions can leave stale pixels behind when only the new geometry is invalidated.

Recent profiling confirmed the first problem. Before the partial-to-full bailout, resize activity could drive partial frames to dozens of clip rects and triple-digit frame times. After adding the bailout, the redraw summary improved materially:

- Average frame time dropped from about 14.94 ms to about 7.43 ms.
- Average clip count dropped from about 6.47 to about 3.13.
- Maximum clip count dropped from 80 to 7.

That validates the overall architecture, but it does not fully solve correctness. The terminal resize artifact shown in the recent screenshot is consistent with stale retained-scene content after bottom-panel geometry changes.

## Current architectural judgment

The architecture still makes sense:

- `Application` owns the retained scene texture and decides whether a frame is full or partial.
- `WorkspaceShell` owns semantic invalidation and surface-level redraw requests.
- Partial redraw is the right fast path for small local updates.
- Full redraw fallback is the right escape hatch for fragmented or broad updates.

What needs work is the policy and invalidation contract, not a rewrite of the redraw model.

## Main problems to solve

### 1. Geometry-changing interactions invalidate too narrowly

Bottom-panel resize currently requests redraw for the new bottom-panel rect, but not necessarily for the old panel area that was just uncovered or shifted. That can leave stale editor pixels in the retained scene texture.

This is the most likely cause of visual corruption during terminal resize.

Status:

- Completed for bottom-panel divider drags by promoting resize frames to full redraws and
  scheduling bounded settle redraws when terminal resize mutates layout during render.
- Completed for sidebar divider drags by promoting active resize frames to full redraws instead of
  relying on fragmented partial invalidation.

### 2. Dirty coverage estimation is not trustworthy

The current dirty-area estimate sums dirty rect areas without coalescing overlaps, so the computed "coverage" can exceed 100%. That makes the metric useful only as a rough fragmentation signal, not as a real coverage measurement.

Status:

- Completed in the current iteration by coalescing dirty regions in clip-space before partial replay.
- Coverage is now computed from the merged clip set and bounded to 100%.
- Promotion logic now uses coalesced clip pressure instead of raw dirty-rect storms.

### 3. Render-time state mutation still exists

`WorkspaceShell::Render()` still performs some stateful maintenance work. In a retained partial frame, `Render()` may be called multiple times, once per clip rect. That increases the risk of subtle behavior differences between full and partial redraws.

Status:

- Partially completed by splitting frame preparation from clip replay.
- Further narrowed in the current iteration by queuing blame-hover refresh from redraw requests
  instead of marking it dirty on every prepared frame.

### 4. Stress coverage is still too narrow

There are retained redraw comparison tests, but layout-changing stress cases are still under-covered. We need tests that specifically exercise resize-driven invalidation.

Status:

- Completed for the known resize-sensitive shell paths with retained-vs-full redraw regression
  tests covering bottom-panel resize settle and sidebar divider dragging.

## Ordered work items

### 1. Fix geometry-aware invalidation for layout changes

Scope:

- Bottom-panel resize first.
- Likely sidebar divider and other layout-changing chrome second.

Approach:

- Capture the old layout before the geometry mutation.
- Apply the geometry mutation.
- Capture the new layout after the mutation.
- Invalidate both the old and new affected regions, not just the current rect.

For bottom-panel resize, that means repainting at least:

- the old bottom-panel bounds
- the new bottom-panel bounds
- the editor area exposed or displaced by the height change

This is the first slice because it targets correctness directly and should address resize artifacts like stale editor content after terminal-panel dragging.

Implementation note:

- The first shipped correction may intentionally use a conservative full-redraw fallback during active bottom-panel resize, plus an immediate follow-up redraw if render-time terminal resizing mutates state.
- That is acceptable as a correctness-first step while broader retained layout-transition redraw is iterated and proven by regression tests.

### 2. Coalesce dirty regions before estimating coverage

Scope:

- Add a canonicalization step in `Application` before using dirty rects to decide whether to stay partial.

Approach:

- Merge overlapping or near-adjacent dirty rects.
- Compute coverage from the merged set instead of the raw list.
- Ensure reported coverage can never exceed 100%.

This keeps the current bailout idea, but replaces a noisy heuristic with a real one.

Implementation note:

- The coalescing step should happen in the same clip-space used for partial replay, including text clip padding, so the coverage metric matches the actual retained redraw work.
- Replaying the merged clip set is preferable to only using it for policy, because it reduces redundant `WorkspaceShell::Render()` passes directly.

### 3. Retune the partial-to-full promotion thresholds

After coalescing is in place:

- Revisit the dirty-rect-count threshold.
- Revisit the coverage threshold.
- Use traces from resize and normal editing flows to keep promotion aggressive during fragmentation without making ordinary edits fall back to full redraw too early.

Implementation note:

- Once coalescing is in place, thresholds should be based on coalesced clip count first, with coverage used as a tie-breaker for broad-but-still-fragmented redraws.
- A single large coalesced region should usually stay partial. The pathological case is multiple large regions, not merely a high raw dirty-rect count.

Status:

- Post-coalescing validation did not expose a new ordinary-editing hotspot after the resize policy
  fixes, so the current coalesced thresholds remain in place.
- Active outer-layout resize now bypasses partial replay entirely, which removed the main remaining
  pathological case that was likely to force further threshold tuning.

### 4. Move non-rendering state updates out of `WorkspaceShell::Render()`

Scope:

- Identify state mutation currently happening inside render.
- Move it to event handling, layout update, or pre-render staging where appropriate.

Goal:

- Make partial redraw replay behavior closer to pure "draw this clip against stable state".

This reduces the chance that replaying several clips in one frame diverges from a single full render.

Status:

- Partially completed in the current iteration by splitting one-time frame preparation from draw replay.
- `Application` now asks `WorkspaceShell` to prepare a frame once, then replays only the prepared draw path per coalesced clip rect.
- Project-search updates, project-open dialog results, text-input surface sync, editor-tab normalization, terminal panel resize, and cursor refresh no longer run once per replayed clip.
- Blame-hover refresh is still render-coupled, but it is now scheduled by redraw-producing state
  changes instead of every prepared frame.

### 5. Add retained redraw regression tests for layout changes

Add tests that compare retained partial redraw output against a full redraw for:

- bottom-panel resize
- sidebar resize or mode transition if applicable
- any other layout transition that changes large surface boundaries

The target is simple: after a geometry change, retained redraw output must match a fresh full redraw.

Status:

- Completed for bottom-panel resize.
- Completed for sidebar divider resize.

### 6. Consider explicit policy for active resize mode

If traces still show poor behavior during drag-resize after the previous steps:

- treat active resize as a coarse redraw mode
- or force full redraw while a panel divider is actively dragged

This is intentionally later in the plan. We should first fix the invalidation contract and improve dirty-region quality before adding a more aggressive mode switch.

Status:

- Completed for active outer-layout resize.
- Bottom-panel and sidebar divider drags now use full redraw during the drag rather than replaying
  retained partial clips through geometry changes.

## Success criteria

We should consider this iteration successful when all of the following are true:

- No visible stale pixels during bottom-panel resize.
- Retained redraw output matches full redraw in resize regression tests.
- Dirty coverage reporting is bounded and meaningful.
- Resize traces no longer show large clip storms or misleading coverage values.
- Ordinary editing interactions still benefit from retained partial redraw.

## Current progress

Completed:

- geometry-aware invalidation for bottom-panel resize
- redraw settle requests for render-time terminal layout changes
- clip-space dirty-region coalescing with bounded coverage metrics
- outer-layout active-resize full redraw policy for sidebar and bottom-panel divider drags
- authoritative multi-rect invalidation contract with helper accessors instead of a stored legacy
  single-rect field
- redraw-request-driven blame-hover refresh scheduling
- retained-vs-full redraw regression coverage for sidebar resize

Current focus:

- monitor future traces for new ordinary-editing promotion hotspots
- add app-level burst harnesses only if a new bug escapes the current shell-level redraw coverage
