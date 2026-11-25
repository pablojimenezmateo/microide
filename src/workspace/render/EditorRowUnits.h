#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "editor/BracketScanner.h"
#include "editor/DiagnosticsStore.h"
#include "editor/EditTypes.h"
#include "editor/EditorViewRenderer.h"
#include "editor/FoldingModel.h"
#include "editor/TextViewport.h"
#include "workspace/render/DerivedDamage.h"

namespace microide::editor {
struct FileDecorations;
}

namespace microide::workspace {

struct EditorRowKeyInputs {
  const editor::TextViewport* viewport = nullptr;
  SDL_FRect pane_rect{};
  editor::EditorViewMetrics metrics{};
  std::uint64_t pane_key = 0;
  const editor::FileDecorations* decorations = nullptr;
  bool rows_have_gaps = false;
  std::span<const std::size_t> sticky_lines;
  bool caret_drawn = false;
  std::span<const editor::PublishedDiagnostic> diagnostics;
  std::span<const std::pair<std::size_t, std::uint64_t>> occurrence_lines;
  std::optional<editor::BracketMatchPair> bracket_pair;
  const editor::FoldingModel* folds = nullptr;
  std::span<const editor::FoldGutterMark> fold_marks;
  std::optional<editor::SelectionRange> active_search_match;
  const editor::EditorBlameOverlay* blame = nullptr;
  std::span<const std::pair<std::size_t, std::uint64_t>> extra_line_keys;
};

inline constexpr std::size_t kEditorRowKeyMaxHashedBytes = 16 * 1024;

inline std::uint64_t MixRowKey(std::uint64_t hash, std::uint64_t value) {
  constexpr std::uint64_t kMultiplier = 0x9E3779B97F4A7C15ull;
  hash = (hash ^ value) * kMultiplier;
  return hash ^ (hash >> 32);
}

inline std::uint64_t MixRowKeyBytes(std::uint64_t hash, const char* data, std::size_t size) {
  hash = MixRowKey(hash, size);
  while (size >= 8) {
    std::uint64_t word = 0;
    std::memcpy(&word, data, 8);
    hash = MixRowKey(hash, word);
    data += 8;
    size -= 8;
  }
  if (size > 0) {
    std::uint64_t word = 0;
    std::memcpy(&word, data, size);
    hash = MixRowKey(hash, word);
  }
  return hash;
}

inline std::uint64_t RowKeyFloatBits(float value) {
  std::uint32_t bits = 0;
  std::memcpy(&bits, &value, sizeof bits);
  return bits;
}
inline std::uint64_t RowKeyColorBits(SDL_Color color) {
  return static_cast<std::uint64_t>(color.r) | static_cast<std::uint64_t>(color.g) << 8 |
         static_cast<std::uint64_t>(color.b) << 16 | static_cast<std::uint64_t>(color.a) << 24;
}
inline std::uint64_t MixRowKeyRect(std::uint64_t key, const SDL_FRect& rect) {
  key = MixRowKey(key, RowKeyFloatBits(rect.x) | RowKeyFloatBits(rect.y) << 32);
  return MixRowKey(key, RowKeyFloatBits(rect.w) | RowKeyFloatBits(rect.h) << 32);
}
inline std::uint64_t MixRowKeyText(std::uint64_t key, std::string_view text) {
  return MixRowKeyBytes(key, text.data(), text.size());
}

std::uint64_t LineDecorationKey(std::uint64_t key, const editor::FileDecorations& decorations, std::size_t line);

void AppendEditorRowUnits(const EditorRowKeyInputs& inputs, std::vector<std::uint64_t>& row_keys,
                          std::vector<DerivedDamage::Unit>& units);

}
