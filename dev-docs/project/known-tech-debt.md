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

### TD-2026-11-13-449 — SDL's X11 clipboard can paste the previous text right after another application copies. [OPEN]

Found standing up the QA driver's foreign clipboard (`App.foreign_clipboard`,
`tools/qa/x_clipboard.py`): a paste a few milliseconds after another client took
the CLIPBOARD returned the text of the paste BEFORE it, into a buffer confirmed
empty -- about one paste in three in `edit-foreign-clipboard` without a settle.

The mechanism is SDL 3.2's X11 backend (`SDL_x11clipboard.c` / `SDL_x11events.c`):
on the XFixes notice that the owner changed, SDL asks the new owner for TARGETS
(into the `SDL_FORMATS` property). A paste then runs `GetSelectionData`, which
converts into the `SDL_SELECTION` property and waits on `selection_waiting` --
a flag ANY SelectionNotify clears, the TARGETS answer included. It then reads
`SDL_SELECTION`, which still holds the previous paste (read with delete=False),
and returns it. The same read also happens after its 1 s timeout, where it then
takes the selection itself with empty data.

Who meets it: a paste right after a copy elsewhere, and more often with a
clipboard manager, which re-takes the selection after every copy. The app cannot
tell: `SystemClipboard::Read` gets well-formed text. The fix is SDL's: match the
SelectionNotify to the request (its target and property), and delete or clear
the property before converting. Report upstream; the QA scenario settles 0.5 s
before pasting meanwhile, as a person does.

### TD-2026-11-11-444 — a cold structural refinement of a heavily churned file costs several times the old intraline pass. [OPEN]

`microide_diff_bench`, min of runs, interleaved with `main` on the same machine,
four files diffed across ~4 months of history (table in
`openspec/changes/archive/2026-11-11-structural-diff/bench.md`): code files
2-4x the line-era cost (`CompareModel.cpp`, 110 hunks: 1.5 ms -> 5.8 ms), and a
Markdown file rewritten end to end ~9x (1.5 ms -> 13.8 ms). The compare perf
scenarios are at parity, allocations included, and a rebuild of an open compare
refines only the hunks that changed (`HunkRefinementCache`), so this is the cost
of the FIRST build of an extreme diff, not of typing.

What is left, measured: an A* step costs ~100 ns (hash probe, stack-group
reads, heuristic, bucket push for ~3-5 edges), and prose gaps keep the search
because Markdown's backtick spans are string literals on both sides, which the
atom-only shortcut must not skip (a replaced literal is a graph edge). Leads, in
order of expected value: (1) a `Replaced`-aware flat matcher so literal-bearing
atom gaps skip the graph too; (2) packing a vertex's identity into one 64-bit
key and dropping `Vertex::stack` group reads from the hot path; (3) a lower
search cap for gaps that contain no list at all. Gate each on the bench table.

Lead (1) measured 2026-11-13 and NOT taken: an exact DP aligner for atom-only
gaps with literals on both sides (the search's own edge costs, tabulated; its
cost matched the A* path cost on 600 random gaps) took 206 of the Markdown
file's 111,061 vertices and 34 of `PerfMain.cpp`'s 26,221 -- such gaps are a
rounding error, and the DP always fills n*m cells where A* runs near-linear on
near-identical gaps; wall time showed no gain. The Markdown cost is in gaps
that CONTAIN lists (link `[text](url)` pairs, parentheses in prose), so the
remaining leads are (2) and (3), plus a profile: `perf` needs
`kernel.perf_event_paranoid` <= 2 on this machine (it is 4) and valgrind is not
installed, so the next pass should start by getting one of them.

### TD-2026-11-11-440 — the structural diff reads a lexer's tree, not a grammar's. [OPEN]

The compare view's structural refinement (`src/compare/DiffSyntax.*`,
`StructuralDiff.*`, openspec `structural-diff`) builds each hunk side's syntax
forest from a language-aware lexer plus bracket matching, with the rules
(`DiffSyntax`) derived from the language contract: comment tokens and bracket
pairs. difftastic parses with tree-sitter, so its lists are real syntax nodes --
an `if` owns its condition and its block, a call owns its callee. Here those are
siblings inside the enclosing brackets, which makes some matches coarser than
difftastic's: a novel `if (x) {` head is three atoms and a list rather than one
node, and the graph search cannot prefer "the same statement, edited" over "a
different statement" beyond what bracket nesting says.

Deliberately not taken now: a grammar per language is a C dependency plus one
generated parser per language (several MB each), and the lexer covers every
language the editor knows. The seam is ready for it: `BuildDiffSyntaxTree` is the
only producer of a `DiffSyntaxTree`, and the search reads nothing but nodes,
tokens and content ids. A grammar-backed builder for the top languages, with the
lexer kept as the fallback, is the follow-up; gate it on `microide_diff_bench`.

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
  chase them from that evidence. Second pass (same day): the runner threw a known
  signature's later sightings away, so that evidence never reached a bundle --
  `8381257b6` keeps them under `sightings/`. The first bytes in hand fixed two
  causes: adopting the disk baseline left the undo clean point behind
  (`994643047`; code.py, trailing-ws.txt -- which writer made the file equal the
  dirty buffer is still not identified) and Revert File split a binary file's lone
  CRs into lines (`571042f73`; binary.bin, deterministic). The other signatures
  (bom, crlf, link-to-crlf, mixed, tabs, data.json, ...) wait for their sightings.
- **Reopened after this pass's fixes, waiting for a post-fix sighting:**
  `d36a78ef295c` (an Untitled buffer missing after kill -9 although `193dda043`
  arms the flush for every buffer -- seen at `8c740eb00` in two cells). Note that a
  `QA-STALE` signature keys on its shape only, so `49a23ffecdf7`/`2eef96d45d54`
  reopening means "some one-/two-clip stale frame", not the status-bar defect.
- **Banner over another pane's strip** (`95c502fb591f`, `62b9153820d6`): the
  control source lays the banner on `layout.editor_surface`, the whole editor area,
  not the active pane -- check which rect paint uses with a split open.
- Not yet looked at: the undo-algebra findings (5), the leak findings (fds,
  threads, children), the idle-wake and latency findings, the two crashes
  (`707c77a79c1d`, `7ab9a5476905`, flaky), focus-on-a-hidden-overlay
  (`1dd177bb2104`) and the status-bar Ln/Col disagreement (`5a2cb8ba7f6b`).
- **Containment breach, 2026-11-09 06:23 (fixed; one question open).** A soak
  run's git escaped its scratch project into the checkout: "qa commit 29" on
  main, `qa-branch-0..3`, two stash/pop pairs, the checkout switched to another
  branch, and `qa-branch-3` pushed to GitHub. Mechanism: the project sat under
  `build/soak/runs/`, its `.git` was no longer a repository (the monkey's
  `delete-file` creates `.git/qa-deleted`, so a removed `.git` comes back as a
  plain directory), the app's `HasGitMarker` still read it as one, and git's
  upward search found the checkout -- for the monkey's `git -C` and the app's own
  git (which is how the switch and the push happened). Fixed by a ceiling in the
  app's single git choke point (`Git/CommandNeverReachesAnEnclosingRepository`),
  the harness fences (`tools/qa/test_containment.py`) and the runner's alarm
  (`AGENT.md` § A containment breach). Open: what removed the project's `.git` in
  the first place (the app's tree delete, or a monkey op) is not identified.
- **A truncated tab's background one device pixel past its pane** (soak
  `49a23ffecdf7` sighting at `9e17f0048`, release, 1440x900 at ui.scale 2): a
  narrow pane's last tab ("Welc…") painted its background over the divider at
  x=1151 in the full frame; a partial frame clipped to the pane did not. The
  strip layout keeps tabs inside fractional-width strips
  (`TabStripService/EditorStripContentsStayInsideANarrowStrip`), so the extra
  pixel comes from the strip rect or the paint. The sighting's replay is not
  deterministic. Its signature now carries `clip-edge|line` (`8ff6e03f7`), so its
  next sighting is filed on its own.
- **An editable Welcome placeholder, and a breadcrumb that changed undamaged**
  (soak `538a455016ef`, its 640x480 ui.scale 1.25 replay, 936 actions after
  ddmin; the replay's step `type alpha` after `focus-other-group 999999`). The
  top pane's strip showed `*empty.txt` while the pane drew the Welcome text with
  the typed `a` in it ("amicroide"), and the pane's breadcrumb went from
  "Welcome" to "Untitled" with no rect requested for it -- the partial frame kept
  "Welcome". Two defects: (1) a keystroke reached the welcome surface's viewport
  (and its `placeholder` flag cleared) while a file tab was active in that
  group; (2) the breadcrumb label is drawn from state that more than one path
  changes, and no prep-time check repaints it when it changes -- the
  `PreparedFrameState::painted_*` per-group pattern fits (`painted_breadcrumb`,
  damaging `EditorGroupRect::breadcrumb`). Neither reproduced in a short fixture
  (closing a pane's last tab and typing, or with a split and
  `focus-other-group`), so both wait for a shorter replay or a fixture that
  reaches an editable placeholder.
- **The stale-paint signature's `elsewhere` surface is a catch-all.** A box whose
  centre is on no surface the `ui` query names (`ControlUiQuery.cpp` lists nine:
  no splitter, no merge/compare pane, no gap beside a scrollbar) gets
  `elsewhere`, so `ee7ad7b12a7e` (`elsewhere|outside|area`) held three unrelated
  defects after its first fix: the merge panes' shared scroll (`8fc1d580f`), and
  two sightings at `6da237807` not yet chased. Naming more rects in the `ui`
  answer, or keying `elsewhere` on the nearest surface, would split them.
  Most of the merging was a units bug, fixed in `498eea75b`: the line's bbox and
  first pixel are device pixels, the clips and surfaces logical units, so at
  density 2 (most soak cells) nearly every box landed `elsewhere|outside`. The
  line now carries `scale=` and the bucketing scales by it; what is left of this
  entry is the genuinely unnamed rects.
- **`ExternalRepoChange/IdenticalRewriteConfirmsOffTheShellThread` and
  `IdenticalRewriteWithSplitViewsReadsOnce` flaked under load (fixed).** About 1
  in 48 and 1 in 64 with 48 copies in parallel. Instrumented, the failing runs
  posted the second phase's confirm and never applied it before the assertions:
  `FileReadService::FlushPendingReads` (and `CancelAllAndFlush`) returned early on
  "nothing pending, nothing in the mailbox", which is also what a flush sees
  between a worker dropping its pending count and posting its completion. The
  shortcut is gone (`FileReadService/AFlushNeverReturnsBeforeTheCompletionLands`,
  failing 2000 of 2000 before); 0 in 384 and 0 in 192 after. The production
  callers are project switch and teardown, where the late completion arrived
  after the project it was read for.

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
  `2026-11-08-resolved-debt-ledger.md` (TD-406, 408, 429-434) and
  `2026-11-13-resolved-debt-ledger.md` (TD-437 to 439, 441 to 443, 445 onward)
- `CHANGELOG.md` — shipped, user-facing release history
- `openspec/changes/archive/` — the full proposal/spec/tasks record per shipped change

The broader 2026-04-20 architectural review is archived at
`dev-docs/archive/production-tech-debt-review.md`.
