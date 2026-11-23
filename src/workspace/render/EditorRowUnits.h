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
#include <cstring>
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

namespace microide::editor {
struct FileDecorations;
}  // namespace microide::editor

namespace microide::workspace {

struct EditorRowKeyInputs {
  const editor::TextViewport* viewport = nullptr;
  SDL_FRect pane_rect{};
  editor::EditorViewMetrics metrics{};
  // Everything that changes every row at once (scroll, size, language, the
  // settings the pane paints with, ...), folded by the caller.
  std::uint64_t pane_key = 0;
  // The file's plugin and language-server decorations, or null: each line keys on
  // its own (text styles, inline texts, gutter mark, code lenses).
  const editor::FileDecorations* decorations = nullptr;
  // Rows do not sit on a fixed pitch (inline insets open gaps between them): the
  // pane is then one unit keyed by every row.
  bool rows_have_gaps = false;
  // The lines the sticky band shows, top to bottom (the view model's): the band
  // draws each as a row -- its text and syntax colours, the caret's highlight when
  // the caret is on it, its diagnostics and fold mark -- so its unit keys on them.
  std::span<const std::size_t> sticky_lines;
  // Whether this frame's paint draws the pane's carets (the pane has the keyboard
  // and the blink is in its lit phase). Folded into the rows that hold a caret, so
  // a blink repaints those rows and no other -- not into the rest, whose keys a
  // blink must leave alone.
  bool caret_drawn = false;
  std::span<const editor::PublishedDiagnostic> diagnostics;
  // The focused pane's occurrence highlights, one (line, hash) per line, sorted.
  std::span<const std::pair<std::size_t, std::uint64_t>> occurrence_lines;
  std::optional<editor::BracketMatchPair> bracket_pair;
  const editor::FoldingModel* folds = nullptr;
  // The fold marks paint draws, from the pane's view model (sorted by visual row).
  std::span<const editor::FoldGutterMark> fold_marks;
  std::optional<editor::SelectionRange> active_search_match;
  const editor::EditorBlameOverlay* blame = nullptr;
  // Whatever else a surface draws on a line of this viewport, one (line, hash) per
  // line, sorted by line: a merge result's conflict boxes, their selection and
  // hover. Folded into the rows of that line.
  std::span<const std::pair<std::size_t, std::uint64_t>> extra_line_keys;
};

// Lines longer than this fold their length and the document's content revision
// instead of their bytes: a minified megabyte line is not hashed every frame,
// and its rows repaint whenever the document changes.
inline constexpr std::size_t kEditorRowKeyMaxHashedBytes = 16 * 1024;

// One value into a row key: one multiply, not FNV's one per byte. The editor's and
// the diff surfaces' rows fold every caret, mark and decoration of every visible
// line on every prepared frame, so both use this one step.
inline std::uint64_t MixRowKey(std::uint64_t hash, std::uint64_t value) {
  constexpr std::uint64_t kMultiplier = 0x9E3779B97F4A7C15ull;
  hash = (hash ^ value) * kMultiplier;
  return hash ^ (hash >> 32);
}

// `size` bytes, eight to a multiply. The length goes in first, so a short tail
// (zero-padded to a word) cannot stand for a longer run that ends in zero bytes.
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

// Folds into `key` what plugins and language servers decorate `line` with: its
// text styles (semantic tokens), inline texts (inlay hints), gutter mark and code
// lenses. A republish rewrites them in place under the same FileDecorations, so a
// row keys on them, not on the store's address. Shared with the diff surfaces.
std::uint64_t LineDecorationKey(std::uint64_t key, const editor::FileDecorations& decorations, std::size_t line);

// Appends the pane's units to `units`. `row_keys` is scratch kept by the caller
// across frames (cleared here, capacity kept).
void AppendEditorRowUnits(const EditorRowKeyInputs& inputs, std::vector<std::uint64_t>& row_keys,
                          std::vector<DerivedDamage::Unit>& units);

}  // namespace microide::workspace
