#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "editor/BracketScanner.h"
#include "editor/DiagnosticsStore.h"
#include "editor/EditorViewRenderer.h"
#include "workspace/render/DerivedDamage.h"
#include "workspace/state/WorkspaceTabState.h"

namespace microide::editor {
struct FileDecorations;
}

namespace microide::workspace {

struct CompareRowGeometry {
  SDL_FRect surface{};
  float rows_y = 0.0f;
  float line_height = 14.0f;
  float content_width = 0.0f;
  int visible_rows = 0;
};

std::optional<editor::BracketMatchPair> CompareBracketMatch(CompareTabState& compare, bool enabled);

struct CompareRowKeyInputs {
  std::uint64_t surface_key = 0;
  std::optional<editor::BracketMatchPair> bracket_pair;
  const editor::EditorBlameOverlay* blame = nullptr;
  bool caret_drawn = false;
  std::span<const editor::PublishedDiagnostic> diagnostics;
  const editor::FileDecorations* decorations = nullptr;
};

void AppendCompareRowUnits(const CompareTabState& compare, const CompareRowGeometry& geometry,
                           const CompareRowKeyInputs& inputs, std::vector<DerivedDamage::Unit>& units);

void AppendMergeSideRowUnits(const MergeTabState& merge, const CompareRowGeometry& geometry,
                             std::uint64_t surface_key, std::vector<DerivedDamage::Unit>& units);

}
