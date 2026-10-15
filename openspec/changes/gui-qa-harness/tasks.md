## 1. Redraw verifier

- [ ] 1.1 Add `src/app/RedrawVerifier.{h,cpp}`: a pure diff of two equally sized RGBA buffers returning count, bounding box and first differing pixel; unit tests in `microide_tests` (identical, one pixel, size mismatch)
- [ ] 1.2 In `Application::RenderFrame`, read `MICROIDE_QA_VERIFY_REDRAW` once at startup; when on and the frame rendered partially, read back the scene texture, render the same `FrameToken` in full into a lazily created verify texture, read it back, diff, and log one `QA-STALE` line (count, bbox, merged clips, redraw reason) on a difference
- [ ] 1.3 Prove it fires: a test or scripted run that reintroduces the Edit-menu bounding-box skip locally reports `QA-STALE`; with the fix, a scripted Edit-menu + caret-blink run reports nothing
- [ ] 1.4 Run the app with the verifier through the media capture scenes and fix (or document as an exclusion) every time-dependent paint it reports
- [ ] 1.5 Confirm the off path adds no work: perf-tests and perf-canary lanes unchanged

## 2. `ui` control query

- [ ] 2.1 Add `src/workspace/control/ControlUiQuery.{h,cpp}` with an `Operations` struct and the JSON builder for window size, focus, sidebar, overlay, menu, panel and layout surface rects
- [ ] 2.2 Elements: menu bar items (`menu:`), open popup rows (`menu-item:`), editor tabs per group (`tab:`), sidebar view buttons (`sidebar-view:`) and visible rows (`row:`), panel tabs (`panel-tab:`), status segments (`status:`), banner/notification/sidebar-header buttons (`button:`), with unique-name suffixes
- [ ] 2.3 Wire the operations from the shell using the hit-test helpers; register `ui` in `ControlProtocol`'s query verbs
- [ ] 2.4 Tests: each element's center hit-tests to that element (menu item opens its menu, tab click activates it, row click selects it); repeated queries are identical and request no redraw
- [ ] 2.5 Document the query in `dev-docs/control/control-channel.md`

## 3. Keybinding reachability matrix

- [ ] 3.1 Record the last dispatched binding in `KeyInputCoordinator` (no allocation) and expose it through TestAccess
- [ ] 3.2 Test: for every built-in keybinding x focus target (editor, tree, search view, git view, output panel, terminal, debug pane, quick-open overlay), dispatch happens exactly where the registration says; one named table of documented gates
- [ ] 3.3 Fix or file every mismatch the matrix finds

## 4. QA driver

- [ ] 4.1 Finish `tools/qa/microide_qa.py` (App lifecycle, input, control, `ui` locators, oracles, screenshots, action record/replay) against the real `ui` query
- [ ] 4.2 `tools/qa/scenarios.py`: menus and rows, overlays, sidebar views, tabs/panel tabs/status segments, divider drags, split/close, terminal panel, scrolling -- each under editor, tree and terminal focus
- [ ] 4.3 `tools/qa/run.py` CLI: `suite`, `monkey --seed --steps`, `replay <file>`, `remote`; non-zero exit on any finding; findings summary with screenshots and replay file
- [ ] 4.4 Monkey: weighted actions per design; saves the action list on a finding; verify a saved list replays the finding
- [ ] 4.5 `tools/qa/README.md`: setup, running, reading findings, adding a scenario

## 5. Remote scenarios

- [ ] 5.1 `tools/qa/agents.py` (simulated agents: bursts, churn with commits, reset) copied to the host per run
- [ ] 5.2 `tools/qa/remote.py`: open host folder and wait for the mirror; host edits under open/unopened tabs; save conflict and resolve; drop and restore the link; host terminal typing; agent churn with the Edit menu, search view and a split open; mirror byte-identical at the end
- [ ] 5.3 Run the remote suite against worky and fix or file every finding

## 6. Lane and wrap-up

- [ ] 6.1 `tools/run-checks.sh qa`: build, suite, 300-action monkey (seed 1), `/tmp/microide-qa.log`, fail on finding or missing Xvfb/xdotool/xwd
- [ ] 6.2 Run the full suite, the monkey with several seeds, and the remote suite; every fixed finding lands with a failing-before unit test and its own commit; unfixed ones filed as TD
- [ ] 6.3 Update `dev-docs/project/validation-traps.md` (the verifier as the stale-paint oracle) and CLAUDE.md's validation list with the `qa` lane
- [ ] 6.4 Run the check lanes (tests, perf-tests, clang-build, asan, ubsan, tsan) green
