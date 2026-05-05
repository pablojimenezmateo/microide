## Why

Delete, backspace, text input, and `Ctrl+Z` currently stall the editor on large files because the input path still pays full-buffer costs on the main thread: whole-document line snapshots for redraw diffing, full-text LSP sync, and inconsistent fallback behavior across editor surfaces. The current worktree attempts to optimize part of that path, but it does not yet make edit-delta handling the single correct path, so freezes remain visible in undo and other edit entry points.

## What Changes

- Make single-range editor mutations publish an exact applied-edit delta that the workspace can reuse for redraw invalidation and incremental LSP sync without copying the whole buffer.
- Extend undo and redo to emit the same applied-edit metadata so `Ctrl+Z` and redo stay on the fast path instead of falling back to full-text synchronization.
- Route editor text-input, keyboard edit commands, completion insertion, and other single-edit workspace entry points through the same delta-based redraw and LSP update flow.
- Keep full-buffer fallback only for operations that cannot be expressed as one precise edit delta, such as multi-caret aggregate edits or other structural transforms.
- Add regression coverage for applied-edit metadata, undo or redo delta correctness, and the workspace edit paths that previously copied full buffers.

## Capabilities

### New Capabilities
- `editor-edit-delta-pipeline`: Defines the canonical applied-edit metadata that editor mutations expose to workspace redraw and LSP consumers.

### Modified Capabilities
- `performance-budgets`: Tighten the typing-path contract so delete, undo, and other editor mutations on large files stay off the full-buffer path and remain interactive.

## Impact

- Affected code: `src/editor/TextViewport.*`, workspace key and text input coordinators, workspace redraw and LSP sync paths, completion application, compare or merge editor edit entry points, and `tests/TextViewportTests.cpp` plus workspace regression coverage.
- Affected systems: editor mutation history, retained-scene invalidation, LSP document sync, and large-file input responsiveness.
