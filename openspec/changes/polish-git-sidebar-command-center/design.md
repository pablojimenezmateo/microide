## Context

The Git sidebar already exposes compare, merge, stage, unstage, discard, outgoing-file views, bulk stage-all, and confirmed discard-all. The current roadmap asks for a calmer, safer workflow where every row has predictable actions and destructive operations show what will be lost.

This change is UI and command routing work, not a Git parser rewrite. It should consume repository snapshots and publish action requests through services instead of making the sidebar a second Git owner.

## Goals / Non-Goals

**Goals:**
- Make section grouping and row actions deterministic across keyboard and mouse input.
- Surface branch/upstream, ahead/behind, refresh, stale, and failed states.
- Add preview-first flows for discard file/hunk-compatible future actions.
- Keep render translation units view-model driven and allocation-free in steady state.
- Add focused interaction tests for keyboard actions and confirmation behavior.

**Non-Goals:**
- No hunk or line staging implementation.
- No commit pane implementation beyond opening the staged commit flow.
- No hosted-provider review, pull-request, or network state.
- No plugin-owned sidebar replacement.

## Decisions

- The sidebar groups rows by action semantics: Conflicts, Staged, Changed, Untracked, Outgoing. This reduces ambiguity around what `s`, `u`, `x`, and `m` do on a selected row.
- Row actions are represented as command IDs with typed payloads. Keyboard, mouse, context-menu, and command-prompt paths dispatch the same action rather than duplicating behavior.
- Destructive operations open a preview/confirmation prompt before applying. File discard previews the diff or a summary when a diff is unavailable; future hunk discard can reuse the same prompt shape.
- View models own display strings and enabled/disabled action flags. Render code paints rows only and does not inspect Git state.

## Risks / Trade-offs

- Reworking sidebar grouping can disrupt existing tests -> update tests around stable sections and action IDs rather than incidental row indexes.
- Previewing discard may require extra diff work -> show a loading state and dispatch diff generation asynchronously if the preview is not already available.
- Keyboard shortcuts can conflict with existing sidebar behavior -> keep the action grammar local to the Git sidebar and document it in Help/About or command labels.

## Migration Plan

1. Introduce typed Git sidebar section and row view-models.
2. Route current actions through row action descriptors without changing behavior.
3. Add stale/refresh/error state banners.
4. Add destructive preview prompts and confirmation tests.
5. Update docs/help text once interaction grammar is stable.
