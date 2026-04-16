# Diff, Editor, and Merge Rewrite Plan

Reviewed on 2026-04-16.

Progress note on 2026-04-16:

- compare and merge source panes now use a shared decorated text-grid renderer for row fills,
  syntax runs, and underlines
- compare no longer degrades modified-row changed spans into coarse full-line spans just because a
  diff is large
- editor and merge result now build row fills and syntax runs through the same decorated text-grid
  renderer
- `TextViewport` no longer disables syntax highlighting just because a buffer crossed the large-file
  thresholds
- editor breadcrumbs and inline blame no longer special-case large buffers
- profiling now covers the rewritten diff pipeline via `microide_diff_bench` stage timings and
  live resize or redraw tracing via `MICROIDE_PERF_TRACE`

## Scope

This plan covers a deliberate rewrite of the text presentation pipeline used by:

- `src/compare/CompareModel.cpp`
- `src/editor/TextViewport.cpp`
- `src/editor/EditorViewRenderer.cpp`
- `src/workspace/WorkspaceShellCompareRender.cpp`
- `src/workspace/WorkspaceShellMergeRender.cpp`
- the compare and merge setup paths in `src/workspace/WorkspaceShellCompare.cpp`

The goal is not to patch the current behavior. The goal is to replace the current mixed rendering
and fallback model with one that is correct first, then profiled, then optimized.

## Problem Statement

The current implementation mixes three separate concerns:

1. full-line semantic state
2. intraline changed-range state
3. size-based fallback behavior

That mixing is why the visuals are inconsistent:

- compare rows currently tint only changed spans, not the whole changed row
- underline quality depends on diff size because the diff model changes behavior once row or byte
  thresholds are crossed
- editor, compare, and merge do not share one decoration model, so each surface makes different
  tradeoffs

The user-visible regressions match the current code:

- `WorkspaceShellCompareRender.cpp` paints changed-span backgrounds inside
  `draw_syntax_text(...)`, but the row background stays `theme_.editor_background`
- `CompareModel.cpp` switches to coarse intraline spans for large diffs via
  `kMaxDetailedChangedSpanRows` and `kMaxDetailedChangedSpanBytes`
- `CompareModel.cpp` also changes line alignment behavior when matrix-size thresholds are crossed
- `TextViewport.cpp` disables editor syntax highlighting in large-file mode via
  `kLargeFileByteThreshold` and `kLargeFileLineThreshold`
- merge source panes and the merge result pane do not follow one shared rendering contract

## Decision

We should remove the current big/small quality branches for diff, editor, and merge, implement the
correct behavior everywhere, then profile the result and add targeted optimizations that preserve
the same output.

This rewrite is allowed to be large and compatibility-breaking. Cleanliness, clarity, and stable
rendering semantics matter more than preserving the current layering.

## Target Behavior

For every surface:

- line highlight marks the whole logical row
- changed-range underline marks only the changed ranges
- syntax coloring is orthogonal to both of the above
- selection, caret, blame, search, and conflict state are layered explicitly instead of being
  implicit side effects of draw order
- output quality is independent of file size

For diff specifically:

- row-level add/delete/modify state paints the full row background
- intraline change detection remains precise for small and large diffs alike
- alignment quality does not silently degrade because a matrix-size threshold was crossed

For editor specifically:

- normal editor rendering and large-buffer rendering use one rendering contract
- any later “large file” mode can reduce scheduling cost, caching scope, or background work, but
  must not silently disable syntax or change visual semantics

For merge specifically:

- incoming, result, and current panes use the same row/decorations vocabulary
- merge conflict visuals are layered on top of the shared row model instead of using a separate
  ad-hoc path

## Current Issues To Delete

### 1. Compare row background is implemented at span level

Current state:

- modified rows compute `left_changed_background` and `right_changed_background`
- those backgrounds are painted only for changed spans
- unchanged prefixes and suffixes inside modified rows remain on the plain editor background

Effect:

- the underline can be correct while the full-row changed highlight is wrong
- screenshot 1 is a direct consequence of this design

Rewrite direction:

- row fill must be decided before text rendering
- changed spans should only add an underline or an optional tighter emphasis layer

### 2. Diff quality depends on size heuristics

Current state in `CompareModel.cpp`:

- `kMaxLineLcsMatrixCells`
- `kMaxHunkAlignmentMatrixCells`
- `kMaxIntralineLcsMatrixCells`
- `kMaxDetailedChangedSpanRows`
- `kMaxDetailedChangedSpanBytes`

Effect:

- large diffs do not just run slower; they use a different algorithm and different span quality
- screenshot 2 is consistent with the coarse-span fallback path

Rewrite direction:

- correctness decisions must not depend on document size
- performance controls must become execution-policy decisions, not output-quality decisions

### 3. Editor large-file mode changes functionality, not just cost

Current state in `TextViewport.cpp` and `TextViewport.h`:

- `large_file_mode` is computed from byte and line thresholds
- `syntax_highlighting_enabled()` returns false in large-file mode

Effect:

- the editor has different semantics for large buffers
- merge result rendering inherits this because it goes through `EditorViewRenderer`

Rewrite direction:

- replace “large-file mode disables features” with “render policy schedules work differently”
- if a buffer needs progressive or windowed tokenization, that is fine, but the final output must be
  the same

### 4. Editor, compare, and merge duplicate text-decoration logic

Current state:

- `EditorViewRenderer.cpp` owns one row rendering model
- `WorkspaceShellCompareRender.cpp` owns a second one
- `WorkspaceShellMergeRender.cpp` owns a third one

Effect:

- behavior drifts
- bug fixes must be reimplemented three times
- layering rules are hard to reason about

Rewrite direction:

- one reusable decorated text-grid renderer should exist
- the three surfaces should supply data, not reimplement paint order

## Proposed Architecture

### 1. Introduce a shared row-decoration model

Create a small renderer-facing model that is independent of editor/compare/merge storage:

- `RenderedRow`
- `RowBackgroundStyle`
- `InlineDecoration`
- `UnderlineDecoration`
- `TextRun`

Each rendered row should answer:

- what fills the row background
- what fills the gutter background
- what inline rectangles exist for selection/search/conflict overlays
- what underlines exist
- what text runs exist after syntax and viewport clipping
- what caret or blame adornments exist

This is the core simplification. Once this exists, editor, compare, and merge become producers of
the same row description.

### 2. Introduce a shared text-grid renderer

Move draw-order and clipping logic into one reusable renderer, for example under `src/editor/` or
`src/workspace/` as a neutral component.

Responsibilities:

- visible column slicing
- row background painting
- gutter painting
- inline overlay painting
- syntax-aware text-run painting
- underline painting
- caret painting
- blame painting

Responsibilities that should stay outside:

- diff computation
- merge conflict tracking
- editor buffer mutation
- tab/session/persistence concerns

### 3. Split diff computation into stable stages

The compare model should stop hiding policy inside a single builder. Separate these stages:

1. line alignment
2. hunk construction
3. row pairing
4. intraline change extraction
5. presentation decorations

Benefits:

- easier profiling
- easier testing of exact failure modes
- easier replacement of one algorithm without touching rendering

### 4. Replace quality fallbacks with explicit execution policies

If optimization is needed later, use policies such as:

- chunked preprocessing
- cached tokenization
- windowed recomputation
- background precomputation
- work-per-frame budgets

Do not use:

- coarse changed spans because the diff is big
- weaker alignment because the matrix is big
- feature disablement because the file is big

The output must remain identical.

## Rewrite Workstreams

### Workstream 1: Rendering contract cleanup

Deliverables:

- new shared decorated-row types
- shared text-grid renderer
- explicit paint order documentation

Rules:

- whole-row fills happen first
- inline fills such as selection/search/conflict happen second
- text is painted after fills
- underline and caret paint last

### Workstream 2: Compare rewrite

Deliverables:

- new compare row presentation builder
- full-row add/delete/modify highlighting
- precise intraline underline for all diff sizes
- removal of coarse span fallback behavior

Expected deletions:

- `coarse_changed_spans`
- `PopulateCoarseChangedSpans(...)`
- threshold-driven intraline degradation

Potential deeper rewrite:

- replace current thresholded alignment fallback path with a more scalable exact-enough diff
  strategy that does not degrade output
- if needed, prefer a better global diff algorithm over special-casing “large”

### Workstream 3: Editor rewrite

Deliverables:

- `TextViewport` stops using `large_file_mode` as a feature gate
- syntax/highlight computation becomes progressive or cached, but semantically stable
- `EditorViewRenderer` becomes a thin adapter over the shared renderer

Expected deletions:

- `syntax_highlighting_enabled()` meaning “disabled because file is large”
- threshold-branded breadcrumb behavior tied to disabled rendering semantics

Possible replacement:

- a neutral status such as “background tokenization incomplete” if we need observability during
  progressive work

### Workstream 4: Merge rewrite

Deliverables:

- incoming/result/current panes all produced through the same row-decoration pipeline
- merge conflict highlighting becomes one decoration layer, not a special renderer
- result pane no longer inherits editor large-file feature disablement

Expected simplification:

- merge source panes and compare panes should share most of their rendering stack
- merge result pane should share the editor stack
- conflict borders and action affordances should sit above the shared text-grid output

### Workstream 5: Performance pass after correctness

Only after the rewrite is correct:

- measure build-diff time
- measure intraline extraction time
- measure syntax-token generation time
- measure row-paint time
- measure cache hit rates

Then optimize the hottest stable path without changing output.

## Testing Plan

### Model-level tests

Expand `tests/CompareModelTests.cpp` to cover:

- whole-line highlight semantics separately from intraline spans
- large synthetic diffs that previously crossed the coarse-span thresholds
- repeated-structure alignment cases
- long single-line edits
- import expansion and shared-prefix cases like the current regressions

Add new guarantees:

- identical changed spans for the same edit regardless of file size padding around it
- unchanged shared prefixes and suffixes remain unchanged in both small and large fixtures

### Renderer-level tests

Add pixel or structural tests for:

- modified compare row paints the full row
- underline paints only the changed ranges
- selection and changed underline compose correctly
- merge source and result panes use consistent row semantics
- editor and compare selected-row behavior match

### Regression fixtures

Add fixtures modeled after the screenshots:

- small-file import expansion case
- large-file padded import expansion case
- long shared-prefix assignment case

The same semantic edit should render the same underline ranges in both the small and padded-large
fixtures.

## Profiling Plan

Profile only after the rewrite is in place.

Use:

- `microide_diff_bench` as a starting point, but update it so it reflects the rewritten pipeline
- targeted timers around diff stages, not only one aggregate diff-build time
- retained-render tracing and any existing redraw tracing from `docs/startup-tracing.md` and
  `docs/performance-findings.md`

We should also fix the current conceptual mismatch where the benchmark still models a compare-tab
syntax cutoff even though the rewrite will intentionally remove quality cutoffs.

## Migration Order

1. Add the shared row-decoration types and renderer without changing behavior.
2. Port editor rendering to the shared renderer.
3. Port compare rendering to the shared renderer and switch row highlight to full-line semantics.
4. Rewrite compare intraline extraction to remove coarse large-diff fallbacks.
5. Port merge source and result panes to the same decoration pipeline.
6. Delete large-file feature gating and old duplicated rendering paths.
7. Profile and optimize.

This order keeps the rendering contract stable before we replace the diff engine details.

## Deliberate Non-Goals

Not part of this rewrite:

- preserving session or serialized compatibility if it blocks cleanup
- preserving old “large file mode” UI wording
- micro-optimizing before the new pipeline exists
- keeping current helper boundaries if they fight the new design

## Acceptance Criteria

The rewrite is done when all of the following are true:

- modified diff rows paint the full row consistently
- underlines only cover changed ranges
- small and large files produce the same diff semantics for the same edit
- editor, compare, and merge share one row-decoration renderer
- no user-visible rendering semantics depend on file-size thresholds
- performance is measured after the rewrite, then optimized without changing output

## Recommended Starting Point

Start with the rendering contract, not the diff algorithm.

Reason:

- the full-line highlight bug is fundamentally a rendering-model bug
- the large-file underline bug is partly a diff-model bug, but it is easier to verify once row and
  decoration layering are explicit and shared
- once one renderer exists, compare and merge cleanup stop being three separate rewrites
