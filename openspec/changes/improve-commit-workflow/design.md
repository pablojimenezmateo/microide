## Context

The current Git sidebar supports staging and committing at a basic level. The roadmap asks to treat commit creation as a review completion flow: staged summary beside message entry, checks before execution, hook output, draft persistence, and conservative amend/no-hook flows.

This change should reuse existing prompt/text input primitives and background subprocess seams. It must not add synchronous Git commit waits to workspace input paths.

## Goals / Non-Goals

**Goals:**
- Build a host-owned commit workflow surface using staged snapshot data.
- Persist draft subject/body per repository.
- Run pre-commit checks and show warnings before execution.
- Execute commit operations asynchronously and display hook output.
- Refresh repository state after commit success/failure.

**Non-Goals:**
- No rebase sequencer UI.
- No hosted provider push/PR flow.
- No commit signing/key management UI beyond surfacing Git failure output.
- No AI commit message generation.

## Decisions

- Commit draft state is repository-scoped and persisted through `PersistenceService`. Drafts are cleared only after a successful commit for the same repository generation.
- Pre-commit checks are structured. Blocking checks prevent commit; warning checks require acknowledgement.
- Hook output is captured as operation output and shown in the bottom panel or a dedicated native output surface, not only as raw stderr in a prompt.
- Amend and commit-without-hooks require explicit confirmation because they can rewrite history or bypass user safeguards.

## Risks / Trade-offs

- Hooks can run for a long time or prompt unexpectedly -> execute asynchronously, stream/capture output, and keep UI responsive with cancellation where Git supports it.
- Draft persistence can accidentally reuse stale text across branches -> key drafts by repository and branch/head context, and show when a draft was restored.
- Warning overload can desensitize users -> keep blocking/warning categories limited and actionable.

## Migration Plan

1. Add commit workflow state and view models.
2. Add draft persistence.
3. Add pre-commit check engine.
4. Add async commit/amend/no-hook execution and hook output panel.
5. Wire Git sidebar commit action to open the workflow.
