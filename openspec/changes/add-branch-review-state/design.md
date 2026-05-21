## Context

The roadmap calls out local review state as small but powerful. The project already has structured persistence and host-owned workflow surfaces; review state should follow those boundaries and remain local to avoid provider/auth complexity.

This change must not revive old bespoke persistence formats or introduce plugin-owned rendering. Review marks are data used by host view models.

## Goals / Non-Goals

**Goals:**
- Track reviewed files and hunks for branch review.
- Mark files/hunks as changed since reviewed when the diff identity changes.
- Persist review state with typed records through `PersistenceService`.
- Add optional local notes attached to file or hunk identities.
- Keep review markers cheap to render through prebuilt view models.

**Non-Goals:**
- No hosted pull-request review comments.
- No account/auth provider integration.
- No collaborative sync.
- No plugin SCM/review API expansion.

## Decisions

- Review state is project-scoped and repository-root keyed. This keeps it close to branch/session data and avoids leaking state between unrelated repos.
- Hunk identity is derived from stable diff inputs: base ref, head ref or worktree generation, path, old/new ranges, and normalized hunk content hash. If identity changes, the UI marks it changed rather than pretending it is still reviewed.
- Notes are plain local text records attached to file or hunk identities. Rendering remains host-owned.
- Persistence uses typed records and atomic writes through existing persistence seams.

## Risks / Trade-offs

- Hunk hashes can churn after rebases or formatting -> changed-since-reviewed markers are expected and should be conservative.
- Notes can become stale after renames -> use repository rename metadata where available and otherwise mark as path-missing/stale.
- Persisting many hunk records could grow project state -> compact by branch/review key and prune obsolete generations after configurable limits if needed.

## Migration Plan

1. Add review state model and persistence records.
2. Add branch review file/hunk marker view-model fields.
3. Implement mark reviewed/unreviewed for files and hunks.
4. Implement changed-since-reviewed detection.
5. Add optional notes and pruning tests.
