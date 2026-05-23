## Context

File-level stage/unstage/discard exists today. The roadmap calls for hunk and line operations that preview exactly what will change and fail safely when a file changed underneath. This work sits between diff presentation and Git operation execution, so it needs a service seam rather than embedding patch construction in the sidebar or compare tab.

## Dependencies

- Depends on: `establish-git-repository-state-service` and `expand-diff-review-workflows`.
- Consumed by: `improve-commit-workflow` and preview workflow confidence criteria.
- Unblocks: precise commit composition without terminal fallbacks.

## Goals / Non-Goals

**Goals:**
- Implement hunk and selected-line stage/unstage operations through generated patches.
- Implement hunk and selected-line discard with explicit preview and confirmation.
- Detect stale diff generations before applying a patch.
- Refresh repository snapshots after operation completion.
- Provide structured failure categories for patch did not apply, dirty target, cancelled, and unknown errors.

**Non-Goals:**
- No interactive patch editor beyond selecting hunks/line ranges in compare.
- No commit workflow implementation.
- No binary hunk staging.
- No hosted-provider review operations.

## Decisions

- Patch operations are owned by `PatchApplyService`. Compare and sidebar surfaces produce typed targets; the service builds and applies patches.
- Selected-line staging is computed from semantic hunk ranges plus presentation selection mapping. This avoids coupling patch generation to painted rows.
- Staging and unstaging apply to the index; discard applies to the worktree and requires confirmation. The service never auto-retries after a patch fails due to stale content.
- Operation requests include repository snapshot generation and diff model generation. If either is stale, the service fails with `patch_did_not_apply` or `stale_diff` and offers refresh.
- The service preflights patch applicability (for example `git apply --check` equivalent) before mutation where possible, and SHALL avoid reject-mode partial-apply behavior.

## Risks / Trade-offs

- Selected-line patch generation has edge cases around context lines -> start with conservative ranges and robust failure messages rather than guessing.
- Discard selected lines can be destructive and surprising -> require preview and confirmation for all discard paths.
- Applying patches through Git can be slow on huge diffs -> dispatch off the main thread and add perf gates for large hunk/line operations.

## Migration Plan

1. Add service request/result types and tests for patch construction.
2. Implement hunk stage/unstage first.
3. Add selected-line stage/unstage once line mapping tests are in place.
4. Add discard hunk/line with preview and confirmation.
5. Wire compare/sidebar actions and refresh repository snapshots after operation completion.
