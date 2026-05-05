## Context

The freeze shows up in `WorkspaceEventDispatcher::Handle::KeyDown`, while render work stays cheap. The hot path is editor mutation plumbing on the main thread: keyboard and text-input handlers copy whole `std::vector<std::string>` buffers to compute redraw spans, and undo or redo can still force full-text LSP synchronization because the workspace has no canonical edit-delta contract to reuse after history application. The current worktree starts introducing `last_applied_edit`, but it only covers some direct edit paths and currently drops back to the slow path for undo or redo and several other workspace entry points.

This change crosses `TextViewport`, workspace input coordinators, redraw invalidation, and LSP sync, so the fix needs an explicit design rather than another local optimization.

## Goals / Non-Goals

**Goals:**
- Make a single applied-edit delta the canonical output of editor mutations when the change can be expressed as one contiguous range replacement.
- Keep delete, backspace, text input, completion acceptance, and undo or redo off the full-buffer path for normal editor usage.
- Preserve correctness for compare and merge tabs, LSP synchronization, and undo-history behavior.
- Fall back safely to full redraw or full-text sync only when a precise delta is unavailable or would be misleading.

**Non-Goals:**
- Rework the entire undo-history storage model.
- Introduce asynchronous LSP edit delivery or a new background queue for editor mutations.
- Optimize operations that are inherently large structural rewrites beyond providing a correct fallback.
- Change editor semantics unrelated to the freeze, such as indentation behavior or multi-caret command rules.

## Decisions

### 1. Derive applied-edit metadata from history entries, not from each caller

`TextViewport` will treat the applied-edit delta as editor-owned state derived from the same history entry used to mutate the document. That keeps the canonical range and replacement text close to the real document mutation and avoids every workspace caller reconstructing the change independently.

Why this over caller-side diffing:
- Caller-side diffing is exactly what caused the regression: it requires whole-buffer snapshots on the input thread.
- Deriving the delta in `TextViewport` keeps keyboard, text-input, completion, mouse paste, and undo or redo on the same contract.

### 2. Undo and redo must publish reverse or forward deltas

Undo and redo will no longer clear applied-edit metadata. When a history entry is applied in reverse, `TextViewport` will expose the reverse replacement range and text for that change; when reapplied forward, it will expose the forward delta again.

Why this over keeping undo on the fallback path:
- The reported freeze includes `Ctrl+Z`, so leaving undo outside the fast path would preserve the user-visible bug.
- The reverse delta is already implied by the history entry, so the extra work is deterministic and local.

### 3. Workspace edit entry points will consume `RequestActiveEditableLastChangeRedraw()` whenever the edit is editor-owned

Keyboard edits, text input, completion insertion, and mouse middle-paste flows that mutate the active viewport through `TextViewport` will use the last-change redraw and LSP path. Full-buffer `RequestActiveEditableChangeRedraw(before, after)` remains only for paths that truly arrive with independent before or after snapshots or that cannot trust editor-owned metadata.

Why this over deleting the old API immediately:
- A narrow fallback path is still useful for structural transforms and makes the migration safer.
- Removing every old call site in one pass is not required to fix the freeze, but the hot editor paths must stop using it.

### 4. Partial redraw stays conservative

The redraw helper will use the applied edit for partial invalidation only when the edit preserves line-count parity between removed and inserted text. Otherwise it will promote to focused-editor redraw. This keeps the retained-scene path simple and avoids subtle stale-pixel bugs for line-structure changes.

Why this over trying to compute exact geometry for every mutation:
- The freeze is from input-thread document work, not from clip-rect math.
- A conservative redraw policy still removes the expensive whole-buffer diff without risking visual corruption.

## Risks / Trade-offs

- [Risk] History-derived edit deltas could be computed incorrectly for edge cases such as single-line replacements with shared prefix and suffix.
  Mitigation: add focused `TextViewport` regression tests for range edits, undo or redo, newline insertion, and multi-line replacements.

- [Risk] Compare or merge flows still require full pre-edit snapshots for derived-state refresh.
  Mitigation: keep those snapshots only where the compare or merge service truly needs them, while still routing redraw and LSP through the editor-owned delta.

- [Risk] Some edit producers may still bypass the new fast path and silently regress later.
  Mitigation: migrate every active viewport mutation entry point touched by this change and document the capability contract so new code knows to consume editor-owned applied edits first.

## Migration Plan

1. Finish the `TextViewport` applied-edit contract so direct edits, undo, redo, and history-based aggregate edits can all publish a correct delta or explicitly publish none.
2. Move the hot workspace edit entry points to `RequestActiveEditableLastChangeRedraw()`.
3. Keep the old before or after snapshot API as a fallback for non-editor-owned or structurally complex mutations.
4. Verify with focused tests and targeted traces; rollback is a normal revert because no on-disk format or external protocol changes are introduced.

## Open Questions

- None. The remaining work is implementation and verification, not requirements discovery.
