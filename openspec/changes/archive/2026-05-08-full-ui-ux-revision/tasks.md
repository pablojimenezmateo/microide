## 1. UX Audit And Acceptance Baseline

- [x] 1.1 Inventory all host-owned shell interaction paths (status bar segments, View menu mode controls, settings overlay helper text) and map current labels, click targets, and hidden behaviors.
- [x] 1.2 Define explicit acceptance criteria for discoverability and naming consistency, including branch cleanliness visibility, state-first control labels, and sidebar header row cohesion.
- [x] 1.3 Add/adjust regression test expectations for status-bar segment composition, menu label semantics, settings copy taxonomy, and panel-header row structure consistency.

## 2. Status Bar Discoverability Rework

- [x] 2.1 Update status-bar view-model construction to include branch identity plus clean/dirty working-tree state in visible source-control segment text.
- [x] 2.2 Refine status-bar segment metadata so click-routable segments expose clear destination intent in label and tooltip copy.
- [x] 2.3 Validate compact-mode drop behavior keeps source-control state, line/column, diagnostics, and LSP state visible per spec.

## 3. Layout Mode Control Semantics

- [x] 3.1 Replace verb-oriented compact-mode menu wording with state-oriented `Compact mode` semantics and checked-state rendering.
- [x] 3.2 Ensure layout-mode persistence and `LayoutModeService` observability stay unchanged while UI labels and menu presentation are updated.
- [x] 3.3 Verify status-surface and menu representations of compact mode remain consistent across auto/regular/compact configurations.

## 4. Settings Copy Taxonomy Cleanup

- [x] 4.1 Audit settings overlay rows for mislabeled helper copy (for example behavior text tagged as `Tip`) and update text taxonomy to neutral description/`Note`/`Tip` as appropriate.
- [x] 4.2 Update settings overlay view-model or rendering helpers to enforce semantic label usage for helper text categories.
- [x] 4.3 Add focused tests confirming baseline behavior explanations are not labeled `Tip` and optional advice retains `Tip`.

## 5. End-To-End Validation

- [x] 5.1 Run targeted workspace UI tests covering status-bar routing, responsive layout mode controls, and settings overlay content.
- [x] 5.2 Perform manual UX verification in regular and compact modes to confirm discoverability improvements in all revised app paths.
- [x] 5.3 Update `dev-docs/project/active-work.md` if the shipped UX baseline or rollout priorities change after implementation.

## 6. Sidebar Header Cohesion Pass

- [x] 6.1 Update the Project sidebar header layout so selector/title remains on the first row and action buttons render on a dedicated second row, matching Source Control structure.
- [x] 6.2 Keep Project action set and order stable after layout change, and ensure button hit targets and spacing match Source Control header conventions.
- [x] 6.3 Add focused UI regression coverage for Project/Source Control header-row parity across relevant layout modes.
