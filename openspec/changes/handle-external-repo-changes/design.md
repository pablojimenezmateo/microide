## Context

The shipped baseline includes host-owned filesystem helpers and native tree watching, and the docs call out correctness under on-disk rename, delete, reload, diagnostics, blame, compare, and merge state. This change defines how those signals affect the Git workstation surfaces.

The implementation must use existing background execution and idle-hint rules. File-watch events cannot cause synchronous Git refreshes or zero-delay idle polling.

## Dependencies

- Depends on: `establish-git-repository-state-service`, `expand-diff-review-workflows`, and `harden-merge-resolution-workflow`.
- Consumed by: `improve-commit-workflow` and `prepare-git-workstation-preview`.
- Unblocks: trusted behavior under external edits, branch switches, and tool-driven file rewrites.

## Goals / Non-Goals

**Goals:**
- Make external file and repository changes visible and safe across open workstation surfaces.
- Refresh clean buffers automatically or by configured prompt, while protecting dirty buffers.
- Mark diff/merge tabs stale when their target changed and refresh with anchors where safe.
- Invalidate merge resolved state after external result/index changes.
- Coalesce watcher bursts and keep idle CPU near zero after settling.

**Non-Goals:**
- No cross-process locking protocol with external editors.
- No background search-index rewrite beyond the existing index/update seams.
- No hosted provider or remote filesystem support.
- No new plugin file-watch API.

## Decisions

- Watcher events are normalized into project-relative change records before fan-out. Services consume typed events instead of raw platform watcher details.
- Dirty buffer protection is mandatory. A clean buffer can auto-reload if configured, but a dirty buffer gets a conflict prompt and no silent overwrite.
- Diff and merge tabs retain a model generation and source identity. External changes mark them stale first; refresh happens asynchronously and preserves scroll/caret anchors only when mapping remains valid.
- Branch/index changes invalidate repository snapshots and any merge mark-resolved assumptions.

## Risks / Trade-offs

- Watchers are noisy and platform-specific -> normalize and coalesce events before service fan-out, then test burst behavior.
- Anchor preservation can be wrong after heavy edits -> prefer a safe approximate anchor or top-of-hunk fallback over pretending exact mapping survived.
- Search/blame/diagnostic updates can create cascades -> each service should ignore stale generations and publish one visible stale state before recomputing.

## Migration Plan

1. Add normalized project file/repository event types.
2. Wire events into Git snapshot invalidation and sidebar stale state.
3. Add editor reload/dirty-conflict prompts.
4. Add diff/merge stale refresh with anchor preservation.
5. Fan out to tree/search/diagnostics/blame cleanup and add burst tests/perf coverage.
