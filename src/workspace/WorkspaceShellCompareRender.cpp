#include "workspace/WorkspaceShell.h"

#include <array>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <string_view>
#include <vector>

#include "editor/DecoratedTextGridRenderer.h"
#include "editor/DiagnosticsRender.h"
#include "editor/SyntaxHighlighter.h"
#include "editor/TextLayout.h"
#include "util/PerformanceTrace.h"
#include "workspace/WorkspaceLayout.h"

namespace microide::workspace {

namespace {

constexpr float kDiffRowTint = 0.16f;
constexpr float kDiffRowTintSelected = 0.24f;

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

std::string_view FormatLineNumber(std::size_t value, std::array<char, 20>& scratch) {
  const auto [end, ec] =
      std::to_chars(scratch.data(), scratch.data() + scratch.size(), value);
  if (ec != std::errc{}) {
    return {};
  }
  return std::string_view(scratch.data(),
                          static_cast<std::size_t>(end - scratch.data()));
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

void WorkspaceShell::RenderCompareSurface(SDL_Renderer* renderer,
                                          const SDL_FRect& rect,
                                          CompareTabState& compare_tab_state,
                                          bool draw_compare_caret,
                                          const editor::DiagnosticsStore& diagnostics_store) {
  CompareTabState* compare_tab = &compare_tab_state;
  if (renderer == nullptr || rect.w <= 0.0f || rect.h <= 0.0f) {
    return;
  }

  util::PerformanceTrace::Scope trace_scope("WorkspaceShell::RenderCompareSurface");
  ++render_compare_surface_invocation_count_;
  static const std::vector<editor::SyntaxTokenKind> kEmptyTokens;
  static const editor::DecoratedTextGridRenderer kDecoratedRowRenderer;

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
  const std::optional<editor::SelectionRange> right_selection =
      compare_tab->right_view_active ? compare_tab->right_viewport.selection_range() : std::nullopt;
  const std::optional<editor::EditorBlameOverlay> blame_overlay =
      compare_tab->right_editable && compare_tab->right_view_active
          ? BuildCompareBlameOverlay(*compare_tab, surface, rect)
          : std::nullopt;
  const auto* right_diagnostics =
      compare_tab->right_editable && !compare_tab->right_viewport.path().empty() &&
              !compare_tab->right_viewport.dirty()
          ? diagnostics_store.FindByPath(compare_tab->right_viewport.path())
          : nullptr;
  visible_editor_blame_overlay_ = blame_overlay;
  const float bottom_reserved =
      surface.show_horizontal ? kWorkspaceDiffScrollbarReserve : 0.0f;
  const float right_reserved =
      surface.show_vertical ? kWorkspaceDiffScrollbarReserve : 0.0f;
  const float content_width = std::max(0.0f, rect.w - right_reserved);
  const float content_height = std::max(0.0f, rect.h - bottom_reserved);
  const float divider_x = surface.center_x + std::min(1.0f, std::max(0.0f, surface.divider_width - 1.0f));
  std::size_t blame_index = 0;

  DrawFilledRect(renderer, MakeRect(rect.x, surface.rows_y - 6.0f, content_width, 1.0f), theme_.border);
  DrawFilledRect(renderer,
                 MakeRect(surface.center_x, rect.y, 1.0f, content_height),
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
      DrawFilledRect(renderer, MakeRect(rect.x + 1.0f, y - 1.0f, 2.0f, surface.line_height),
                     theme_.accent);
      DrawFilledRect(renderer,
                     MakeRect(rect.x + content_width - 3.0f, y - 1.0f, 2.0f, surface.line_height),
                     theme_.accent);
    }

    const auto draw_text = [&](float x, float width, SDL_Color color, std::string_view text) {
      const std::string display_text = TruncateLabel(text, width);
      if (display_text.empty()) {
        return;
      }
      text_renderer_.DrawString(renderer, x, y, color, display_text);
    };
    const auto append_changed_underlines =
        [&](editor::DecoratedTextRow& row_desc,
            float x,
            std::size_t visible_columns,
            const std::string& text,
            const std::vector<compare::CompareTextSpan>& changed_spans,
            SDL_Color underline_color) {
          if (text.empty() || changed_spans.empty()) {
            return;
          }

          const editor::VisibleTextWindow window =
              editor::SliceVisibleColumns(text, compare_tab->horizontal_scroll, visible_columns);
          if (window.text.empty()) {
            return;
          }
          const std::size_t window_end = window.byte_offset + window.text.size();

          for (const auto& span : changed_spans) {
            if (span.end <= window.byte_offset) {
              continue;
            }
            if (span.start >= window_end) {
              break;
            }

            const std::size_t clipped_start = std::max(span.start, window.byte_offset);
            const std::size_t clipped_end = std::min(span.end, window_end);
            if (clipped_end <= clipped_start) {
              continue;
            }

            const std::size_t local_start = clipped_start - window.byte_offset;
            const std::size_t local_end = clipped_end - window.byte_offset;
            const std::string_view prefix_text(window.text.data(), local_start);
            const std::string_view changed_text(window.text.data() + local_start,
                                                local_end - local_start);
            const float start_x = x + text_renderer_.MeasureWidth(prefix_text);
            const float span_width = text_renderer_.MeasureWidth(changed_text);
            if (span_width <= 0.0f) {
              continue;
            }
            row_desc.underlines.push_back(editor::DecoratedUnderline{
                .rect = MakeRect(start_x, y + surface.line_height - 2.0f, span_width, 1.0f),
                .color =
                    SDL_Color{underline_color.r, underline_color.g, underline_color.b,
                              static_cast<Uint8>(std::clamp(
                                  std::lround(static_cast<double>(underline_color.a) * 0.55), 0l,
                                  255l))},
            });
          }
        };
    const SDL_Color neutral_text_color = selected ? theme_.text_primary : theme_.text_secondary;
    SDL_Color left_color = neutral_text_color;
    SDL_Color right_color = neutral_text_color;
    SDL_Color marker_color = selected ? theme_.text_secondary : theme_.text_muted;
    char marker = ' ';
    switch (compare_row.kind) {
      case compare::CompareRowKind::Added:
        marker_color = theme_.diff_added;
        marker = '+';
        break;
      case compare::CompareRowKind::Deleted:
        marker_color = theme_.diff_deleted;
        marker = '-';
        break;
      case compare::CompareRowKind::Modified:
        marker_color = theme_.diff_modified;
        marker = '~';
        break;
      case compare::CompareRowKind::Unchanged:
      default:
        break;
    }
    const SDL_Color left_row_background = [&]() {
      const SDL_Color base = selected ? theme_.row_highlight : theme_.editor_background;
      switch (compare_row.kind) {
        case compare::CompareRowKind::Deleted:
          return BlendColor(base, theme_.diff_deleted,
                            selected ? kDiffRowTintSelected : kDiffRowTint);
        case compare::CompareRowKind::Modified:
          return BlendColor(base, theme_.diff_modified,
                            selected ? kDiffRowTintSelected : kDiffRowTint);
        case compare::CompareRowKind::Added:
        case compare::CompareRowKind::Unchanged:
        default:
          return base;
      }
    }();
    const SDL_Color right_row_background = [&]() {
      const SDL_Color base = selected ? theme_.row_highlight : theme_.editor_background;
      switch (compare_row.kind) {
        case compare::CompareRowKind::Added:
          return BlendColor(base, theme_.diff_added,
                            selected ? kDiffRowTintSelected : kDiffRowTint);
        case compare::CompareRowKind::Modified:
          return BlendColor(base, theme_.diff_modified,
                            selected ? kDiffRowTintSelected : kDiffRowTint);
        case compare::CompareRowKind::Deleted:
        case compare::CompareRowKind::Unchanged:
        default:
          return base;
      }
    }();

    if (compare_row.left_line > 0) {
      std::array<char, 20> line_number_buf;
      editor::DecoratedTextRow left_row;
      left_row.fills.push_back(editor::DecoratedTextFill{
          .rect = MakeRect(surface.left_x, y - 1.0f,
                           surface.gutter_width + surface.left_width, surface.line_height),
          .color = left_row_background,
      });
      const std::vector<editor::SyntaxTokenKind>* cached_tokens =
          static_cast<std::size_t>(model_index) < compare_tab->left_tokens_by_row.size()
              ? &compare_tab->left_tokens_by_row[static_cast<std::size_t>(model_index)]
              : &kEmptyTokens;
      editor::AppendVisibleSyntaxTextRuns(
          left_row, text_renderer_, theme_, surface.left_x + surface.gutter_width, y,
          compare_row.left_text, compare_tab->horizontal_scroll, surface.left_visible_columns,
          selected ? theme_.text_primary : left_color, *cached_tokens);
      append_changed_underlines(
          left_row, surface.left_x + surface.gutter_width, surface.left_visible_columns,
          compare_row.left_text, compare_row.left_changed_spans,
          compare_row.kind == compare::CompareRowKind::Deleted ? theme_.diff_deleted
                                                               : theme_.diff_modified);
      kDecoratedRowRenderer.RenderRow(renderer, text_renderer_, left_row);
      draw_text(surface.left_x, surface.gutter_width - 4.0f,
                selected ? theme_.current_line_number : theme_.line_number,
                FormatLineNumber(static_cast<std::size_t>(compare_row.left_line), line_number_buf));
    }
    if (compare_row.right_line > 0) {
      std::array<char, 20> line_number_buf;
      editor::DecoratedTextRow right_row;
      right_row.fills.push_back(editor::DecoratedTextFill{
          .rect = MakeRect(surface.right_x, y - 1.0f,
                           surface.gutter_width + surface.right_width, surface.line_height),
          .color = right_row_background,
      });
      const std::size_t right_line_index = static_cast<std::size_t>(compare_row.right_line - 1);
      if (right_selection.has_value()) {
        // Copy out of the optional so GCC's optimizer sees a definitely-initialized
        // SelectionRange instead of complaining about `*right_selection` storage
        // bytes through the inlined `std::optional` access path.
        const editor::SelectionRange sel = *right_selection;
        if (right_line_index >= sel.start.line && right_line_index <= sel.end.line) {
        const std::size_t line_start =
            right_line_index == sel.start.line ? sel.start.column : 0;
        const std::size_t line_end =
            right_line_index == sel.end.line ? sel.end.column
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
          right_row.fills.push_back(editor::DecoratedTextFill{
              .rect = MakeRect(
                  TextGridCursorX(right_interaction, visible_start), y - 1.0f,
                  static_cast<float>(visible_end - visible_start) * right_interaction.char_width,
                  surface.line_height),
              .color = theme_.selection_fill,
          });
        }
        }
      }
      const std::vector<editor::SyntaxTokenKind>* cached_tokens =
          static_cast<std::size_t>(model_index) < compare_tab->right_tokens_by_row.size()
              ? &compare_tab->right_tokens_by_row[static_cast<std::size_t>(model_index)]
              : &kEmptyTokens;
      editor::AppendVisibleSyntaxTextRuns(
          right_row, text_renderer_, theme_, right_interaction.text_x, y, compare_row.right_text,
          compare_tab->horizontal_scroll, surface.right_visible_columns,
          selected ? theme_.text_primary : right_color, *cached_tokens);
      append_changed_underlines(
          right_row, right_interaction.text_x, surface.right_visible_columns,
          compare_row.right_text, compare_row.right_changed_spans,
          compare_row.kind == compare::CompareRowKind::Added ? theme_.diff_added
                                                             : theme_.diff_modified);
      if (right_diagnostics != nullptr) {
        editor::AppendDiagnosticUnderlines(
            right_row, text_renderer_, theme_, right_interaction.text_x, y, surface.line_height,
            compare_row.right_text, right_line_index, compare_tab->horizontal_scroll,
            surface.right_visible_columns, compare_tab->right_viewport.tab_size(),
            std::span<const editor::PublishedDiagnostic>(*right_diagnostics));
      }
      kDecoratedRowRenderer.RenderRow(renderer, text_renderer_, right_row);
      if (right_diagnostics != nullptr) {
        if (const auto severity = editor::HighestDiagnosticSeverityForLine(
                std::span<const editor::PublishedDiagnostic>(*right_diagnostics), right_line_index);
            severity.has_value()) {
          editor::DrawDiagnosticGutterMarker(renderer, theme_, surface.right_x, y,
                                             surface.gutter_width, surface.line_height,
                                             *severity);
        }
      }
      draw_text(surface.right_x, surface.gutter_width - 4.0f,
                selected ? theme_.current_line_number : theme_.line_number,
                FormatLineNumber(static_cast<std::size_t>(compare_row.right_line), line_number_buf));
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
                                     blame_overlay->lines[blame_index].rect.y,
                                     theme_.text_disabled, right_row_background,
                                     blame_overlay->lines[blame_index].text);
        }
      }
    }
    draw_text(divider_x, surface.divider_width, marker_color, std::string(1, marker));
  }
}

void WorkspaceShell::RenderCompareScrollbars(SDL_Renderer* renderer,
                                             const SDL_FRect& editor_surface,
                                             CompareTabState& compare_tab_state) {
  CompareTabState* compare_tab = &compare_tab_state;
  if (renderer == nullptr) {
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
                       context_.interaction_state.drag_target == DragTarget::CompareVerticalScrollbar);
  }

  if (scroll_layout.horizontal_scrollbar.has_value()) {
    DrawScrollbar(renderer, theme_, scroll_layout.horizontal_scrollbar->track,
                  scroll_layout.horizontal_scrollbar->thumb,
                  context_.interaction_state.drag_target == DragTarget::CompareHorizontalScrollbar);
  }
}

}  // namespace microide::workspace
