#include "workspace/render/CompareRowUnits.h"

#include <algorithm>
#include <string_view>

#include "util/Fnv1a.h"
#include "workspace/MergeWrapRows.h"
#include "workspace/git/CompareTabReview.h"
#include "workspace/render/CompareReviewControls.h"
#include "workspace/render/EditorRowUnits.h"

namespace microide::workspace {
namespace {

std::uint64_t Mix(std::uint64_t hash, std::uint64_t value) { return MixRowKey(hash, value); }

std::uint64_t MixText(std::uint64_t hash, std::string_view text, std::uint64_t long_line_stand_in) {
  return text.size() <= kEditorRowKeyMaxHashedBytes ? MixRowKeyBytes(hash, text.data(), text.size())
                                                    : Mix(Mix(hash, text.size()), long_line_stand_in);
}

std::uint64_t MixTokens(std::uint64_t hash, const std::vector<editor::SyntaxTokenKind>& tokens) {
  return MixRowKeyBytes(hash, reinterpret_cast<const char*>(tokens.data()),
                        tokens.size() * sizeof(editor::SyntaxTokenKind));
}

std::uint64_t MixSpans(std::uint64_t hash, const std::vector<compare::CompareTextSpan>& spans) {
  hash = Mix(hash, spans.size());
  for (const compare::CompareTextSpan& span : spans) {
    hash = Mix(Mix(hash, span.start), span.end);
  }
  return hash;
}

std::uint64_t MixRightMarks(std::uint64_t hash, const CompareTabState& compare,
                            const std::optional<editor::BracketMatchPair>& bracket_pair, bool caret_drawn,
                            std::size_t line) {
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
    hash = Mix(Mix(Mix(hash, 0xCA), right.cursor_column()), caret_drawn ? 1u : 2u);
  }
  if (const auto selection = right.selection_range(); selection.has_value()) {
    fold_range(0x5E, *selection);
  }
  for (const auto& caret : right.secondary_caret_range_view()) {
    if (caret.position.line == line) {
      hash = Mix(Mix(Mix(hash, 0xCB), caret.position.column), caret_drawn ? 1u : 2u);
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

SDL_FRect RowBand(const CompareRowGeometry& geometry, int row) {
  return SDL_FRect{geometry.surface.x - 1.0f, geometry.rows_y + static_cast<float>(row) * geometry.line_height - 2.0f,
                   geometry.surface.w + 2.0f, geometry.line_height + 3.0f};
}

}

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
                           const CompareRowKeyInputs& inputs, std::vector<DerivedDamage::Unit>& units) {
  const std::size_t visual_count = CompareTabVisualRowCount(compare);
  const std::uint64_t long_line_stand_in = compare.right_viewport.content_revision();
  struct RightLine {
    std::size_t line = 0;
    std::size_t row = 0;
  };
  thread_local std::vector<std::uint64_t> keys;
  thread_local std::vector<RightLine> right_lines;
  keys.clear();
  right_lines.clear();
  std::uint64_t unit_key = 0;
  std::size_t unit_key_unit = 0;
  bool unit_key_valid = false;
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
    const bool model_row = presentation->kind == compare::ComparePresentationRowKind::Model &&
                           presentation->model_row_index < compare.model.rows.size();
    if (!unit_key_valid || wrap.unit != unit_key_unit) {
      std::uint64_t content = Mix(util::kFnv1aOffsetBasis, static_cast<std::uint64_t>(presentation->kind));
      content = Mix(content, presentation->model_row_index);
      content = MixText(content, presentation->display_summary_text, long_line_stand_in);
      content = MixText(content, presentation->review_marker_label, long_line_stand_in);
      for (const std::int64_t part :
           {std::int64_t{presentation->collapsed_line_count},
            static_cast<std::int64_t>(presentation->collapsed_run_start_model_row),
            static_cast<std::int64_t>(presentation->collapsed_run_length), std::int64_t{presentation->context_above},
            std::int64_t{presentation->previous_hunk_index >= 0}, std::int64_t{presentation->next_hunk_index >= 0},
            std::int64_t{presentation->has_review_note}, std::int64_t{presentation->hunk_index}}) {
        content = Mix(content, static_cast<std::uint64_t>(part));
      }
      if (model_row) {
        const std::size_t model_index = presentation->model_row_index;
        const compare::CompareRow& model = compare.model.rows[model_index];
        content = Mix(content, static_cast<std::uint64_t>(model.kind));
        content = Mix(content, static_cast<std::uint64_t>(static_cast<std::int64_t>(model.left_line)));
        content = Mix(content, static_cast<std::uint64_t>(static_cast<std::int64_t>(model.right_line)));
        const bool in_change = model.hunk >= 0;
        const bool first_of_change =
            in_change && (model_index == 0 || compare.model.rows[model_index - 1].hunk != model.hunk);
        content = Mix(content, (in_change ? 1u : 0u) | (first_of_change ? 2u : 0u));
        content = Mix(Mix(content, model.left_moved ? 1u : 2u), model.right_moved ? 1u : 2u);
        content = MixText(content, model.left_text, long_line_stand_in);
        content = MixText(content, model.right_text, long_line_stand_in);
        content = MixSpans(content, model.left_changed_spans);
        content = MixSpans(content, model.right_changed_spans);
        content = MixTokens(content, CompareRowLeftTokens(compare, model));
        content = Mix(content, CompareRowRightTokensAliasLeft(compare, model_index) ? 1u : 2u);
        content = MixTokens(content, CompareRowRightOwnTokens(compare, model));
        if (model.right_line > 0 && inputs.decorations != nullptr) {
          content = LineDecorationKey(Mix(content, 0xDEC0), *inputs.decorations,
                                      static_cast<std::size_t>(model.right_line - 1));
        }
      }
      unit_key = content;
      unit_key_unit = wrap.unit;
      unit_key_valid = true;
    }
    std::uint64_t key = Mix(inputs.surface_key, wrap.unit);
    for (const std::uint64_t part : {std::uint64_t{wrap.left_start}, std::uint64_t{wrap.left_end},
                                     std::uint64_t{wrap.left_indent}, std::uint64_t{wrap.right_start},
                                     std::uint64_t{wrap.right_end}, std::uint64_t{wrap.right_indent},
                                     std::uint64_t{wrap.first}, std::uint64_t{wrap.left_present},
                                     std::uint64_t{wrap.right_present}}) {
      key = Mix(key, part);
    }
    key = Mix(key, unit_key);
    key = Mix(key, wrap.unit == compare.selected_row ? 1u : 2u);
    if (compare.hover_state.has_value() && compare.hover_state->presentation_row == wrap.unit) {
      key = Mix(Mix(key, 0x40), static_cast<std::uint64_t>(compare.hover_state->kind));
    }
    if (const int hunk = CompareHunkControlsOnRow(compare, wrap, row); hunk >= 0) {
      key = Mix(Mix(Mix(Mix(key, 0x4E + static_cast<std::uint64_t>(hunk) * 256u),
                        static_cast<std::uint64_t>(CompareHunkReviewStatus(compare, hunk))),
                    CompareHunkShowsReviewButtons(compare, hunk) ? 1u : 2u),
                CompareHunkHasNote(compare, hunk) ? 1u : 2u);
    }
    if (presentation->kind == compare::ComparePresentationRowKind::ReviewNote) {
      key = Mix(key, util::Fnv1aHash(CompareNoteCaption(compare, presentation->hunk_index)));
    }
    if (model_row) {
      const compare::CompareRow& model = compare.model.rows[presentation->model_row_index];
      if (model.right_line > 0) {
        const auto right_line = static_cast<std::size_t>(model.right_line - 1);
        key = MixRightMarks(key, compare, inputs.bracket_pair, inputs.caret_drawn, right_line);
        if (inputs.blame != nullptr && inputs.blame->visible) {
          for (const editor::EditorBlameLine& line : inputs.blame->lines) {
            if (line.line_index == right_line) {
              key = MixText(Mix(Mix(key, 0xB7), static_cast<std::uint64_t>(line.rect.x)), line.text, 0);
            }
          }
        }
        right_lines.push_back(RightLine{.line = right_line, .row = keys.size()});
      }
    }
    keys.push_back(key);
  }
  if (!inputs.diagnostics.empty() && !right_lines.empty()) {
    const std::size_t first_line = right_lines.front().line;
    const std::size_t last_line = right_lines.back().line;
    for (const editor::PublishedDiagnostic& diagnostic : inputs.diagnostics) {
      const bool forward = diagnostic.range.start.line < diagnostic.range.end.line ||
                           (diagnostic.range.start.line == diagnostic.range.end.line &&
                            diagnostic.range.start.column <= diagnostic.range.end.column);
      const editor::TextPosition& start = forward ? diagnostic.range.start : diagnostic.range.end;
      const editor::TextPosition& end = forward ? diagnostic.range.end : diagnostic.range.start;
      if (end.line < first_line || start.line > last_line) {
        continue;
      }
      auto it = std::lower_bound(right_lines.begin(), right_lines.end(), start.line,
                                 [](const RightLine& ref, std::size_t wanted) { return ref.line < wanted; });
      for (; it != right_lines.end() && it->line <= end.line; ++it) {
        keys[it->row] = Mix(Mix(Mix(keys[it->row], 0xD0 + static_cast<std::uint64_t>(diagnostic.severity)),
                                it->line == start.line ? start.column : 0),
                            it->line == end.line ? end.column : ~std::uint64_t{0});
      }
    }
  }
  for (std::size_t row = 0; row < keys.size(); ++row) {
    units.push_back({RowBand(geometry, static_cast<int>(row)), keys[row]});
  }
  const float bottom = geometry.surface.y + geometry.surface.h;
  const float tail_top = geometry.rows_y + static_cast<float>(keys.size()) * geometry.line_height - 1.0f;
  units.push_back({SDL_FRect{geometry.surface.x, tail_top, geometry.content_width, std::max(0.0f, bottom - tail_top)},
                   Mix(inputs.surface_key, 0x7A11)});
}

void AppendMergeSideRowUnits(const MergeTabState& merge, const CompareRowGeometry& geometry,
                             std::uint64_t surface_key, std::vector<DerivedDamage::Unit>& units) {
  const std::size_t visual_count = MergeSourceVisualRowCount(merge);
  const std::uint64_t long_line_stand_in = merge.result_viewport.content_revision();
  const std::size_t selected =
      merge.conflicts.empty() ? 0 : std::min(merge.selected_hunk, merge.conflicts.size() - 1);
  int painted_rows = 0;
  std::uint64_t line_key = 0;
  std::size_t line_key_line = 0;
  bool line_key_valid = false;
  for (int row = 0; row < geometry.visible_rows; ++row) {
    const std::size_t visual = static_cast<std::size_t>(std::max(0, merge.scroll_row + row));
    if (visual >= visual_count) {
      break;
    }
    const DiffWrapRow wrap = merge.wrap_layout.RowAt(visual);
    const std::size_t line = wrap.unit;
    std::uint64_t key = Mix(surface_key, line);
    for (const std::uint64_t part : {std::uint64_t{wrap.left_start}, std::uint64_t{wrap.left_end},
                                     std::uint64_t{wrap.left_indent}, std::uint64_t{wrap.right_start},
                                     std::uint64_t{wrap.right_end}, std::uint64_t{wrap.right_indent},
                                     std::uint64_t{wrap.first}, std::uint64_t{wrap.left_present},
                                     std::uint64_t{wrap.right_present}}) {
      key = Mix(key, part);
    }
    if (!line_key_valid || line != line_key_line) {
      std::uint64_t content = util::kFnv1aOffsetBasis;
      if (line < merge.model.incoming_lines.size()) {
        content = MixText(Mix(content, 0x11), merge.model.incoming_lines[line], long_line_stand_in);
        content = MixTokens(content, merge.incoming_token_window.Tokens(line));
      }
      if (line < merge.model.current_lines.size()) {
        content = MixText(Mix(content, 0x22), merge.model.current_lines[line], long_line_stand_in);
        content = MixTokens(content, merge.current_token_window.Tokens(line));
      }
      line_key = content;
      line_key_line = line;
      line_key_valid = true;
    }
    key = Mix(key, line_key);
    for (std::size_t i = 0; i < merge.conflicts.size(); ++i) {
      const MergeTrackedConflict& conflict = merge.conflicts[i];
      const bool incoming = line >= conflict.incoming_start_line && line < conflict.incoming_end_line;
      const bool current = line >= conflict.current_start_line && line < conflict.current_end_line;
      if (incoming || current) {
        key = Mix(Mix(Mix(key, i), (incoming ? 1u : 0u) | (current ? 2u : 0u)),
                  (conflict.valid ? 1u : 0u) | (conflict.resolved ? 2u : 0u) | (i == selected ? 4u : 0u));
        key = Mix(key, static_cast<std::uint64_t>(conflict.last_choice));

      }
    }
    units.push_back({RowBand(geometry, row), key});
    ++painted_rows;
  }
  const float bottom = geometry.surface.y + geometry.surface.h;
  const float tail_top = geometry.rows_y + static_cast<float>(painted_rows) * geometry.line_height - 1.0f;
  units.push_back({SDL_FRect{geometry.surface.x, tail_top, geometry.content_width, std::max(0.0f, bottom - tail_top)},
                   Mix(surface_key, 0x7A12)});
}

}
