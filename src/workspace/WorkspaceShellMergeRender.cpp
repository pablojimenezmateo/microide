#include "workspace/WorkspaceShell.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <string_view>
#include <vector>

#include "editor/SyntaxHighlighter.h"
#include "editor/TextLayout.h"
#include "util/StringUtil.h"
#include "workspace/WorkspaceShellShared.h"

namespace microide::workspace {

namespace {

constexpr float kScrollbarReserve = 12.0f;
constexpr float kCompareMarkerLaneWidth = 6.0f;
constexpr float kCompareMarkerLaneGap = 3.0f;

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

SDL_Color MergeMarkerColor(const render::Theme& theme,
                           compare::MergeChoice choice,
                           bool valid) {
  if (!valid) {
    return theme.text_disabled;
  }

  switch (choice) {
    case compare::MergeChoice::Incoming:
      return theme.diff_added;
    case compare::MergeChoice::Current:
      return theme.diff_modified;
    case compare::MergeChoice::Both:
      return theme.accent;
    case compare::MergeChoice::Base:
    default:
      return theme.diff_deleted;
  }
}

void DrawMergeScrollbarMarkers(SDL_Renderer* renderer,
                               const render::Theme& theme,
                               const SDL_FRect& track,
                               std::size_t total_rows,
                               const std::vector<MergeScrollbarMarkerInput>& inputs) {
  if (renderer == nullptr) {
    return;
  }

  const auto markers = BuildMergeScrollbarMarkers(track, total_rows, inputs);
  for (const MergeScrollbarMarker& marker : markers) {
    const SDL_Color color = MergeMarkerColor(theme, marker.choice, marker.valid);
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

std::optional<WorkspaceShell::TextInputVisual> WorkspaceShell::BuildMergeTextInputVisual(
    const SDL_FRect& editor_surface) {
  MergeTabState* merge_tab = ActiveMergeTab();
  if (merge_tab == nullptr) {
    return std::nullopt;
  }

  const MergeSurfaceLayout surface_layout =
      ComputeMergeSurfaceLayout(editor_surface, *merge_tab);
  const MergeResultInteractionLayout interaction =
      BuildMergeResultInteractionLayout(editor_surface, surface_layout, *merge_tab);
  const float cursor_x =
      TextGridCursorX(interaction.text, merge_tab->result_viewport.cursor_visual_column());
  const float cursor_y =
      TextGridLineY(interaction.text, merge_tab->result_viewport.cursor_line());
  return TextInputVisual{
      .surface = TextInputSurface::Editor,
      .area = MakeRect(cursor_x, cursor_y - 1.0f, interaction.text.char_width,
                       interaction.text.line_height),
      .text_x = cursor_x,
      .text_y = cursor_y,
      .cursor_x = cursor_x,
      .foreground = theme_.text_primary,
      .background = theme_.editor_background,
  };
}

void WorkspaceShell::RenderMergeScrollbars(SDL_Renderer* renderer, const SDL_FRect& editor_surface) {
  MergeTabState* merge_tab = ActiveMergeTab();
  if (renderer == nullptr || merge_tab == nullptr) {
    return;
  }

  const MergeSurfaceLayout surface_layout =
      ComputeMergeSurfaceLayout(editor_surface, *merge_tab);
  const auto scroll_layout =
      ComputeMergeScrollLayout(editor_surface, surface_layout, *merge_tab);
  merge_tab->scroll_row = scroll_layout.vertical_scroll;
  merge_tab->horizontal_scroll = scroll_layout.horizontal_scroll;

  if (scroll_layout.vertical_scrollbar.has_value()) {
    const std::size_t line_count =
        std::max({merge_tab->model.incoming_lines.size(), merge_tab->result_viewport.line_count(),
                  merge_tab->model.current_lines.size(), std::size_t{1}});
    const SDL_FRect marker_lane = MakeRect(
        std::max(editor_surface.x,
                 scroll_layout.vertical_scrollbar->track.x - kCompareMarkerLaneGap -
                     kCompareMarkerLaneWidth),
        scroll_layout.vertical_scrollbar->track.y, kCompareMarkerLaneWidth,
        scroll_layout.vertical_scrollbar->track.h);
    const SDL_FRect marker_inner_lane =
        MakeRect(marker_lane.x + 1.0f, marker_lane.y + 1.0f, std::max(0.0f, marker_lane.w - 2.0f),
                 std::max(0.0f, marker_lane.h - 2.0f));
    std::vector<MergeScrollbarMarkerInput> inputs;
    inputs.reserve(merge_tab->conflicts.size());
    for (const auto& conflict : merge_tab->conflicts) {
      const int start_row = static_cast<int>(std::min(
          {conflict.incoming_start_line, conflict.start_line, conflict.current_start_line}));
      const int end_row = static_cast<int>(std::max(
          {std::max(conflict.incoming_end_line, conflict.incoming_start_line + 1),
           std::max(conflict.end_line, conflict.start_line + 1),
           std::max(conflict.current_end_line, conflict.current_start_line + 1)}));
      inputs.push_back(MergeScrollbarMarkerInput{
          .start_row = start_row,
          .end_row = end_row,
          .choice = conflict.last_choice,
          .valid = conflict.valid,
      });
    }
    DrawFilledRect(renderer, marker_lane, theme_.surface_raised);
    DrawRect(renderer, marker_lane, theme_.border);
    DrawMergeScrollbarMarkers(renderer, theme_, marker_inner_lane, line_count, inputs);
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
  const MergeInteractionLayout interaction = BuildMergeInteractionLayout(rect, surface, *merge_tab);
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
        const std::size_t next =
            segment_end + util::Utf8SequenceLength(window.text, segment_end);
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
    if (const auto conflict_index = FindMergeTrackedConflictAtSourceLine(*merge_tab, line, incoming);
        conflict_index.has_value() && *conflict_index < merge_tab->conflicts.size()) {
      return &merge_tab->conflicts[*conflict_index];
    }
    return nullptr;
  };
  const auto source_button_rect = [&](const MergeTrackedConflict& conflict, bool incoming) {
    return BuildMergeSourceActionButtonRect(surface, interaction, conflict, incoming);
  };
  const auto result_action_rects =
      [&](const MergeTrackedConflict& conflict) -> std::array<SDL_FRect, 4> {
    return BuildMergeResultActionButtonRects(surface, interaction, conflict);
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
                 surface_.drag_target == DragTarget::MergeLeftDivider ? theme_.accent : theme_.border);
  DrawFilledRect(renderer,
                 MakeRect(surface.right_x - surface.divider_width * 0.5f, rect.y, 1.0f, content_height),
                 surface_.drag_target == DragTarget::MergeRightDivider ? theme_.accent : theme_.border);

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

  const std::optional<editor::EditorBlameOverlay> merge_blame_overlay =
      BuildEditorBlameOverlay(merge_tab->result_viewport, interaction.result.rect, 280.0f);
  visible_editor_blame_overlay_ = merge_blame_overlay;
  editor_view_renderer_.Render(renderer, text_renderer_, theme_, merge_tab->result_viewport,
                               interaction.result.rect,
                               surface_.focus == FocusTarget::Editor && CaretVisibleNow(), "", std::nullopt,
                               merge_blame_overlay);
  merge_tab->scroll_row = static_cast<int>(merge_tab->result_viewport.scroll_line());
  merge_tab->horizontal_scroll = merge_tab->result_viewport.horizontal_scroll();

  for (std::size_t i = 0; i < merge_tab->conflicts.size(); ++i) {
    const auto& conflict = merge_tab->conflicts[i];
    const std::optional<SDL_FRect> conflict_rect = ComputeVisibleLineRangeRect(
        interaction.result.rect, interaction.result.lines, conflict.start_line,
        std::max(conflict.end_line, conflict.start_line + std::size_t{1}));
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
      const std::size_t preview_height_lines =
          std::max(preview_lines.size(), conflict.end_line > conflict.start_line
                                          ? conflict.end_line - conflict.start_line
                                          : std::size_t{1});
      const std::optional<SDL_FRect> preview_rect = ComputeVisibleLineRangeRect(
          interaction.result.rect, interaction.result.lines, conflict.start_line,
          conflict.start_line + preview_height_lines);
      if (preview_rect.has_value()) {
        DrawFilledRect(renderer, *preview_rect,
                       BlendColor(theme_.editor_background, theme_.diff_modified, 0.18f));
        for (std::size_t line = 0; line < preview_lines.size(); ++line) {
          const float y =
              interaction.result.lines.first_line_y +
              static_cast<float>(std::max(conflict.start_line, interaction.result.lines.scroll_line) -
                                 interaction.result.lines.scroll_line + line) *
                  interaction.result.lines.line_height;
          text_renderer_.DrawStringOn(renderer, interaction.result.rect.x, y, theme_.line_number,
                                      theme_.editor_background,
                                      std::to_string(conflict.start_line + line + 1));
          const VisibleTextWindow window =
              SliceVisibleColumns(preview_lines[line], merge_tab->horizontal_scroll,
                                  interaction.result.metrics.visible_columns);
          if (window.text.empty()) {
            continue;
          }
          text_renderer_.DrawStringOn(renderer, interaction.result.metrics.text_x, y,
                                      theme_.text_primary, theme_.editor_background, window.text);
        }
        DrawRect(renderer, *preview_rect, theme_.accent);
      }
    }
  }

  if (merge_tab->hover_state.has_value() &&
      (merge_tab->hover_state->kind == MergeHoverState::Kind::IncomingConflict ||
       merge_tab->hover_state->kind == MergeHoverState::Kind::IncomingAccept) &&
      merge_tab->hover_state->conflict_index < merge_tab->conflicts.size()) {
    const auto& conflict = merge_tab->conflicts[merge_tab->hover_state->conflict_index];
    if (conflict.valid) {
      draw_button(source_button_rect(conflict, true), "Accept Theirs",
                  merge_tab->hover_state->kind == MergeHoverState::Kind::IncomingAccept, true);
    }
  }

  if (merge_tab->hover_state.has_value() &&
      (merge_tab->hover_state->kind == MergeHoverState::Kind::CurrentConflict ||
       merge_tab->hover_state->kind == MergeHoverState::Kind::CurrentAccept) &&
      merge_tab->hover_state->conflict_index < merge_tab->conflicts.size()) {
    const auto& conflict = merge_tab->conflicts[merge_tab->hover_state->conflict_index];
    if (conflict.valid) {
      draw_button(source_button_rect(conflict, false), "Accept Ours",
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
      draw_button(action_rects[1], "Theirs",
                  merge_tab->hover_state->kind == MergeHoverState::Kind::ResultAction &&
                      merge_tab->hover_state->preview_choice == compare::MergeChoice::Incoming,
                  true);
      draw_button(action_rects[2], "Ours",
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
