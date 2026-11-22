#pragma once

// Derived damage for an editor pane, row by row (derived-damage D4/D5, the lazy
// fallback): each visible row is a unit whose fingerprint is folded at frame
// prep from cheap per-row keys -- the line's bytes, the row's place in the
// wrapped layout, the caret and selections on it, the find, occurrence, bracket
// and diagnostic marks on it, its fold mark and blame line, its syntax tokens --
// with no allocation in steady state, tokenizing only lines whose tokens are
// stale (which their repaint would do anyway). The row itself is built only
// when it is damaged, as before.
//
// Syntax colours are in the key: each row folds its line's tokens, read from
// the viewport's token cache after an edit that kept its carried state has been
// settled (TextViewport::SettleHighlightAfterLastEdit), so a comment opened above
// a row recolours -- and repaints -- exactly the rows it recoloured.

#include <SDL3/SDL.h>

#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "editor/BracketScanner.h"
#include "editor/DiagnosticsStore.h"
#include "editor/EditTypes.h"
#include "editor/EditorViewRenderer.h"
#include "editor/FoldingModel.h"
#include "editor/TextViewport.h"
#include "workspace/render/DerivedDamage.h"

namespace microide::workspace {

struct EditorRowKeyInputs {
  const editor::TextViewport* viewport = nullptr;
  SDL_FRect pane_rect{};
  editor::EditorViewMetrics metrics{};
  // Everything that changes every row at once (scroll, size, language, the
  // settings the pane paints with, ...), folded by the caller.
  std::uint64_t pane_key = 0;
  // Rows do not sit on a fixed pitch (inline insets open gaps between them): the
  // pane is then one unit keyed by every row.
  bool rows_have_gaps = false;
  std::span<const editor::PublishedDiagnostic> diagnostics;
  // The focused pane's occurrence highlights, one (line, hash) per line, sorted.
  std::span<const std::pair<std::size_t, std::uint64_t>> occurrence_lines;
  std::optional<editor::BracketMatchPair> bracket_pair;
  const editor::FoldingModel* folds = nullptr;
  std::optional<editor::SelectionRange> active_search_match;
  const editor::EditorBlameOverlay* blame = nullptr;
};

// Lines longer than this fold their length and the document's content revision
// instead of their bytes: a minified megabyte line is not hashed every frame,
// and its rows repaint whenever the document changes.
inline constexpr std::size_t kEditorRowKeyMaxHashedBytes = 16 * 1024;

// Appends the pane's units to `units`. `row_keys` is scratch kept by the caller
// across frames (cleared here, capacity kept).
void AppendEditorRowUnits(const EditorRowKeyInputs& inputs, std::vector<std::uint64_t>& row_keys,
                          std::vector<DerivedDamage::Unit>& units);

}  // namespace microide::workspace
