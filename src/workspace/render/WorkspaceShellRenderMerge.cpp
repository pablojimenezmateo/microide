#include "workspace/shell/WorkspaceShell.h"

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
#include "workspace/render/CompareMergeRender.h"
#include "workspace/git/MergeResolverContext.h"
#include "workspace/render/OverviewRuler.h"
#include "workspace/MergeWrapRows.h"
#include "workspace/SettingFlags.h"
#include "workspace/WorkspaceLayout.h"
#include "workspace/render/WorkspaceShellRenderPrimitives.h"

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

// Rebuilds the cached overview markers for `merge_tab` when the model or the lane
// geometry changed. One marker per conflict, pre-mapped to `inner_lane`.
void EnsureMergeOverviewMarkers(const render::Theme& theme,
                                const SDL_FRect& inner_lane,
                                std::size_t total_rows,
                                std::uint64_t theme_token,
                                MergeTabState& merge_tab) {
  // The lane is scaled against on-screen rows, so the row count is part of the
  // key: a divider drag that re-wraps must not keep stale marker positions.
  if (merge_tab.scrollbar_marker_cache_valid &&
      merge_tab.scrollbar_marker_cache_revision == merge_tab.model_revision &&
      merge_tab.scrollbar_marker_cache_rows == total_rows &&
      merge_tab.scrollbar_marker_cache_theme_token == theme_token &&
      RectsEqual(merge_tab.scrollbar_marker_cache_track, inner_lane)) {
    return;
  }

  const std::span<const MergeTrackedConflict> conflicts = MergeVisualConflicts(merge_tab);
  std::vector<overview::MarkerInput>& inputs = merge_tab.scrollbar_marker_input_scratch;
  inputs.clear();
  inputs.reserve(conflicts.size());
  for (const auto& conflict : conflicts) {
    const int start_row = static_cast<int>(std::min(
        {conflict.incoming_start_line, conflict.start_line, conflict.current_start_line}));
    const int end_row = static_cast<int>(std::max(
        {std::max(conflict.incoming_end_line, conflict.incoming_start_line + 1),
         std::max(conflict.end_line, conflict.start_line + 1),
         std::max(conflict.current_end_line, conflict.current_start_line + 1)}));
    inputs.push_back(overview::MarkerInput{
        .start_row = start_row,
        .end_row = end_row,
        .color = MergeMarkerColor(theme, conflict.last_choice, conflict.valid),
        .priority = 0});
  }
  overview::BuildMarkersInto(inner_lane, total_rows, inputs, merge_tab.scrollbar_marker_cache);
  merge_tab.scrollbar_marker_cache_track = inner_lane;
  merge_tab.scrollbar_marker_cache_revision = merge_tab.model_revision;
  merge_tab.scrollbar_marker_cache_rows = total_rows;
  merge_tab.scrollbar_marker_cache_theme_token = theme_token;
  merge_tab.scrollbar_marker_cache_valid = true;
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
  const editor::EditorViewMetrics metrics = editor::EditorViewRenderer::ComputeMetrics(
      text_renderer_, merge_tab->result_viewport, result_rect, 0, LineNumbersEnabled());
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
    const std::size_t line_count = MergeTotalVisualRowCount(*merge_tab);
    const SDL_FRect track = scroll_layout.vertical_scrollbar->track;
    const SDL_FRect lane = overview::LaneRect(track, editor_surface.x);
    const SDL_FRect inner_lane = overview::LaneInnerRect(lane);
    EnsureMergeOverviewMarkers(theme_, inner_lane, line_count, overview::ThemeMarkerToken(theme_),
                               *merge_tab);
    overview::DrawLane(renderer, theme_, lane, merge_tab->scrollbar_marker_cache);
    detail::DrawScrollbarTrack(renderer, theme_, track);
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
    const std::string_view display = TruncateLabelView(label, button_rect.w - 18.0f);
    detail::DrawButtonCentered(text_renderer_, renderer, theme_, button_rect, display,
                               detail::ButtonTone::Neutral,
                               detail::ButtonVisualState{
                                   .enabled = primary,
                                   .hovered = PointerOver(button_rect),
                                   .active = selected,
                               });
  };
  // Geometry and hit testing run on the conflicts projected into on-screen row
  // space; the model-facing list beside it keeps the choices and hunk indices.
  const std::span<const MergeTrackedConflict> visual_conflicts = MergeVisualConflicts(*merge_tab);
  const auto conflict_at_source_row =
      [&](std::size_t visual_row, bool incoming) -> std::optional<std::size_t> {
    const auto conflict_index =
        FindMergeTrackedConflictAtSourceLine(*merge_tab, visual_row, incoming);
    return conflict_index.has_value() && *conflict_index < visual_conflicts.size()
               ? conflict_index
               : std::nullopt;
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

  {
  // Toolbar + status + optional base band: fixed-cost chrome, separate from the
  // per-row work below so a regression in either is attributable.
  util::PerformanceTrace::Scope chrome_scope("WorkspaceShell::RenderMergeSurface::Chrome");
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
  const std::string_view status_text = merge_tab->status_message.empty()
                                           ? std::string_view(resolver_status.progress_label)
                                           : std::string_view(merge_tab->status_message);
  const float status_max_width =
      std::max(0.0f, unresolved_rect.x - kMergeToolbarButtonGap - surface.left_x);
  text_renderer_.DrawString(renderer, surface.left_x, surface.secondary_button_y,
                            theme_.text_secondary,
                            TruncateLabelView(status_text, status_max_width));
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
                              TruncateLabelView(merge_tab->base_label, content_width - 16.0f));
    for (std::size_t row = 0; row < merge_tab->model.base_lines.size() && row < 4; ++row) {
      const float y = base_y + 2.0f + surface.line_height * static_cast<float>(row + 1);
      text_renderer_.DrawString(renderer, surface.left_x + surface.gutter_width, y,
                                theme_.text_secondary,
                                TruncateLabelView(merge_tab->model.base_lines[row],
                                                  content_width - 16.0f));
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

  }
  text_renderer_.DrawString(renderer, surface.left_x + surface.gutter_width, surface.header_y,
                            theme_.text_secondary,
                            TruncateLabelView(merge_tab->incoming_label, surface.left_width - 8.0f));
  text_renderer_.DrawString(renderer, surface.center_x + surface.gutter_width, surface.header_y,
                            theme_.text_secondary,
                            TruncateLabelView(merge_tab->result_label, surface.center_width - 8.0f));
  text_renderer_.DrawString(renderer, surface.right_x + surface.gutter_width, surface.header_y,
                            theme_.text_secondary,
                            TruncateLabelView(merge_tab->current_label, surface.right_width - 8.0f));

  // The merge surface paints THREE panes: the result pane goes through
  // EditorViewRenderer (already scoped), while the incoming and current side
  // panes are painted by this loop. Without a scope of its own the loop sat in
  // RenderMergeSurface's self time, which is 63 us per frame and was the largest
  // unattributed per-frame cost in the suite.
  {
  util::PerformanceTrace::Scope side_rows_scope("WorkspaceShell::RenderMergeSurface::SideRows");
  const bool wrapped = merge_tab->wrap_layout.active();
  const float char_width_px = text_renderer_.CharWidth();
  for (int row = 0; row < surface.visible_rows; ++row) {
    const std::size_t visual_row =
        static_cast<std::size_t>(std::max(0, merge_tab->scroll_row + row));
    if (visual_row >= MergeSourceVisualRowCount(*merge_tab)) {
      break;
    }
    const DiffWrapRow wrap_row = merge_tab->wrap_layout.RowAt(visual_row);
    const std::size_t line_index = wrap_row.unit;
    const float y = surface.rows_y + static_cast<float>(row) * surface.line_height;
    const std::optional<std::size_t> incoming_conflict_index =
        conflict_at_source_row(visual_row, true);
    const std::optional<std::size_t> current_conflict_index =
        conflict_at_source_row(visual_row, false);
    const MergeTrackedConflict* incoming_conflict =
        incoming_conflict_index.has_value() ? &merge_tab->conflicts[*incoming_conflict_index]
                                            : nullptr;
    const MergeTrackedConflict* current_conflict =
        current_conflict_index.has_value() ? &merge_tab->conflicts[*current_conflict_index]
                                           : nullptr;
    const bool selected_incoming =
        incoming_conflict_index.has_value() && *incoming_conflict_index == selected_hunk;
    const bool selected_current =
        current_conflict_index.has_value() && *current_conflict_index == selected_hunk;
    const std::size_t left_row_start = wrapped ? wrap_row.left_start : merge_tab->horizontal_scroll;
    const std::size_t left_row_end =
        wrapped ? wrap_row.left_end : merge_tab->horizontal_scroll + surface.visible_columns;
    const std::size_t right_row_start =
        wrapped ? wrap_row.right_start : merge_tab->horizontal_scroll;
    const std::size_t right_row_end =
        wrapped ? wrap_row.right_end : merge_tab->horizontal_scroll + surface.visible_columns;

    if (line_index < merge_tab->model.incoming_lines.size() && wrap_row.left_present) {
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
          merge_tab->incoming_token_window.Tokens(line_index);
      editor::RowDecorationInput incoming_input;
      incoming_input.text_x = surface.left_x + surface.gutter_width +
                              static_cast<float>(wrap_row.left_indent) * char_width_px;
      incoming_input.y = y;
      incoming_input.char_width = char_width_px;
      incoming_input.line_height = surface.line_height;
      incoming_input.row_visual_start = left_row_start;
      incoming_input.row_visual_end = left_row_end;
      incoming_input.text = merge_tab->model.incoming_lines[line_index];
      incoming_input.line_length = incoming_input.text.size();
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
      editor::DecoratedTextRow& incoming_row = merge_incoming_scratch_row_;
      editor::BuildDecoratedRow(incoming_row, incoming_input);
      kDecoratedRowRenderer.RenderRow(renderer, text_renderer_, incoming_row);
      if (wrap_row.first) {
        text_renderer_.DrawString(renderer, surface.left_x, y, number_color,
                                  FormatLineNumber(line_index + 1, line_number_buf));
      }
    }

    if (line_index < merge_tab->model.current_lines.size() && wrap_row.right_present) {
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
          merge_tab->current_token_window.Tokens(line_index);
      editor::RowDecorationInput current_input;
      current_input.text_x = surface.right_x + surface.gutter_width +
                             static_cast<float>(wrap_row.right_indent) * char_width_px;
      current_input.y = y;
      current_input.char_width = char_width_px;
      current_input.line_height = surface.line_height;
      current_input.row_visual_start = right_row_start;
      current_input.row_visual_end = right_row_end;
      current_input.text = merge_tab->model.current_lines[line_index];
      current_input.line_length = current_input.text.size();
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
      editor::DecoratedTextRow& current_row = merge_current_scratch_row_;
      editor::BuildDecoratedRow(current_row, current_input);
      kDecoratedRowRenderer.RenderRow(renderer, text_renderer_, current_row);
      if (wrap_row.first) {
        text_renderer_.DrawString(renderer, surface.right_x, y, number_color,
                                  FormatLineNumber(line_index + 1, line_number_buf));
      }
    }
  }
  }

  const std::optional<editor::EditorBlameOverlay> merge_blame_overlay =
      editor_blame_overlay_service_.BuildEditorOverlay(project_root,
                                                       text_renderer_, git_blame_service_,
                                                       merge_tab->result_viewport,
                                                       interaction.result.rect, 280.0f, 0,
                                                       LineNumbersEnabled());
  editor_blame_overlay_service_.SetVisibleOverlay(merge_blame_overlay);
  const auto* merge_diagnostics =
      !merge_tab->result_viewport.path().empty() && !merge_tab->result_viewport.dirty()
          ? diagnostics_store.FindByPathKey(merge_tab->result_viewport.path_key())
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
  const bool line_numbers_enabled = merge_setting_enabled("editor.line_numbers", true);
  editor_view_renderer_.Render(renderer, text_renderer_, theme_, merge_tab->result_viewport,
                               interaction.result.rect,
                               draw_caret, "", std::nullopt,
                               merge_blame_overlay,
                               merge_diagnostics != nullptr
                                   ? std::span<const editor::PublishedDiagnostic>(*merge_diagnostics)
                                   : std::span<const editor::PublishedDiagnostic>{},
                               nullptr,
                               bracket_match_highlight_enabled,
                               indent_guides_enabled, render_whitespace_enabled,
                               /*folding_model=*/nullptr,
                               /*welcome_view=*/nullptr,
                               /*plugin_decorations=*/nullptr,
                               line_numbers_enabled);
  merge_tab->scroll_row = static_cast<int>(merge_tab->result_viewport.scroll_line());
  merge_tab->horizontal_scroll = merge_tab->result_viewport.horizontal_scroll();

  for (std::size_t i = 0; i < visual_conflicts.size(); ++i) {
    const auto& conflict = visual_conflicts[i];
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

  if (preview_choice.has_value() && preview_choice->first < visual_conflicts.size()) {
    const auto& conflict = visual_conflicts[preview_choice->first];
    // The band is placed in on-screen rows; the gutter still labels DOCUMENT
    // lines, so it counts from the conflict's real start line.
    const std::size_t preview_first_line =
        merge_tab->conflicts[preview_choice->first].start_line;
    // Choice lines are cached on the tab (keyed by conflict/choice/revision) so
    // hovering no longer reallocates them every frame; the copy lives outside this
    // lint-constrained render TU.
    const std::span<const std::string> preview_lines =
        EnsureMergePreviewLines(*merge_tab, preview_choice->first, preview_choice->second);
    if (!preview_lines.empty()) {
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
          // Content row `conflict.start_line + line` maps to screen Y relative to
          // the first visible line. Rows scrolled above the viewport are skipped
          // rather than clamped to the top, which would desync the preview text
          // from its (correctly clipped) preview_rect when the conflict starts
          // above scroll_line.
          const std::size_t content_line = conflict.start_line + line;
          if (content_line < interaction.result.lines.scroll_line) {
            continue;
          }
          const float y =
              interaction.result.lines.first_line_y +
              static_cast<float>(content_line - interaction.result.lines.scroll_line) *
                  interaction.result.lines.line_height;
          // Symmetric with the above-viewport skip: a "Both" preview yields more lines
          // than fit when the conflict starts near the pane bottom, so stop before
          // drawing text past the result pane into the scrollbar reserve / bottom chrome.
          // Lines are visited in ascending y, so break rather than continue.
          if (y >= interaction.result.rect.y + interaction.result.rect.h) {
            break;
          }
          text_renderer_.DrawStringOn(renderer, interaction.result.rect.x, y, theme_.line_number,
                                      theme_.editor_background,
                                      FormatLineNumber(preview_first_line + line + 1,
                                                       line_number_buf));
          // Render through the canonical tab-aware decorated-row path (the same one
          // the result viewport underneath uses) instead of a codepoint slice, so
          // the preview text stays column-aligned with tab-expanded result rows.
          const std::string& preview_text = preview_lines[line];
          const editor::LayoutLine preview_layout = editor::TextLayout::BuildVisibleLine(
              preview_text, merge_tab->horizontal_scroll,
              interaction.result.metrics.visible_columns, merge_tab->result_viewport.tab_size());
          editor::RowDecorationInput preview_input;
          preview_input.text_x = interaction.result.metrics.text_x;
          preview_input.y = y;
          preview_input.char_width = text_renderer_.CharWidth();
          preview_input.line_height = interaction.result.lines.line_height;
          preview_input.row_visual_start = merge_tab->horizontal_scroll;
          preview_input.row_visual_end =
              merge_tab->horizontal_scroll + interaction.result.metrics.visible_columns;
          preview_input.text = preview_text;
          preview_input.line_length = preview_input.text.size();
          preview_input.tokens = &kEmptyTokens;
          preview_input.plain_color = theme_.text_primary;
          preview_input.layout = &preview_layout;
          preview_input.tab_size = merge_tab->result_viewport.tab_size();
          preview_input.has_background_fill = false;
          preview_input.text_renderer = &text_renderer_;
          preview_input.theme = &theme_;
          editor::DecoratedTextRow& preview_row = merge_incoming_scratch_row_;
          editor::BuildDecoratedRow(preview_row, preview_input);
          kDecoratedRowRenderer.RenderRow(renderer, text_renderer_, preview_row);
        }
        DrawRect(renderer, *preview_rect, theme_.accent);
      }
    }
  }

  if (merge_tab->hover_state.has_value() &&
      (merge_tab->hover_state->kind == MergeHoverState::Kind::IncomingConflict ||
       merge_tab->hover_state->kind == MergeHoverState::Kind::IncomingAccept) &&
      merge_tab->hover_state->conflict_index < visual_conflicts.size()) {
    const auto& conflict = visual_conflicts[merge_tab->hover_state->conflict_index];
    if (conflict.valid) {
      draw_button(source_button_rect(conflict, true), "Accept Theirs",
                  merge_tab->hover_state->kind == MergeHoverState::Kind::IncomingAccept, true);
    }
  }

  if (merge_tab->hover_state.has_value() &&
      (merge_tab->hover_state->kind == MergeHoverState::Kind::CurrentConflict ||
       merge_tab->hover_state->kind == MergeHoverState::Kind::CurrentAccept) &&
      merge_tab->hover_state->conflict_index < visual_conflicts.size()) {
    const auto& conflict = visual_conflicts[merge_tab->hover_state->conflict_index];
    if (conflict.valid) {
      draw_button(source_button_rect(conflict, false), "Accept Ours",
                  merge_tab->hover_state->kind == MergeHoverState::Kind::CurrentAccept, true);
    }
  }

  if (merge_tab->hover_state.has_value() &&
      (merge_tab->hover_state->kind == MergeHoverState::Kind::ResultConflict ||
       merge_tab->hover_state->kind == MergeHoverState::Kind::ResultAction) &&
      merge_tab->hover_state->conflict_index < visual_conflicts.size()) {
    const auto& conflict = visual_conflicts[merge_tab->hover_state->conflict_index];
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
