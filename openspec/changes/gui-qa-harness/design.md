## Context

The app paints a retained scene: `Application::RenderFrame` keeps a scene texture,
and a frame with damage repaints only the merged dirty clips, each through
`WorkspaceShell::RenderClip(token, ..., hint)`, where the hint lets whole surfaces
be skipped. Correctness of a partial frame therefore depends on every damage site
requesting the right rects AND every skip decision being right -- two places where
today's bugs lived, and neither visible to a unit test that paints without hints
(`tests/support/RetainedRedraw.h` now passes hints, but only for the cases a test
author thought of).

Headless driving already works: `tools/capture-media/lib.sh` runs the app on
Xvfb with isolated XDG state, injects keys through xdotool and grabs frames with
xwd (+ `xwd2png.py`). The control channel answers queries (`tabs`, `editor`,
`status`, `commands`, `terminals`, `notifications`, ...) but nothing that says
where anything is on screen, so a driver can only click coordinates.

Keyboard dispatch: `KeyInputCoordinator::HandleKeyDown` asks
`HandleGlobalKeyDown` (resolved keybindings, `ActiveKeybindingContext()` from the
focus target) before any surface handler; the rule for which context a binding is
active in is `KeybindingContextsOverlap` / `BindingMatches` in
`WorkspaceKeybindingRegistry`.

## Goals / Non-Goals

**Goals:**
- Catch stale paint anywhere, during any session, with no reference images.
- Let a script name what it clicks and assert on UI state, not pixels.
- Prove every shortcut reaches exactly the focus targets its registration claims.
- Exercise everything a person can reach -- including a real remote host -- with
  a replayable record when something breaks.

**Non-Goals:**
- Visual regression testing against golden images (the oracles are invariants).
- Testing on real GPUs or compositors (Xvfb is software GL; GPU-only bugs stay
  manual; the redraw verifier still catches the logic bugs behind them).
- A general-purpose UI automation API for plugins; `ui` is for test tooling.
- Windows/macOS drivers.

## Decisions

**1. The redraw verifier lives in the app, at the frame, not in the driver.**
A driver can only sample screenshots between actions and cannot force "the same
state, painted in full" without changing it. The app can: right after the partial
loop in `RenderFrame`, with the scene texture still the target, read it back
(`SDL_RenderReadPixels`), then render the same `FrameToken` with no clip and no
hint into a second target texture of the same size, read that back, and diff. The
diff is a pure function in a new `src/app/RedrawVerifier.{h,cpp}` (bounding box,
count, first differing pixel) so it is unit-tested without SDL windows; the
Application part only renders, reads and logs. Alternative considered: an
offscreen full render on every *event* inside the shell -- rejected, it would test
the shell's request logic but not the app's clip merging and promotion.

**2. The second render must see the same state; time-dependent paint is a bug.**
Both renders use the one prepared frame. Anything that reads the clock while
painting (a fading toast, a blink computed at paint time) would differ between the
two renders and show up as a finding. That is treated as a real finding to fix --
paint should read the frame's prepared time -- not suppressed with a tolerance.
If a surface proves impossible to make deterministic quickly, it gets an explicit,
documented exclusion rect in the verifier, never a global tolerance.

**3. `ui` is assembled by a focused unit from existing geometry, not new state.**
`src/workspace/control/ControlUiQuery.{h,cpp}` builds the JSON from an
`Operations` struct of narrow callbacks the shell wires (layout, menu bar item
rects, the open popup's visible items via `ComputePopupMenuRect` and the menu
registry, tab strip rects per group, sidebar view buttons and visible rows, panel
tab rects, status segment rects, banner and notification button rects) -- the same
helpers hit-testing calls, so a reported rect is the tested rect. It follows the
coordinator pattern (no `WorkspaceShell&`), adds no member to the capped shell
`.inc` beyond wiring, and is registered in `ControlProtocol`'s query list.
Alternative: expose the render view models -- rejected, they hold paint-only data
and are rebuilt per frame; the query must answer without a frame.

**4. The keybinding matrix runs in-process and observes dispatch, not effects.**
An effect-based check ("Ctrl+Shift+F opened search") needs a bespoke oracle per
action. Instead `KeyInputCoordinator` records the id of the last binding it
dispatched (a pointer/index, no allocation), readable through TestAccess; the test
sets each focus target, sends each built-in chord, and compares "dispatched" with
the expectation computed from the registration (`context`, `skips_terminal`) via
the same rule the registry exports. Documented gates that legitimately refuse a
binding (a modal overlay over editor text shortcuts, Save on a compare tab) are
part of the expectation, each named in one table in the test so a new exception is
a visible edit. The live driver repeats a sample of it against the real window.

**5. The driver is Python on the capture recipe, split by concern.**
`tools/qa/microide_qa.py` (App: lifecycle, input, control, oracles, replay),
`tools/qa/scenarios.py` (local suite), `tools/qa/remote.py` (remote suite;
copies `tools/qa/agents.py` to the host for simulated agents), `tools/qa/run.py`
(CLI: `suite`, `monkey --seed --steps`, `replay <file>`, `remote`). It reuses
`make-fixture.sh` and `xwd2png.py` rather than copying them. Python over bash:
scenarios need loops, JSON, assertions and replay files.

**6. The monkey draws actions over named elements first.**
Weighted: click/hover a random element from `ui` (60%), a known chord from the
keybinding list (15%), typed text (10%), a drag from a divider or tab (5%), wheel
(5%), a random point (5%). Each action is recorded before it runs; the oracles are
checked after a short settle; a finding stops the run and writes the action list.
Seeded `random.Random` only -- no wall-clock-dependent choices.

**7. The `qa` lane is local-only and bounded.**
`run-checks.sh qa` builds `microide`, runs the suite and a 300-action monkey with
seed 1, and fails on any finding or missing tool. The remote suite needs a host
and the network, so it runs only on demand (`MQA_REMOTE_HOST=worky
tools/qa/run.py remote`).

## Risks / Trade-offs

- [Readback per frame is slow] → QA-only; under Xvfb a 1440x900 readback is a few
  ms. The flag is read once at startup; off means no texture, no readback.
- [Second render has side effects (hover refresh, blame overlay clearing)] → it
  runs after the partial frame was produced, so it cannot change what was
  compared; any effect on the *next* frame is QA-only and noted in the verifier's
  header. If one masks a bug, the render path gets the side effect moved to
  frame prep, which is where paint-time mutation belongs anyway.
- [xdotool timing flakiness] → every action is followed by a settle and oracles
  that do not depend on timing (staleness, crashes); scenario expectations poll
  `ui` with a deadline instead of sleeping a fixed time.
- [Xvfb differs from a GPU] → stated non-goal; the verifier tests our logic, which
  is the same on both.
- [`ui` names drift as the UI changes] → names derive from labels and ids the app
  already shows; the query is documented in `control-channel.md`, and the
  scenario suite failing on a missing name is the signal to update both.

## Migration Plan

Additive. Ships behind an env var and a new query; nothing to migrate or roll
back beyond reverting the change.

## Open Questions

- Whether the verifier should also run on full frames that were *promoted* from
  partial (cheap correctness check of the promotion itself) -- start without.
- Whether the monkey should run under the ASAN build in the `qa` lane by default
  (slower, more findings) -- start with the regular build, ASAN on demand.
