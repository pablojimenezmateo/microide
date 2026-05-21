## Context

The current spec already requires working-tree, arbitrary commit, base-branch, and commit-versus-commit compare tabs with hunk navigation and a change-overview lane. The roadmap asks to elevate compare into a branch-review workstation with more explicit modes and higher-quality diff presentation.

The design must preserve the shared decorated text-grid pipeline and avoid making render code understand Git semantics. File/hunk metadata belongs in model/build steps; paint paths consume presentation rows and decorations.

## Goals / Non-Goals

**Goals:**
- Define explicit compare modes for working tree, commit, branch, and conflict review.
- Split `GitDiffModel`-style semantic data from `DiffPresentationModel`-style visible rows and collapsed regions.
- Add inline word diff, whitespace toggles, context expansion/collapse, metadata rows, and stable file/line mapping.
- Keep large diffs correct while showing progress or degraded interactivity only where necessary.

**Non-Goals:**
- No hunk/line stage or discard operations.
- No persistent reviewed-file state.
- No hosted provider review comments.
- No change to merge result editing behavior.

## Decisions

- Diff mode is explicit in tab state and view models. This prevents branch review, commit review, and working-tree review from being inferred from labels or ad hoc refs.
- Semantic diff data is service/model-owned; presentation data owns collapse state, visible rows, inline highlight ranges, scroll markers, and selection mapping.
- Whitespace ignoring affects diff computation through a clear option and invalidates the semantic model generation. Whitespace visualization affects presentation only.
- Large diffs may show progress, partial presentation readiness, or interaction throttling, but they must not truncate hunks or silently switch to an incorrect algorithm.

## Risks / Trade-offs

- Inline word diff can add CPU cost on large changed lines -> compute lazily per hunk or cache by model generation and measure compare scenarios.
- Context collapse adds state that can desync from line mapping -> keep expansion state keyed by hunk identity/range and verify open-at-line behavior after collapse/expand.
- Branch review can overlap with persistent review state -> this change only supplies mode and navigation hooks; local reviewed marks land separately.

## Migration Plan

1. Add compare mode enum and semantic model metadata without changing visible output.
2. Add presentation model state for context collapse and inline highlights.
3. Implement mode-specific headers/actions.
4. Add whitespace controls and metadata summaries.
5. Add tests and perf coverage for large branch and commit review fixtures.
