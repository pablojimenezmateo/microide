## Context

The current spec requires three-way merge tabs with incoming/result/current panes, per-hunk picks, whole-side apply, overview lane, and open-result action. The roadmap asks to make merge resolution the crown jewel: clear labels, conflict-specific UI, required accept operations, and validation before marking resolved.

Correctness beats compatibility here. If current merge tab state is too flat to represent conflict classes or validation results, the model should be refactored rather than patched locally.

## Dependencies

- Depends on: `establish-git-repository-state-service`, `expand-diff-review-workflows`, and `handle-external-repo-changes`.
- Consumed by: `prepare-git-workstation-preview`.
- Unblocks: trustworthy conflict resolution and validated mark-resolved behavior.

## Goals / Non-Goals

**Goals:**
- Represent conflict classes explicitly in the merge model.
- Render current/result/incoming/base context with impossible-to-miss labels.
- Add required per-conflict actions and remaining-conflict progress state.
- Validate result files before mark-resolved and explain failures.
- Add regression fixtures for common and edge conflict classes.

**Non-Goals:**
- No full binary merge editor.
- No rebase/cherry-pick sequencer UI beyond recognizing ongoing operation state.
- No hosted provider conflict workflows.
- No file-watch implementation beyond validation seams; external-change plumbing is separate.

## Decisions

- Conflict taxonomy belongs in merge model construction, not view rendering. Render/view-model code consumes conflict class and action availability.
- Merge conflict classification MAY combine repository snapshot data, unmerged index entries, name-status or diff metadata, file-existence checks, and conflict-marker or content inspection rather than relying on one status line alone.
- Result validation is a separate service/helper invoked before mark-resolved. It checks file content, save state, expected existence, index conflict generation, and line-ending policy.
- Base pane is collapsible but always available. Users should not need to reopen a tab to understand why both sides changed.
- Raw conflict markers are a diagnostic view/action, not the default model for resolution.

## Risks / Trade-offs

- Some Git conflict classes are hard to synthesize in tests -> build fixture helpers that create real repositories and conflict states.
- Validation can block valid advanced workflows -> allow explicit override for conflict markers, but require an additional confirmation and record the status message.
- Adding base pane can pressure layout -> use existing pane minimum calculations and responsive layout rules.

## Migration Plan

1. Extend merge model with conflict classes and action availability.
2. Add labels/progress/result-state view-model fields.
3. Implement validation helper and mark-resolved gate.
4. Add per-class fixtures and update existing merge tests.
5. Add perf coverage for many conflicts and interleaved accept/edit/scroll paths.
