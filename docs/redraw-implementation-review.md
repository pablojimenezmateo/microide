# Redraw Implementation Review

Reviewed on 2026-04-16.

Scope:
- `src/app/Application.cpp`
- `src/workspace/*`
- redraw-related tests under `tests/WorkspaceShell*`

Validation:
- Reviewed the retained-scene redraw path and the shell invalidation helpers directly.
- Ran focused redraw regression tests with `env SDL_VIDEODRIVER=dummy ./build/microide/microide_tests ...`.
- The targeted redraw tests passed locally, including sidebar retained redraw, tab-open retained redraw,
  terminal caret retained redraw, compare row-band redraw, merge conflict-band redraw, and editor
  blame-neighborhood redraw.

Follow-up status:
- Dirty-region coalescing and partial-to-full promotion are now shipped in `Application`.
- One-time frame preparation is split from retained clip replay in `WorkspaceShell`.
- `RenderInvalidation` now stores only the authoritative multi-rect payload, with helper accessors
  instead of a stored legacy single-rect field.
- Bottom-panel and sidebar divider drags now use coarse full redraws, backed by retained-vs-full
  regression tests.
- Retained redraw comparison tests that use SDL dummy video should be run serially because they
  share global SDL state.

## Summary

The redraw implementation is directionally good. The important structural choices are the right
ones:

- `Application` owns the retained scene texture and presents one composed frame.
- `WorkspaceShell` owns semantic invalidation instead of forcing the app loop to guess.
- the test suite now checks several retained partial-redraw paths against equivalent full redraws.

I do not think this is bad tech debt by default. The main risk is not the retained-scene idea
itself, but how much render-time state mutation still happens around it. That makes the partial
redraw path harder to reason about and more expensive than it looks under event bursts.

## What Looks Good

### 1. Invalidation ownership is much clearer than a heuristic app-loop redraw model

Evidence:
- `Application` now consumes shell-provided dirty rects instead of guessing repaint scope.
- `WorkspaceShell` exposes targeted helpers such as `RequestTabStripRedraw()`,
  `RequestFocusedEditorRedraw()`, `RequestCompareRowRangeRedraw()`, and
  `RequestMergeConflictRedraw()`.

References:
- `src/app/Application.cpp:103`
- `src/workspace/WorkspaceShell.cpp:390`
- `src/workspace/WorkspaceShell.cpp:447`
- `src/workspace/WorkspaceShell.cpp:497`
- `src/workspace/WorkspaceShell.cpp:580`

Why this matters:
- redraw ownership is now closer to the state mutation that actually needs repainting
- compare, merge, blame, chrome, and terminal behavior can request different scopes explicitly

### 2. The implementation already has meaningful correctness coverage

Evidence:
- retained sidebar redraws are compared pixel-for-pixel against full redraws
- opening a tab through the retained path is compared against a full redraw
- terminal caret blink redraws are compared against a full redraw
- compare and merge tests assert that partial invalidation stays scoped to the expected pane or row
  bands
- editor dirty-state transitions explicitly cover blame-neighborhood invalidation

References:
- `tests/WorkspaceShellChromeTests.cpp:424`
- `tests/WorkspaceShellChromeTests.cpp:484`
- `tests/WorkspaceShellTerminalTests.cpp:437`
- `tests/WorkspaceShellCompareTests.cpp:182`
- `tests/WorkspaceShellCompareTests.cpp:220`
- `tests/WorkspaceShellSessionTests.cpp:252`
- `tests/WorkspaceShellSessionTests.cpp:297`
- `tests/WorkspaceShellEditorBlameTests.cpp:123`

Why this matters:
- this is no longer a redraw system held together by hope
- several previous failure modes are now locked down by regression coverage

## Worth Doing

### 1. Coalesce dirty rects and add a full-redraw cutoff

Impact:
- High

Evidence:
- handled events append dirty rects directly into a grow-only vector until the next present
- partial redraw then replays `WorkspaceShell::Render()` once per dirty rect
- there is no coalescing, deduplication, rect-count cap, or “just do a full redraw” threshold

References:
- `src/app/Application.cpp:143`
- `src/app/Application.cpp:149`
- `src/app/Application.cpp:423`
- `src/workspace/WorkspaceShell.cpp:380`

Why this matters:
- a burst of mouse-motion or drag events can turn one frame into many full shell traversals with
  different clip rects
- at some point the partial path becomes more expensive than a single full redraw
- this is the highest-value remaining performance improvement in the current design

Recommended next step:
- merge overlapping or near-adjacent rects before rendering
- cap the number of dirty rects per frame
- fall back to a full redraw when rect count or summed dirty area crosses a threshold

### 2. Move state mutation out of `WorkspaceShell::Render()`

Impact:
- High

Evidence:
- render still consumes background/project-dialog updates before drawing
- render still mutates layout-related state such as editor sync, split normalization, and terminal
  panel resize
- blame overlay construction mutates viewport size and issues async blame requests inside the render
  path

References:
- `src/workspace/WorkspaceShellRender.cpp:197`
- `src/workspace/WorkspaceShellRender.cpp:210`
- `src/workspace/WorkspaceShellRender.cpp:216`
- `src/workspace/WorkspaceShellBlame.cpp:114`
- `src/workspace/WorkspaceShellBlame.cpp:127`

Why this matters:
- `Application` can call `WorkspaceShell::Render()` once per dirty rect in the same frame
- every render-time side effect is therefore multiplied by dirty-rect count
- it also muddies ownership: event handling is supposed to produce invalidation, but render is still
  allowed to mutate state and queue more invalidation

Recommended next step:
- make render as close to pure drawing as practical
- process queued updates before rendering, not during rendering
- keep render-time helpers side-effect free where possible

### 3. Clean up the invalidation contract

Impact:
- Medium

Evidence:
- `RenderInvalidation` carries both `rects` and a legacy single `rect`
- `rect` is only the first rect that was requested, not the union and not the authoritative payload
- shell input/mouse fallback checks also key off `rect.has_value()`

References:
- `src/workspace/WorkspaceShell.h:70`
- `src/workspace/WorkspaceShell.cpp:380`
- `src/workspace/WorkspaceShellInput.cpp:23`
- `src/workspace/WorkspaceShellMouse.cpp:15`

Why this matters:
- the API is easy to misuse because it has two partially overlapping sources of truth
- multi-rect invalidation is already real in compare, merge, and blame paths
- future call sites can accidentally read `rect` and miss most of the invalidation set

Recommended next step:
- remove the single-rect field or make it a derived helper, not stored state
- add a small helper API such as `HasAnyRedraw()` / `SingleRectIfOnlyOne()`
- keep the representation authoritative in one place only

### 4. Add app-level redraw stress tests, not just shell-level retained tests

Impact:
- Medium

Evidence:
- the current regression suite is strong at the shell boundary, but it mostly exercises
  `WorkspaceShell` directly or manually replays retained invalidation
- the app loop still owns important behavior: event burst accumulation, dirty-rect replay ordering,
  and the retained-scene fallback behavior

References:
- `src/app/Application.cpp:113`
- `src/app/Application.cpp:392`

Why this matters:
- the shell tests prove partial redraw correctness for individual scenarios
- they do not yet prove that the full app loop behaves well when many SDL events land before one
  present

Recommended next step:
- add one or two `Application`-level tests for event bursts
- focus on drag, hover, and repeated typing paths where rect accumulation is most likely to matter

## Possible Bugs And Latent Risks

### 1. Project-search overlay mode looks stale and would invalidate the wrong surface if revived

Impact:
- Medium

Evidence:
- `OpenProjectSearch()` no longer opens a project-search overlay; it redirects to the search sidebar
- project-search refresh and async update handling always request sidebar redraws
- the codebase still contains overlay-specific `ProjectSearch` branches in input and render paths

References:
- `src/workspace/WorkspaceShellOverlay.cpp:58`
- `src/workspace/WorkspaceShellOverlay.cpp:83`
- `src/workspace/WorkspaceShellOverlay.cpp:108`

Why this matters:
- this looks like a half-retired mode rather than a cleanly removed one
- if overlay-based project search becomes reachable again, async result updates would invalidate the
  sidebar, not the overlay

Recommendation:
- either delete the overlay-specific project-search mode completely
- or make redraw selection depend on the active presentation surface

### 2. Render-time invalidation can leak into the next event result

Impact:
- Medium

Evidence:
- `HandleEvent()` consumes pending invalidation only when finishing an event
- `WorkspaceShell::Render()` still calls helpers that can request redraws
- `ConsumePendingRenderInvalidation()` simply returns and clears whatever was left queued

References:
- `src/workspace/WorkspaceShellInput.cpp:21`
- `src/workspace/WorkspaceShell.cpp:369`
- `src/workspace/WorkspaceShellRender.cpp:197`

Why this matters:
- if render queues invalidation after drawing the new state, that redraw request is already stale
  for the current frame
- the stale invalidation can then be bundled into the next unrelated handled event
- this is more likely to cause redundant repainting than visual corruption, but it is still a leaky
  contract

Recommendation:
- do not let render enqueue redraw work
- keep redraw production in event/update processing only

## Lower-Priority Notes

### 1. Broad redraw fallbacks are acceptable in a few places

Examples:
- sidebar open/close
- prompt dismissal
- overlay dismissal
- any change that shifts overall layout geometry

Why this is fine:
- those transitions change multiple surfaces or invalidate old geometry
- forcing them into ultra-narrow rect math would increase complexity faster than it would buy real
  performance

### 2. The current trace hook is useful, but not yet enough to tune the remaining hotspots

Evidence:
- redraw tracing tracks frame counts and full-versus-partial counts, but not the distribution of
  dirty-rect counts, total dirty area, or “partial was more expensive than full” situations

References:
- `src/app/Application.cpp:520`

Recommendation:
- if you add rect coalescing or a cutoff heuristic, extend the trace output to log:
- average dirty rect count
- max dirty rect count
- how often partial redraws were promoted to full redraws

## Bottom Line

This is a good implementation, not a failed optimization. The retained-scene approach, explicit
dirty-rect ownership, and current regression coverage are all worthwhile.

The two things most worth doing next are:

1. add dirty-rect coalescing plus a full-redraw cutoff
2. stop letting `WorkspaceShell::Render()` perform update-phase work

If those two are addressed, the remaining redraw debt becomes much more manageable and much less
surprising to maintain.
