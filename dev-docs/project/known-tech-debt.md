# MicroIDE Known Tech Debt

The queue of debt that is open, actionable and still present in the tree. Closed
debt does not live here: when an entry is resolved, decided or won't-do, it moves
to the dated ledger under `guidelines/tech-debt/archive/` (see "Where the history
lives" at the end), so the open work is not buried among the record of the closed.

Reviewed 2026-11-03: the file had drifted back to a record -- 290 of its 294 sections
were resolved, closed or decided, and the head of the file was ~500 lines of pass
summaries. They moved, verbatim, to
`guidelines/tech-debt/archive/2026-11-03-resolved-debt-ledger.md`; the four open
entries and the guardrails stayed.

Reviewed 2026-11-08: the eight entries resolved or closed since (TD-406, 408, 429-434)
moved, verbatim, to `guidelines/tech-debt/archive/2026-11-08-resolved-debt-ledger.md`.

## Open items

### TD-2026-11-24-456 — `WorkspaceShell` is still the god class; its wiring factories are the largest functions. [OPEN]

A 2026-11-24 cleanup pass (duplication, long functions, facades, dead code; net
-2,100 lines) left these on purpose, because each needs a design decision rather
than a mechanical change:

- **The shell's surface.** ~1,000 `WorkspaceShell::` definitions across the
  companion TUs and a 2,200-line `WorkspaceShellMembers.inc`. The declaration
  budget keeps it from growing but also blocks splitting its long members
  (`RenderSettingsOverlay` 318 lines, `HoveredTooltip` 300, `ControlUiDetails` 288,
  `HandleScheduledWakeSources` 237, `ReplaceAllProjectSearchMatches` 225): a named
  phase costs a declaration, so the code keeps lambdas instead. Moving a cluster off the shell needs the cluster's
  reads as an interface first; for ControlUi* that is ~50 callbacks, which is the
  next item's problem in another place.
- **Operations structs of `std::function`.** `MakeActionContext` (604 lines),
  `MakeKeyInputCoordinator` (375), `MakeRemoteHostService` (280) and the other
  `Make*` factories wire hundreds of one-line lambdas back into the shell. The
  hooks are now invoked through `util::InvokeIfSet`, but the shape stays: each
  coordinator depends on a bag of shell callbacks rather than on the services it
  uses. The fix is per coordinator: pass the services (`EditorTabService&`,
  `PatchApplyService&`, ...) and delete the hooks that only forwarded to them.
- **Deliberately kept long:** `ActionAvailability::IsEnabled` (one exhaustive
  switch so `-Wswitch` makes each new action decide), and the spec tables
  (`BuiltinKeybindingSpecs`, `BuiltinSettingSpecs`, `WorkspaceCommandSpecs`,
  `WorkspaceMenuSpecs`, `BuildDefaults`), which are data.
- **2026-11-25 pass.** 56 one-line shell forwarders to services were inlined into
  their callers (Members.inc cap 1512 -> 1428); the action context holds
  `CompareReviewService&` instead of seven forwarding hooks; dead code found by
  linking every binary with `--gc-sections --print-gc-sections` (and functions
  only their own tests called, `PatternCache` among them) is gone; comments were
  removed tree-wide (CLAUDE.md: tool directives only). The cluster moves above
  and the remaining `Make*` hooks, which call shell methods with real bodies
  rather than services, are still open. To re-run the dead-code scan: a clang
  build with `-ffunction-sections` and those linker flags, then the symbols every
  link removed minus those `nm` finds in any binary.
- **2026-11-25 second pass.** A window scan for near-duplicates (identifier-
  preserving, literals normalized, 6-line windows) found ~60 real pairs past
  the exact-clone scan; the substantive ones are folded (SDL surfaces own
  through `render::SurfacePtr`, blame overlays, dirty prompts, folding model,
  file watchers, terminal scroll/mouse, LSP didChange/formatting, compare/merge
  mouse layout). The prescribed coordinator fix landed for the hook bags that
  were mostly forwards: `KeyInputCoordinator` (58 hooks), `PanelMouse`,
  `SidebarMouse`, `ChromeMouse`, `TabMouse` and `Lifecycle` coordinators now
  hold a `Services` struct of references. `DebugPaneMouseCoordinator` keeps its
  debug hooks on purpose: `DebugPaneTests` fake them to observe DAP focus
  without a session. What remains of the factories is hooks into shell
  methods with real bodies, which need those bodies moved to a service first.
  The direction is now enforced: `CheckRedrawHooksAreRequestFrame` (no
  per-surface redraw aliases; twenty were folded) and
  `CheckFactoryHooksDoNotForwardToServices` (one-line service forwards counted
  against a cap that ratchets DOWN, 43 after the lifecycle move). The long
  members split without new declarations by moving their deterministic phases
  to file-local free functions: `ReplaceAllProjectSearchMatches` 186 -> 84
  lines, `HoveredTooltip` 259 -> 172, `ControlUiDetails`/`ControlUiButtons`
  lost their repeated JSON and enum code and the compare section. What is left
  on purpose: the 43 forwards sit in services whose hooks are optional
  (`RemoteHostService` branches on a missing `recents_revision`), in classes
  tests build with `{}` hooks (`TabCoordinator`), in the catalog/persistence/
  terminal-panel ring where a lazy hook is what keeps their factories from
  constructing each other recursively, or bind shell state into their
  arguments; `ControlUiButtons`' merge section and `HandleScheduledWakeSources`
  (a flat list of wake sources) need shell members and stay.
- **2026-11-25, later:** `ControlUiDetails` 200 -> 115 lines (the hover popup,
  the action receipt and each editor pane are file-local JSON builders).
- **2026-11-25, later:** `HoveredTooltip` 172 -> 124 lines (its sidebar mode
  row, panel header, debug toolbar and status bar lookups are file-local).
- **2026-11-25, later:** `RenderSettingsOverlay` 284 -> 27 lines: its frame,
  categories, section header, rows, controls and picker are file-local painters
  over the prepared card (no new declaration).
- **2026-11-25, later:** `ProjectCatalogService` takes a `Services` struct
  (`PromptSurfaceService&`, `MenuCoordinator&`); neither factory builds another,
  so they were not part of the factory ring. Cap 43 -> 41.
- **Near-duplicates left in place:** the Lua wrappers in `PluginRuntimeApiInterop`
  and `PluginHostLuaApi.inc` (their scoping is the longjmp-safety contract; fold
  them only with `tools/audit-lua-longjmp.py` in the loop), and the compare
  surface's `PaintCompareLeftSide` / `PaintCompareRightSide` (the right pane is
  editable and paints carets, diagnostics and blame the left does not).

### TD-2026-11-16-452 — what the soak still cannot say about a finding, or reach. [OPEN]

Left by the 2026-11-16 assessment of the soak (`tools/soak`) and the deep review
that followed it the same day. That review closed the first three items the
assessment listed -- a crash has a stack (`platform/CrashDiagnostics.h`, resolved
by the driver and the signature's key), the soak has source coverage (a clang
coverage build in the rotation, `tools/soak/codecov.py`, `soak.py coverage`), and
a replay says where it left the run's path (`monkey_model.Divergence`; `health`
splits "no replay reproduces" by it). Open, in the order they would pay:

1. **Signatures are not defects.** Data and undo signatures key on the file
   name, stale-paint ones on the clip count: 1336 signatures for a few dozen
   defects. `soak.py` groups them into families for reading; the ledger still
   books a fix per signature.
2. **The fault-injection jobs are not recorded.** rr cannot follow the remote
   server (it runs under the loopback sshd, not under the app).
3. **No write fails on purpose in the running app: the seam landed
   2026-11-25.** A crash-point rule can now fail a durable write with an errno
   (`fail=ENOSPC|EIO|EACCES|EROFS|EDQUOT|EXDEV|<n>:<site>[@<file>][#<nth>]`,
   armed through the same `crash-points` query) at `durable.writing`,
   `durable.fsyncing` and `durable.renaming` (`util::FaultPoint`). Held to it:
   `LocalWriteFaultMatrix/EveryInjectedWriteFailureLeavesTheOldFile` (every
   site x errno x create/replace), `WorkspaceShell/ADiskFailureDuringASaveKeepsTheEditAndSaysWhy`
   and the QA flow `save-disk-full` (the file keeps its bytes, the buffer its
   edit, a row says why, no staging file, the next save writes it). The `crash`
   profile's monkey now does it at random (`diskfull`: a one-shot ENOSPC at a
   random step of saving the focused dirty file, recorded so a replay fails the
   same save). Still open: a full disk under the session and user-config
   writers, whose failure has no row to show and needs its own oracle (the next
   flush stages everything again).
4. **A save killed part-way is not held to old-or-new bytes: FIXED
   2026-11-25.** The app logs `buffer-io: writing` with the sha of the exact
   bytes it is about to write (after the save's own transforms), and the point
   kill records the file's sha just before its save trigger; after the relaunch
   the file must hold one of the two (`DataOracle.kill9_and_check`). The
   monkey's `diskfull` rule now names the resolved file, so a save through a
   symlink is failed where it actually writes.
5. **A real crash loses a coverage build's counters: DECIDED 2026-11-25, kept.**
   The fatal-signal handler will not flush the profile: the writer allocates
   and opens files, so a crash inside `malloc` would hang there and the soak
   would book a stall instead of the crash. Crashes are rare; their runs'
   coverage is the price.
6. **The profile is carried across commits: FIXED 2026-11-25.** `soak.py
   coverage --reset` drops the merged profile; the next coverage job starts it
   afresh. Carrying it stays the default (llvm-cov drops changed functions by
   hash), so reset after deleting much code.
7. **Reach the review listed and did not build:** a scripted language server
   that answers out of order (safety-plan W8); IME composition (a control-channel
   hook for SDL's text-editing events); a Wayland cell in the soak's matrix
   (DONE 2026-11-25: the `wayland` job type, offered where a weston is); the
   `project-search` overlay mode, which no run has ever shown (dead, or reachable
   only from a layout the monkey does not make -- `surface-gaps.md` lists it);
   timestamps on the app's log lines on the driver's clock, so a trace line can
   be put beside the replay row that caused it (a prefix would change every log
   signature: it needs a field on the frame lines instead).
8. **The stall watchdog's signal can interrupt a system call: AUDITED
   2026-11-25.** Its handler (and every other one) is `SA_RESTART`, so reads and
   writes restart; what never restarts is `poll`/`select`/sleeps. Of the
   twelve `poll`s, two treated `EINTR` as a timeout and now retry against the
   remaining time: `WakePipe::PollReadableOrWake` (the LSP transport's reader)
   and the server's `ProcessTable` stdin write, which any signal (SIGCHLD) could
   fail -- a defect for users too, not only under QA.

9. **Left by the 2026-11-17 review** (which tried the deep review in a private
   soak of its branch -- `MICROIDE_SOAK_REF`, `tools/soak/AGENT.md` § A change to
   the soak itself -- and fixed what that showed: replays that changed a
   finding's signature or misread their own path, checks that could not run,
   an inventory that could not either):
   - **Item 1, for stale paint, was measured and not done.** 703 open paint
     signatures are 408 distinct (surface, clip relation, shape, element,
     reason) contexts; the clip COUNT alone is the other 295, and one plugins
     run booked three signatures that differ in nothing else. Masking it
     (`findings.normalize`) re-keys every paint finding at once: each loses its
     minimal replay and is checked again (three replays, and ddmin for the 171
     deterministic ones). Worth doing together with a wave of paint fixes, not
     alone.
   - **The leak findings are unexplained.** Three sightings show 64-171 `bash`
     children against 8-11 terminal panes the `terminals` query counted, in
     runs of the monkey that opened a terminal per terminal action; 40 plain
     `term`s keep shells and panes equal, and a replay of the 171-shell run
     shows nothing. The monkey no longer opens them by the hundred, so the
     finding will go quiet with its cause unknown; a finding's `state.json` now
     holds the `terminals` and `projects` answers for the next sighting.
   - **A build stopped the runner's loop: FIXED 2026-11-25.** The build runs on
     its own thread on a copy of the state (`BackgroundBuild`); the loop keeps
     reaping, timing out and starting jobs, and folds in what the build changed
     when it ends (`test_background_build`).
   - **Rows of one step were replayed with a settle between them: FIXED
     2026-11-25.** The recorder marks a row `n` when the next one was sent
     with no settle in between (App.settled_since_record), and the replay sends
     such rows back to back, as it does a burst. A 120-step run: 57 of 226 rows
     marked, and its replay followed the run's path.
   - **A lane's seeded monkey is a gate on a moving walk.** Any change to what
     the monkey can do re-rolls what seed 1 does; the lanes now keep a real-size
     window, and `qa-scale`'s layout seed 2 at 1.25 still meets a stale 1x8 px
     line at ui.scale 1.1 (an app paint defect).

### TD-2026-11-08-435 — the safety program's first pass: what landed, what it found, and what it left. [OPEN]

The 2026-11-08 implementation of `dev-docs/project/safety-plan.md` (its tracker has
the per-workstream state and commits). It made the monkey ~5x faster (in-process
XTEST input, socket queries, a quiet-log settle), gave it fifteen action sources,
coverage steering, a prefix corpus and data-loss/undo/leak/latency oracles, and put
it on a continuous soak runner with a findings ledger, minimizer and FINDING.md
bundles. The first hunts (12 seeds x 600 steps, then the runner) found and this
session fixed, each with a failing-first test:

- a fold chord in the same event batch as a click reopened itself (frame-prep
  reveal read the click as a caret move into the fold);
- the window had no minimum size, so below a few hundred pixels the chrome was laid
  out off the window (~70 bounds/overlap findings);
- tabs and close buttons lay under the tab strips' overflow chips (editor strip at
  ui.scale 2; the panel strip always, by 4 px);
- an overlay taller than the editor area was painted under the bottom panel, in full
  frames too;
- a symlink to a directory inside the project re-labelled the watched tree, so every
  later outside edit was reported under the link and open buffers went stale (the
  data-loss oracle; minimized 410 -> 50 actions);
- the `surface` query iterated destroyed JSON temporaries (and the range-for lint now
  knows that shape); the canonical query-verb list had drifted (`settings`);
- the pointer-cursor test sized the scrollbar without the sticky band;
- a click on an overflow chip scrolled a tab strip and requested no repaint of it
  (the CI seed; minimized 271 -> 25 actions), and the strip scroll now requests its
  own repaint;
- the tab tooltip hit-tested the focused group's tabs over the whole tab band, the
  wrong tab in a split (and a memo miss every hover frame);
- a picker's title, empty line and key hint ran past a narrow overlay's box;
- the window's minimum size was in window units, so a raised ui.scale shrank the
  layout below the floor again;
- QA flows that raced once the driver lost xdotool's delays (plugin-api, the
  fold chord) and a pre-existing clang narrowing error in a test.
- frame prep's two-step pane sizing clamped the scroll against a band-less view,
  so typing at the end of a document under a sticky band scrolled the caret out
  and repainted the wrong row (CI seed 4; minimized 185 -> 69 actions). The sizing
  is now one helper (`EditorViewRenderer::SizeViewportWithStickyBand`) shared with
  the mouse coordinator, and a pane whose scroll it moves repaints;
- the QA driver never sized the window: the app opens at its own 1440x900 whatever
  the screen, so `monkey --size` and every scaled run drove a 1440x900 window. The
  driver now places and sizes it at startup, and under `MQA_UI_SCALE` window sizes
  are layout units (`device_size`), which is what made the qa-scale lane's
  size-sensitive flows fail at 1.75.
- the presentation scale was pixels / a whole number of units (1.2503 for a
  1024-pixel window at 1.25), at which no clip aligns to device pixels, so a
  partial frame let a device column of the sidebar's text into the editor (qa
  seed 1). Every target now draws at the exact requested scale
  (`app/RenderPresentation.h`), which also closes the residual of
  TD-2026-10-22-376 for windows that are not a multiple of the scale;
- the compact layout's menu-overflow chevron was missing from the shared hover
  probe, so a move off it that another surface consumed left it lit (qa seed 1);
- the external-change banner squeezed into an editor area with no height (panel
  maximized) put its buttons over the window controls (qa-asan seed 4);
- Tab over a selection ending at a line's start reselected whole lines and moved
  the caret up a line per press, and the line it left kept its highlight (qa-asan
  seed 4): indent/outdent keep the selection's ends and direction, and an edit
  that moves the caret's line repaints the focused pane;
- `minimize.py --check-only` compared replays against the literal `auto`.
- a clean tab opened through a symlink kept its old text when the target was saved
  from its own tab (the watcher names the file written; the sweep matched views by
  path). Views record their symlink target and the change is routed to them too
  (qa-asan seed 3, link-to-crlf.txt).
- children inherited an ignored SIGINT/SIGQUIT from an editor started as a
  background job, so Ctrl+C in the terminal and the debugger's Pause of a program
  gdb was still starting did nothing (the coverage lane's DebugService failure; the
  ctest shards passed only because libuv resets signals). Every signal is now reset
  to its default in the child.

What it left, by workstream:

1. **W9 rules 2 and 3: DONE 2026-11-25 as sweeps.** `CheckHitTestsHonourModals` needs the hit-test call graph
   (which entry points move focus, in what order relative to the modal checks), and
   `CheckPointerOverElementsAreHoverTargets` cannot tie `PointerOver(rect)` to the
   hover catch-all by name (16 of the 35 sites pass a variable called `rect`). Both
   want runtime data instead: a QA/unit sweep that, for every element the `ui` query
   names with a hover visual, moves the pointer onto it and requires
   `HoveredInteractiveRect` to name it, and a sweep that clicks every element with a
   modal up and requires focus not to move.
   The hover sweep landed 2026-11-25:
   `QaFlowRegressionUx/EveryClickableElementTheUiNamesIsAHoverTarget` moves the
   pointer onto every enabled button, tab, row, status segment, menu and overlay
   row the `ui` query names -- beside an editor with the git view, with a diff in
   front, with the file finder open -- and requires the shell's hover target (or
   the diff surface's own hover state) to take it. All pass. And
   `QaFlowRegressionUx/AClickAnywhereBehindAModalMovesNoFocus` clicks every
   element of the window outside the unsaved-changes prompt with the prompt up:
   the prompt, the focus and the active tab stay. Item 1 is DONE as runtime
   sweeps (the static rules stay unbuilt, as this item found they cannot tie
   names to behaviour).
2. **W2 frame-prep invariants.** The default build has no `NDEBUG` (CMake build type
   empty), so the `qa` lane and the soak's default job are already the assert-armed
   QA the plan asks for; `guidelines/testing.md` now says so. Not added: asserts that
   every surface view model is built after the frame's last state mutation, that the
   painted tooltip/hover/caret owner agrees with the input owner, and that no prep
   step requests damage outside the window. Each needs its negative control (a past
   bug reverted) before it can be trusted not to abort a healthy run.
3. **W5 remainder.** The local fault matrix covers SIGKILL at every crash point of
   `WriteTextFileAtomically`, and since 2026-11-25 errno injection at its write,
   fsync and rename (TD-452 item 3: ENOSPC, EIO, EROFS, EXDEV, ...; a 5 s fsync
   stall is `delay=5000:durable.fsyncing`) with the editor save shown to keep the
   edit and say why. Not yet: crash points in replace-in-files, merge writes and
   rename/move (the session and user-config writers go through `DurableFile`
   and have them); the same check for every other write path; the write-path
   inventory table for `editor-essentials.md`. The monkey cannot fill the disk
   either (needs a size-limited filesystem for its fixture).
4. **W8 sequence fuzzers -- the LSP one landed 2026-11-25, and found a
   deadlock.** `WorkspaceLspClient/AnswersEveryRequestOnceWhateverOrderTheServerReplies`
   drives the client against a scripted server answering late, out of order,
   twice, for ids never asked and with diagnostics for stale versions (four
   seeds): every request must be answered exactly once. It deadlocked: the
   transport's one I/O thread wrote the outbound batch with a blocking write,
   so with the server's stdin full it stopped reading the server's stdout, and a
   server that writes its replies synchronously then stopped reading too --
   language features frozen until the 30 s write timeout killed the session.
   `StdioJsonRpcClientTransport` now writes without blocking from a pending
   buffer and waits for readable-or-writable (LSP and DAP both). Still open as
   this item: the DAP and control-command sequence fuzzers, and the libFuzzer
   form of `ViewportEditSequenceFuzz`. Previously: **W8 sequence fuzzers** are not built: `ControlCommandFuzz`, `LspSessionFuzz` and
   `DapSessionFuzz` (scripted fake servers answering late, out of order and for old
   versions), and the libFuzzer form of `ViewportEditSequenceFuzz`. The stateful
   multi-view unit sweep (`TextViewport/EditsThroughEveryViewKeepEveryViewsCachesTrue`)
   covers the viewport half with a fixed generator.
5. **Monkey reach not built** (`tools/soak/surface-extra.toml` lists each with what
   covers it instead): IME composition and clipboard-from-another-app events (both
   need a control-channel hook), maximize/fullscreen (Xvfb has no window manager),
   multi-project switching (`project-open` re-roots the session away from the scratch
   copy), fake language servers and debug adapters, a remote-project soak job, and
   the 50 MB / submodule / large-history / empty-folder fixtures.
6. **Oracle not built:** input-to-frame latency with the redraw trace attached. (The
   idle-wake oracle and more always-a-finding log markers landed; `util::Log` has no
   levels, so "any error line" stays the marker list.)
7. **Measurement not built (W1.6/W1.7):** a periodic gcov job in the rotation,
   plateau detection shifting effort to the least-covered areas, pairwise state x
   action and branch-coverage reports per commit. `surface.py` reports reached items
   per category; a keybinding counts as reached when its command ran, not when its
   chord ran in each context.
8. **W10 remainder:** mutation testing over the oracle code (the driver's
   invariants, the redraw verifier, the data oracle). The guard audit landed
   (`validation-traps.md`), and with it the frame-token assert is armed in every
   build with assertions rather than under ASan alone.
9. **DONE by 2026-11-25 (the ledger):** the focus-on-a-hidden-surface
   findings are verified (c0c5719bdbce, 1dd177bb2104, b6cc343b6ee1) and the
   stale-paint and latency families of those hunts are closed. Kept for the
   record: **Findings of the first hunts not yet fixed:** several redraw-verifier findings
   (one is a panel tab's close-button hover left painted after a burst with Ctrl+wheel
   zoom), a 4 s answer to `type` of a Hebrew string (editing near the 300 KB
   single-line file under load), and a focus left on a hidden panel (first seen on a
   tiny window; may be gone with the minimum size). They are in the soak ledger
   (`tools/soak/soak.py report`) with their bundles.
11. **Layouts below the 640x400-unit floor: FIXED 2026-11-25.** The window is
    drawn at the requested scale only while it holds the floor; otherwise at the
    largest 0.05 step that does, never below the display's own scale
    (`util::ComputeWindowPresentation`; ui.scale 3 on 1440x900 lays out 640x400
    units at 2.25). The minimum window size still follows the REQUESTED scale,
    so a window that can grow does. `LAYOUT_FLOOR` stays in the QA driver for a
    window smaller than 640x400 pixels at scale 1.
12. **A caret on the last line could stay out of view under a sticky band:
    FIXED 2026-11-25.** Two defects. The sizing loop re-targeted the scroll the
    band-less pass had clamped, not the one asked for; and at the band-less
    bottom of a document a band whose fold ends before the deepest scroll the
    band allows has no consistent scroll at all. The band rule now takes the
    pane's height and keeps an opener there only if its fold still holds the
    top line at that deepest scroll (`ComputeStickyScrollLinesUncached`), so a
    function running to the end keeps its band and a block that ends just
    before does not. `QaFlowRegression/TheCaretOnTheLastLineStaysInViewWhereAFoldEndsNearTheEnd`
    sweeps 87 shapes (failed at a 23-line tail before).
10. **The soak runner's acceptance** (24 h unattended, a planted stale-paint and a
    planted lost-save defect each found, minimized under 20 actions, deduplicated and
    verified after the revert) has not been run end to end; its pieces have, one by
    one, in this session.

## Guardrails — rejected experiments, do not retry

These are dead ends proven by the perf gate. Re-attempting them in the same shape wastes effort and
the gate will reject them again.

- **Preferring the Vulkan render driver for a faster launch** (2026-08-17). `SDL_CreateRenderer`
  costs 63-80 ms with SDL's own pick here (`opengles2`) and 41 ms with `vulkan`, and
  `SDL_HINT_RENDER_DRIVER` takes a comma-separated fallback list, so the ~30 ms looked free. It is
  paid back immediately: `render::TextBackend::UploadStringTexture` measures **8.9-10.6 ms per 95
  uploads on opengles2 and 27.1-29.0 ms on vulkan** — 3x, on the shell thread, on a path that runs
  on every glyph-cache miss. Trading interactive latency for a one-time 30 ms is the wrong direction
  for a project whose first priority is latency. Do not retry without re-measuring the upload path;
  the startup number is not the one that decides it. (`software` also takes ~75 ms to create here,
  which is the tell that the cost is the Mesa/EGL load rather than anything GL-specific — there is
  no cheap driver to switch to, only a different trade.)

- **Pre-warming the GL stack on a background thread** (2026-08-17). `SDL_CreateRenderer` is
  60-90 ms and traces almost entirely to Mesa's `eglInitialize` — a dlopen chain of 35 shared
  objects including libLLVM, plus DRM ioctls. The shell thread is idle before it, so warming the
  stack from a thread started at the top of `Application::Initialize` looked like most of that
  moved off the critical path. Measured, ten interleaved rounds each, time from process start to a
  created renderer: **108.8 ms cold vs 121.2 ms warmed** — worse. The warm does cut SDL's own
  renderer creation (86 -> 58-64 ms), but it costs 62-66 ms to do it and only ~20 ms of that
  overlaps anything (`SDL_Init` plus window creation), so the join eats the rest. Warming with
  `dlopen` alone and no `eglInitialize` costs 0.4 ms and saves nothing (99.5 vs 102.8 ms): the
  expensive part is the driver initialization, not loading libEGL. And an EGL display we initialize
  on the default (X11/DRM) native display is not the one SDL initializes on the Wayland one, so
  part of the work is done twice by construction. Do not retry without a way to warm *SDL's own*
  display before it needs it.

- **Deferring VSync until the window is mapped** (2026-08-17). The renderer is created with VSync on
  and the first two presents (the startup frame, and the re-blit after `SDL_ShowWindow`) happen
  while the window is still hidden, so pacing them to a refresh boundary looked like up to a frame
  interval each of pure waste. Measured five interleaved launches each on a Wayland session: time to
  `Application::FirstRender` **104.9 ms vs 105.9 ms** — no effect. A present against an unmapped
  surface does not block for a frame callback that was never requested, so there was nothing to
  save. Reverted rather than kept "because it cannot hurt": it is a second VSync state to reason
  about for zero measured gain.

- **Capacity-preserving line assembly in `PieceTree::ExtractLineRange`** (2026-08-10). The spanning
  line path does `out.push_back(std::move(current))`, which hands the buffer away and leaves `current`
  at zero capacity, so the next line that spans pieces re-allocates and grows geometrically. Copying
  out of `current` instead (and constructing single-piece lines straight into `out`) makes it exactly
  one allocation per line in every case. Measured **zero** on `editor_long_line_select_all_edit`,
  `editor_typing_minified_line`, `editor_smart_indent_typing`, `editor_sort_lines_large`,
  `first_line_edit`, `mid_file_edit`, `typing_large_file`, `toggle_line_comment` — before and after,
  same build, identical counts. The reason is structural: an insert appends its whole text as ONE
  piece, so a multi-line slice's lines do not span pieces; only an intra-line edit splits a line, and
  the paths that slice after those edits record inline undo entries instead. The shape the fix
  addresses is not produced by the editor. Do not retry without first exhibiting a scenario whose
  slice actually spans pieces.

- **Editor glyph atlas on the draw path** (GPU / `SDL_RenderGeometry`). RESOLVED 2026-06-28 on
  `perf/gpu-render-path`: the three preconditions were met with measurement (GPU backend confirmed +
  measurable via the new `--renderer=auto` advisory lane; the `editor_scroll_fresh_content_large`
  sweep shows ~9.7% texture-cache miss with heavy eviction churn), and a **GPU-gated, row/gutter-batched**
  atlas shipped — pixel-identical to the composite path (0-pixel-diff certified on `opengles2`),
  −8% to −15% on heavy text scenarios, software path unchanged, default-on for GPU with
  `MICROIDE_RENDER_GLYPH_ATLAS=0` as escape hatch. The original *per-quad* shape stayed wrong (it
  regressed whitespace +27% by flapping the batcher); the fix was batching per row + per gutter flush.
  The 2026-05-15 +48–83% figures were a software-renderer artifact (`SDL_RenderGeometry` rasterizes
  per-pixel there). Detail:
  `guidelines/tech-debt/archive/2026-06-16-terminal-headless-and-glyph-atlas-closeout.md` (§13 Update
  2026-06-28). (The endorsed *miss-path* colour-independent coverage atlas also remains —
  `src/render/AsciiGlyphAtlas.{h,cpp}` — now also the GPU atlas's texture source.)
- **`TextDocumentModel` ownership extraction** from `TextViewport`. Rejected: it regressed hot
  editor/render scenarios (~+15% to +30% wall) and broadly increased allocations. Do not reintroduce
  in the same shape without first proving line access, mutation, revision updates, and cache
  invalidation are allocation-free and performance-neutral in the editor benchmarks. Detail:
  `guidelines/tech-debt/archive/2026-05-20-textviewport-and-shell-decomposition.md`.
- **`UNITY_BUILD` on `microide_tests`** (build-speed). Rejected 2026-06-29: 116 of 121 test TUs
  define helpers in anonymous namespaces, with confirmed same-name collisions across files
  (`MakeService`, `MakeViewport`, `MakeFixtureRoot`, …). CMake's unity batching concatenates files
  into one TU without isolating their unnamed namespaces, so those become colliding
  `(anonymous namespace)::` symbols → redefinition errors. Making it compile would mean rewriting
  ~116 files (per-file `UNITY_BUILD_UNIQUE_ID` namespace wrapping) for a speculative win the PCH pass
  below already captured the safe part of. Do not retry without first solving the anonymous-namespace
  collision mechanically. The shipped win instead was **precompiled headers** (stable std + SDL set,
  shared by `microide`/`microide_tests`/`microide_perf`): clean `microide_tests` build 141.3 s → 120.0 s
  (~15%), suite still green, runtime byte-for-byte unchanged.

## Shipped build-speed wins (2026-06-29, after the PCH pass)

- **Shared `microide_core` object library.** `MICROIDE_CORE_SOURCES` (≈373 TUs) is now an
  `OBJECT` library compiled once and spliced (`$<TARGET_OBJECTS:microide_core>`) into
  `microide`/`microide_tests`/`microide_perf`, instead of being re-listed in each target. This
  removed the previous double compile of the entire core. The prerequisite was making core
  **`MICROIDE_TESTING`-free**: ABI-neutral seams (`*ForTesting` methods, `TestAccess`/test friends,
  the `before_cache_apply_hook`, `test_sent_bytes_`) are now compiled unconditionally, and the
  genuinely behavioral forks (placeholder-vs-real terminal startup, project-init default terminal,
  `SendBytes` capture) are gated at **runtime** via `terminal::SetUsePlaceholderTerminalsForTesting`
  (the test/perf `main()` enables it), mirroring the existing `SetHostPlatformOverrideForTesting`
  precedent. Core must stay free of `#ifdef MICROIDE_TESTING`, or the shared object set diverges by
  ABI between binaries. Measured: clean build of `microide` + `microide_tests` 148 s → 91 s (~38%,
  ccache disabled for the comparison). `WorkspaceShellTestAccess.h` keeps its guard — it is a
  test-only header, never compiled into core.
- **ccache + ld.lld + split-dwarf**, all conditionally enabled in `CMakeLists.txt` (no-ops when the
  tool is absent; toggle with `-DMICROIDE_USE_CCACHE=OFF` / `-DMICROIDE_USE_LLD=OFF`). lld is skipped
  under LTO so the `microide-perf` preset keeps its default linker. `tools/run-checks.sh` exports
  `CCACHE_SLOPPINESS=pch_defines,time_macros` so PCH TUs cache.
- **Scoped inner-loop build.** `tools/run-checks.sh tests` and the documented loop build
  `--target microide_tests` (the only binary `ctest` invokes), skipping `microide` and the bench
  binaries.
- **Splitting `microide_tests` into multiple filtered `add_test` invocations for `ctest -j`**
  (test-run speed). Rejected 2026-06-29: partitioning ~121 files' worth of tests by name-substring
  filters is a silent-coverage hazard — a test matching no subset is dropped from the run with no
  signal, trading a correctness guarantee for ~2× wall-clock on an already-acceptable 47 s suite. The
  in-binary substring filter (`TestRunnerCli`) already covers focused local iteration. Do not retry
  without a mechanically-proven complete-and-disjoint partition (e.g. generated from the registry, not
  hand-maintained filter strings).

## Where the history lives

The detailed record of closed debt — the 2026-04-29 comprehensive cleanup, the render/layout perf
batch, the throughput-pass follow-ups, the layout-revision tiers, the event-driven search/index
work, the `TextViewport` / `WorkspaceShell` decomposition, the 2026-06-11 deep correctness audit, the
2026-06-15/16 render/app/util/terminal closeouts, and the 2026-07-12 deferred-backlog sweep (the
cross-subsystem bug-hunt passes 5–24 closeout) — now lives in:

- `guidelines/tech-debt/archive/` — per-pass archive records (with reproduction notes and lessons),
  and the ledgers of entries resolved here: `2026-08-04-resolved-debt-ledger.md`,
  `2026-11-03-resolved-debt-ledger.md` (every TD id from 2026-08-04 to 2026-11-02 not listed
  above, with the per-pass review summaries that headed this file) and
  `2026-11-08-resolved-debt-ledger.md` (TD-406, 408, 429-434) and
  `2026-11-13-resolved-debt-ledger.md` (TD-437 to 439, 441 to 443, 445 onward) and
  `2026-11-25-resolved-debt-ledger.md` (TD-436, 440, 444, 449, 451, 453, 454, 455)
- `CHANGELOG.md` — shipped, user-facing release history
- `openspec/changes/archive/` — the full proposal/spec/tasks record per shipped change

The broader 2026-04-20 architectural review is archived at
`dev-docs/archive/production-tech-debt-review.md`.
