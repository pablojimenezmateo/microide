#include "workspace/WorkspaceShell.h"

#include <array>
#include <algorithm>
#include <cmath>
#include <string_view>
#include <vector>

#include "editor/SyntaxHighlighter.h"
#include "editor/TextLayout.h"
#include "workspace/WorkspaceShellShared.h"

namespace microide::workspace {

namespace {

constexpr float kMergeToolbarHeight = 54.0f;
constexpr float kMergeToolbarButtonHeight = 22.0f;
constexpr float kMergeToolbarButtonGap = 8.0f;
constexpr float kScrollbarReserve = 12.0f;

SDL_Color BlendColor(SDL_Color base, SDL_Color tint, float amount) {
  const float clamped_amount = std::clamp(amount, 0.0f, 1.0f);
  const auto blend = [&](Uint8 base_component, Uint8 tint_component) {
    return static_cast<Uint8>(std::lround(static_cast<float>(base_component) * (1.0f - clamped_amount) +
                                          static_cast<float>(tint_component) * clamped_amount));
  };
  return SDL_Color{
      blend(base.r, tint.r),
      blend(base.g, tint.g),
      blend(base.b, tint.b),
      0xff,
  };
}

std::size_t Utf8SequenceLength(std::string_view text, std::size_t offset) {
  if (offset >= text.size()) {
    return 0;
  }

  const unsigned char lead = static_cast<unsigned char>(text[offset]);
  if (lead <= 0x7F) {
    return 1;
  }

  auto continuation = [&](std::size_t count) {
    if (offset + count >= text.size()) {
      return false;
    }
    for (std::size_t i = 1; i <= count; ++i) {
      const unsigned char byte = static_cast<unsigned char>(text[offset + i]);
      if ((byte & 0xC0) != 0x80) {
        return false;
      }
    }
    return true;
  };

  if (lead >= 0xC2 && lead <= 0xDF && continuation(1)) {
    return 2;
  }
  if (lead == 0xE0 && continuation(2)) {
    const unsigned char second = static_cast<unsigned char>(text[offset + 1]);
    if (second >= 0xA0 && second <= 0xBF) {
      return 3;
    }
  }
  if (((lead >= 0xE1 && lead <= 0xEC) || (lead >= 0xEE && lead <= 0xEF)) && continuation(2)) {
    return 3;
  }
  if (lead == 0xED && continuation(2)) {
    const unsigned char second = static_cast<unsigned char>(text[offset + 1]);
    if (second >= 0x80 && second <= 0x9F) {
      return 3;
    }
  }
  if (lead == 0xF0 && continuation(3)) {
    const unsigned char second = static_cast<unsigned char>(text[offset + 1]);
    if (second >= 0x90 && second <= 0xBF) {
      return 4;
    }
  }
  if (lead >= 0xF1 && lead <= 0xF3 && continuation(3)) {
    return 4;
  }
  if (lead == 0xF4 && continuation(3)) {
    const unsigned char second = static_cast<unsigned char>(text[offset + 1]);
    if (second >= 0x80 && second <= 0x8F) {
      return 4;
    }
  }

  return 1;
}

struct VisibleTextWindow {
  std::string_view text;
  std::size_t byte_offset = 0;
};

VisibleTextWindow SliceVisibleColumns(std::string_view text,
                                      std::size_t start_column,
                                      std::size_t visible_columns) {
  const std::size_t byte_offset = Utf8ByteOffsetForCodepointCount(text, start_column);
  const std::size_t byte_length =
      Utf8ByteOffsetForCodepointCount(text.substr(byte_offset), visible_columns);
  return VisibleTextWindow{
      .text = text.substr(byte_offset, byte_length),
      .byte_offset = byte_offset,
  };
}

SDL_Color CompareTokenColor(const render::Theme& theme,
                            editor::SyntaxTokenKind kind,
                            SDL_Color fallback,
                            bool selected) {
  switch (kind) {
    case editor::SyntaxTokenKind::Keyword:
      return theme.syntax_keyword;
    case editor::SyntaxTokenKind::Type:
      return theme.syntax_type;
    case editor::SyntaxTokenKind::String:
      return theme.syntax_string;
    case editor::SyntaxTokenKind::Comment:
      return theme.syntax_comment;
    case editor::SyntaxTokenKind::Number:
      return theme.syntax_number;
    case editor::SyntaxTokenKind::Constant:
      return theme.syntax_constant;
    case editor::SyntaxTokenKind::Preprocessor:
      return theme.syntax_preprocessor;
    case editor::SyntaxTokenKind::Operator:
      return theme.syntax_operator;
    case editor::SyntaxTokenKind::Plain:
    default:
      return selected ? theme.text_primary : fallback;
  }
}

}  // namespace

void WorkspaceShell::PopulateCompareSyntaxTokensForWindow(CompareTabState& compare_tab,
                                                          std::size_t visible_start_row,
                                                          std::size_t visible_end_row) {
  if (!compare_tab.syntax_highlighting_enabled || compare_tab.model.rows.empty()) {
    return;
  }

  const std::size_t row_count = compare_tab.model.rows.size();
  const std::size_t clamped_end = std::min(visible_end_row, row_count);
  if (visible_start_row >= clamped_end) {
    return;
  }

  constexpr std::size_t kCompareSyntaxRowsPerFrame = 256;
  std::size_t target_row = std::min(clamped_end, compare_tab.syntax_rows_tokenized + kCompareSyntaxRowsPerFrame);
  while (compare_tab.syntax_rows_tokenized < target_row) {
    const std::size_t index = compare_tab.syntax_rows_tokenized;
    const auto& compare_row = compare_tab.model.rows[index];
    auto& left_tokens = compare_tab.left_tokens_by_row[index];
    auto& right_tokens = compare_tab.right_tokens_by_row[index];

    const bool reuse_tokens =
        compare_row.kind == compare::CompareRowKind::Unchanged && compare_row.left_line > 0 &&
        compare_row.right_line > 0 && compare_row.left_text == compare_row.right_text &&
        compare_tab.left_current_syntax_state.definition_id ==
            compare_tab.right_current_syntax_state.definition_id &&
        compare_tab.left_current_syntax_state.region_id ==
            compare_tab.right_current_syntax_state.region_id;
    if (reuse_tokens) {
      editor::HighlightedLine highlighted = editor::SyntaxHighlighter::HighlightLine(
          compare_row.left_text, compare_tab.path, compare_tab.left_current_syntax_state);
      compare_tab.left_current_syntax_state = highlighted.end_state;
      compare_tab.right_current_syntax_state = highlighted.end_state;
      left_tokens = highlighted.tokens;
      right_tokens = std::move(highlighted.tokens);
      ++compare_tab.syntax_rows_tokenized;
      continue;
    }

    if (compare_row.left_line > 0) {
      editor::HighlightedLine highlighted = editor::SyntaxHighlighter::HighlightLine(
          compare_row.left_text, compare_tab.path, compare_tab.left_current_syntax_state);
      compare_tab.left_current_syntax_state = highlighted.end_state;
      left_tokens = std::move(highlighted.tokens);
    } else {
      left_tokens.clear();
    }

    if (compare_row.right_line > 0) {
      editor::HighlightedLine highlighted = editor::SyntaxHighlighter::HighlightLine(
          compare_row.right_text, compare_tab.path, compare_tab.right_current_syntax_state);
      compare_tab.right_current_syntax_state = highlighted.end_state;
      right_tokens = std::move(highlighted.tokens);
    } else {
      right_tokens.clear();
    }

    ++compare_tab.syntax_rows_tokenized;
  }
}

void WorkspaceShell::RenderCompareSurface(SDL_Renderer* renderer, const SDL_FRect& rect) {
  CompareTabState* compare_tab = ActiveCompareTab();
  if (renderer == nullptr || compare_tab == nullptr || rect.w <= 0.0f || rect.h <= 0.0f) {
    return;
  }

  static const std::vector<editor::SyntaxTokenKind> kEmptyTokens;

  DrawFilledRect(renderer, rect, theme_.editor_background);

  const CompareSurfaceLayout surface = ComputeCompareSurfaceLayout(rect, *compare_tab);
  ClampCompareScrollRow(*compare_tab, surface.visible_rows);
  ClampCompareHorizontalScroll(*compare_tab, surface.visible_columns);
  const std::size_t visible_start_row = static_cast<std::size_t>(std::max(0, compare_tab->scroll_row));
  const std::size_t visible_end_row =
      visible_start_row + static_cast<std::size_t>(std::max(1, surface.visible_rows)) + 64;
  PopulateCompareSyntaxTokensForWindow(*compare_tab, visible_start_row, visible_end_row);
  compare_tab->right_viewport.SetViewportSize(static_cast<std::size_t>(surface.visible_rows),
                                              surface.visible_columns);
  compare_tab->right_viewport.SetHorizontalScroll(compare_tab->horizontal_scroll);
  const bool draw_compare_caret =
      focus_ == FocusTarget::Editor && compare_tab->right_view_active && CaretVisibleNow() &&
      !(CurrentTextInputSurface() == TextInputSurface::Editor && !text_composition_.text.empty());
  const std::optional<editor::SelectionRange> right_selection =
      compare_tab->right_editable ? compare_tab->right_viewport.selection_range() : std::nullopt;
  const std::optional<editor::EditorBlameOverlay> blame_overlay =
      compare_tab->right_editable && compare_tab->right_view_active
          ? BuildCompareBlameOverlay(*compare_tab, surface, rect)
          : std::nullopt;
  visible_editor_blame_overlay_ = blame_overlay;
  const float bottom_reserved = surface.show_horizontal ? kScrollbarReserve : 0.0f;
  const float right_reserved = surface.show_vertical ? kScrollbarReserve : 0.0f;
  const float content_width = std::max(0.0f, rect.w - right_reserved);
  const float content_height = std::max(0.0f, rect.h - bottom_reserved);
  std::size_t blame_index = 0;

  DrawFilledRect(renderer, MakeRect(rect.x, surface.rows_y - 6.0f, content_width, 1.0f), theme_.border);
  DrawFilledRect(renderer, MakeRect(surface.center_x - 6.0f, rect.y, 1.0f, content_height), theme_.border);
  DrawFilledRect(renderer, MakeRect(surface.right_x - 6.0f, rect.y, 1.0f, content_height), theme_.border);

  text_renderer_.DrawString(renderer, surface.left_x + surface.gutter_width, surface.header_y,
                            theme_.text_secondary,
                            TruncateLabel(compare_tab->left_label, surface.left_width - 8.0f));
  text_renderer_.DrawString(renderer, surface.right_x + surface.gutter_width, surface.header_y,
                            theme_.text_secondary,
                            TruncateLabel(compare_tab->right_label, surface.right_width - 8.0f));

  for (int row = 0; row < surface.visible_rows; ++row) {
    const int model_index = compare_tab->scroll_row + row;
    if (model_index >= static_cast<int>(compare_tab->model.rows.size())) {
      break;
    }

    const auto& compare_row = compare_tab->model.rows[static_cast<std::size_t>(model_index)];
    const float y = surface.rows_y + static_cast<float>(row) * surface.line_height;
    const bool selected = static_cast<std::size_t>(model_index) == compare_tab->selected_row;
    if (selected) {
      DrawFilledRect(renderer,
                     MakeRect(rect.x + 1.0f, y - 1.0f, std::max(0.0f, content_width - 2.0f),
                              surface.line_height),
                     theme_.row_highlight);
    }

    const SDL_Color row_background = selected ? theme_.row_highlight : theme_.editor_background;
    const auto draw_text = [&](float x, float width, SDL_Color color, const std::string& text) {
      const std::string display_text = TruncateLabel(text, width);
      if (display_text.empty()) {
        return;
      }
      text_renderer_.DrawStringOn(renderer, x, y, color, row_background, display_text);
    };
    const auto draw_syntax_text = [&](float x,
                                      SDL_Color plain_color,
                                      const std::string& text,
                                      const std::vector<editor::SyntaxTokenKind>& full_tokens,
                                      const std::vector<compare::CompareTextSpan>& changed_spans,
                                      SDL_Color changed_background,
                                      bool suppress_background = false) {
      if (text.empty()) {
        return;
      }

      const VisibleTextWindow window = SliceVisibleColumns(
          text, compare_tab->horizontal_scroll, surface.visible_columns);
      if (window.text.empty()) {
        return;
      }

      const auto token_kind_at = [&](std::size_t byte_offset) {
        const std::size_t absolute_offset = window.byte_offset + byte_offset;
        if (absolute_offset < full_tokens.size()) {
          return full_tokens[absolute_offset];
        }
        return editor::SyntaxTokenKind::Plain;
      };
      std::size_t changed_span_index = 0;
      const auto byte_is_changed = [&](std::size_t byte_offset) {
        const std::size_t absolute_offset = window.byte_offset + byte_offset;
        while (changed_span_index < changed_spans.size() &&
               changed_spans[changed_span_index].end <= absolute_offset) {
          ++changed_span_index;
        }
        return changed_span_index < changed_spans.size() &&
               absolute_offset >= changed_spans[changed_span_index].start &&
               absolute_offset < changed_spans[changed_span_index].end;
      };

      float segment_x = x;
      for (std::size_t segment_start = 0; segment_start < window.text.size();) {
        const editor::SyntaxTokenKind kind = token_kind_at(segment_start);
        const bool changed = byte_is_changed(segment_start);
        std::size_t segment_end = segment_start;
        while (segment_end < window.text.size()) {
          const std::size_t next = segment_end + Utf8SequenceLength(window.text, segment_end);
          if (next >= window.text.size()) {
            segment_end = window.text.size();
            break;
          }
          if (token_kind_at(next) != kind || byte_is_changed(next) != changed) {
            segment_end = next;
            break;
          }
          segment_end = next;
        }

        const std::string_view segment_text(window.text.data() + segment_start,
                                            segment_end - segment_start);
        if (suppress_background) {
          text_renderer_.DrawString(renderer, segment_x, y,
                                    CompareTokenColor(theme_, kind, plain_color, selected),
                                    segment_text);
        } else {
          text_renderer_.DrawStringOn(
              renderer, segment_x, y,
              CompareTokenColor(theme_, kind, plain_color, selected),
              changed ? changed_background : row_background, segment_text);
        }
        segment_x += text_renderer_.MeasureWidth(segment_text);
        segment_start = segment_end;
      }
    };

    if (compare_row.left_line > 0) {
      draw_text(surface.left_x, surface.gutter_width - 4.0f,
                selected ? theme_.current_line_number : theme_.line_number,
                std::to_string(compare_row.left_line));
    }
    if (compare_row.right_line > 0) {
      draw_text(surface.right_x, surface.gutter_width - 4.0f,
                selected ? theme_.current_line_number : theme_.line_number,
                std::to_string(compare_row.right_line));
    }

    SDL_Color left_color = theme_.text_secondary;
    SDL_Color right_color = theme_.text_secondary;
    SDL_Color marker_color = selected ? theme_.text_secondary : theme_.text_muted;
    char marker = ' ';
    switch (compare_row.kind) {
      case compare::CompareRowKind::Added:
        right_color = theme_.diff_added;
        marker_color = theme_.diff_added;
        marker = '+';
        break;
      case compare::CompareRowKind::Deleted:
        left_color = theme_.diff_deleted;
        marker_color = theme_.diff_deleted;
        marker = '-';
        break;
      case compare::CompareRowKind::Modified:
        left_color = theme_.diff_modified;
        right_color = theme_.diff_modified;
        marker_color = theme_.diff_modified;
        marker = '~';
        break;
      case compare::CompareRowKind::Unchanged:
      default:
        break;
    }

    const SDL_Color left_changed_background = BlendColor(
        row_background,
        compare_row.kind == compare::CompareRowKind::Deleted ? theme_.diff_deleted
                                                             : theme_.diff_modified,
        selected ? 0.42f : 0.28f);
    const SDL_Color right_changed_background = BlendColor(
        row_background,
        compare_row.kind == compare::CompareRowKind::Added ? theme_.diff_added
                                                           : theme_.diff_modified,
        selected ? 0.42f : 0.28f);

    if (compare_row.left_line > 0) {
      const std::vector<editor::SyntaxTokenKind>* cached_tokens =
          static_cast<std::size_t>(model_index) < compare_tab->left_tokens_by_row.size()
              ? &compare_tab->left_tokens_by_row[static_cast<std::size_t>(model_index)]
              : &kEmptyTokens;
      draw_syntax_text(surface.left_x + surface.gutter_width, left_color, compare_row.left_text,
                       *cached_tokens, compare_row.left_changed_spans, left_changed_background);
    }
    if (compare_row.right_line > 0) {
      const std::size_t right_line_index = static_cast<std::size_t>(compare_row.right_line - 1);
      const bool selection_active =
          right_selection.has_value() &&
          right_line_index >= right_selection->start.line &&
          right_line_index <= right_selection->end.line;
      if (selection_active) {
        const std::size_t line_start =
            right_line_index == right_selection->start.line ? right_selection->start.column : 0;
        const std::size_t line_end =
            right_line_index == right_selection->end.line ? right_selection->end.column
                                                          : compare_row.right_text.size();
        const std::size_t start_visual =
            editor::TextLayout::VisualColumnForTextColumn(compare_row.right_text, line_start,
                                                          compare_tab->right_viewport.tab_size());
        const std::size_t end_visual =
            editor::TextLayout::VisualColumnForTextColumn(compare_row.right_text, line_end,
                                                          compare_tab->right_viewport.tab_size());
        const std::size_t visible_start =
            std::max(start_visual, compare_tab->horizontal_scroll);
        const std::size_t visible_end =
            std::min(end_visual, compare_tab->horizontal_scroll + surface.visible_columns);
        if (visible_end > visible_start) {
          DrawFilledRect(
              renderer,
              MakeRect(surface.right_x + surface.gutter_width +
                           static_cast<float>(visible_start - compare_tab->horizontal_scroll) *
                               text_renderer_.CharWidth(),
                       y - 1.0f,
                       static_cast<float>(visible_end - visible_start) * text_renderer_.CharWidth(),
                       surface.line_height),
              theme_.selection_fill);
        }
      }
      const std::vector<editor::SyntaxTokenKind>* cached_tokens =
          static_cast<std::size_t>(model_index) < compare_tab->right_tokens_by_row.size()
              ? &compare_tab->right_tokens_by_row[static_cast<std::size_t>(model_index)]
              : &kEmptyTokens;
      draw_syntax_text(surface.right_x + surface.gutter_width, right_color,
                       compare_row.right_text, *cached_tokens, compare_row.right_changed_spans,
                       right_changed_background, selection_active);
      if (draw_compare_caret && right_line_index == compare_tab->right_viewport.cursor_line()) {
        const std::size_t caret_visual =
            editor::TextLayout::VisualColumnForTextColumn(compare_row.right_text,
                                                          compare_tab->right_viewport.cursor_column(),
                                                          compare_tab->right_viewport.tab_size());
        if (caret_visual >= compare_tab->horizontal_scroll &&
            caret_visual <= compare_tab->horizontal_scroll + surface.visible_columns) {
          DrawFilledRect(
              renderer,
              MakeRect(surface.right_x + surface.gutter_width +
                           static_cast<float>(caret_visual - compare_tab->horizontal_scroll) *
                               text_renderer_.CharWidth(),
                       y - 1.0f, 1.5f, surface.line_height),
              theme_.cursor);
        }
      }
      if (blame_overlay.has_value() && blame_overlay->visible) {
        while (blame_index < blame_overlay->lines.size() &&
               blame_overlay->lines[blame_index].line_index < right_line_index) {
          ++blame_index;
        }
        if (blame_index < blame_overlay->lines.size() &&
            blame_overlay->lines[blame_index].line_index == right_line_index) {
          text_renderer_.DrawStringOn(renderer, blame_overlay->lines[blame_index].rect.x,
                                     blame_overlay->lines[blame_index].rect.y, theme_.text_disabled,
                                     row_background, blame_overlay->lines[blame_index].text);
        }
      }
    }
    draw_text(surface.center_x + 4.0f, surface.divider_width - 6.0f, marker_color,
              std::string(1, marker));
  }
}

void WorkspaceShell::RenderMergeSurface(SDL_Renderer* renderer, const SDL_FRect& rect) {
  MergeTabState* merge_tab = ActiveMergeTab();
  if (renderer == nullptr || merge_tab == nullptr || rect.w <= 0.0f || rect.h <= 0.0f) {
    return;
  }

  static const std::vector<editor::SyntaxTokenKind> kEmptyTokens;

  DrawFilledRect(renderer, rect, theme_.editor_background);

  const MergeSurfaceLayout surface = ComputeMergeSurfaceLayout(rect, *merge_tab);
  merge_tab->result_viewport.SetScrollLine(static_cast<std::size_t>(std::max(0, merge_tab->scroll_row)));
  merge_tab->result_viewport.SetHorizontalScroll(merge_tab->horizontal_scroll);
  merge_tab->scroll_row = static_cast<int>(merge_tab->result_viewport.scroll_line());
  merge_tab->horizontal_scroll = merge_tab->result_viewport.horizontal_scroll();
  ClampMergeScrollRow(*merge_tab, surface.visible_rows);
  ClampMergeHorizontalScroll(*merge_tab, surface.visible_columns);
  merge_tab->result_viewport.SetScrollLine(static_cast<std::size_t>(std::max(0, merge_tab->scroll_row)));
  merge_tab->result_viewport.SetHorizontalScroll(merge_tab->horizontal_scroll);
  const float bottom_reserved = surface.show_horizontal ? kScrollbarReserve : 0.0f;
  const float right_reserved = surface.show_vertical ? kScrollbarReserve : 0.0f;
  const float content_width = std::max(0.0f, rect.w - right_reserved);
  const float content_height = std::max(0.0f, rect.h - bottom_reserved);
  const std::size_t visible_start_row = static_cast<std::size_t>(std::max(0, merge_tab->scroll_row));
  const std::size_t visible_end_row =
      visible_start_row + static_cast<std::size_t>(std::max(1, surface.visible_rows)) + 64;
  PopulateMergeSyntaxTokensForWindow(*merge_tab, visible_start_row, visible_end_row);
  const std::size_t selected_hunk =
      merge_tab->conflicts.empty() ? 0 : std::min(merge_tab->selected_hunk, merge_tab->conflicts.size() - 1);

  const auto draw_button = [&](const SDL_FRect& button_rect,
                               std::string_view label,
                               bool selected,
                               bool primary = false) {
    const SDL_Color background =
        selected ? theme_.chrome_active : primary ? theme_.surface_raised : theme_.surface_background;
    DrawFilledRect(renderer, button_rect, background);
    DrawRect(renderer, button_rect, selected ? theme_.accent : theme_.border);
    const std::string display = TruncateLabel(label, button_rect.w - 18.0f);
    const float text_x =
        button_rect.x + std::max(0.0f, (button_rect.w - text_renderer_.MeasureWidth(display)) * 0.5f);
    const float text_y =
        button_rect.y + std::max(0.0f, (button_rect.h - text_renderer_.LineHeight()) * 0.5f);
    text_renderer_.DrawStringOn(renderer, text_x, text_y,
                                selected ? theme_.text_primary : theme_.text_secondary,
                                background, display);
  };
  const auto make_button_rect = [&](float x, float y, std::string_view label) {
    const float width =
        std::clamp(text_renderer_.MeasureWidth(label) + 18.0f, 64.0f, 160.0f);
    return MakeRect(x, y, width, kMergeToolbarButtonHeight);
  };
  const auto draw_source_text = [&](float x,
                                    float y,
                                    SDL_Color plain_color,
                                    SDL_Color background,
                                    const std::string& text,
                                    const std::vector<editor::SyntaxTokenKind>& full_tokens) {
    if (text.empty()) {
      return;
    }

    const VisibleTextWindow window =
        SliceVisibleColumns(text, merge_tab->horizontal_scroll, surface.visible_columns);
    if (window.text.empty()) {
      return;
    }

    const auto token_kind_at = [&](std::size_t byte_offset) {
      const std::size_t absolute_offset = window.byte_offset + byte_offset;
      if (absolute_offset < full_tokens.size()) {
        return full_tokens[absolute_offset];
      }
      return editor::SyntaxTokenKind::Plain;
    };

    float segment_x = x;
    for (std::size_t segment_start = 0; segment_start < window.text.size();) {
      const editor::SyntaxTokenKind kind = token_kind_at(segment_start);
      std::size_t segment_end = segment_start;
      while (segment_end < window.text.size()) {
        const std::size_t next = segment_end + Utf8SequenceLength(window.text, segment_end);
        if (next >= window.text.size()) {
          segment_end = window.text.size();
          break;
        }
        if (token_kind_at(next) != kind) {
          segment_end = next;
          break;
        }
        segment_end = next;
      }

      const std::string_view segment_text(window.text.data() + segment_start,
                                          segment_end - segment_start);
      text_renderer_.DrawStringOn(
          renderer, segment_x, y, CompareTokenColor(theme_, kind, plain_color, false), background,
          segment_text);
      segment_x += text_renderer_.MeasureWidth(segment_text);
      segment_start = segment_end;
    }
  };
  const auto conflict_at_source_line =
      [&](std::size_t line, bool incoming) -> const MergeTrackedConflict* {
    for (const auto& conflict : merge_tab->conflicts) {
      const std::size_t start = incoming ? conflict.incoming_start_line : conflict.current_start_line;
      const std::size_t end = incoming ? conflict.incoming_end_line : conflict.current_end_line;
      if (line >= start && line < end) {
        return &conflict;
      }
    }
    return nullptr;
  };
  const auto conflict_rect_for_result = [&](const MergeTrackedConflict& conflict)
      -> std::optional<SDL_FRect> {
    const editor::EditorViewMetrics metrics = editor::EditorViewRenderer::ComputeMetrics(
        text_renderer_, merge_tab->result_viewport,
        MakeRect(surface.center_x, surface.rows_y - 8.0f, surface.gutter_width + surface.center_width,
                 std::max(0.0f, rect.y + content_height - (surface.rows_y - 8.0f))));
    merge_tab->result_viewport.SetViewportSize(metrics.visible_rows, metrics.visible_columns);
    const std::size_t scroll_line = merge_tab->result_viewport.scroll_line();
    const std::size_t visible_end_line = scroll_line + metrics.visible_rows;
    const std::size_t rect_start = std::max(conflict.start_line, scroll_line);
    const std::size_t rect_end =
        std::max(conflict.end_line, conflict.start_line + 1);
    if (rect_end <= scroll_line || rect_start >= visible_end_line) {
      return std::nullopt;
    }
    const float y =
        metrics.first_line_y + static_cast<float>(rect_start - scroll_line) * metrics.line_height;
    const float h =
        static_cast<float>(std::min(rect_end, visible_end_line) - rect_start) * metrics.line_height;
    return MakeRect(surface.center_x, y - 1.0f, surface.gutter_width + surface.center_width, h);
  };
  const auto source_button_rect = [&](const MergeTrackedConflict& conflict, bool incoming) {
    const std::size_t end_line =
        incoming ? conflict.incoming_end_line : conflict.current_end_line;
    const float x = incoming ? surface.left_x + surface.gutter_width : surface.right_x + surface.gutter_width;
    float y = surface.rows_y +
              static_cast<float>(static_cast<long long>(end_line) - merge_tab->scroll_row) *
                  surface.line_height +
              2.0f;
    y = std::min(y, rect.y + content_height - kMergeToolbarButtonHeight - 4.0f);
    return make_button_rect(x, y, incoming ? "Accept Incoming" : "Accept Current");
  };
  const auto result_action_rects =
      [&](const MergeTrackedConflict& conflict) -> std::array<SDL_FRect, 4> {
    const std::optional<SDL_FRect> conflict_rect = conflict_rect_for_result(conflict);
    float y = conflict_rect.has_value()
                  ? conflict_rect->y + conflict_rect->h + 2.0f
                  : surface.rows_y + 2.0f;
    if (y + kMergeToolbarButtonHeight > rect.y + content_height - 4.0f && conflict_rect.has_value()) {
      y = std::max(surface.rows_y + 2.0f, conflict_rect->y - kMergeToolbarButtonHeight - 2.0f);
    }
    float x = surface.center_x + surface.gutter_width;
    const SDL_FRect base_rect = make_button_rect(x, y, "Base");
    x += base_rect.w + kMergeToolbarButtonGap;
    const SDL_FRect incoming_rect = make_button_rect(x, y, "Incoming");
    x += incoming_rect.w + kMergeToolbarButtonGap;
    const SDL_FRect current_rect = make_button_rect(x, y, "Current");
    x += current_rect.w + kMergeToolbarButtonGap;
    const SDL_FRect both_rect = make_button_rect(x, y, "Both");
    return {base_rect, incoming_rect, current_rect, both_rect};
  };
  const auto preview_choice = [&]() -> std::optional<std::pair<std::size_t, compare::MergeChoice>> {
    if (!merge_tab->hover_state.has_value()) {
      return std::nullopt;
    }
    switch (merge_tab->hover_state->kind) {
      case MergeHoverState::Kind::IncomingConflict:
        case MergeHoverState::Kind::IncomingAccept:
        case MergeHoverState::Kind::CurrentConflict:
        case MergeHoverState::Kind::CurrentAccept:
        case MergeHoverState::Kind::ResultAction:
        return std::pair<std::size_t, compare::MergeChoice>{
            merge_tab->hover_state->conflict_index,
            merge_tab->hover_state->preview_choice,
        };
      case MergeHoverState::Kind::None:
      case MergeHoverState::Kind::ResultConflict:
      default:
        return std::nullopt;
    }
  }();

  const MergeToolbarLayout toolbar = ComputeMergeToolbarLayout(rect, surface);
  draw_button(toolbar.prev_rect, "Prev", false, !merge_tab->conflicts.empty());
  draw_button(toolbar.next_rect, "Next", false, !merge_tab->conflicts.empty());
  draw_button(toolbar.save_rect, "Save", false, true);
  draw_button(toolbar.open_rect, "Open Result", false, true);

  DrawFilledRect(renderer, MakeRect(rect.x, surface.rows_y - 6.0f, content_width, 1.0f),
                 theme_.border);
  DrawFilledRect(renderer,
                 MakeRect(surface.center_x - surface.divider_width * 0.5f, rect.y, 1.0f, content_height),
                 drag_target_ == DragTarget::MergeLeftDivider ? theme_.accent : theme_.border);
  DrawFilledRect(renderer,
                 MakeRect(surface.right_x - surface.divider_width * 0.5f, rect.y, 1.0f, content_height),
                 drag_target_ == DragTarget::MergeRightDivider ? theme_.accent : theme_.border);

  text_renderer_.DrawString(renderer, surface.left_x + surface.gutter_width, surface.header_y,
                            theme_.text_secondary,
                            TruncateLabel(merge_tab->incoming_label, surface.left_width - 8.0f));
  text_renderer_.DrawString(renderer, surface.center_x + surface.gutter_width, surface.header_y,
                            theme_.text_secondary,
                            TruncateLabel(merge_tab->result_label, surface.center_width - 8.0f));
  text_renderer_.DrawString(renderer, surface.right_x + surface.gutter_width, surface.header_y,
                            theme_.text_secondary,
                            TruncateLabel(merge_tab->current_label, surface.right_width - 8.0f));

  for (int row = 0; row < surface.visible_rows; ++row) {
    const std::size_t line_index =
        static_cast<std::size_t>(std::max(0, merge_tab->scroll_row + row));
    const float y = surface.rows_y + static_cast<float>(row) * surface.line_height;
    const MergeTrackedConflict* incoming_conflict = conflict_at_source_line(line_index, true);
    const MergeTrackedConflict* current_conflict = conflict_at_source_line(line_index, false);
    const bool selected_incoming =
        incoming_conflict != nullptr &&
        static_cast<std::size_t>(incoming_conflict - merge_tab->conflicts.data()) == selected_hunk;
    const bool selected_current =
        current_conflict != nullptr &&
        static_cast<std::size_t>(current_conflict - merge_tab->conflicts.data()) == selected_hunk;

    if (line_index < merge_tab->model.incoming_lines.size()) {
      const SDL_Color background =
          incoming_conflict != nullptr
              ? BlendColor(selected_incoming ? theme_.row_highlight : theme_.editor_background,
                           incoming_conflict->valid ? theme_.diff_added : theme_.diff_deleted,
                           selected_incoming ? 0.42f : 0.24f)
              : (selected_incoming ? theme_.row_highlight : theme_.editor_background);
      const SDL_Color number_color =
          selected_incoming ? theme_.current_line_number : theme_.line_number;
      text_renderer_.DrawStringOn(renderer, surface.left_x, y, number_color, background,
                                  std::to_string(line_index + 1));
      const std::vector<editor::SyntaxTokenKind>& tokens =
          line_index < merge_tab->incoming_tokens.size() ? merge_tab->incoming_tokens[line_index]
                                                         : kEmptyTokens;
      draw_source_text(surface.left_x + surface.gutter_width, y,
                       incoming_conflict != nullptr ? theme_.diff_added : theme_.text_secondary,
                       background, merge_tab->model.incoming_lines[line_index], tokens);
    }

    if (line_index < merge_tab->model.current_lines.size()) {
      const SDL_Color background =
          current_conflict != nullptr
              ? BlendColor(selected_current ? theme_.row_highlight : theme_.editor_background,
                           current_conflict->valid ? theme_.diff_modified : theme_.diff_deleted,
                           selected_current ? 0.42f : 0.24f)
              : (selected_current ? theme_.row_highlight : theme_.editor_background);
      const SDL_Color number_color =
          selected_current ? theme_.current_line_number : theme_.line_number;
      text_renderer_.DrawStringOn(renderer, surface.right_x, y, number_color, background,
                                  std::to_string(line_index + 1));
      const std::vector<editor::SyntaxTokenKind>& tokens =
          line_index < merge_tab->current_tokens.size() ? merge_tab->current_tokens[line_index]
                                                        : kEmptyTokens;
      draw_source_text(surface.right_x + surface.gutter_width, y,
                       current_conflict != nullptr ? theme_.diff_modified : theme_.text_secondary,
                       background, merge_tab->model.current_lines[line_index], tokens);
    }
  }

  const SDL_FRect result_rect =
      MakeRect(surface.center_x, surface.rows_y - 8.0f, surface.gutter_width + surface.center_width,
               std::max(0.0f, rect.y + content_height - (surface.rows_y - 8.0f)));
  const std::optional<editor::EditorBlameOverlay> merge_blame_overlay =
      BuildEditorBlameOverlay(merge_tab->result_viewport, result_rect, 280.0f);
  visible_editor_blame_overlay_ = merge_blame_overlay;
  editor_view_renderer_.Render(renderer, text_renderer_, theme_, merge_tab->result_viewport, result_rect,
                               focus_ == FocusTarget::Editor && CaretVisibleNow(), "", std::nullopt,
                               merge_blame_overlay);
  merge_tab->scroll_row = static_cast<int>(merge_tab->result_viewport.scroll_line());
  merge_tab->horizontal_scroll = merge_tab->result_viewport.horizontal_scroll();

  for (std::size_t i = 0; i < merge_tab->conflicts.size(); ++i) {
    const auto& conflict = merge_tab->conflicts[i];
    const std::optional<SDL_FRect> conflict_rect = conflict_rect_for_result(conflict);
    if (!conflict_rect.has_value()) {
      continue;
    }
    const SDL_Color border =
        !conflict.valid ? theme_.diff_deleted
        : i == selected_hunk ? theme_.accent
                             : theme_.border;
    DrawRect(renderer, *conflict_rect, border);
  }

  if (preview_choice.has_value() && preview_choice->first < merge_tab->conflicts.size()) {
    const auto& conflict = merge_tab->conflicts[preview_choice->first];
    if (conflict.valid && conflict.hunk_index < merge_tab->model.hunks.size()) {
      const std::vector<std::string> preview_lines =
          compare::MergeChoiceLines(merge_tab->model.hunks[conflict.hunk_index], preview_choice->second);
      const editor::EditorViewMetrics metrics = editor::EditorViewRenderer::ComputeMetrics(
          text_renderer_, merge_tab->result_viewport, result_rect);
      merge_tab->result_viewport.SetViewportSize(metrics.visible_rows, metrics.visible_columns);
      const std::size_t scroll_line = merge_tab->result_viewport.scroll_line();
      const std::size_t preview_start = std::max(conflict.start_line, scroll_line);
      const std::size_t preview_height_lines =
          std::max(preview_lines.size(), conflict.end_line > conflict.start_line
                                          ? conflict.end_line - conflict.start_line
                                          : std::size_t{1});
      const float preview_y =
          metrics.first_line_y + static_cast<float>(preview_start - scroll_line) * metrics.line_height;
      const SDL_FRect preview_rect =
          MakeRect(result_rect.x, preview_y - 1.0f, result_rect.w,
                   static_cast<float>(preview_height_lines) * metrics.line_height);
      DrawFilledRect(renderer, preview_rect,
                     BlendColor(theme_.editor_background, theme_.diff_modified, 0.18f));
      for (std::size_t line = 0; line < preview_lines.size(); ++line) {
        const float y = preview_y + static_cast<float>(line) * metrics.line_height;
        text_renderer_.DrawStringOn(renderer, result_rect.x, y, theme_.line_number,
                                    theme_.editor_background,
                                    std::to_string(conflict.start_line + line + 1));
        const VisibleTextWindow window =
            SliceVisibleColumns(preview_lines[line], merge_tab->horizontal_scroll, metrics.visible_columns);
        if (window.text.empty()) {
          continue;
        }
        text_renderer_.DrawStringOn(renderer, metrics.text_x, y, theme_.text_primary,
                                    theme_.editor_background, window.text);
      }
      DrawRect(renderer, preview_rect, theme_.accent);
    }
  }

  if (merge_tab->hover_state.has_value() &&
      (merge_tab->hover_state->kind == MergeHoverState::Kind::IncomingConflict ||
       merge_tab->hover_state->kind == MergeHoverState::Kind::IncomingAccept) &&
      merge_tab->hover_state->conflict_index < merge_tab->conflicts.size()) {
    const auto& conflict = merge_tab->conflicts[merge_tab->hover_state->conflict_index];
    if (conflict.valid) {
      draw_button(source_button_rect(conflict, true), "Accept Incoming",
                  merge_tab->hover_state->kind == MergeHoverState::Kind::IncomingAccept, true);
    }
  }

  if (merge_tab->hover_state.has_value() &&
      (merge_tab->hover_state->kind == MergeHoverState::Kind::CurrentConflict ||
       merge_tab->hover_state->kind == MergeHoverState::Kind::CurrentAccept) &&
      merge_tab->hover_state->conflict_index < merge_tab->conflicts.size()) {
    const auto& conflict = merge_tab->conflicts[merge_tab->hover_state->conflict_index];
    if (conflict.valid) {
      draw_button(source_button_rect(conflict, false), "Accept Current",
                  merge_tab->hover_state->kind == MergeHoverState::Kind::CurrentAccept, true);
    }
  }

  if (merge_tab->hover_state.has_value() &&
      (merge_tab->hover_state->kind == MergeHoverState::Kind::ResultConflict ||
       merge_tab->hover_state->kind == MergeHoverState::Kind::ResultAction) &&
      merge_tab->hover_state->conflict_index < merge_tab->conflicts.size()) {
    const auto& conflict = merge_tab->conflicts[merge_tab->hover_state->conflict_index];
    if (conflict.valid) {
      const auto action_rects = result_action_rects(conflict);
      draw_button(action_rects[0], "Base",
                  merge_tab->hover_state->kind == MergeHoverState::Kind::ResultAction &&
                      merge_tab->hover_state->preview_choice == compare::MergeChoice::Base,
                  true);
      draw_button(action_rects[1], "Incoming",
                  merge_tab->hover_state->kind == MergeHoverState::Kind::ResultAction &&
                      merge_tab->hover_state->preview_choice == compare::MergeChoice::Incoming,
                  true);
      draw_button(action_rects[2], "Current",
                  merge_tab->hover_state->kind == MergeHoverState::Kind::ResultAction &&
                      merge_tab->hover_state->preview_choice == compare::MergeChoice::Current,
                  true);
      draw_button(action_rects[3], "Both",
                  merge_tab->hover_state->kind == MergeHoverState::Kind::ResultAction &&
                      merge_tab->hover_state->preview_choice == compare::MergeChoice::Both,
                  true);
    }
  }
}

}  // namespace microide::workspace
