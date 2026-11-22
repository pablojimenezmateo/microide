#pragma once

// Derived damage for a compare surface, row by row (derived-damage 2.5): each
// visible wrapped row is a unit keyed at frame prep on what it shows -- its place
// in the wrap layout, its presentation row (kind, summary, hunk and collapsed-run
// fields, review marks), its model row (both sides' text, line numbers, kind,
// changed spans, moved flags), both sides' syntax tokens, whether it is the
// selected row or under the hover, and the right side's carets, selections and
// bracket pair on its line -- plus a surface key for what moves every row.

#include <SDL3/SDL.h>

#include <cstdint>
#include <optional>
#include <vector>

#include "editor/BracketScanner.h"
#include "editor/EditorViewRenderer.h"
#include "workspace/render/DerivedDamage.h"
#include "workspace/state/WorkspaceTabState.h"

namespace microide::workspace {

struct CompareRowGeometry {
  SDL_FRect surface{};      // the compare tab's surface
  float rows_y = 0.0f;      // the first row's text top
  float line_height = 14.0f;
  float content_width = 0.0f;
  int visible_rows = 0;
};

// The editable (right) side's bracket-match pair at its caret, memoized on the
// tab on (content revision, caret line, caret column): FindBracketMatch is
// O(file). Frame prep asks first (the rows key on it), and paint hits the memo.
std::optional<editor::BracketMatchPair> CompareBracketMatch(CompareTabState& compare, bool enabled);

// Appends the surface's row units and the empty stretch below its last row.
// `surface_key` folds what moves every row (scroll, size, focus, settings).
// `blame` is the working side's inline blame (none: not shown), folded into the
// row of each line it annotates.
void AppendCompareRowUnits(const CompareTabState& compare, const CompareRowGeometry& geometry,
                           std::uint64_t surface_key, const std::optional<editor::BracketMatchPair>& bracket_pair,
                           const editor::EditorBlameOverlay* blame, std::vector<DerivedDamage::Unit>& units);

// A merge surface's two source columns, row by row (derived-damage 2.5): each
// visible wrapped row keyed on its wrap-layout place, the incoming and current
// lines' text and tokens, and the conflicts covering them (their index, validity,
// resolution and whether selected). The result column is an editor viewport and
// takes the editor's row units (AppendEditorRowUnits) under the same surface key.
// The row bands span the surface; the tail unit covers the stretch below.
void AppendMergeSideRowUnits(const MergeTabState& merge, const CompareRowGeometry& geometry,
                             std::uint64_t surface_key, std::vector<DerivedDamage::Unit>& units);

}  // namespace microide::workspace
