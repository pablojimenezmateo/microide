#include "workspace/render/CompareRowUnits.h"

#include <algorithm>
#include <string_view>

#include "util/Fnv1a.h"
#include "workspace/git/CompareTabReview.h"
#include "workspace/render/EditorRowUnits.h"

namespace microide::workspace {
namespace {

std::uint64_t Mix(std::uint64_t hash, std::uint64_t value) { return util::Fnv1aValue(hash, value); }

std::uint64_t MixText(std::uint64_t hash, std::string_view text, std::uint64_t long_line_stand_in) {
  hash = Mix(hash, text.size());
  return text.size() <= kEditorRowKeyMaxHashedBytes ? util::Fnv1aBytes(hash, text) : Mix(hash, long_line_stand_in);
}

std::uint64_t MixTokens(std::uint64_t hash, const std::vector<editor::SyntaxTokenKind>& tokens) {
  hash = Mix(hash, tokens.size());
  return util::Fnv1aBytes(hash, std::string_view(reinterpret_cast<const char*>(tokens.data()),
                                                 tokens.size() * sizeof(editor::SyntaxTokenKind)));
}

std::uint64_t MixSpans(std::uint64_t hash, const std::vector<compare::CompareTextSpan>& spans) {
  hash = Mix(hash, spans.size());
  for (const compare::CompareTextSpan& span : spans) {
    hash = util::Fnv1aBytes(hash, std::string_view(reinterpret_cast<const char*>(&span), sizeof span));
  }
  return hash;
}

// Folds the right side's caret, selections and bracket marks on `line` (0-based).
std::uint64_t MixRightMarks(std::uint64_t hash, const CompareTabState& compare,
                            const std::optional<editor::BracketMatchPair>& bracket_pair, std::size_t line) {
  const editor::TextViewport& right = compare.right_viewport;
  if (!compare.right_view_active) {
    return hash;
  }
  const auto fold_range = [&](std::uint64_t tag, const editor::SelectionRange& range) {
    const bool forward = range.start.line < range.end.line ||
                         (range.start.line == range.end.line && range.start.column <= range.end.column);
    const editor::TextPosition& start = forward ? range.start : range.end;
    const editor::TextPosition& end = forward ? range.end : range.start;
    if (line < start.line || line > end.line) {
      return;
    }
    hash = Mix(Mix(Mix(hash, tag), line == start.line ? start.column : 0),
               line == end.line ? end.column : ~std::uint64_t{0});
  };
  if (right.cursor_line() == line) {
    hash = Mix(Mix(hash, 0xCA), right.cursor_column());
  }
  if (const auto selection = right.selection_range(); selection.has_value()) {
    fold_range(0x5E, *selection);
  }
  for (const auto& caret : right.secondary_caret_range_view()) {
    if (caret.position.line == line) {
      hash = Mix(Mix(hash, 0xCB), caret.position.column);
    }
    if (caret.selection_anchor.has_value()) {
      fold_range(0x5F, editor::SelectionRange{.start = *caret.selection_anchor, .end = caret.position});
    }
  }
  if (bracket_pair.has_value()) {
    const editor::BracketMatchPair& pair = *bracket_pair;
    if (pair.open_line == line) {
      hash = Mix(Mix(hash, 0xB0), pair.open_column);
    }
    if (pair.close_line == line) {
      hash = Mix(Mix(hash, 0xB1), pair.close_column);
    }
  }
  return hash;
}

}  // namespace

std::optional<editor::BracketMatchPair> CompareBracketMatch(CompareTabState& compare, bool enabled) {
  if (!enabled || !compare.right_view_active) {
    return std::nullopt;
  }
  const editor::TextViewport& right = compare.right_viewport;
  const std::uint64_t content_revision = right.content_revision();
  const std::size_t caret_line = right.cursor_line();
  const std::size_t caret_column = right.cursor_column();
  if (compare.bracket_match_valid && compare.bracket_match_content_revision == content_revision &&
      compare.bracket_match_caret_line == caret_line && compare.bracket_match_caret_column == caret_column) {
    return compare.bracket_match_pair;
  }
  compare.bracket_match_pair = editor::FindBracketMatch(right, caret_line, caret_column);
  compare.bracket_match_content_revision = content_revision;
  compare.bracket_match_caret_line = caret_line;
  compare.bracket_match_caret_column = caret_column;
  compare.bracket_match_valid = true;
  return compare.bracket_match_pair;
}

void AppendCompareRowUnits(const CompareTabState& compare, const CompareRowGeometry& geometry,
                           std::uint64_t surface_key, const std::optional<editor::BracketMatchPair>& bracket_pair,
                           std::vector<DerivedDamage::Unit>& units) {
  const std::size_t visual_count = CompareTabVisualRowCount(compare);
  const std::uint64_t long_line_stand_in = compare.right_viewport.content_revision();
  int painted_rows = 0;
  for (int row = 0; row < geometry.visible_rows; ++row) {
    const int visual_index = compare.scroll_row + row;
    if (visual_index < 0 || static_cast<std::size_t>(visual_index) >= visual_count) {
      break;
    }
    const DiffWrapRow wrap = compare.wrap_layout.RowAt(static_cast<std::size_t>(visual_index));
    const compare::ComparePresentationRow* presentation = CompareTabPresentationRowAt(compare, wrap.unit);
    if (presentation == nullptr) {
      break;
    }
    std::uint64_t key = Mix(surface_key, wrap.unit);
    for (const std::uint64_t part : {std::uint64_t{wrap.left_start}, std::uint64_t{wrap.left_end},
                                     std::uint64_t{wrap.left_indent}, std::uint64_t{wrap.right_start},
                                     std::uint64_t{wrap.right_end}, std::uint64_t{wrap.right_indent},
                                     std::uint64_t{wrap.first}, std::uint64_t{wrap.left_present},
                                     std::uint64_t{wrap.right_present}}) {
      key = Mix(key, part);
    }
    key = Mix(key, static_cast<std::uint64_t>(presentation->kind));
    key = Mix(key, presentation->model_row_index);
    key = MixText(key, presentation->display_summary_text, long_line_stand_in);
    key = MixText(key, presentation->review_marker_label, long_line_stand_in);
    for (const std::int64_t part :
         {std::int64_t{presentation->hunk_index}, std::int64_t{presentation->collapsed_line_count},
          static_cast<std::int64_t>(presentation->collapsed_run_start_model_row),
          static_cast<std::int64_t>(presentation->collapsed_run_length), std::int64_t{presentation->context_above},
          std::int64_t{presentation->previous_hunk_index}, std::int64_t{presentation->next_hunk_index},
          std::int64_t{presentation->has_review_note}}) {
      key = Mix(key, static_cast<std::uint64_t>(part));
    }
    key = Mix(key, wrap.unit == compare.selected_row ? 1u : 2u);
    if (compare.hover_state.has_value() && compare.hover_state->presentation_row == wrap.unit) {
      key = Mix(Mix(key, 0x40), static_cast<std::uint64_t>(compare.hover_state->kind));
    }
    if (presentation->kind == compare::ComparePresentationRowKind::Model &&
        presentation->model_row_index < compare.model.rows.size()) {
      const std::size_t model_index = presentation->model_row_index;
      const compare::CompareRow& model = compare.model.rows[model_index];
      key = Mix(key, static_cast<std::uint64_t>(model.kind));
      key = Mix(key, static_cast<std::uint64_t>(static_cast<std::int64_t>(model.left_line)));
      key = Mix(key, static_cast<std::uint64_t>(static_cast<std::int64_t>(model.right_line)));
      key = Mix(key, static_cast<std::uint64_t>(static_cast<std::int64_t>(model.hunk)));
      key = Mix(Mix(key, model.left_moved ? 1u : 2u), model.right_moved ? 1u : 2u);
      key = MixText(key, model.left_text, long_line_stand_in);
      key = MixText(key, model.right_text, long_line_stand_in);
      key = MixSpans(key, model.left_changed_spans);
      key = MixSpans(key, model.right_changed_spans);
      key = MixTokens(key, compare.left_token_window.Tokens(model_index));
      key = MixTokens(key, compare.right_token_window.Tokens(model_index));
      if (model.right_line > 0) {
        key = MixRightMarks(key, compare, bracket_pair, static_cast<std::size_t>(model.right_line - 1));
      }
    }
    // The row band a summary or model row paints: from a pixel above its text top.
    units.push_back({SDL_FRect{geometry.surface.x, geometry.rows_y + static_cast<float>(row) * geometry.line_height - 1.0f,
                               geometry.content_width, geometry.line_height},
                     key});
    ++painted_rows;
  }
  const float bottom = geometry.surface.y + geometry.surface.h;
  const float tail_top = geometry.rows_y + static_cast<float>(painted_rows) * geometry.line_height - 1.0f;
  units.push_back({SDL_FRect{geometry.surface.x, tail_top, geometry.content_width, std::max(0.0f, bottom - tail_top)},
                   Mix(surface_key, 0x7A11)});
}

}  // namespace microide::workspace
