## Context

The app paints a retained scene: `Application::Render` keeps a scene texture
(`SceneTexturePresenter`), and a frame with damage repaints only the merged dirty
clips (`AnalyzeDirtyRegions`), each through `WorkspaceShell::RenderClip(token, ...,
hint)`, where the hint lets whole surfaces be skipped. Correctness of a partial
frame therefore depends on every damage site requesting the right rects AND every
skip decision being right -- two places where this week's bugs lived, and neither
visible to a unit test that paints without hints (`tests/support/RetainedRedraw.h`
now passes hints, but only for the cases a test author thought of). The event loop
folds each event's own `EventResult::redraw` with the shell's queued invalidation;
a test of the click path has to fold them the same way (the suite's helpers return
only `handled`, which is how the first draft of the click test painted nothing).

What exists and is reused rather than rebuilt:

- `tools/capture-media/lib.sh` runs the app on Xvfb with isolated XDG state,
  injects keys through xdotool and grabs frames with xwd (+ `xwd2png.py`); the
  control socket and instance descriptors live under `$XDG_RUNTIME_DIR`, so a
  private runtime dir isolates discovery too.
- `tools/fuzz-control-commands.py` already drives the shipped binary with seeded
  random control commands and on-disk churn under `xvfb-run`, built for the ASAN
  binary; it found TD-2026-09-07-291. It has its own launcher and isolation.
- `tools/repro/overlay-backdrop-repro.sh` is a one-off Xvfb scenario.
- The control channel answers queries (`tabs`, `editor`, `status`, `commands`,
  `terminals`, `notifications`, ...) and takes `key <chord>` / `type <text>`
  through the window's real keyboard path, but nothing says where anything is on
  screen, so a driver can only click coordinates.
- `tests/ApplicationTests.cpp` drives `Application::Initialize/Render/Shutdown`
  in-process under the dummy video driver through `ApplicationTestAccess`; the
  software renderer supports target textures, so the verifier can be exercised
  there with no window.

Keyboard dispatch: `KeyInputCoordinator::HandleKeyDown` asks
`HandleGlobalKeyDown` (resolved keybindings, `ActiveKeybindingContext()` from the
focus target: Editor, Sidebar, Terminal when the panel shows a live terminal pane,
Global otherwise) before any surface handler; the rule for which context a binding
is active in is `BindingMatches` / `KeybindingContextsOverlap` in
`WorkspaceKeybindingRegistry` (92 built-in bindings today).

Paint-time clock reads: `CaretVisibleNow()` calls `SDL_GetTicks()` and is read at
paint time by the bottom panel, the sidebar, the text-input caret and three sites
in `WorkspaceShellRenderFrame.cpp`; only the editor caret already goes through a
per-frame prepared value (`prepared_frame_draw_editor_caret_`).

## Goals / Non-Goals

**Goals:**
- Catch stale paint anywhere, during any session, with no reference images.
- Let a script name what it clicks and assert on UI state, not pixels.
- Prove every shortcut reaches exactly the focus targets its registration claims.
- Exercise everything a person can reach -- including a real remote host -- with
  a replayable record AND the frame trace when something breaks.
- One launcher, one isolation recipe, one oracle set for every headless driver in
  the repo.

**Non-Goals:**
- Visual regression testing against golden images (the oracles are invariants).
- Testing on real GPUs or compositors (Xvfb is software GL; GPU-only bugs stay
  manual; the redraw verifier still catches the logic bugs behind them).
- A general-purpose UI automation API for plugins; `ui` is for test tooling.
- Windows/macOS drivers.
- Rewriting `tools/capture-media` on the Python driver (its bash recipe keeps
  working; folding it in is a follow-up, see the TD filed with this change).

## Decisions

**1. The redraw verifier lives in the app, at the frame, not in the driver.**
A driver can only sample screenshots between actions and cannot force "the same
state, painted in full" without changing it. The app can: right after the partial
loop in `Render`, with the scene texture still the target, read it back
(`SDL_RenderReadPixels`), then render the same `FrameToken` with no clip and no
hint into a second target texture of the same size, read that back, and diff. The
diff is a pure function over two RGBA buffers in `src/render/PixelDiff.{h,cpp}`
(count, bounding box, first differing pixel), unit-tested without SDL windows and
consumed by `tests/support/RetainedRedraw.h` in place of its per-pixel
`SDL_ReadSurfacePixel` loop; `src/app/RedrawVerifier.{h,cpp}` owns the second
texture and the readbacks, and `Application` only calls it and logs. Alternative
considered: an offscreen full render on every *event* inside the shell --
rejected, it would test the shell's request logic but not the app's clip merging
and promotion.

**1a. The verifier is proven able to fire, in-process.** A `microide_tests` test
drives `Application` through `ApplicationTestAccess` with the verifier on, paints
a partial frame whose damage is correct and expects no report, then forces a
wrong skip (a test seam that makes `RenderClip` treat the hint as contained in
chrome, the exact shape of 2383101b) and expects one `QA-STALE` line. A verifier
that cannot fail is the perf-gate vacuity trap again
(`dev-docs/project/validation-traps.md`); the probe is part of the change, not a
follow-up.

**2. The second render must see the same state; time-dependent paint is a bug.**
Both renders use the one prepared frame. Anything that reads the clock while
painting would differ between the two renders and show up as a finding. The one
known case is the caret blink above: `PrepareFrameOnce` decides blink visibility
once per frame and every paint site reads that, as the editor caret already does.
Anything else the verifier reports is treated as a real finding to fix -- paint
reads the frame's prepared time -- not suppressed with a tolerance. If a surface
proves impossible to make deterministic quickly, it gets an explicit, documented
exclusion rect in the verifier, never a global tolerance.

**3. `ui` is assembled by a focused unit from the calls the mouse paths make.**
`src/workspace/control/ControlUiQuery.{h,cpp}` builds the JSON from an
`Operations` struct of narrow callbacks the shell wires: `CurrentWorkspaceLayout`,
`ComputeVisibleMenuBarItems`, the open popup's rows through `ComputePopupMenuRect`
+ `HitTestPopupRow` geometry and the menu registry, `WorkspaceTabStripChrome::
ComputeVisibleProjectTabs/EditorTabs/TerminalTabs`, `SidebarModeRow`, each sidebar
mode's list layout (`ScrollableListIndexAtY` inverted per visible row),
`ComputeEditorBannerButtonRects`, `BuildNotifications` + `NotificationHitAt`, and
`ForEachStatusBarSegmentRect`. The rule is "whatever hit-testing calls", which for
notifications and the status bar IS a view-model builder invoked on demand -- so
the earlier "never a view model" framing was wrong; the point is that a reported
rect is the rect a click is tested against, and that the query answers without a
frame having been painted. It follows the coordinator pattern (no
`WorkspaceShell&`), adds no member to the capped shell `.inc` beyond wiring, and
is registered in `ControlProtocol`'s query list. Answering must not request a
redraw: the test samples the damage generation before and after.

**4. The keybinding matrix runs in-process and observes dispatch, not effects.**
An effect-based check ("Ctrl+Shift+F opened search") needs a bespoke oracle per
action. Instead `KeyInputCoordinator` records the id of the last binding it
dispatched (an index into the resolved list, no allocation), readable through
TestAccess; the test sets each focus target, sends each built-in chord, and
compares "dispatched" with the expectation computed from the registration
(`context`, `skips_terminal`) via `BindingMatches`. The gates that legitimately
refuse a matching binding are the ones in `HandleGlobalKeyDown` today, each named
in one table in the test so a new exception is a visible edit: editor text
shortcuts (select all, undo/redo, copy/cut/paste, find/replace in buffer) outside
the editor or under a modal overlay; find/replace on a merge tab; select-all with
no navigable viewport; save on a compare tab; `Tab` and any command-name binding
under an open overlay; the Ctrl+K leader. The live driver repeats a sample of it
against the real window through the control channel's `key` command, which goes
through the same keyboard path.

**5. The driver is Python on the capture recipe, split by concern, and absorbs
the two headless tools that already exist.** `tools/qa/microide_qa.py` (App:
lifecycle, input, control, oracles, evidence, replay), `tools/qa/scenarios.py`
(local suite), `tools/qa/monkey.py` (action sources and weights, including the
command generator and churn from `fuzz-control-commands.py`), `tools/qa/remote.py`
(remote suite; copies `tools/qa/agents.py` to the host for simulated agents),
`tools/qa/run.py` (CLI: `suite`, `monkey --seed --steps`, `replay <file>`,
`remote`). It reuses `make-fixture.sh` and `xwd2png.py` rather than copying them.
Control traffic goes through `microide control-send` (3 ms per query measured;
discovery is unambiguous under the private runtime dir). Xvfb picks the display
(`-displayfd`): scanning lock files raced when two drivers started at once on this
machine. Every scenario runs on a fresh copy of the fixture so a seed means the
same thing twice. Python over bash: scenarios need loops, JSON, assertions and
replay files.

**6. The monkey draws actions over named elements first.**
Weighted: click/hover a random element from `ui` (50%), a known chord from the
keybinding list (15%), a control command from the fuzzer's generators (10%),
typed text (10%), a drag from a divider or tab (5%), wheel (5%), a random point
(5%). Each action is recorded before it runs; the oracles are checked after a
short settle; a finding stops the run and writes the evidence. Seeded
`random.Random` only -- no wall-clock-dependent choices.

**7. The `qa` lane is local-only and bounded.**
`run-checks.sh qa` builds `microide`, runs the suite and a 300-action monkey with
seed 1, and fails on any finding or missing tool. The remote suite needs a host
and the network, so it runs only on demand (`MQA_REMOTE_HOST=worky
tools/qa/run.py remote`).

**8. A finding carries the frames that led to it.** The driver always runs the
app with `MICROIDE_TRACE_REDRAW_VERBOSE=1` and `MICROIDE_TRACE_MENU_HOVER=1` (one
`SDL_Log` line per frame and per clip under a menu; negligible under Xvfb), keeps
the log tail per action, and takes a screenshot after every action into a ring of
the last three. A finding is written as a directory: the action list up to it, the
frame lines since the previous action, the ring of screenshots, the finding's own
screenshot and the replay file. The blank-sidebar frame of 2026-10-15 is why: it
showed four times and then never, and all that survived was one screenshot.

## Risks / Trade-offs

- [Readback per frame is slow] -> QA-only; under Xvfb a 1440x900 readback is a few
  ms. The flag is read once at startup; off means no texture, no readback.
- [Second render has side effects (hover refresh, blame overlay clearing,
  signature-help expiry, text-input area update)] -> it runs after the partial
  frame was produced, so it cannot change what was compared; any effect on the
  *next* frame is QA-only and noted in the verifier's header. If one masks a bug,
  the render path gets the side effect moved to frame prep, which is where
  paint-time mutation belongs anyway.
- [xdotool timing flakiness] -> every action is followed by a settle and oracles
  that do not depend on timing (staleness, crashes); scenario expectations poll
  `ui` with a deadline instead of sleeping a fixed time.
- [Two drivers on one machine] -> Xvfb-chosen displays, private XDG trees, and
  `MQA_WORK` under `/tmp/mqa-*` per run; nothing is discovered by scanning.
- [Xvfb differs from a GPU] -> stated non-goal; the verifier tests our logic, which
  is the same on both.
- [`ui` names drift as the UI changes] -> names derive from labels and ids the app
  already shows; the query is documented in `control-channel.md`, and the
  scenario suite failing on a missing name is the signal to update both.
- [The fuzzer's users] -> `fuzz-control-commands.py` is referenced from
  `validation-traps.md` and the TD ledger; those references move to
  `tools/qa/run.py monkey --profile commands`, which keeps its seeds, rounds,
  profiles and churn flags.

## Migration Plan

Additive. Ships behind an env var and a new query; two tools under `tools/` are
replaced by the driver and deleted in the same change. Nothing to migrate or roll
back beyond reverting the change.

## Open Questions

- Whether the verifier should also run on full frames that were *promoted* from
  partial (cheap correctness check of the promotion itself) -- start without.
- Whether the monkey should run under the ASAN build in the `qa` lane by default
  (slower, more findings) -- start with the regular build, ASAN on demand.
