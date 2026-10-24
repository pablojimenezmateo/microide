#pragma once

#include <cstddef>

#include "workspace/state/WorkspaceTabState.h"

namespace microide::editor {
class TextViewport;
}  // namespace microide::editor

namespace microide::workspace {

struct LanguageContract;

// Lazy refresh entry point for the per-tab `editor::FoldingModel`. Called by the
// render path and by fold-action handlers before reading the model. The helper
// is a no-op when the model's fingerprint already matches `(layout_revision,
// tab_size, language_id)` AND the viewport's line window is already resolved.
// Recompute is bounded by `kFoldingModelComputeBudget` (see implementation) so
// very large files do not pay full-document cost on a single frame.
//
// The window is derived from the viewport's *visual* rows, so a collapsed fold
// hiding a thousand lines still resolves the line range those rows really span.
//
// When `fold_enabled` is `false`, the helper expands every collapsed range and
// clears the model so the renderer paints no fold marks and no rows are hidden.
// The model is rebuilt as soon as the toggle is flipped back on.
//
// `contract` may be null when no language metadata is available; in that case
// only the indent-source fold scan runs.
void EnsureFoldingModelFresh(TabEntry::EditorTabState& tab,
                             editor::TextViewport& viewport,
                             const LanguageContract* contract,
                             std::size_t tab_size,
                             bool fold_enabled,
                             std::size_t visible_rows);

// A restored view's collapsed folds (`restore.collapsed_fold_openers`), once its
// content is here: each opener that still starts a fold collapses again, one the
// file no longer folds at is dropped, and the restored scroll -- a row of the
// folded layout -- is placed again over them unless the view moved meanwhile.
// Runs from EnsureFoldingModelFresh; a no-op with nothing pending.
void CollapseRestoredFolds(TabEntry::EditorTabState& tab, editor::TextViewport& viewport);

// A caret that has moved into the hidden body of a collapsed fold since the last
// prepared frame expands every collapsed fold hiding it (VS Code's
// FoldingController.revealCursor): a goto-line, a definition jump, a diagnostic
// jump, an undo, or a Backspace that joins onto a fold's last line all land the
// caret on a line that cannot be seen otherwise. A fold that collapses AROUND a
// caret standing still is left alone, as in VS Code, so `fold` with the caret
// inside the region still folds it. Runs once per prepared frame per editor
// group; returns true when a fold was expanded (the caller re-scrolls to the
// caret and repaints the editor).
bool RevealCaretsInsideCollapsedFolds(TabEntry::EditorTabState& tab,
                                      const editor::TextViewport& viewport);

}  // namespace microide::workspace
