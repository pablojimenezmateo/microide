# Tasks

## 0. Baseline and instrumentation

- [x] 0.1 Record base numbers: `tools/run-checks.sh perf-tests` and the perf harness paint scenarios (`editor_typing_paint`, caret blink, `scroll_large_file`, a 2560x1440 variant), interleaved base runs on the main checkout's disk; save the table in this change's `notes.md`. Verify: the table exists with p50/p95 and allocation counts per scenario.
- [x] 0.2 Add perf counters for units fingerprinted and units damaged by derivation per frame, reported by the perf harness. Verify: counters show 0 on base scenarios and appear in the harness output.

## 1. Framework and whole-surface pilot

- [x] 1.1 Add the derived-damage unit table to frame prep (per unit: last painted fingerprint and rect; damage old and new rect on change). Verify: a unit test drives prep with a changed fingerprint and sees exactly that unit damaged, and none when unchanged.
- [x] 1.2 Migrate the status bar and the breadcrumb row: their view models become complete snapshots, fingerprinted by the builder; delete the `painted_status_items` / `painted_breadcrumb_labels` comparisons they subsume. Verify: `RetainedRedraw` tests that change status-bar and breadcrumb state with no damage request stay fresh; QA lane green. (Done: StatusBar is a derived surface; each group's breadcrumb band is a Chrome unit keyed on its label, the status items and the focus.)
- [x] 1.3 Field-completeness test per migrated view model (every field changes the fingerprint). Verify: the test fails when a field is left out of the hash (negative control recorded).
- [x] 1.4 Extend the render-surface lint so migrated TUs read only their view-model parameters and immutable resources. Verify: fixtures both ways in `ArchitectureRuleFixtures.cpp`; a seeded violation fails the lint.
- [x] 1.5 Perf A/B for stage 1. Verify: within budget (no p50 > +5%, no new steady-state allocations), numbers appended to `notes.md`.

## 2. Editor rows

- [x] 2.1 Spike: build and fingerprint every visible row's paint inputs at prep behind a debug toggle; measure base vs spike per 0.1. Verify: numbers in `notes.md` and the D5 decision (eager records or lazy fallback) recorded.
- [x] 2.2 (step 1 done: rows are derived units keyed on everything but syntax colours, added to the hand-written damage; A/B in notes.md. Step 2: syntax state at each row's start, then the differential property test) Introduce per-row paint keys folded at prep (lazy fallback, per notes.md: no allocation, no tokenizing) and the differential property test (random state transitions, damage only from row keys, partial vs full render). Verify: `perf-tests` allocation counters unchanged; unit tests compare records with what paint drew. (Done: syntax colours in the key, 2.2 step 2; EditorRowDamageFollowsEveryStateTransition.)
- [x] 2.3 Switch editor paint to draw rows from the records; remove input gathering from `EditorViewRenderer::Render`. Verify: redraw tests and the QA lane green; editor paint tests unchanged. (Done: PreparedFrameState::EditorPanePaint feeds PaintEditorPane, 4e7cdf82e.)
- [x] 2.4 Derive editor-row damage from row fingerprints (caret kept as its own unit); delete the editor `painted_*` comparisons subsumed (occurrences, bracket pair, fold markers, language, decorations, pane content/scroll where covered). Verify: a test changing each former input with no damage request stays fresh; soak editorSurface `outside` sightings tracked for a week drop. (Done: the editor's hand-written damage retired, 3efe79878, 95dadf6db.)
- [x] 2.5 Same for compare and merge panes. Verify: compare/merge redraw tests green; `painted_compare_*` fields removed where covered. (Done: CompareRowUnits / merge rows, DiffSurfacePaint, b3ce1167a; diagnostics and decorations keyed, 3f2984473.)
- [x] 2.6 Perf A/B for stage 2. Verify: within budget, numbers in `notes.md`. (2026-11-23: within budget after a harness fix and a long-line key fix; typing paint -70.8%.)

## 3. Bottom panel and right pane

- [x] 3.1 Debug-pane rows, panel tab row, output panel and terminal chrome as derived units. Verify: redraw tests per surface; QA lane green.
  - Bottom panel header (tab row, chevrons, label, new-terminal and maximize buttons): done with clip-discipline stage 3 (the header commit); `painted_bottom_panel_tabs` retired with it.
  - Output body (rows, empty hint, scrollbar): done. A row is named by its entry's serial (`WorkspaceOutputChannels::FirstEntrySerial` + index; entries are immutable), not by hashing its text, which can be 1 MiB; the scroll settles in prep, no longer in paint; `painted_output_revision` retired (a line into a hidden channel now repaints nothing).
  - Terminal chrome (each pane's hint and scrollbar, the pane dividers, the no-session hint): done. The pane divider's drag-press damage (`DragTargetHighlightRect`) retired; the panel scrollbar's stays for the plugin preview.
  - Terminal grid: a unit per visible row of each pane, plus the pane's body (`BottomPanelTerminalGridPaint.h`, D3); paint is `PaintTerminalGridPane`, from the prepared rows only, and skips rows the clip cannot reach. A new snapshot or a selection change no longer repaints the pane, and the hovered link has damage (it had none once "the panel changed" became a frame). The terminal's find bar is a Chrome unit keyed on all it paints. `RequestBottomPanelContentRedraw` is a frame for everything but a plugin's surface.
  - Panel divider: a unit of the header, drawn last; it had been painted over by the header's fill since the base commit. Its drag-press damage retired.
  - Notification toasts: done (`NotificationToastPaint.cpp`; the view model is built once at prep, not in paint). Hover damage narrowed from the window to the toast stack. `Notify`'s full redraw stays: a derived surface needs a frame to run in, and a present-only request does not prepare one -- a "frame, no damage" request would let it go (open question).
  - Bottom-panel half: done, its perf A/B within budget (notes.md, 2026-11-22).
  - Debug pane: done (mode tabs, rows as band + text runs with the inline value editor in its own slot, hint, scrollbar; the scroll settles in prep; row text sits on its band). `DragTargetHighlightRect`'s DebugPaneScrollbar damage retired. Its perf A/B: within budget (notes.md, "Stage 3 perf A/B, the debug pane").
- [x] 3.2 Perf A/B for stage 3. Verify: within budget.
  - Bottom panel: done (notes.md, "Stage 3 perf A/B, the bottom panel"); the debug pane's to follow.

## 4. Tab strips and sidebar

- [x] 4.1 Editor tab strips (per tab), project strip, sidebar visible rows as derived units; retire `painted_editor_tab_labels`, `painted_project_strip_version`, `painted_sidebar_selection`. Verify: redraw tests; QA lane green. (Done, with one deviation: an editor group's strip is ONE unit keyed on every tab it paints (f67154fdc), not a unit per tab; the project strip keys its source version, scroll and active project. The file tree, the sidebar mode row and the git view are derived surfaces (3191a943e, 7f0120f99, df6ad76d5). Open menus too (47fc57f16).)
- [x] 4.2 Perf A/B for stage 4. Verify: within budget. (Done in the 2.6 A/B of 6edafb798, which carried the strips, tree, git view and menus: git and menu scenarios within budget.)

## 5. Retire and document

- [x] 5.1 Remove every remaining `PreparedFrameState` comparison a snapshot subsumes; keep the ones that are frame decisions (caret phase, pointer occluder, pane metrics). Verify: the struct shrinks; `tests` and `qa` green. (Done: no `painted_*` field is left on PreparedFrameState.)
- [ ] 5.3 What is left of the hand-written system, outside that struct: the terminal pane's caret comparison (`TerminalPaneState::painted_caret_*`, WorkspaceShellTerminal.cpp) as units of the terminal chrome surface (the selection comparison went with the grid's row units); `RequestBottomPanelRedraw` and `RequestSidebarRedraw` callers for the views not derived yet; the overlays and prompts. (The Settings / Help card is derived: `SettingsCardPaint.h`, a unit for its backdrop, its chrome, the scope tooltip, the font picker and each rail and value row; `RequestOverlayRedraw` is a frame while it is open.) Verify: each converted site has a no-request test; the remaining rect requests each say why.
- [ ] 5.4 QA and soak coverage of derived damage beyond the redraw verifier: property tests like EditorRowDamageFollowsEveryStateTransition for the chrome, sidebar and menu surfaces; a soak counter for over-damage (a partial frame repainting most of a derived surface for a one-unit change) so a key that changes every frame shows up as a finding, not only as a perf number.
- [x] 5.2 Update `dev-docs/project/architecture-invariants.md`, `guidelines/ui-shell.md` and the CLAUDE.md invariants list with the derived-damage rule and its lint. Verify: docs name the rule, the lint and the bug class it closes. (Done: f555911bf.)

## Workflow follow-up

- After a week of soak on the final stage: compare stale-paint `outside` sightings per day against the 0.1-era rate, then archive this change (`openspec archive derived-damage`) and validate specs.
- Open a separate change for clip-edge / in-clip stale paint (clip discipline).
