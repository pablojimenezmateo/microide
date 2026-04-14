#include "workspace/WorkspaceShell.h"

#include <array>
#include <algorithm>
#include <cmath>
#include <string_view>
#include <vector>

#include "editor/SyntaxHighlighter.h"
#include "editor/TextLayout.h"
#include "util/StringUtil.h"
#include "workspace/WorkspaceShellShared.h"

namespace microide::workspace {

namespace {

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

SDL_Color CompareMarkerColor(const render::Theme& theme, compare::CompareRowKind kind) {
  switch (kind) {
    case compare::CompareRowKind::Added:
      return theme.diff_added;
    case compare::CompareRowKind::Deleted:
      return theme.diff_deleted;
    case compare::CompareRowKind::Modified:
      return theme.diff_modified;
    case compare::CompareRowKind::Unchanged:
    default:
      return theme.text_muted;
  }
}

void DrawCompareScrollbarMarkers(SDL_Renderer* renderer,
                                 const render::Theme& theme,
                                 const SDL_FRect& track,
                                 const compare::CompareModel& model) {
  if (renderer == nullptr) {
    return;
  }

  const auto markers = BuildCompareScrollbarMarkers(track, model);
  for (const CompareScrollbarMarker& marker : markers) {
    const SDL_Color color = CompareMarkerColor(theme, marker.kind);
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    SDL_RenderFillRect(renderer, &marker.rect);
  }
}

void DrawScrollbarTrack(SDL_Renderer* renderer,
                        const render::Theme& theme,
                        const SDL_FRect& track) {
  if (renderer == nullptr || track.w <= 0.0f || track.h <= 0.0f) {
    return;
  }

  SDL_SetRenderDrawColor(renderer, theme.surface_raised.r, theme.surface_raised.g,
                         theme.surface_raised.b, theme.surface_raised.a);
  SDL_RenderFillRect(renderer, &track);
}

void DrawScrollbarThumb(SDL_Renderer* renderer,
                        const render::Theme& theme,
                        const SDL_FRect& thumb,
                        bool active = false) {
  if (renderer == nullptr || thumb.w <= 0.0f || thumb.h <= 0.0f) {
    return;
  }

  const SDL_Color thumb_color = active ? theme.accent : theme.text_disabled;
  SDL_SetRenderDrawColor(renderer, thumb_color.r, thumb_color.g, thumb_color.b, thumb_color.a);
  SDL_RenderFillRect(renderer, &thumb);
}

void DrawScrollbar(SDL_Renderer* renderer,
                   const render::Theme& theme,
                   const SDL_FRect& track,
                   const SDL_FRect& thumb,
                   bool active = false) {
  DrawScrollbarTrack(renderer, theme, track);
  DrawScrollbarThumb(renderer, theme, thumb, active);
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
  const TextGridInteractionLayout right_interaction =
      BuildCompareRightInteractionLayout(surface, *compare_tab);
  const bool draw_compare_caret =
      surface_.focus == FocusTarget::Editor && compare_tab->right_view_active && CaretVisibleNow() &&
      !(CurrentTextInputSurface() == TextInputSurface::Editor && !text_composition_.text.empty());
  const std::optional<editor::SelectionRange> right_selection =
      compare_tab->right_editable ? compare_tab->right_viewport.selection_range() : std::nullopt;
  const std::optional<editor::EditorBlameOverlay> blame_overlay =
      compare_tab->right_editable && compare_tab->right_view_active
          ? BuildCompareBlameOverlay(*compare_tab, surface, rect)
          : std::nullopt;
  visible_editor_blame_overlay_ = blame_overlay;
  const float bottom_reserved =
      surface.show_horizontal ? kWorkspaceDiffScrollbarReserve : 0.0f;
  const float right_reserved =
      surface.show_vertical ? kWorkspaceDiffScrollbarReserve : 0.0f;
  const float content_width = std::max(0.0f, rect.w - right_reserved);
  const float content_height = std::max(0.0f, rect.h - bottom_reserved);
  const float divider_x = surface.center_x;
  std::size_t blame_index = 0;

  DrawFilledRect(renderer, MakeRect(rect.x, surface.rows_y - 6.0f, content_width, 1.0f), theme_.border);
  DrawFilledRect(renderer,
                 MakeRect(surface.center_x - 1.0f, rect.y, 1.0f, content_height),
                 theme_.border);
  DrawFilledRect(renderer, MakeRect(surface.right_x - 1.0f, rect.y, 1.0f, content_height),
                 theme_.border);

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
                                      std::size_t visible_columns,
                                      SDL_Color plain_color,
                                      const std::string& text,
                                      const std::vector<editor::SyntaxTokenKind>& full_tokens,
                                      const std::vector<compare::CompareTextSpan>& changed_spans,
                                      SDL_Color changed_background,
                                      bool suppress_background = false) {
      if (text.empty()) {
        return;
      }

      const VisibleTextWindow window =
          SliceVisibleColumns(text, compare_tab->horizontal_scroll, visible_columns);
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
          const std::size_t next =
              segment_end + util::Utf8SequenceLength(window.text, segment_end);
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
      draw_syntax_text(surface.left_x + surface.gutter_width, surface.left_visible_columns,
                       left_color, compare_row.left_text, *cached_tokens,
                       compare_row.left_changed_spans, left_changed_background);
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
            std::min(end_visual,
                     compare_tab->horizontal_scroll + surface.right_visible_columns);
        if (visible_end > visible_start) {
          DrawFilledRect(
              renderer,
              MakeRect(TextGridCursorX(right_interaction, visible_start),
                       y - 1.0f,
                       static_cast<float>(visible_end - visible_start) * right_interaction.char_width,
                       surface.line_height),
              theme_.selection_fill);
        }
      }
      const std::vector<editor::SyntaxTokenKind>* cached_tokens =
          static_cast<std::size_t>(model_index) < compare_tab->right_tokens_by_row.size()
              ? &compare_tab->right_tokens_by_row[static_cast<std::size_t>(model_index)]
              : &kEmptyTokens;
      draw_syntax_text(right_interaction.text_x, surface.right_visible_columns, right_color,
                       compare_row.right_text, *cached_tokens, compare_row.right_changed_spans,
                       right_changed_background, selection_active);
      if (draw_compare_caret && right_line_index == compare_tab->right_viewport.cursor_line()) {
        const std::size_t caret_visual =
            editor::TextLayout::VisualColumnForTextColumn(compare_row.right_text,
                                                          compare_tab->right_viewport.cursor_column(),
                                                          compare_tab->right_viewport.tab_size());
        if (caret_visual >= compare_tab->horizontal_scroll &&
            caret_visual <= compare_tab->horizontal_scroll + surface.right_visible_columns) {
          DrawFilledRect(
              renderer, MakeRect(TextGridCursorX(right_interaction, caret_visual), y - 1.0f, 1.5f,
                                 surface.line_height),
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
    draw_text(divider_x, surface.divider_width, marker_color, std::string(1, marker));
  }
}

void WorkspaceShell::RenderCompareScrollbars(SDL_Renderer* renderer, const SDL_FRect& editor_surface) {
  CompareTabState* compare_tab = ActiveCompareTab();
  if (renderer == nullptr || compare_tab == nullptr) {
    return;
  }

  const CompareSurfaceLayout surface_layout =
      ComputeCompareSurfaceLayout(editor_surface, *compare_tab);
  const auto scroll_layout =
      ComputeCompareScrollLayout(editor_surface, surface_layout, *compare_tab);
  compare_tab->scroll_row = scroll_layout.vertical_scroll;
  compare_tab->horizontal_scroll = scroll_layout.horizontal_scroll;

  if (scroll_layout.vertical_scrollbar.has_value()) {
    const SDL_FRect marker_lane = MakeRect(
        std::max(editor_surface.x,
                 scroll_layout.vertical_scrollbar->track.x - kWorkspaceDiffMarkerLaneGap -
                     kWorkspaceDiffMarkerLaneWidth),
        scroll_layout.vertical_scrollbar->track.y, kWorkspaceDiffMarkerLaneWidth,
        scroll_layout.vertical_scrollbar->track.h);
    const SDL_FRect marker_inner_lane =
        MakeRect(marker_lane.x + 1.0f, marker_lane.y + 1.0f, std::max(0.0f, marker_lane.w - 2.0f),
                 std::max(0.0f, marker_lane.h - 2.0f));
    DrawFilledRect(renderer, marker_lane, theme_.surface_raised);
    DrawRect(renderer, marker_lane, theme_.border);
    DrawCompareScrollbarMarkers(renderer, theme_, marker_inner_lane, compare_tab->model);
    DrawScrollbarTrack(renderer, theme_, scroll_layout.vertical_scrollbar->track);
    DrawScrollbarThumb(renderer, theme_, scroll_layout.vertical_scrollbar->thumb,
                       surface_.drag_target == DragTarget::CompareVerticalScrollbar);
  }

  if (scroll_layout.horizontal_scrollbar.has_value()) {
    DrawScrollbar(renderer, theme_, scroll_layout.horizontal_scrollbar->track,
                  scroll_layout.horizontal_scrollbar->thumb,
                  surface_.drag_target == DragTarget::CompareHorizontalScrollbar);
  }
}

}  // namespace microide::workspace
