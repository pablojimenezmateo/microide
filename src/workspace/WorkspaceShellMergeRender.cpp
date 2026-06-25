#include "workspace/WorkspaceShell.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <optional>
#include <string_view>
#include <vector>

#include "editor/DecoratedTextGridRenderer.h"
#include "editor/RowDecorationBuilder.h"
#include "editor/SyntaxHighlighter.h"
#include "editor/TextLayout.h"
#include "render/Theme.h"
#include "util/PerformanceTrace.h"
#include "workspace/CompareMergeRender.h"
#include "workspace/MergeResolverContext.h"
#include "workspace/SettingFlags.h"
#include "workspace/WorkspaceLayout.h"
#include "workspace/WorkspaceShellRenderPrimitives.h"

namespace microide::workspace {

namespace {

constexpr float kMergeDiffRowTint = 0.14f;
constexpr float kMergeDiffRowTintSelected = 0.22f;
constexpr float kMergeToolbarButtonHeight = 22.0f;
constexpr float kMergeToolbarButtonGap = 8.0f;

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
    case compare::MergeChoice::BothCurrentFirst:
    case compare::MergeChoice::BothIncomingFirst:
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
                               const std::vector<MergeScrollbarMarkerInput>& inputs,
                               MergeTabState& merge_tab) {
  if (renderer == nullptr) {
    return;
  }

  if (!merge_tab.scrollbar_marker_cache_valid ||
      merge_tab.scrollbar_marker_cache_revision != merge_tab.model_revision ||
      !RectsEqual(merge_tab.scrollbar_marker_cache_track, track)) {
    merge_tab.scrollbar_marker_cache = BuildMergeScrollbarMarkers(track, total_rows, inputs);
    merge_tab.scrollbar_marker_cache_track = track;
    merge_tab.scrollbar_marker_cache_revision = merge_tab.model_revision;
    merge_tab.scrollbar_marker_cache_valid = true;
  }
  for (const MergeScrollbarMarker& marker : merge_tab.scrollbar_marker_cache) {
    const SDL_Color color = MergeMarkerColor(theme, marker.choice, marker.valid);
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    SDL_RenderFillRect(renderer, &marker.rect);
  }
}

}  // namespace

std::optional<WorkspaceShell::TextInputVisual> WorkspaceShell::BuildMergeTextInputVisual(
    const SDL_FRect& editor_surface) const {
  const MergeTabState* merge_tab = ActiveMergeTab();
  if (merge_tab == nullptr) {
    return std::nullopt;
  }

  const MergeSurfaceLayout surface_layout =
      ComputeMergeSurfaceLayout(editor_surface, *merge_tab);
  const SDL_FRect result_rect = ComputeMergeResultViewportRect(
      editor_surface, surface_layout.center_x, surface_layout.rows_y, surface_layout.gutter_width,
      surface_layout.center_width, surface_layout.show_horizontal);
  const editor::EditorViewMetrics metrics =
      editor::EditorViewRenderer::ComputeMetrics(text_renderer_, merge_tab->result_viewport, result_rect);
  const MergeResultInteractionLayout interaction = MergeResultInteractionLayout{
      .rect = result_rect,
      .metrics = metrics,
      .lines =
          VisibleLineRangeLayout{
              .first_line_y = metrics.first_line_y,
              .line_height = metrics.line_height,
              .scroll_line = merge_tab->result_viewport.scroll_line(),
              .visible_rows = metrics.visible_rows,
          },
      .text = ComputeTextGridInteractionLayout(
          result_rect, metrics.text_x, metrics.first_line_y, metrics.line_height,
          text_renderer_.CharWidth(), merge_tab->result_viewport.scroll_line(),
          merge_tab->result_viewport.line_count(), merge_tab->result_viewport.horizontal_scroll(),
          metrics.visible_rows, metrics.visible_columns),
  };
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
      .displayed_text = {},
      .selection_bytes = std::nullopt,
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
                 scroll_layout.vertical_scrollbar->track.x - kWorkspaceDiffMarkerLaneGap -
                     kWorkspaceDiffMarkerLaneWidth),
        scroll_layout.vertical_scrollbar->track.y, kWorkspaceDiffMarkerLaneWidth,
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
    DrawMergeScrollbarMarkers(renderer, theme_, marker_inner_lane, line_count, inputs,
                              *merge_tab);
    detail::DrawScrollbarTrack(renderer, theme_, scroll_layout.vertical_scrollbar->track);
    detail::DrawScrollbarThumb(renderer, theme_, scroll_layout.vertical_scrollbar->thumb,
                       context_.interaction_state.drag_target == DragTarget::CompareVerticalScrollbar);
  }

  if (scroll_layout.horizontal_scrollbar.has_value()) {
    detail::DrawScrollbar(renderer, theme_, scroll_layout.horizontal_scrollbar->track,
                  scroll_layout.horizontal_scrollbar->thumb,
                  context_.interaction_state.drag_target == DragTarget::CompareHorizontalScrollbar);
  }
}

void WorkspaceShell::RenderMergeSurface(SDL_Renderer* renderer,
                                        const SDL_FRect& rect,
                                        const std::filesystem::path& project_root,
                                        bool draw_caret,
                                        const editor::DiagnosticsStore& diagnostics_store) {
  MergeTabState* merge_tab = ActiveMergeTab();
  if (renderer == nullptr || merge_tab == nullptr || rect.w <= 0.0f || rect.h <= 0.0f) {
    return;
  }

  util::PerformanceTrace::Scope trace_scope("WorkspaceShell::RenderMergeSurface");
  static const std::vector<editor::SyntaxTokenKind> kEmptyTokens;
  static const editor::DecoratedTextGridRenderer kDecoratedRowRenderer;

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
  const float bottom_reserved =
      surface.show_horizontal ? kWorkspaceDiffScrollbarReserve : 0.0f;
  const float right_reserved =
      surface.show_vertical ? kWorkspaceDiffScrollbarReserve : 0.0f;
  const float content_width = std::max(0.0f, rect.w - right_reserved);
  const float content_height = std::max(0.0f, rect.h - bottom_reserved);
  const std::size_t visible_start_row = static_cast<std::size_t>(std::max(0, merge_tab->scroll_row));
  const std::size_t visible_end_row =
      visible_start_row + static_cast<std::size_t>(std::max(1, surface.visible_rows)) + 64;
  PopulateMergeSyntaxTokensForWindow(*merge_tab, visible_start_row, visible_end_row);
  const MergeInteractionLayout interaction = BuildMergeInteractionLayout(rect, surface, *merge_tab);
  const std::size_t selected_hunk =
      merge_tab->conflicts.empty() ? 0 : std::min(merge_tab->selected_hunk, merge_tab->conflicts.size() - 1);

  // Route merge buttons through the shared button primitive so they read identically to
  // every other button in the shell. `primary` (the actionable/emphasized flag) maps to
  // the enabled state — which de-emphasizes prev/next/unresolved into the shared disabled
  // tone when there are no conflicts — and `selected` maps to the active state for the
  // base toggle and the currently-previewed conflict choice.
  const auto draw_button = [&](const SDL_FRect& button_rect,
                               std::string_view label,
                               bool selected,
                               bool primary = false) {
    const std::string display = TruncateLabel(label, button_rect.w - 18.0f);
    detail::DrawButtonCentered(text_renderer_, renderer, theme_, button_rect, display,
                               detail::ButtonTone::Neutral,
                               detail::ButtonVisualState{
                                   .enabled = primary,
                                   .hovered = false,
                                   .active = selected,
                               });
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

  const SDL_FRect mark_resolved_rect = MakeRect(
      rect.x + rect.w - 8.0f -
          ComputeChromeButtonWidth(text_renderer_.MeasureWidth("Mark Resolved")),
      surface.secondary_button_y,
      ComputeChromeButtonWidth(text_renderer_.MeasureWidth("Mark Resolved")),
      kMergeToolbarButtonHeight);
  const SDL_FRect toggle_base_rect = MakeRect(
      mark_resolved_rect.x - kMergeToolbarButtonGap -
          ComputeChromeButtonWidth(text_renderer_.MeasureWidth("Toggle Base")),
      surface.secondary_button_y,
      ComputeChromeButtonWidth(text_renderer_.MeasureWidth("Toggle Base")),
      kMergeToolbarButtonHeight);
  const SDL_FRect unresolved_rect = MakeRect(
      toggle_base_rect.x - kMergeToolbarButtonGap -
          ComputeChromeButtonWidth(text_renderer_.MeasureWidth("Unresolved")),
      surface.secondary_button_y,
      ComputeChromeButtonWidth(text_renderer_.MeasureWidth("Unresolved")),
      kMergeToolbarButtonHeight);
  const MergeResolverStatus resolver_status =
      BuildMergeResolverStatus(*merge_tab, merge_tab->remaining_conflicted_files);
  const std::string status_text = merge_tab->status_message.empty()
                                      ? resolver_status.progress_label
                                      : merge_tab->status_message;
  const float status_max_width =
      std::max(0.0f, unresolved_rect.x - kMergeToolbarButtonGap - surface.left_x);
  text_renderer_.DrawString(renderer, surface.left_x, surface.secondary_button_y,
                            theme_.text_secondary,
                            TruncateLabel(status_text, status_max_width));
  draw_button(unresolved_rect, "Unresolved", false, !merge_tab->conflicts.empty());
  draw_button(toggle_base_rect, "Toggle Base", merge_tab->base_pane_visible, true);
  draw_button(mark_resolved_rect, "Mark Resolved", false, true);

  const float base_band_height =
      merge_tab->base_pane_visible && !merge_tab->model.base_lines.empty()
          ? surface.line_height * static_cast<float>(std::min<std::size_t>(4, merge_tab->model.base_lines.size())) +
                8.0f
          : 0.0f;
  if (base_band_height > 0.0f) {
    const float base_y = surface.header_y - base_band_height;
    DrawFilledRect(renderer, MakeRect(surface.left_x, base_y, content_width, base_band_height),
                   theme_.surface_raised);
    text_renderer_.DrawString(renderer, surface.left_x + surface.gutter_width, base_y + 2.0f,
                              theme_.text_secondary,
                              TruncateLabel(merge_tab->base_label, content_width - 16.0f));
    for (std::size_t row = 0; row < merge_tab->model.base_lines.size() && row < 4; ++row) {
      const float y = base_y + 2.0f + surface.line_height * static_cast<float>(row + 1);
      text_renderer_.DrawString(renderer, surface.left_x + surface.gutter_width, y,
                                theme_.text_secondary,
                                TruncateLabel(merge_tab->model.base_lines[row], content_width - 16.0f));
    }
  }

  DrawFilledRect(renderer, MakeRect(rect.x, surface.rows_y - 6.0f, content_width, 1.0f),
                 theme_.border);
  DrawFilledRect(renderer,
                 MakeRect(surface.center_x - surface.divider_width * 0.5f, rect.y, 1.0f, content_height),
                 context_.interaction_state.drag_target == DragTarget::MergeLeftDivider ? theme_.accent
                                                                                : theme_.border);
  DrawFilledRect(renderer,
                 MakeRect(surface.right_x - surface.divider_width * 0.5f, rect.y, 1.0f, content_height),
                 context_.interaction_state.drag_target == DragTarget::MergeRightDivider ? theme_.accent
                                                                                 : theme_.border);

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
      std::array<char, 20> line_number_buf;
      const SDL_Color background =
          incoming_conflict != nullptr
              ? render::BlendColors(selected_incoming ? theme_.row_highlight : theme_.editor_background,
                           incoming_conflict->valid ? theme_.diff_added : theme_.diff_deleted,
                           selected_incoming ? kMergeDiffRowTintSelected : kMergeDiffRowTint)
              : (selected_incoming ? theme_.row_highlight : theme_.editor_background);
      const SDL_Color number_color =
          selected_incoming ? theme_.current_line_number : theme_.line_number;
      const std::vector<editor::SyntaxTokenKind>& tokens =
          line_index < merge_tab->incoming_tokens.size() ? merge_tab->incoming_tokens[line_index]
                                                         : kEmptyTokens;
      editor::RowDecorationInput incoming_input;
      incoming_input.text_x = surface.left_x + surface.gutter_width;
      incoming_input.y = y;
      incoming_input.char_width = text_renderer_.CharWidth();
      incoming_input.line_height = surface.line_height;
      incoming_input.row_visual_start = merge_tab->horizontal_scroll;
      incoming_input.row_visual_end = merge_tab->horizontal_scroll + surface.visible_columns;
      incoming_input.text = &merge_tab->model.incoming_lines[line_index];
      incoming_input.tokens = &tokens;
      incoming_input.plain_color =
          selected_incoming ? theme_.text_primary : theme_.text_secondary;
      incoming_input.has_background_fill = true;
      incoming_input.background_fill = editor::DecoratedTextFill{
          .rect = MakeRect(surface.left_x, y - 1.0f,
                           surface.gutter_width + surface.left_width, surface.line_height),
          .color = background,
      };
      incoming_input.text_renderer = &text_renderer_;
      incoming_input.theme = &theme_;
      editor::DecoratedTextRow incoming_row;
      editor::BuildDecoratedRow(incoming_row, incoming_input);
      kDecoratedRowRenderer.RenderRow(renderer, text_renderer_, incoming_row);
      text_renderer_.DrawString(renderer, surface.left_x, y, number_color,
                                FormatLineNumber(line_index + 1, line_number_buf));
    }

    if (line_index < merge_tab->model.current_lines.size()) {
      std::array<char, 20> line_number_buf;
      const SDL_Color background =
          current_conflict != nullptr
              ? render::BlendColors(selected_current ? theme_.row_highlight : theme_.editor_background,
                           current_conflict->valid ? theme_.diff_modified : theme_.diff_deleted,
                           selected_current ? kMergeDiffRowTintSelected : kMergeDiffRowTint)
              : (selected_current ? theme_.row_highlight : theme_.editor_background);
      const SDL_Color number_color =
          selected_current ? theme_.current_line_number : theme_.line_number;
      const std::vector<editor::SyntaxTokenKind>& tokens =
          line_index < merge_tab->current_tokens.size() ? merge_tab->current_tokens[line_index]
                                                        : kEmptyTokens;
      editor::RowDecorationInput current_input;
      current_input.text_x = surface.right_x + surface.gutter_width;
      current_input.y = y;
      current_input.char_width = text_renderer_.CharWidth();
      current_input.line_height = surface.line_height;
      current_input.row_visual_start = merge_tab->horizontal_scroll;
      current_input.row_visual_end = merge_tab->horizontal_scroll + surface.visible_columns;
      current_input.text = &merge_tab->model.current_lines[line_index];
      current_input.tokens = &tokens;
      current_input.plain_color =
          selected_current ? theme_.text_primary : theme_.text_secondary;
      current_input.has_background_fill = true;
      current_input.background_fill = editor::DecoratedTextFill{
          .rect = MakeRect(surface.right_x, y - 1.0f,
                           surface.gutter_width + surface.right_width, surface.line_height),
          .color = background,
      };
      current_input.text_renderer = &text_renderer_;
      current_input.theme = &theme_;
      editor::DecoratedTextRow current_row;
      editor::BuildDecoratedRow(current_row, current_input);
      kDecoratedRowRenderer.RenderRow(renderer, text_renderer_, current_row);
      text_renderer_.DrawString(renderer, surface.right_x, y, number_color,
                                FormatLineNumber(line_index + 1, line_number_buf));
    }
  }

  const std::optional<editor::EditorBlameOverlay> merge_blame_overlay =
      editor_blame_overlay_service_.BuildEditorOverlay(project_root,
                                                       text_renderer_, git_blame_service_,
                                                       merge_tab->result_viewport,
                                                       interaction.result.rect, 280.0f, 0);
  editor_blame_overlay_service_.SetVisibleOverlay(merge_blame_overlay);
  const auto* merge_diagnostics =
      !merge_tab->result_viewport.path().empty() && !merge_tab->result_viewport.dirty()
          ? diagnostics_store.FindByPath(merge_tab->result_viewport.path())
          : nullptr;
  const auto merge_setting_enabled = [this](std::string_view id, bool default_value) {
    return SettingFlagEnabled(GetSettingValue(id), default_value);
  };
  const bool bracket_match_highlight_enabled =
      merge_setting_enabled("editor.brackets.match_highlight.enabled", true);
  const bool indent_guides_enabled =
      merge_setting_enabled("editor.view.indent_guides.enabled", true);
  const bool render_whitespace_enabled =
      merge_setting_enabled("editor.view.render_whitespace", false);
  editor_view_renderer_.Render(renderer, text_renderer_, theme_, merge_tab->result_viewport,
                               interaction.result.rect,
                               draw_caret, "", std::nullopt,
                               merge_blame_overlay,
                               merge_diagnostics != nullptr
                                   ? std::span<const editor::PublishedDiagnostic>(*merge_diagnostics)
                                   : std::span<const editor::PublishedDiagnostic>{},
                               nullptr,
                               bracket_match_highlight_enabled,
                               indent_guides_enabled, render_whitespace_enabled);
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
                       render::BlendColors(theme_.editor_background, theme_.diff_modified, 0.18f));
        for (std::size_t line = 0; line < preview_lines.size(); ++line) {
          std::array<char, 20> line_number_buf;
          const float y =
              interaction.result.lines.first_line_y +
              static_cast<float>(std::max(conflict.start_line, interaction.result.lines.scroll_line) -
                                 interaction.result.lines.scroll_line + line) *
                  interaction.result.lines.line_height;
          text_renderer_.DrawStringOn(renderer, interaction.result.rect.x, y, theme_.line_number,
                                      theme_.editor_background,
                                      FormatLineNumber(conflict.start_line + line + 1,
                                                       line_number_buf));
          const editor::VisibleTextWindow window =
              editor::SliceVisibleColumns(preview_lines[line], merge_tab->horizontal_scroll,
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
