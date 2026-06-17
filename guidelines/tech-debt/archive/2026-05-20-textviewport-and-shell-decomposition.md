# TextViewport decomposition (+ rejected `TextDocumentModel`) and WorkspaceShell companion sprawl

- Date: 2026-05-20
- Area: editor, workspace, architecture
- Source: §15 and §16; commits `715b66b`, `e07e073`, `06ef475`, `b31b026`
- Related guardrail: **the rejected `TextDocumentModel` extraction below — do not retry in the same
  shape** (linked from `dev-docs/project/known-tech-debt.md`).

## Summary

`TextViewport.cpp` was decomposed from ~3,553 to ~763 lines across focused ownership-bearing TU
splits (view-state, file I/O, language behavior, highlight cache, multi-caret, undo history, layout
cache) with no public API change; the `WorkspaceShell*.cpp` companion count was ratcheted 51 → 45
with real tab-strip/panel behavior moved off the shell. A `TextDocumentModel` ownership extraction
was attempted and **rejected** by the perf gate.

## Resolution — §15 `TextViewport` ownership concentration

The UndoHistory seam landed 2026-05-20 (commit `715b66b`): `src/editor/TextViewportUndoHistory.{h,cpp}`
owns `undo_stack_`/`redo_stack_` (moved out of `DocumentState`), the per-viewport `group_stack_`, the
`HistoryEntry`/`ViewState`/`SecondaryCaret` types, and the static merge helpers. `TextViewport` keeps
`ApplyHistoryEntry`/`BuildRangeHistoryEntry`/`BuildLineHistoryEntry` (they touch viewport-private
caches) and forwards `Push*`/`Begin*`/`Finish*`. New value header `editor/EditTypes.h` carries
`TextPosition`/`SelectionRange`/`AppliedEdit`.

Earlier (2026-05-18) extractions, all keeping methods as members of the same `TextViewport` class
(no header/API change, no friending):
- Language-pair behavior → `src/editor/TextViewportLanguageBehavior.cpp` (shared helpers in
  `src/editor/TextViewportInternal.h`).
- Save normalization + file I/O → `src/editor/TextViewportFileIO.cpp`.
- Multi-caret apply pipeline → `src/editor/TextViewportMultiCaret.cpp`.
- Highlight cache → `src/editor/TextViewportHighlightCache.cpp`.
- Viewport view-state + cursor/scroll movement → `src/editor/TextViewportViewState.cpp`.
- Layout cache → `src/editor/TextLayoutCache.{h,cpp}` (wrapped rows, visible-line cache, max-column).

Result: a coherent coordinator for selection, cache invalidation, and document metadata — not a
catch-all. The remaining coupling (shared `TextViewport` state and the invalidation contract between
editing, highlighting, folding, layout) is intentional until a deeper document-buffer boundary is
justified by measurements. **Decision (wontdo for now):** the next `TextViewport` work should reduce
ownership, not line count; do not add sibling `TextViewport*.cpp` files unless they are a stepping
stone toward moving state into smaller tested objects.

### Rejected experiment: `TextDocumentModel` ownership extraction — DO NOT RETRY in the same shape

Rejected 2026-05-18 after benchmark-gate comparison against `main`. The branch
`refactor/text-document-model` extracted document-owned state (line storage, dirty state, revision
counters, newline metadata, mutation helpers) into a `TextDocumentModel`. Behavior tests passed, but
the perf gate failed with regressions in hot editor/render scenarios:
- `editor_fold_viewport_refresh`: ~+28–30% wall
- `editor_sticky_scroll_scroll`: ~+21–23%
- `editor_indent_guides_paint`: ~+20–21%
- `editor_render_whitespace_paint`: ~+15–16%
- `editor_shaping_multi_caret`: up to ~+15–22%
- `editor_auto_close_typing`: ~+5–6%; `typing_large_file`: ~+5–7% p95/max
Broad allocation increases across typing, scrolling, idle, startup, terminal, menu. Branch abandoned.

**Lesson:** architectural cleanup is not acceptable in this codebase if it materially hurts latency or
render-path throughput. This was a successful gate — perf checks prevented a bad abstraction from
landing. Future attempts must prove line access, mutation, revision updates, and cache invalidation
are allocation-free and performance-neutral in the editor benchmarks *first*; prefer tiny cold-path
extractions and pure helpers over shared ownership of hot editor state; no full-buffer copies in
render/layout/scroll/typing paths.

## Resolution — §16 `WorkspaceShell*.cpp` companion sprawl

Three slices took the companion cap 51 → 45 and moved tab-strip/panel-tab behavior off the shell:
- `e07e073` — collapsed four single-delegation companions (`WorkspaceShellInput.cpp`,
  `...Blame.cpp`, `...TerminalService.cpp`, `...CommandPrompt.cpp`) into the bootstrapper/shell core
  (51 → 46).
- `06ef475` — folded `WorkspaceShellChrome.cpp` (15 tab-strip/overlay-rect/status-bar wrappers) into
  `WorkspaceShellPresentation.cpp` (46 → 45).
- `b31b026` — extracted `WorkspaceTabStripChrome` (non-shell-named TU, doesn't count against the cap)
  holding refs to `WorkspaceContext`, `TabStripService`, `LayoutModeService`,
  `WorkspaceOutputChannels` plus an `Operations` struct; 12 tab-strip + bottom-panel-tab method
  bodies left `WorkspaceShell`'s symbol surface.

Real service candidates that landed: `EditorBlameOverlayService` (blame-overlay state, line lookup,
hit-testing, overlay construction); `AssistService` (completion, snippet edits, code-action overlays,
go-to-definition, find-references); `TabStripService` (editor/project/bottom-panel tab-strip layout,
overflow, bottom-panel tab models, geometry queries). `WorkspaceShell.h` (≤400) and
`WorkspaceShell.cpp` (≤600) caps satisfied; `CheckWorkspaceShellCompanionTuCount` enforces the
45-companion ratchet (lower on migration, never raise).

What stayed open and was accepted as low/wontdo: `ComputeOverlayRect` (one-line wrapper around
`ComputeOverlaySurfaceRect`, ~14 call sites — a mechanical rename) and `RefreshStatusBar` (blocked
from inlining at its render-TU call site by `CheckRenderSurfaceStateAccess`; would need to land on a
non-render service first). **Policy retained:** do not add new `WorkspaceShell*.cpp` files for new
behavior — the cap hard-fails it.
