# Tasks

## 0. Baseline and instrumentation

- [ ] 0.1 Record base numbers: `tools/run-checks.sh perf-tests` and the perf harness paint scenarios (`editor_typing_paint`, caret blink, `scroll_large_file`, a 2560x1440 variant), interleaved base runs on the main checkout's disk; save the table in this change's `notes.md`. Verify: the table exists with p50/p95 and allocation counts per scenario.
- [ ] 0.2 Add perf counters for units fingerprinted and units damaged by derivation per frame, reported by the perf harness. Verify: counters show 0 on base scenarios and appear in the harness output.

## 1. Framework and whole-surface pilot

- [ ] 1.1 Add the derived-damage unit table to frame prep (per unit: last painted fingerprint and rect; damage old and new rect on change). Verify: a unit test drives prep with a changed fingerprint and sees exactly that unit damaged, and none when unchanged.
- [ ] 1.2 Migrate the status bar and the breadcrumb row: their view models become complete snapshots, fingerprinted by the builder; delete the `painted_status_items` / `painted_breadcrumb_labels` comparisons they subsume. Verify: `RetainedRedraw` tests that change status-bar and breadcrumb state with no damage request stay fresh; QA lane green.
- [ ] 1.3 Field-completeness test per migrated view model (every field changes the fingerprint). Verify: the test fails when a field is left out of the hash (negative control recorded).
- [ ] 1.4 Extend the render-surface lint so migrated TUs read only their view-model parameters and immutable resources. Verify: fixtures both ways in `ArchitectureRuleFixtures.cpp`; a seeded violation fails the lint.
- [ ] 1.5 Perf A/B for stage 1. Verify: within budget (no p50 > +5%, no new steady-state allocations), numbers appended to `notes.md`.

## 2. Editor rows

- [ ] 2.1 Spike: build and fingerprint every visible row's paint inputs at prep behind a debug toggle; measure base vs spike per 0.1. Verify: numbers in `notes.md` and the D5 decision (eager records or lazy fallback) recorded.
- [ ] 2.2 Introduce per-row paint records built at prep (reused scratch, no steady-state allocation) per the decision. Verify: `perf-tests` allocation counters unchanged; unit tests compare records with what paint drew.
- [ ] 2.3 Switch editor paint to draw rows from the records; remove input gathering from `EditorViewRenderer::Render`. Verify: redraw tests and the QA lane green; editor paint tests unchanged.
- [ ] 2.4 Derive editor-row damage from row fingerprints (caret kept as its own unit); delete the editor `painted_*` comparisons subsumed (occurrences, bracket pair, fold markers, language, decorations, pane content/scroll where covered). Verify: a test changing each former input with no damage request stays fresh; soak editorSurface `outside` sightings tracked for a week drop.
- [ ] 2.5 Same for compare and merge panes. Verify: compare/merge redraw tests green; `painted_compare_*` fields removed where covered.
- [ ] 2.6 Perf A/B for stage 2. Verify: within budget, numbers in `notes.md`.

## 3. Bottom panel and right pane

- [ ] 3.1 Debug-pane rows, panel tab row, output panel and terminal chrome as derived units. Verify: redraw tests per surface; QA lane green.
- [ ] 3.2 Perf A/B for stage 3. Verify: within budget.

## 4. Tab strips and sidebar

- [ ] 4.1 Editor tab strips (per tab), project strip, sidebar visible rows as derived units; retire `painted_editor_tab_labels`, `painted_project_strip_version`, `painted_sidebar_selection`. Verify: redraw tests; QA lane green.
- [ ] 4.2 Perf A/B for stage 4. Verify: within budget.

## 5. Retire and document

- [ ] 5.1 Remove every remaining `PreparedFrameState` comparison a snapshot subsumes; keep the ones that are frame decisions (caret phase, pointer occluder, pane metrics). Verify: the struct shrinks; `tests` and `qa` green.
- [ ] 5.2 Update `dev-docs/project/architecture-invariants.md`, `guidelines/ui-shell.md` and the CLAUDE.md invariants list with the derived-damage rule and its lint. Verify: docs name the rule, the lint and the bug class it closes.

## Workflow follow-up

- After a week of soak on the final stage: compare stale-paint `outside` sightings per day against the 0.1-era rate, then archive this change (`openspec archive derived-damage`) and validate specs.
- Open a separate change for clip-edge / in-clip stale paint (clip discipline).
