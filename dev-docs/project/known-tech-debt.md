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

### TD-2026-11-09-436 — soak triage, 2026-11-09: what the first four hours left open. [OPEN]

The soak runner's first four hours on `6d7e96e07` produced 113 signatures. The
2026-11-09 pass fixed the classes it could reproduce or prove with a failing-first
test (inverted std::clamp at ui.scale 3; overlapping status-bar segments; the find
widget, tab-strip contents, banner and merge-toolbar buttons leaving their area;
the crash-safety flush missing edits to a non-active buffer) and left these:

- **A tab drag abandoned with Escape, then released, paints stale.** Three panes
  side by side (`split-right` twice), press `tab:2/b.txt`, drag it into the first
  pane's strip, Escape, release: the release requests no damage, yet a full repaint
  then differs over every strip (13k px; the hover tooltip moves to the tab under
  the pointer and the third pane's breadcrumb changes from a.txt back to b.txt). The
  Escape frame is a full repaint, so it already painted stale state -- a cache or
  ordering defect in `CancelDrag`, not a missing rect. Reproduces at ui.scale 1 in
  a `QaFlowRegression` fixture (the drag-and-drop half is covered by
  `ATabDraggedAcrossThreePanesPaintsFresh`).
- **Deterministic stale frames whose minimal replay still fails at `193dda043`:**
  `9e662fe598fe` (tab drag across three panes: the bare gesture is fresh, so a
  precondition of the 198-action minimal matters), `7f858b8312bb`, `b5c3ab96f87c`,
  `5faca6fc43a7`, `8043295890fd`, `ee495f65b1b5`; `2de80b070bd6`'s replay now stops
  at a different stale frame (450) before its own (3060).
- **Nineteen "clean buffer differs from its file on disk" signatures**, all flaky at
  a 0% replay rate. `0a81fa8b3` makes the oracle save both sides' bytes beside the
  next sighting (`mismatch-*.buffer` / `.disk`, summary inlined in FINDING.md);
  chase them from that evidence.
- **Banner over another pane's strip** (`95c502fb591f`, `62b9153820d6`): the
  control source lays the banner on `layout.editor_surface`, the whole editor area,
  not the active pane -- check which rect paint uses with a split open.
- Not yet looked at: the undo-algebra findings (5), the leak findings (fds,
  threads, children), the idle-wake and latency findings, the two crashes
  (`707c77a79c1d`, `7ab9a5476905`, flaky), focus-on-a-hidden-overlay
  (`1dd177bb2104`) and the status-bar Ln/Col disagreement (`5a2cb8ba7f6b`).

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

1. **W9 rules 2 and 3.** `CheckHitTestsHonourModals` needs the hit-test call graph
   (which entry points move focus, in what order relative to the modal checks), and
   `CheckPointerOverElementsAreHoverTargets` cannot tie `PointerOver(rect)` to the
   hover catch-all by name (16 of the 35 sites pass a variable called `rect`). Both
   want runtime data instead: a QA/unit sweep that, for every element the `ui` query
   names with a hover visual, moves the pointer onto it and requires
   `HoveredInteractiveRect` to name it, and a sweep that clicks every element with a
   modal up and requires focus not to move.
2. **W2 frame-prep invariants.** The default build has no `NDEBUG` (CMake build type
   empty), so the `qa` lane and the soak's default job are already the assert-armed
   QA the plan asks for; `guidelines/testing.md` now says so. Not added: asserts that
   every surface view model is built after the frame's last state mutation, that the
   painted tooltip/hover/caret owner agrees with the input owner, and that no prep
   step requests damage outside the window. Each needs its negative control (a past
   bug reverted) before it can be trusted not to abort a healthy run.
3. **W5 remainder.** The local fault matrix covers SIGKILL at every crash point of
   `WriteTextFileAtomically`. Not yet: errno injection (ENOSPC, EIO, EACCES, EXDEV on
   rename, a 5 s fsync stall), which needs a syscall seam in `DurableFile`; crash
   points in the session/user-config writers, replace-in-files, merge writes and
   rename/move; the check that every failed save shows the user an error; and the
   write-path inventory table for `editor-essentials.md`. The monkey cannot fill the
   disk either (needs a size-limited filesystem for its fixture).
4. **W8 sequence fuzzers** are not built: `ControlCommandFuzz`, `LspSessionFuzz` and
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
9. **Findings of the first hunts not yet fixed:** several redraw-verifier findings
   (one is a panel tab's close-button hover left painted after a burst with Ctrl+wheel
   zoom), a 4 s answer to `type` of a Hebrew string (editing near the 300 KB
   single-line file under load), and a focus left on a hidden panel (first seen on a
   tiny window; may be gone with the minimum size). They are in the soak ledger
   (`tools/soak/soak.py report`) with their bundles.
11. **Layouts below the 640x400-unit floor.** The window's minimum size now follows
    ui.scale (`util::MinimumWindowSize`), but it is capped to the screen: ui.scale 3
    on a 1440x900 screen, or 2 on a 1366x768 laptop, still lays the workbench out in
    fewer units than the floor, where the chrome runs off the window and over itself.
    The fix is a layout that degrades below the floor (collapse the sidebar, the
    panel and the status segments in order) or an effective scale clamped to what the
    screen can hold; until then the QA driver's geometry oracles skip such layouts
    (`LAYOUT_FLOOR` in `tools/qa/microide_qa.py`) so they do not drown the rest.
12. **A caret on the last line can stay out of view under a sticky band.** Where
    the end of a document is reached just past a fold, the scroll that shows the
    last line has no band, and the taller band-less view clamps it back into the
    fold, whose band then hides the last row. The frame settles on the consistent
    scroll (no stale paint: `QaFlowRegression/TypingWhereTheEndLeavesAFold...`), but
    the caret stays one row below the view while typing there. The fix is a band
    rule for the end of the document (drop the band when the view reaches the last
    line, as the band then pins nothing the user scrolled past) applied in
    `StickyScrollLines` so every consumer agrees.
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
  `2026-11-08-resolved-debt-ledger.md` (TD-406, 408, 429-434)
- `CHANGELOG.md` — shipped, user-facing release history
- `openspec/changes/archive/` — the full proposal/spec/tasks record per shipped change

The broader 2026-04-20 architectural review is archived at
`dev-docs/archive/production-tech-debt-review.md`.
