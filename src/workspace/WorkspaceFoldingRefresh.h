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

}  // namespace microide::workspace
