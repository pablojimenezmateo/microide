## 1. Redraw verifier

- [ ] 1.1 Add `src/render/PixelDiff.{h,cpp}`: a pure diff of two equally sized RGBA buffers returning count, bounding box and first differing pixel; unit tests in `microide_tests` (identical, one pixel, size mismatch); `tests/support/RetainedRedraw.h` consumes it in place of its own loop
- [ ] 1.2 Add `src/app/RedrawVerifier.{h,cpp}` (second target texture, the two readbacks, the `QA-STALE` line) and call it from `Application::Render` after a partial frame when `MICROIDE_QA_VERIFY_REDRAW` was `1` at startup; the line carries count, bbox, the merged clips and the redraw reason
- [ ] 1.3 Prove it fires in-process: an `ApplicationTests` test with the verifier on expects no report for a correct partial frame and exactly one for a frame with a forced wrong skip (test seam on `RenderClip`'s chrome containment); the forced-skip half is the vacuity probe
- [ ] 1.4 Decide caret blink once per prepared frame and make the five `CaretVisibleNow()` paint sites (bottom panel, sidebar, text input, three in `WorkspaceShellRenderFrame.cpp`) read it; run the app with the verifier through the media capture scenes and fix (or document as an exclusion) anything else it reports
- [ ] 1.5 Confirm the off path adds no work: perf-tests and perf-canary lanes unchanged

## 2. `ui` control query

- [ ] 2.1 Add `src/workspace/control/ControlUiQuery.{h,cpp}` with an `Operations` struct and the JSON builder for window size, focus, sidebar, overlay, menu, panel and layout surface rects
- [ ] 2.2 Elements: menu bar items (`menu:`), open popup rows (`menu-item:`), editor tabs per group (`tab:`), sidebar view buttons (`sidebar-view:`) and visible rows (`row:`), panel tabs (`panel-tab:`), status segments (`status:`), banner/notification/sidebar-header buttons (`button:`), with unique-name suffixes
- [ ] 2.3 Wire the operations from the shell using the same calls the mouse paths make; register `ui` in `ControlProtocol`'s query verbs
- [ ] 2.4 Tests: each element's center hit-tests to that element (menu item opens its menu, tab click activates it, row click selects it); repeated queries are identical and the damage generation does not move
- [ ] 2.5 Document the query in `dev-docs/control/control-channel.md`

## 3. Keybinding reachability matrix

- [ ] 3.1 Record the last dispatched binding in `KeyInputCoordinator` (no allocation) and expose it through TestAccess
- [ ] 3.2 Test: for every built-in keybinding x focus target (editor, tree, search view, git view, output panel, terminal, debug pane, quick-open overlay), dispatch happens exactly where the registration says; one named table of the `HandleGlobalKeyDown` gates
- [ ] 3.3 Fix or file every mismatch the matrix finds

## 4. QA driver

- [ ] 4.1 Finish `tools/qa/microide_qa.py` against the real `ui` query: App lifecycle (Xvfb `-displayfd`, fresh fixture copy per run), input, control, `ui` locators, oracles, per-action screenshot ring and frame-trace tail, finding directories, action record/replay
- [ ] 4.2 `tools/qa/monkey.py`: the weighted action sources per design, including the command generators, profiles and churn from `tools/fuzz-control-commands.py`; delete that script and repoint its references (`validation-traps.md`, the TD ledger)
- [ ] 4.3 `tools/qa/scenarios.py`: menus and rows, overlays, sidebar views, tabs/panel tabs/status segments, divider drags, split/close, terminal panel, scrolling -- each under editor, tree and terminal focus; the overlay-backdrop repro as a scenario (delete `tools/repro/overlay-backdrop-repro.sh`, update `media-generation.md`)
- [ ] 4.4 `tools/qa/run.py` CLI: `suite`, `monkey --seed --steps [--profile]`, `replay <file>`, `remote`; non-zero exit on any finding; findings summary naming each finding directory
- [ ] 4.5 Verify a saved monkey action list replays its finding; verify two drivers can run at once on one machine
- [ ] 4.6 `tools/qa/README.md`: setup, running, reading a finding directory, adding a scenario

## 5. Remote scenarios

- [x] 5.1 `tools/qa/agents.py` (simulated agents: bursts, churn with commits, reset) copied to the host per run
- [x] 5.2 `tools/qa/remote.py`: open host folder and wait for the mirror; host edits under open/unopened tabs; save conflict and resolve; drop and restore the link; host terminal typing; agent churn with the Edit menu, search view and a split open; mirror byte-identical at the end
- [ ] 5.3 Run the remote suite against worky and fix or file every finding

## 6. Lane and wrap-up

- [ ] 6.1 `tools/run-checks.sh qa`: build `microide`, suite, 300-action monkey (seed 1), `/tmp/microide-qa.log`, fail on finding or missing Xvfb/xdotool/xwd
- [ ] 6.2 Run the full suite, the monkey with several seeds, and the remote suite; re-find TD-2026-10-15-343 (the blank sidebar under an open menu) with its frame trace, or record what the traced runs rule out; every fixed finding lands with a failing-before unit test and its own commit; unfixed ones filed as TD
- [ ] 6.3 Update `dev-docs/project/validation-traps.md` (the verifier as the stale-paint oracle; a finding without its frames) and CLAUDE.md's validation list with the `qa` lane
- [ ] 6.4 Run the check lanes (tests, perf-tests, clang-build, asan, ubsan, tsan) green
