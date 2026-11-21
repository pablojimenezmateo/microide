# Tasks

## 0. Measurement and baseline

- [x] 0.1 Tally open clip-edge and in-clip signatures by surface, element and scale from the ledger; classify a sample's causes. Verify: the tables and the sample are in `notes.md`.
- [ ] 0.2 Classify 20 more sightings from their retained/reference crops (the crop script in `notes.md`), widening the sample past twelve. Verify: each has a cause from the four in `proposal.md` or a new one recorded.
- [ ] 0.3 Record base perf numbers (`perf-tests` and the paint scenarios, interleaved on one disk), as `derived-damage` 0.1 did. Verify: the table is in `notes.md`.

## 1. Framework and pilot (tab strips)

- [x] 1.1 `SnapToDevicePixels` in `src/render/ScopedRenderClip.h`; clips, damage requests and scoped fills use it. Verify: unit test -- at scales 1, 1.25, 1.5, 1.75, 2 and the soak's 0.75/3, a damage rect and the fill of the same logical rect cover the same device pixels.
- [x] 1.2 `PaintElement` scope (clip, opaque fill, overrun recorder behind the verifier flag) and the recorder in `RetainedRedraw`. Verify: a test element drawing one pixel past its rect fails `PaintAndDiffWithFullRepaint` naming the element (negative control recorded).
- [ ] 1.3 Migrate the editor and project tab strips: each tab, its separator and the overflow chips are elements tiling the strip; labels fitted before drawing. Verify: the tab-gap regression (053365e38e57, dfaecf57f6db) as a failing-first `RetainedRedraw` test; a tiling assertion in the strip layout test.
- [ ] 1.4 Lint: draw calls in the migrated TU only inside a `PaintElement` scope. Verify: fixtures both ways in `ArchitectureRuleFixtures.cpp`; `RequireRuleTarget` on the TU.
- [x] 1.5 `tools/qa`: an overrun line becomes a `paint-overrun` finding with the element. Verify: a test in `test_findings.py`.
- [ ] 1.6 Perf A/B for stage 1; the per-row clip cost spike (Open Question 1). Verify: numbers in `notes.md`, the D1 fallback decision recorded.

## 2. Editor surface (after derived-damage stage 2)

- [ ] 2.1 Row, gutter, caret, scrollbar and divider elements on derived-damage's row records; wide glyphs widen their cell (D3). Verify: the terminal/editor wide-glyph and divider regressions as failing-first tests; QA lane green.
- [ ] 2.2 Perf A/B. Verify: within budget.

## 3. Bottom panel

- [ ] 3.1 Panel tab row, terminal cells (a wide glyph's cell and cursor), output rows, notifications, the panel divider. Verify: bfa3565938ee's shape as a failing-first test; QA lane green.
- [ ] 3.2 Perf A/B. Verify: within budget.

## 4. Sidebar and right pane

- [ ] 4.1 Sidebar rows (git panel messages opaque: eab90dac1acf, 0cb638ffcb39), debug-pane tabs and rows. Verify: failing-first tests for the git-panel overlap; QA lane green.
- [ ] 4.2 Perf A/B. Verify: within budget.

## 5. The rest, and docs

- [ ] 5.1 Breadcrumb, overlays, prompts, settings, menus. Verify: the lint covers every render TU.
- [ ] 5.2 `dev-docs/project/architecture-invariants.md`, `guidelines/ui-shell.md` and the CLAUDE.md invariants list name the rule, the recorder and the lint. Verify: docs updated.

## Workflow follow-up

- After a week of soak on the final stage: clip-edge and in-clip sightings per day against `notes.md`'s baseline, then `openspec archive clip-discipline`.
