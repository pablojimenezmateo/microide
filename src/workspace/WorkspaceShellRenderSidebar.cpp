#include "workspace/WorkspaceShellRenderPrimitives.h"

#include <algorithm>
#include <cmath>
#include <string>

#include "workspace/CommitWorkflowState.h"
#include "workspace/GitSidebarCommandCenter.h"
#include "workspace/WorkspaceGitSidebarPresentation.h"

namespace microide::workspace {

using namespace detail;

namespace {

constexpr float kSidebarInset = 10.0f;
constexpr float kTreeIndentWidth = 14.0f;
constexpr float kTreeChevronSlotWidth = 12.0f;

// Reuse a thread-local scratch so the per-result label assembly is allocation-bounded by max label
// width, not by `results × frames`. The render path only inspects the returned view immediately
// after this call, so the scratch's lifetime is safe.
std::string_view BuildProjectSearchResultLabel(std::size_t line,
                                                std::size_t column,
                                                std::string_view snippet) {
  thread_local std::string label;
  label.clear();
  label.reserve(snippet.size() + 32);
  AppendUnsigned(label, line + 1);
  label += ":";
  AppendUnsigned(label, column + 1);
  label += "  ";
  label += snippet;
  return label;
}

std::string CompactCommitSummaryLabel(std::string_view commit_summary) {
  constexpr std::string_view kDelimiter = "  |  ";
  const std::size_t first_delimiter = commit_summary.find(kDelimiter);
  if (first_delimiter == std::string_view::npos) {
    return std::string(commit_summary);
  }
  const std::string_view prefix = commit_summary.substr(0, first_delimiter);
  const std::size_t remainder_start = first_delimiter + kDelimiter.size();
  const std::size_t second_delimiter = commit_summary.find(kDelimiter, remainder_start);
  const std::size_t remainder_end =
      second_delimiter == std::string_view::npos ? commit_summary.size() : second_delimiter;
  const std::string_view remainder =
      commit_summary.substr(remainder_start, remainder_end - remainder_start);
  if (prefix.find("Commit blocked") == 0 && !remainder.empty()) {
    return std::string(prefix) + " - " + std::string(remainder);
  }
  return std::string(prefix);
}

// Renders the multi-line commit body edit area (visible lines, selection, caret) and clamps
// the body viewport's scroll so the caret row stays on screen. Writes the focused field's
// caret rect to `caret_rect_out` so the caret-redraw path can target a tight dirty region.
void RenderCommitBodyField(const render::TextRenderer& tr, const render::Theme& theme,
                           SDL_Renderer* renderer, editor::TextViewport& body,
                           const SDL_FRect& field, int visible_rows, bool focused,
                           bool caret_visible, SDL_FRect* caret_rect_out) {
  const float line_height = tr.LineHeight();
  const float char_width = std::max(1.0f, tr.CharWidth());
  const float text_x = field.x + 6.0f;
  const float top_y = field.y + 3.0f;
  const float avail_w = std::max(1.0f, field.w - 12.0f);

  const std::size_t rows = static_cast<std::size_t>(std::max(1, visible_rows));
  body.SetViewportSize(rows, static_cast<std::size_t>(std::max(1.0f, avail_w / char_width)));
  std::size_t scroll = body.scroll_line();
  const std::size_t caret_line_now = body.cursor_line();
  if (caret_line_now < scroll) {
    scroll = caret_line_now;
  } else if (caret_line_now >= scroll + rows) {
    scroll = caret_line_now - rows + 1;
  }
  body.SetScrollLine(scroll);

  const std::vector<std::string>& lines = body.lines();
  scroll = body.scroll_line();
  const std::optional<editor::SelectionRange> selection = body.selection_range();
  const std::size_t caret_line = body.cursor_line();
  const std::size_t caret_col = body.cursor_column();

  if (lines.size() == 1 && lines[0].empty() && !focused) {
    DrawTextOn(tr, renderer, text_x, top_y, theme.text_muted, theme.surface_background, "<optional>");
    return;
  }

  for (int row = 0; row < visible_rows; ++row) {
    const std::size_t line_index = scroll + static_cast<std::size_t>(row);
    if (line_index >= lines.size()) {
      break;
    }
    const std::string& text = lines[line_index];
    const std::string_view text_view = text;
    const float row_y = top_y + static_cast<float>(row) * line_height;

    if (selection.has_value() && line_index >= selection->start.line &&
        line_index <= selection->end.line) {
      const std::size_t sel_start_col =
          line_index == selection->start.line ? selection->start.column : 0;
      const std::size_t sel_end_col =
          line_index == selection->end.line ? std::min(selection->end.column, text.size())
                                            : text.size();
      if (sel_start_col < sel_end_col && sel_start_col <= text.size()) {
        const float sx = text_x + tr.MeasureWidth(text_view.substr(0, sel_start_col));
        const float sw = tr.MeasureWidth(text_view.substr(sel_start_col, sel_end_col - sel_start_col));
        if (sw > 0.0f) {
          FillRect(renderer, MakeRect(sx, row_y - 1.0f, sw, line_height), theme.selection_fill);
        }
      }
    }

    DrawTextOn(tr, renderer, text_x, row_y, theme.text_primary, theme.surface_background,
               tr.TruncateToWidth(text, avail_w));

    if (focused && line_index == caret_line) {
      const std::size_t clamped_col = std::min(caret_col, text.size());
      const float caret_x = std::min(text_x + tr.MeasureWidth(text_view.substr(0, clamped_col)),
                                     field.x + field.w - 2.0f);
      const SDL_FRect caret = MakeRect(caret_x, row_y - 1.0f, 1.0f, line_height);
      if (caret_rect_out != nullptr) {
        *caret_rect_out = caret;
      }
      if (caret_visible) {
        FillRect(renderer, caret, theme.text_primary);
      }
    }
  }
}

}  // namespace

void WorkspaceShell::RenderSidebarSurface(SDL_Renderer* renderer, const WorkspaceLayout& layout) {
  const SidebarSurfaceViewModel& sidebar_vm = *prepare_cached_sidebar_vm_;
  const TextInputSurfaceViewModel& text_input_vm = *prepare_cached_text_input_vm_;
  ProjectWorkspaceState& project_state = *sidebar_vm.project_state;
  if (!sidebar_vm.visible) {
    return;
  }

  const auto draw_vertical_scrollbar = [&](const SDL_FRect& area,
                                           float total_units,
                                           float visible_units,
                                           float scroll_units,
                                           bool active = false,
                                           bool reserve_horizontal = false) {
    if (const auto geometry = MakeVerticalScrollbarGeometry(area, total_units, visible_units,
                                                            scroll_units, reserve_horizontal);
        geometry.has_value()) {
      DrawScrollbar(renderer, theme_, geometry->track, geometry->thumb, active);
    }
  };
  const auto draw_action_button = [&](const SDL_FRect& button_rect,
                                      std::string_view label,
                                      bool enabled,
                                      ButtonTone tone = ButtonTone::Neutral) {
    DrawButtonCentered(
        text_renderer_, renderer, theme_, button_rect, label, tone,
        ButtonVisualState{
            .enabled = enabled,
            .hovered = enabled && last_mouse_position_valid_ &&
                       Contains(button_rect, last_mouse_x_, last_mouse_y_),
            .active = false,
        });
  };

  const SDL_FRect sidebar_mode_rect = SidebarModeControlRect(layout.sidebar);
  const bool sidebar_mode_hovered =
      last_mouse_position_valid_ && Contains(sidebar_mode_rect, last_mouse_x_, last_mouse_y_);
  const bool sidebar_mode_open =
      context_.menu_state.menu_bar_open && context_.menu_state.active_menu_id == MenuId::SidebarMode &&
      context_.menu_state.active_menu_anchor_rect.has_value();
  DrawFilledRect(renderer, sidebar_mode_rect,
                 sidebar_mode_open || sidebar_mode_hovered ? theme_.row_highlight
                                                          : theme_.surface_raised);
  DrawRect(renderer, sidebar_mode_rect,
           sidebar_mode_open ? theme_.accent
                             : sidebar_mode_hovered ? theme_.text_secondary : theme_.border);
  DrawVCenteredTextOn(text_renderer_, renderer, sidebar_mode_rect, 8.0f,
                      sidebar_mode_open || sidebar_mode_hovered ? theme_.text_primary
                                                                : theme_.text_secondary,
                      sidebar_mode_open || sidebar_mode_hovered ? theme_.row_highlight
                                                                : theme_.surface_raised,
                      SidebarModeControlLabel());
  DrawChevron(renderer, sidebar_mode_rect.x + sidebar_mode_rect.w - 18.0f,
              sidebar_mode_rect.y + sidebar_mode_rect.h * 0.5f, true,
              sidebar_mode_open || sidebar_mode_hovered ? theme_.text_primary : theme_.text_muted);

  const SidebarMode sidebar_mode = sidebar_vm.mode;
  if (sidebar_mode == SidebarMode::Search) {
    const auto& ps = project_state.overlay.workflow.project_search;
    const bool editing_query =
        ps.editing && ps.edit_field == ProjectSearchEditField::Query;
    const bool editing_replace =
        ps.editing && ps.edit_field == ProjectSearchEditField::Replace;

    const TextInputSurface current_surface = text_input_vm.current_surface;
    const bool sidebar_needs_visual =
        current_surface == TextInputSurface::SidebarSearchQuery ||
        current_surface == TextInputSurface::SidebarSearchReplace;
    const auto visual =
        sidebar_needs_visual ? BuildActiveTextInputVisual(layout, std::nullopt) : std::nullopt;
    const auto sidebar_display_text = [&](TextInputSurface surface,
                                          std::string_view fallback) -> std::string_view {
      if (visual.has_value() && visual->surface == surface && !visual->displayed_text.empty()) {
        return visual->displayed_text;
      }
      return fallback;
    };

    const SDL_FRect query_rect = ProjectSearchQueryRect(layout.sidebar);
    const SDL_FRect replace_rect = ProjectSearchReplaceRect(layout.sidebar);
    DrawTextFieldFrame(renderer, theme_, query_rect,
                       current_surface == TextInputSurface::SidebarSearchQuery);
    DrawTextFieldFrame(renderer, theme_, replace_rect,
                       current_surface == TextInputSurface::SidebarSearchReplace);
    DrawSingleLineTextTail(
        renderer, query_rect.x + 6.0f, query_rect.y + 3.0f,
        std::max(1.0f, query_rect.w - 12.0f),
        editing_query ? theme_.text_primary : theme_.text_secondary,
        theme_.surface_background,
        sidebar_display_text(TextInputSurface::SidebarSearchQuery, sidebar_vm.query_fallback_text));
    DrawSingleLineTextTail(
        renderer, replace_rect.x + 6.0f, replace_rect.y + 3.0f,
        std::max(1.0f, replace_rect.w - 12.0f),
        editing_replace ? theme_.text_primary : theme_.text_secondary,
        theme_.surface_background,
        sidebar_display_text(TextInputSurface::SidebarSearchReplace,
                             sidebar_vm.replace_fallback_text));
    const auto draw_search_button = [&](const SDL_FRect& rect,
                                        std::string_view label,
                                        bool active) {
      DrawButtonCentered(
          text_renderer_, renderer, theme_, rect, label, ButtonTone::Neutral,
          ButtonVisualState{
              .enabled = true,
              .hovered = last_mouse_position_valid_ && Contains(rect, last_mouse_x_, last_mouse_y_),
              .active = active,
          });
    };

    draw_search_button(ProjectSearchModeButtonRect(layout.sidebar), ProjectSearchModeButtonLabel(),
                       project_state.overlay.workflow.project_search.options.pattern_mode ==
                           project::ProjectSearchPatternMode::Regex);
    draw_search_button(ProjectSearchCaseButtonRect(layout.sidebar), ProjectSearchCaseButtonLabel(),
                       project_state.overlay.workflow.project_search.options.case_mode !=
                           project::ProjectSearchCaseMode::Smart);
    draw_search_button(ProjectSearchHiddenButtonRect(layout.sidebar),
                       ProjectSearchHiddenButtonLabel(),
                       project_state.overlay.workflow.project_search.options.show_hidden);

    const std::string match_actions =
        ProjectSearchCanReplaceAll()
            ? JoinHintSegments({"/ query", "= replace", "r rerun", "R replace all"})
            : JoinHintSegments({"/ query", "= replace", "r rerun", "R literal mode required"});
    const std::string status_text =
        project_state.overlay.workflow.project_search.editing
            ? (project_state.overlay.workflow.project_search.edit_field ==
                       ProjectSearchEditField::Query
                   ? JoinHintSegments({"Editing query", "Enter apply", "Esc cancel"})
                   : JoinHintSegments({"Editing replace", "Enter apply", "Esc cancel"}))
            : !project_state.overlay.workflow.project_search.error.empty()
                ? JoinHintSegments({"Error", "/ query", "= replace", "r rerun"})
            : project_state.overlay.workflow.project_search.running
                ? BuildCountStatus(
                      "Searching ",
                      project_state.overlay.workflow.project_search.results.size(),
                      " matches") +
                      BuildSearchProgressSuffix(
                          project_state.overlay.workflow.project_search.searched_files,
                          project_state.overlay.workflow.project_search.total_files)
            : project_state.overlay.workflow.project_search.results.empty()
                ? (project_state.overlay.workflow.project_search.query.text().empty()
                       ? JoinHintSegments(
                             {"/ query", "= replace", "buttons change mode, case, hidden"})
                       : FormatEmptyState("matches") + "  |  " + match_actions)
            : project_state.overlay.workflow.project_search.truncated
                ? BuildCountStatus(
                      "Showing first ",
                      project_state.overlay.workflow.project_search.results.size(),
                      " matches  |  " + match_actions)
                : BuildCountStatus(
                      "",
                      project_state.overlay.workflow.project_search.results.size(),
                      " matches  |  " + match_actions);
    DrawTextOn(text_renderer_, renderer, layout.sidebar.x + kSidebarInset,
               layout.sidebar.y + kProjectSearchStatusTop, theme_.text_muted,
               theme_.surface_background,
               TruncateLabel(status_text, layout.sidebar.w - kSidebarInset * 2.0f));

    const auto line_map = BuildProjectSearchLineMap();
    const auto list_layout = ComputeProjectSearchSidebarListLayout(layout.sidebar, line_map.size());
    int scroll_row = list_layout.scroll_row;
    const int selected_line =
        ProjectSearchLineForResult(project_state.overlay.workflow.project_search.selected_index);
    scroll_row = RevealScrollableListIndex(list_layout, selected_line);
    project_state.sidebar.scroll_row = scroll_row;

    for (int row = 0; row < list_layout.visible_rows; ++row) {
      const int line_index = scroll_row + row;
      if (line_index >= static_cast<int>(line_map.size())) {
        break;
      }

      SDL_FRect row_rect = ScrollableListRowRect(list_layout, row);
      const int result_index = line_map[static_cast<std::size_t>(line_index)];
      if (result_index < 0) {
        const std::size_t next_result_index = static_cast<std::size_t>(
            std::min(line_index + 1, static_cast<int>(line_map.size()) - 1));
        const auto& file_result =
            project_state.overlay.workflow.project_search
                .results[static_cast<std::size_t>(line_map[next_result_index])];
        DrawVCenteredTextOn(text_renderer_, renderer, row_rect, 4.0f, theme_.text_primary,
                            theme_.surface_background,
                            TruncateLabel(file_result.relative_path_string.empty()
                                              ? file_result.relative_path.string()
                                              : std::string_view(file_result.relative_path_string),
                                          row_rect.w - 8.0f));
        continue;
      }

      const auto& result =
          project_state.overlay.workflow.project_search.results[static_cast<std::size_t>(result_index)];
      const bool selected =
          static_cast<std::size_t>(result_index) ==
          project_state.overlay.workflow.project_search.selected_index;
      DrawSelectableRowBackground(renderer, theme_, row_rect, theme_.surface_background, selected,
                                  selected);

      const std::string_view label =
          BuildProjectSearchResultLabel(result.line, result.column, result.preview);
      DrawVCenteredTextOn(text_renderer_, renderer, row_rect, 6.0f,
                          selected ? theme_.text_primary : theme_.text_secondary,
                          selected ? theme_.row_highlight : theme_.surface_background,
                          TruncateLabel(label, row_rect.w - 12.0f));
    }

    if (line_map.empty()) {
      const std::string placeholder =
          !project_state.overlay.workflow.project_search.error.empty()
              ? "Error: " + project_state.overlay.workflow.project_search.error
          : project_state.overlay.workflow.project_search.running
              ? "Searching..."
          : project_state.overlay.workflow.project_search.query.text().empty()
              ? "Project Search is idle"
              : FormatEmptyState("matches");
      DrawTextOn(text_renderer_, renderer, layout.sidebar.x + kSidebarInset,
                 list_layout.row_y + 4.0f, theme_.text_muted, theme_.surface_background,
                 TruncateLabel(placeholder, layout.sidebar.w - kSidebarInset * 2.0f));
    }

    draw_vertical_scrollbar(list_layout.list_rect, static_cast<float>(line_map.size()),
                            list_layout.visible_units, static_cast<float>(scroll_row),
                            context_.interaction_state.drag_target == DragTarget::SidebarScrollbar);
  } else if (sidebar_mode == SidebarMode::Git) {
    const bool outgoing_base_menu_open =
        context_.menu_state.menu_bar_open &&
        context_.menu_state.active_menu_id == MenuId::GitOutgoingBase &&
        context_.menu_state.active_menu_anchor_rect.has_value();
    draw_action_button(GitSidebarStageAllButtonRect(layout.sidebar), "Stage All",
                       CanStageAllGitSidebarEntries(), ButtonTone::Neutral);
    draw_action_button(GitSidebarDiscardAllButtonRect(layout.sidebar), "Discard All",
                       CanDiscardAllGitSidebarEntries(), ButtonTone::Destructive);
    DrawButtonCentered(
        text_renderer_, renderer, theme_, GitSidebarRefreshButtonRect(layout.sidebar), "Refresh",
        ButtonTone::Neutral,
        ButtonVisualState{
            .enabled = true,
            .hovered = last_mouse_position_valid_ &&
                       Contains(GitSidebarRefreshButtonRect(layout.sidebar), last_mouse_x_,
                                last_mouse_y_),
            .active = false,
        });

    float summary_y = GitSidebarActionRowRect(layout.sidebar).y +
                      GitSidebarActionRowRect(layout.sidebar).h + 6.0f;
    if (sidebar_vm.git_sidebar.has_value()) {
      const GitSidebarViewModel& git_vm = *sidebar_vm.git_sidebar;
      const float text_width = layout.sidebar.w - kSidebarInset * 2.0f;
      const bool sidebar_focus_details =
          project_state.surface.focus == FocusTarget::Sidebar &&
          sidebar_mode == SidebarMode::Git;
      float detail_y = summary_y;
      for (std::size_t i = 0; i < git_vm.summary_lines.size(); ++i) {
        const std::string& summary = git_vm.summary_lines[i];
        const SDL_Color color = summary == git_vm.error_banner ? theme_.diff_deleted
                                : i == 0 ? theme_.text_primary
                                         : summary == git_vm.stale_banner ? theme_.text_secondary
                                                                          : theme_.text_muted;
        DrawTextOn(text_renderer_, renderer, layout.sidebar.x + kSidebarInset, detail_y, color,
                   theme_.surface_background, TruncateLabel(summary, text_width));
        detail_y += 14.0f;
      }
      if (!git_vm.workflow_summary_line.empty()) {
        DrawTextOn(text_renderer_, renderer, layout.sidebar.x + kSidebarInset, detail_y,
                   theme_.text_secondary, theme_.surface_background,
                   TruncateLabel(git_vm.workflow_summary_line, text_width));
        detail_y += 14.0f;
      }
      if (!git_vm.commit_summary_line.empty()) {
        const std::string commit_line = CompactCommitSummaryLabel(git_vm.commit_summary_line);
        const SDL_Color commit_color =
            commit_line.find("blocked") != std::string::npos ? theme_.diff_deleted
                                                             : theme_.text_secondary;
        DrawTextOn(text_renderer_, renderer, layout.sidebar.x + kSidebarInset, detail_y,
                   commit_color, theme_.surface_background,
                   TruncateLabel(commit_line, text_width));
        detail_y += 14.0f;
      }
      if (sidebar_focus_details && !git_vm.selection_summary_line.empty()) {
        DrawTextOn(text_renderer_, renderer, layout.sidebar.x + kSidebarInset, detail_y,
                   theme_.text_primary, theme_.surface_background,
                   TruncateLabel(git_vm.selection_summary_line, text_width));
        detail_y += 14.0f;
        DrawTextOn(text_renderer_, renderer, layout.sidebar.x + kSidebarInset, detail_y,
                   theme_.text_muted, theme_.surface_background,
                   "Ctrl+E for row actions");
        detail_y += 14.0f;
      }
      summary_y = detail_y;
    } else {
      for (const std::string& summary : GitSidebarSummaryLines()) {
        DrawTextOn(text_renderer_, renderer, layout.sidebar.x + kSidebarInset, summary_y,
                   theme_.text_muted, theme_.surface_background,
                   TruncateLabel(summary, layout.sidebar.w - kSidebarInset * 2.0f));
        summary_y += 14.0f;
      }
    }
    if (project_state.sidebar.git.commit_workflow.open) {
      auto& workflow = project_state.sidebar.git.commit_workflow;
      const float field_x = layout.sidebar.x + kSidebarInset;
      const float field_w = layout.sidebar.w - kSidebarInset * 2.0f;
      const float line_height = text_renderer_.LineHeight();
      const bool panel_focused =
          project_state.surface.focus == FocusTarget::Sidebar && sidebar_mode == SidebarMode::Git;
      const bool subject_focused =
          panel_focused && workflow.focus_field == CommitWorkflowFocusField::Subject;
      const bool body_focused =
          panel_focused && workflow.focus_field == CommitWorkflowFocusField::Body;
      const bool caret_on = CaretVisibleNow();
      workflow.caret_rect = SDL_FRect{};
      float panel_y = summary_y + 4.0f;
      DrawTextOn(text_renderer_, renderer, field_x, panel_y, theme_.text_primary,
                 theme_.surface_background,
                 TruncateLabel(workflow.staged_summary_line, field_w));
      panel_y += 14.0f;

      // --- Subject: a framed single-line input. ComputeSingleLineViewMetrics gives the
      //     in-field horizontal scroll, caret x, and visible selection slice. ---
      DrawTextOn(text_renderer_, renderer, field_x, panel_y, theme_.text_muted,
                 theme_.surface_background, "Subject:");
      panel_y += 16.0f;
      const SDL_FRect subject_field =
          MakeRect(field_x, panel_y, field_w, line_height + 6.0f);
      workflow.subject_field_rect = subject_field;
      DrawTextFieldFrame(renderer, theme_, subject_field, subject_focused);
      const float subject_text_x = subject_field.x + 6.0f;
      const float subject_text_y = subject_field.y + std::floor((subject_field.h - line_height) * 0.5f);
      const float subject_avail = std::max(1.0f, subject_field.w - 12.0f);
      if (workflow.subject.text().empty() && !subject_focused) {
        DrawTextOn(text_renderer_, renderer, subject_text_x, subject_text_y, theme_.text_muted,
                   theme_.surface_background, "<required>");
      } else {
        const auto subject_vm = ComputeSingleLineViewMetrics(workflow.subject, "", subject_avail);
        if (subject_vm.selection_bytes.has_value()) {
          const auto [sb, se] = *subject_vm.selection_bytes;
          const std::string_view shown = subject_vm.displayed_text;
          if (se <= shown.size() && sb < se) {
            const float sx = subject_text_x + text_renderer_.MeasureWidth(shown.substr(0, sb));
            const float sw = text_renderer_.MeasureWidth(shown.substr(sb, se - sb));
            DrawFilledRect(renderer, MakeRect(sx, subject_field.y + 2.0f, sw, subject_field.h - 4.0f),
                           theme_.selection_fill);
          }
        }
        DrawTextOn(text_renderer_, renderer, subject_text_x, subject_text_y, theme_.text_primary,
                   theme_.surface_background, subject_vm.displayed_text);
        if (subject_focused) {
          const SDL_FRect caret = MakeRect(subject_text_x + subject_vm.cursor_x,
                                           subject_field.y + 2.0f, 1.0f, subject_field.h - 4.0f);
          workflow.caret_rect = caret;
          if (caret_on) {
            DrawFilledRect(renderer, caret, theme_.text_primary);
          }
        }
      }
      panel_y += subject_field.h + 6.0f;

      // --- Body: a framed multi-line edit area with its own caret/selection + scroll. ---
      DrawTextOn(text_renderer_, renderer, field_x, panel_y, theme_.text_muted,
                 theme_.surface_background, "Body:");
      panel_y += 16.0f;
      const int body_rows = 4;
      const SDL_FRect body_field =
          MakeRect(field_x, panel_y, field_w, line_height * static_cast<float>(body_rows) + 6.0f);
      workflow.body_field_rect = body_field;
      workflow.body_visible_rows = body_rows;
      DrawTextFieldFrame(renderer, theme_, body_field, body_focused);
      RenderCommitBodyField(text_renderer_, theme_, renderer, workflow.body, body_field, body_rows,
                            body_focused, caret_on, &workflow.caret_rect);
      panel_y += body_field.h + 6.0f;

      for (const project::CommitPreCheck& check : workflow.checks) {
        const SDL_Color color = check.severity == project::CommitPreCheckSeverity::Blocking
                                    ? theme_.text_primary
                                    : theme_.text_muted;
        DrawTextOn(text_renderer_, renderer, layout.sidebar.x + kSidebarInset, panel_y, color,
                   theme_.surface_background,
                   TruncateLabel(check.message, layout.sidebar.w - kSidebarInset * 2.0f));
        panel_y += 12.0f;
      }
      if (!workflow.status_message.empty()) {
        DrawTextOn(text_renderer_, renderer, layout.sidebar.x + kSidebarInset, panel_y,
                   theme_.text_muted, theme_.surface_background,
                   TruncateLabel(workflow.status_message, layout.sidebar.w - kSidebarInset * 2.0f));
      }
    }

    const auto lines = BuildGitSidebarLines();
    const auto list_layout = ComputeGitSidebarListLayout(layout.sidebar, lines.size());
    const int scroll_row = list_layout.scroll_row;
    project_state.sidebar.scroll_row = scroll_row;

    for (int row = 0; row < list_layout.visible_rows; ++row) {
      const int line_index = scroll_row + row;
      if (line_index >= static_cast<int>(lines.size())) {
        break;
      }

      const auto& line = lines[static_cast<std::size_t>(line_index)];
      SDL_FRect row_rect = ScrollableListRowRect(list_layout, row);

      if (line.kind == GitSidebarLine::Kind::Header) {
        // Section band: a subtle raised strip with a hairline divider on top so the
        // Conflicts / Staged / Unstaged / Untracked / Outgoing groups read as distinct
        // blocks instead of a flat run of text.
        DrawFilledRect(renderer, row_rect, theme_.surface_raised);
        DrawFilledRect(renderer, MakeRect(row_rect.x, row_rect.y, row_rect.w, 1.0f),
                       theme_.border);
        float label_width = row_rect.w - 8.0f;
        if (line.section == GitSidebarEntry::Section::Outgoing) {
          if (const auto button_rect = GitSidebarOutgoingBaseButtonRect(layout.sidebar);
              button_rect.has_value()) {
            const bool hovered =
                last_mouse_position_valid_ && Contains(*button_rect, last_mouse_x_, last_mouse_y_);
            DrawFilledRect(renderer, *button_rect,
                           outgoing_base_menu_open || hovered ? theme_.row_highlight
                                                              : theme_.surface_background);
            DrawRect(renderer, *button_rect,
                     outgoing_base_menu_open ? theme_.accent
                                             : hovered ? theme_.text_secondary : theme_.border);
            const float chevron_x =
                std::floor(button_rect->x + (button_rect->w - 8.0f) * 0.5f);
            const float chevron_center_y =
                std::floor(button_rect->y + button_rect->h * 0.5f);
            DrawChevron(renderer, chevron_x, chevron_center_y, true,
                        outgoing_base_menu_open || hovered ? theme_.text_primary
                                                           : theme_.text_muted);
            label_width =
                std::max(0.0f, button_rect->x - row_rect.x - 8.0f);
          }
        }
        DrawVCenteredTextOn(text_renderer_, renderer, row_rect, 8.0f, theme_.text_secondary,
                            theme_.surface_raised,
                            TruncateLabel(line.label, label_width));
        continue;
      }
      if (line.kind == GitSidebarLine::Kind::Directory) {
        const float depth_offset = static_cast<float>(line.depth) * kTreeIndentWidth;
        const float tree_x = row_rect.x + 6.0f + depth_offset;
        const float chevron_x = tree_x;
        const float label_x = tree_x + kTreeChevronSlotWidth + 4.0f;
        const float label_width = std::max(20.0f, row_rect.x + row_rect.w - label_x - 8.0f);
        DrawChevron(renderer, chevron_x, row_rect.y + row_rect.h * 0.5f, line.expanded,
                    theme_.text_muted);
        DrawVCenteredTextOn(
            text_renderer_, renderer,
            MakeRect(label_x, row_rect.y, label_width, row_rect.h), 0.0f, theme_.text_primary,
            theme_.surface_background, TruncateLabel(line.label, label_width));
        continue;
      }
      if (line.kind == GitSidebarLine::Kind::Empty || line.entry_index < 0) {
        DrawVCenteredTextOn(text_renderer_, renderer, row_rect, 4.0f, theme_.text_disabled,
                            theme_.surface_background,
                            TruncateLabel(line.label, row_rect.w - 8.0f));
        continue;
      }
      if (static_cast<std::size_t>(line.entry_index) >= project_state.sidebar.git.entries.size()) {
        continue;
      }

      const auto& entry = project_state.sidebar.git.entries[static_cast<std::size_t>(line.entry_index)];
      const GitSidebarRowViewModel* row_vm = nullptr;
      if (sidebar_vm.git_sidebar.has_value()) {
        for (const GitSidebarSectionViewModel& section : sidebar_vm.git_sidebar->sections) {
          for (const GitSidebarRowViewModel& candidate : section.rows) {
            if (candidate.entry_index == line.entry_index) {
              row_vm = &candidate;
              break;
            }
          }
          if (row_vm != nullptr) {
            break;
          }
        }
      }
      const bool selected =
          static_cast<std::size_t>(line.entry_index) == project_state.sidebar.git.selected_index;
      DrawSelectableRowBackground(renderer, theme_, row_rect, theme_.surface_background, selected,
                                  selected);

      const float depth_offset = static_cast<float>(line.depth) * kTreeIndentWidth;
      const float tree_x = row_rect.x + 6.0f + depth_offset;
      const float label_x = tree_x + kTreeChevronSlotWidth + 4.0f;

      const project::GitFileStatus row_status = row_vm != nullptr ? row_vm->status : entry.status;
      const char git_marker = GitMarker(row_status);
      const std::string_view marker_text =
          git_marker == ' ' ? std::string_view{} : std::string_view(&git_marker, 1);
      const GitSidebarEntryActionLayout actions =
          ComputeGitSidebarEntryActionLayout(row_rect, entry);
      float right_edge = actions.content_right_edge;

      const auto draw_button = [&](const SDL_FRect& button_rect,
                                   std::string_view label,
                                   ButtonTone tone,
                                   bool enabled) {
        DrawButtonCentered(
            text_renderer_, renderer, theme_, button_rect, label, tone,
            ButtonVisualState{
                .enabled = enabled,
                .hovered = enabled && last_mouse_position_valid_ &&
                           Contains(button_rect, last_mouse_x_, last_mouse_y_),
                .active = selected,
            });
      };

      const GitSidebarActionAvailability row_actions =
          row_vm != nullptr ? row_vm->actions
                            : GitSidebarActionAvailabilityForEntry(
                                  entry, project_state.sidebar.git.repo_available,
                                  project_state.sidebar.git.supports_mutations);
      if (actions.primary_rect.has_value()) {
        draw_button(*actions.primary_rect, row_actions.unstage ? "Unstage" : "Stage",
                    ButtonTone::Accent, row_actions.stage || row_actions.unstage);
      }
      if (actions.discard_rect.has_value()) {
        draw_button(*actions.discard_rect, "Discard", ButtonTone::Destructive, row_actions.discard);
      }

      // Leading status badge: a bright, status-colored M/A/D/U/! glyph in a fixed slot
      // before the filename. This carries the per-file git state with color instead of a
      // dim letter tucked at the right edge, which is what made the panel read as a wall
      // of text. No background chip — a saturated glyph on the row reads cleanly and avoids
      // the contrast/coverage problems of a small filled box behind the letter.
      const SDL_Color row_background =
          selected ? theme_.row_highlight : theme_.surface_background;
      constexpr float kBadgeSlot = 18.0f;
      float name_x = label_x;
      if (!marker_text.empty()) {
        const SDL_Color badge_color = selected ? theme_.text_primary : GitMarkerColor(theme_, row_status);
        DrawCenteredTextOn(text_renderer_, renderer,
                           MakeRect(label_x, row_rect.y, kBadgeSlot, row_rect.h), badge_color,
                           row_background, marker_text);
        name_x = label_x + kBadgeSlot;
      }

      std::string primary_label = line.label.empty() ? entry.relative_path.filename().string() : line.label;
      if (row_vm != nullptr && !row_vm->review_marker_label.empty()) {
        primary_label = "[" + row_vm->review_marker_label + "] " + primary_label;
      }
      const std::string secondary_label;
      const SDL_Color primary_color = selected ? theme_.text_primary : theme_.text_secondary;
      const SDL_Color secondary_color = selected ? theme_.text_secondary : theme_.text_muted;
      DrawPrimarySecondaryRowText(text_renderer_, renderer, row_rect, name_x, right_edge,
                                  primary_color, secondary_color, row_background, primary_label,
                                  secondary_label, 1.0f);
    }

    draw_vertical_scrollbar(list_layout.list_rect, static_cast<float>(lines.size()),
                            list_layout.visible_units, static_cast<float>(scroll_row),
                            context_.interaction_state.drag_target == DragTarget::SidebarScrollbar);
  } else if (sidebar_mode == SidebarMode::Plugin) {
    const auto list_layout =
        ComputePluginSidebarListLayout(layout.sidebar, project_state.sidebar.plugin.items.size());
    const int scroll_row = list_layout.scroll_row;

    for (int row = 0; row < list_layout.visible_rows; ++row) {
      const int item_index = scroll_row + row;
      if (item_index >= static_cast<int>(project_state.sidebar.plugin.items.size())) {
        break;
      }

      const auto& item = project_state.sidebar.plugin.items[static_cast<std::size_t>(item_index)];
      SDL_FRect row_rect = ScrollableListRowRect(list_layout, row);
      const bool selected =
          static_cast<std::size_t>(item_index) == project_state.sidebar.plugin.selected_index;
      const SDL_Color row_background =
          selected ? theme_.row_highlight : theme_.surface_background;
      DrawSelectableRowBackground(renderer, theme_, row_rect, theme_.surface_background, selected,
                                  selected);

      const SDL_Color primary_color = selected ? theme_.text_primary : theme_.text_secondary;
      const SDL_Color secondary_color = selected ? theme_.text_secondary : theme_.text_muted;
      DrawPrimarySecondaryRowText(text_renderer_, renderer, row_rect, row_rect.x + 6.0f,
                                  row_rect.x + row_rect.w - 6.0f, primary_color, secondary_color,
                                  row_background, item.label, item.detail, 0.62f);
    }

    const std::string placeholder =
        !project_state.sidebar.plugin.error.empty()
            ? "Error: " + project_state.sidebar.plugin.error
            : project_state.sidebar.plugin.items.empty()
                ? FormatEmptyState("items")
                : std::string{};
    if (!placeholder.empty()) {
      DrawTextOn(text_renderer_, renderer, layout.sidebar.x + kSidebarInset,
                 list_layout.row_y + 4.0f,
                 project_state.sidebar.plugin.error.empty() ? theme_.text_muted : theme_.diff_deleted,
                 theme_.surface_background,
                 TruncateLabel(placeholder, layout.sidebar.w - kSidebarInset * 2.0f));
    }

    draw_vertical_scrollbar(list_layout.list_rect, static_cast<float>(project_state.sidebar.plugin.items.size()),
                            list_layout.visible_units, static_cast<float>(scroll_row),
                            context_.interaction_state.drag_target == DragTarget::SidebarScrollbar);
  } else {
    const SDL_FRect collapse_rect = TreeSidebarCollapseButtonRect(layout.sidebar);
    const SDL_FRect refresh_rect = TreeSidebarRefreshButtonRect(layout.sidebar);
    const bool compact_tree_header = collapse_rect.w <= 24.0f || refresh_rect.w <= 24.0f;
    draw_action_button(collapse_rect, compact_tree_header ? "C" : "Collapse",
                       project_state.directory_tree.CanCollapseAll());
    draw_action_button(refresh_rect, compact_tree_header ? "R" : "Refresh", true);

    if (!compact_tree_header) {
      const std::string tree_root_label = ProjectLabel();
      const float root_label_left = sidebar_mode_rect.x + sidebar_mode_rect.w + 10.0f;
      // Project actions now render on a second row, so first-row label width
      // should not reserve horizontal space for those buttons anymore.
      const float root_label_right = layout.sidebar.x + layout.sidebar.w - kSidebarInset;
      const float root_label_max_width = std::max(0.0f, root_label_right - root_label_left);
      const std::string root_label = TruncateLabel(tree_root_label, root_label_max_width);
      if (!root_label.empty()) {
        DrawCenteredTextOn(text_renderer_, renderer,
                           MakeRect(root_label_left, layout.sidebar.y + 4.0f, root_label_max_width,
                                    18.0f),
                           theme_.chrome_text_secondary, theme_.chrome_background, root_label);
      }
    }

    const auto& entries = project_state.directory_tree.entries();
    const auto list_layout = ComputeTreeSidebarListLayout(layout.sidebar, entries.size());
    const int scroll_row = list_layout.scroll_row;

    for (int row = 0; row < list_layout.visible_rows; ++row) {
      const int entry_index = scroll_row + row;
      if (entry_index >= static_cast<int>(entries.size())) {
        break;
      }

      const auto& entry = entries[entry_index];
      SDL_FRect row_rect = ScrollableListRowRect(list_layout, row);
      const bool selected =
          static_cast<std::size_t>(entry_index) == project_state.directory_tree.selected_index();
      DrawSelectableRowBackground(renderer, theme_, row_rect, theme_.surface_background, selected,
                                  selected);

      const float depth_offset = static_cast<float>(entry.depth) * kTreeIndentWidth;
      const float tree_x = row_rect.x + 6.0f + depth_offset;
      const float chevron_x = tree_x;
      const float label_x = tree_x + kTreeChevronSlotWidth + 4.0f;
      const float chevron_center_y = row_rect.y + row_rect.h * 0.5f;
      const char git_marker = GitMarker(entry.git_status);
      const bool has_git_marker = git_marker != ' ';
      // Hold as a single-char view into the local `git_marker` storage; allocation-free per row.
      const std::string_view git_marker_text =
          has_git_marker ? std::string_view(&git_marker, 1) : std::string_view{};
      const float marker_width =
          has_git_marker ? text_renderer_.MeasureWidth(git_marker_text) : 0.0f;
      const float marker_x = row_rect.x + row_rect.w - marker_width - 8.0f;
      const float label_width =
          has_git_marker ? std::max(20.0f, marker_x - label_x - 8.0f)
                         : std::max(20.0f, row_rect.x + row_rect.w - label_x - 8.0f);

      if (entry.is_directory) {
        DrawChevron(renderer, chevron_x, chevron_center_y, entry.expanded,
                    selected ? theme_.text_primary : theme_.text_muted);
      }

      DrawVCenteredTextOn(
          text_renderer_, renderer,
          MakeRect(label_x, row_rect.y, label_width, row_rect.h), 0.0f,
          selected
              ? theme_.text_primary
              : (entry.ignored ? theme_.text_muted
                               : (entry.is_directory ? theme_.text_primary : theme_.text_secondary)),
          selected ? theme_.row_highlight : theme_.surface_background,
          TruncateLabel(entry.label, label_width));
      if (has_git_marker) {
        DrawVCenteredTextOn(
            text_renderer_, renderer,
            MakeRect(marker_x, row_rect.y, marker_width, row_rect.h), 0.0f,
            selected ? theme_.text_primary : GitMarkerColor(theme_, entry.git_status),
            selected ? theme_.row_highlight : theme_.surface_background, git_marker_text);
      }
    }

    draw_vertical_scrollbar(list_layout.list_rect, static_cast<float>(entries.size()),
                            list_layout.visible_units, static_cast<float>(scroll_row),
                            context_.interaction_state.drag_target == DragTarget::SidebarScrollbar);
  }

  const auto draw_sidebar_tooltip = [&](const std::string& label) {
    if (label.empty()) {
      return;
    }
    const auto tooltip = BuildTooltipLayout(text_renderer_, label,
                                            std::max(160.0f, layout.full.w - 24.0f));
    const float tooltip_x =
        std::clamp(last_mouse_x_ + 12.0f, layout.full.x + 8.0f,
                   layout.full.x + layout.full.w - tooltip.rect.w - 8.0f);
    const float tooltip_y =
        last_mouse_y_ - tooltip.rect.h - 10.0f >= layout.full.y + 8.0f
            ? last_mouse_y_ - tooltip.rect.h - 10.0f
            : std::clamp(last_mouse_y_ + 14.0f, layout.full.y + 8.0f,
                         layout.full.y + layout.full.h - tooltip.rect.h - 8.0f);
    DrawTooltip(text_renderer_, renderer, theme_,
                MakeRect(tooltip_x, tooltip_y, tooltip.rect.w, tooltip.rect.h), tooltip.text);
  };

  if (project_state.surface.focus == FocusTarget::Sidebar) {
    DrawSurfaceFocusRing(renderer, layout.sidebar);
  }

  draw_sidebar_tooltip(HoveredGitSidebarTooltipLabel(layout.sidebar));
  draw_sidebar_tooltip(HoveredSidebarSearchTooltipLabel(layout.sidebar));
}

}  // namespace microide::workspace
