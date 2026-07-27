#include "workspace/WorkspaceShellRenderPrimitives.h"

#include "workspace/ProjectSearchPanelLayout.h"
#include "workspace/GitSidebarHeaderLayout.h"

#include <algorithm>
#include <cmath>
#include <string>

#include "editor/GutterIconRegistry.h"
#include "workspace/CommitWorkflowLayout.h"
#include "workspace/CommitWorkflowService.h"
#include "workspace/CommitWorkflowState.h"
#include "workspace/GitSidebarCommandCenter.h"
#include "workspace/SettingFlags.h"
#include "workspace/WorkspaceGitSidebarPresentation.h"
#include "workspace/WorkspaceSidebarRegistry.h"

namespace microide::workspace {

using namespace detail;

namespace {

constexpr float kSidebarInset = 10.0f;
// Frame prep derives the commit-draft body field width from the sidebar width and
// its own copy of this inset (it has no panel layout to read). If the two ever
// drift, the viewport is sized for a different width than the field is drawn at
// and the body's wrap/scroll silently disagrees with what is painted.
static_assert(kSidebarInset == kCommitWorkflowFieldInset,
              "PrepareCommitBodyViewportForFrame sizes the commit body field with "
              "kCommitWorkflowFieldInset; keep it equal to the sidebar inset");
constexpr float kTreeIndentWidth = kWorkspaceSidebarTreeIndentWidth;
constexpr float kTreeChevronSlotWidth = kWorkspaceSidebarTreeChevronSlotWidth;

// Reuse a thread-local scratch so the per-result label assembly is allocation-bounded by max label
// width, not by `results × frames`. The render path only inspects the returned view immediately
// after this call, so the scratch's lifetime is safe.
// Number of decimal digits in `value` (>=1). Used to size the "line:col  "
// prefix when positioning the preview match highlight, without materializing the
// label string (render TUs must avoid std::to_string).
std::size_t DecimalDigitCount(std::size_t value) {
  std::size_t digits = 1;
  while (value >= 10) {
    value /= 10;
    ++digits;
  }
  return digits;
}

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

// Renders the multi-line commit body edit area (visible lines, selection, caret). The
// viewport sizing + caret-keep-visible scroll clamp happen in frame prep
// (PrepareCommitBodyViewportForFrame), so this is a pure draw of already-prepared
// state (TD-2026-07-17-083 residual). Writes the focused field's caret rect to
// `caret_rect_out` so the caret-redraw path can target a tight dirty region.
void RenderCommitBodyField(const render::TextRenderer& tr, const render::Theme& theme,
                           SDL_Renderer* renderer, const editor::TextViewport& body,
                           const SDL_FRect& field, int visible_rows, bool focused,
                           bool caret_visible, SDL_FRect* caret_rect_out) {
  const float line_height = tr.LineHeight();
  const float text_x = field.x + 6.0f;
  const float top_y = field.y + 3.0f;
  const float avail_w = std::max(1.0f, field.w - 12.0f);

  // Draw only the visible rows via zero-copy LineView. TextBuffer::Snapshot() is a
  // whole-document materialization documented for cold paths; a large pasted commit
  // body must not pay an O(body) copy every paint. (TD-2026-07-17-083.)
  const editor::TextBuffer& body_lines = body.lines();
  const std::size_t line_count = body_lines.size();
  const std::size_t scroll = body.scroll_line();
  const std::optional<editor::SelectionRange> selection = body.selection_range();
  const std::size_t caret_line = body.cursor_line();
  const std::size_t caret_col = body.cursor_column();

  if (line_count == 1 && body_lines.LineView(0).empty() && !focused) {
    DrawTextOn(tr, renderer, text_x, top_y, theme.text_muted, theme.surface_background, "<optional>");
    return;
  }

  for (int row = 0; row < visible_rows; ++row) {
    const std::size_t line_index = scroll + static_cast<std::size_t>(row);
    if (line_index >= line_count) {
      break;
    }
    const std::string_view text = body_lines.LineView(line_index);
    const float row_y = top_y + static_cast<float>(row) * line_height;

    if (selection.has_value() && line_index >= selection->start.line &&
        line_index <= selection->end.line) {
      const std::size_t sel_start_col =
          line_index == selection->start.line ? selection->start.column : 0;
      const std::size_t sel_end_col =
          line_index == selection->end.line ? std::min(selection->end.column, text.size())
                                            : text.size();
      if (sel_start_col < sel_end_col && sel_start_col <= text.size()) {
        const float sx = text_x + tr.MeasureWidth(text.substr(0, sel_start_col));
        const float sw = tr.MeasureWidth(text.substr(sel_start_col, sel_end_col - sel_start_col));
        if (sw > 0.0f) {
          FillRect(renderer, MakeRect(sx, row_y - 1.0f, sw, line_height), theme.selection_fill);
        }
      }
    }

    DrawTextOn(tr, renderer, text_x, row_y, theme.text_primary, theme.surface_background,
               tr.TruncateToWidth(text, avail_w));

    if (focused && line_index == caret_line) {
      const std::size_t clamped_col = std::min(caret_col, text.size());
      const float caret_x = std::min(text_x + tr.MeasureWidth(text.substr(0, clamped_col)),
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

  // Leading git status badge shared by the file (Tree) sidebar and the Source Control
  // (Git) sidebar so both trees show the colored M/A/D/U/! marker at the START of the
  // row in a fixed slot. Returns the x where the row label/name should begin.
  constexpr float kGitBadgeSlot = 18.0f;
  const auto draw_leading_git_badge = [&](const SDL_FRect& row_rect, float label_x,
                                          project::GitFileStatus status, bool selected,
                                          const SDL_Color& row_background) -> float {
    const char marker = GitMarker(status);
    if (marker == ' ') {
      return label_x;
    }
    const std::string_view marker_text(&marker, 1);
    DrawCenteredTextOn(text_renderer_, renderer,
                       MakeRect(label_x, row_rect.y, kGitBadgeSlot, row_rect.h),
                       selected ? theme_.text_primary : GitMarkerColor(theme_, status),
                       row_background, marker_text);
    return label_x + kGitBadgeSlot;
  };

  // Mode-switch tab row (Project / Search / Source Control + optional overflow), replacing the
  // old dropdown. Single-click switches view; the row collapses to icon-only when narrow.
  const std::string_view active_view_id = project_state.sidebar.view_id;
  const SidebarModeRowLayout mode_row = SidebarModeRow(layout.sidebar);
  const bool sidebar_mode_open =
      context_.menu_state.menu_bar_open && context_.menu_state.active_menu_id == MenuId::SidebarMode &&
      context_.menu_state.active_menu_anchor_rect.has_value();
  const auto draw_mode_glyph = [&](SidebarMode mode, const SDL_FRect& icon_rect, SDL_Color color) {
    switch (mode) {
      case SidebarMode::Tree: DrawFolderGlyph(renderer, icon_rect, color); break;
      case SidebarMode::Search: DrawSearchGlyph(renderer, icon_rect, color); break;
      case SidebarMode::Git: DrawBranchGlyph(renderer, icon_rect, color); break;
      default: break;
    }
  };
  for (int i = 0; i < mode_row.tab_count; ++i) {
    const SidebarModeTab& tab = mode_row.tabs[static_cast<std::size_t>(i)];
    const bool active = tab.id == active_view_id;
    const bool hovered =
        last_mouse_position_valid_ && Contains(tab.rect, last_mouse_x_, last_mouse_y_);
    const ButtonColors colors = ResolveButtonColors(
        theme_, ButtonTone::Neutral,
        ButtonVisualState{.enabled = true, .hovered = hovered, .active = active});
    DrawFilledRect(renderer, tab.rect, colors.fill);
    DrawRect(renderer, tab.rect, colors.border);
    if (mode_row.icon_only) {
      draw_mode_glyph(tab.mode, tab.rect, colors.text);
    } else {
      const SDL_FRect icon_rect = MakeRect(tab.rect.x + 4.0f, tab.rect.y, 16.0f, tab.rect.h);
      draw_mode_glyph(tab.mode, icon_rect, colors.text);
      const float label_x = icon_rect.x + icon_rect.w + 1.0f;
      const SidebarViewSpec* spec = FindBuiltinSidebarView(tab.mode);
      const std::string_view tab_label = spec != nullptr ? spec->label : std::string_view{};
      DrawVCenteredTextOn(
          text_renderer_, renderer,
          MakeRect(label_x, tab.rect.y, std::max(0.0f, tab.rect.x + tab.rect.w - label_x - 4.0f),
                   tab.rect.h),
          0.0f, colors.text, colors.fill, tab_label);
    }
  }
  if (mode_row.has_overflow) {
    const bool hovered =
        last_mouse_position_valid_ && Contains(mode_row.overflow_rect, last_mouse_x_, last_mouse_y_);
    const bool active = sidebar_mode_open || FindBuiltinSidebarView(active_view_id) == nullptr;
    const ButtonColors colors = ResolveButtonColors(
        theme_, ButtonTone::Neutral,
        ButtonVisualState{.enabled = true, .hovered = hovered, .active = active});
    DrawFilledRect(renderer, mode_row.overflow_rect, colors.fill);
    DrawRect(renderer, mode_row.overflow_rect, colors.border);
    DrawEllipsisGlyph(renderer, mode_row.overflow_rect, colors.text);
  }

  const SidebarMode sidebar_mode = sidebar_vm.mode;
  if (sidebar_mode == SidebarMode::Search) {
    const auto& ps = project_state.overlay.workflow.project_search;
    const TextInputSurface current_surface = text_input_vm.current_surface;
    const bool sidebar_needs_visual = IsSidebarSearchFieldSurface(current_surface);
    const auto visual =
        sidebar_needs_visual ? BuildActiveTextInputVisual(layout, std::nullopt) : std::nullopt;
    const auto sidebar_display_text = [&](TextInputSurface surface,
                                          std::string_view fallback) -> std::string_view {
      if (visual.has_value() && visual->surface == surface && !visual->displayed_text.empty()) {
        return visual->displayed_text;
      }
      return fallback;
    };

    // One draw per search field: query, replace, and (when the scope section is
    // expanded) the include/exclude glob boxes. A collapsed scope field has a
    // zero-sized rect, which the loop skips.
    // Placeholder/committed text per field, indexed the same as the shared field
    // list below so the two never fall out of order.
    const std::string_view fallbacks[] = {
        sidebar_vm.query_fallback_text,
        sidebar_vm.replace_fallback_text,
        sidebar_vm.include_fallback_text,
        sidebar_vm.exclude_fallback_text,
    };
    const auto search_fields =
        project_search_panel::SidebarSearchFieldRects(layout.sidebar, ps.scope_expanded);
    for (std::size_t index = 0; index < search_fields.size(); ++index) {
      const auto& field = search_fields[index];
      if (field.rect.w <= 0.0f) {
        continue;
      }
      DrawTextFieldFrame(renderer, theme_, field.rect, current_surface == field.surface);
      DrawSingleLineTextTail(renderer, field.rect.x + 6.0f, field.rect.y + 3.0f,
                             std::max(1.0f, field.rect.w - 12.0f),
                             ps.editing && ps.edit_field == field.field ? theme_.text_primary
                                                                       : theme_.text_secondary,
                             theme_.surface_background,
                             sidebar_display_text(field.surface, fallbacks[index]));
    }
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

    draw_search_button(project_search_panel::ModeButtonRect(layout.sidebar), ProjectSearchModeButtonLabel(),
                       project_state.overlay.workflow.project_search.options.pattern_mode ==
                           project::ProjectSearchPatternMode::Regex);
    draw_search_button(project_search_panel::CaseButtonRect(layout.sidebar), ProjectSearchCaseButtonLabel(),
                       project_state.overlay.workflow.project_search.options.case_mode !=
                           project::ProjectSearchCaseMode::Smart);
    draw_search_button(project_search_panel::HiddenButtonRect(layout.sidebar),
                       ProjectSearchHiddenButtonLabel(),
                       project_state.overlay.workflow.project_search.options.show_hidden);
    draw_search_button(project_search_panel::ScopeButtonRect(layout.sidebar),
                       // "..." mirrors VS Code's show/hide-scope affordance; a
                       // constant label needs no shell accessor.
                       "...", ps.scope_expanded);

    // The status/hint line is composed once (and cached) in
    // RenderViewModelBuilder::BuildSidebarSurface; draw the prebuilt view.
    DrawTextOn(text_renderer_, renderer, layout.sidebar.x + kSidebarInset,
               layout.sidebar.y + project_search_panel::StatusTop(ps.scope_expanded), theme_.text_muted,
               theme_.surface_background,
               TruncateLabelView(sidebar_vm.project_search_status_text,
                                 layout.sidebar.w - kSidebarInset * 2.0f));

    const auto line_map = BuildProjectSearchLineMap();
    const auto list_layout = ComputeProjectSearchSidebarListLayout(layout.sidebar, line_map.size());
    // Honor the stored (clamped) scroll position so mouse-wheel/scrollbar scrolling
    // lets the user scroll freely past the selected row. The selected row is pulled
    // into view only on navigation (see MoveProjectSearchSelection), matching how
    // the git/problems/tests sidebars behave.
    const int scroll_row = list_layout.scroll_row;
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
                            TruncateLabelView(std::string_view(file_result.relative_path_string),
                                          row_rect.w - 8.0f));
        continue;
      }

      const auto& result =
          project_state.overlay.workflow.project_search.results[static_cast<std::size_t>(result_index)];
      const bool selected =
          static_cast<std::size_t>(result_index) ==
          project_state.overlay.workflow.project_search.selected_index;
      const bool emphasized = selected || PointerOver(row_rect);
      DrawSelectableRowBackground(renderer, theme_, row_rect, theme_.surface_background, emphasized,
                                  selected);

      // Highlight the matched span inside the preview with the editor's search
      // match colour. Positions assume the monospace sidebar font; non-ASCII
      // previews degrade to a slight horizontal offset.
      if (result.match_preview_length > 0) {
        const float char_width = std::max(1.0f, text_renderer_.CharWidth());
        const std::size_t prefix_chars = DecimalDigitCount(result.line + 1) + 1 +
                                         DecimalDigitCount(result.column + 1) + 2;
        const float text_x = row_rect.x + 6.0f;
        const float text_right = row_rect.x + row_rect.w - 6.0f;
        const float highlight_x =
            text_x +
            static_cast<float>(prefix_chars + result.match_preview_start) * char_width;
        const float highlight_w = static_cast<float>(result.match_preview_length) * char_width;
        if (highlight_x < text_right && highlight_w > 0.0f) {
          FillRect(renderer,
                   MakeRect(highlight_x, row_rect.y + 2.0f,
                            std::min(highlight_w, text_right - highlight_x), row_rect.h - 4.0f),
                   theme_.search_match);
        }
      }

      const std::string_view label =
          BuildProjectSearchResultLabel(result.line, result.column, result.preview);
      DrawVCenteredTextOn(text_renderer_, renderer, row_rect, 6.0f,
                          emphasized ? theme_.text_primary : theme_.text_secondary,
                          emphasized ? theme_.row_highlight : theme_.surface_background,
                          TruncateLabelView(label, row_rect.w - 12.0f));
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
                 TruncateLabelView(placeholder, layout.sidebar.w - kSidebarInset * 2.0f));
    }

    draw_vertical_scrollbar(list_layout.list_rect, static_cast<float>(line_map.size()),
                            list_layout.visible_units, static_cast<float>(scroll_row),
                            context_.interaction_state.drag_target == DragTarget::SidebarScrollbar);
  } else if (sidebar_mode == SidebarMode::Git) {
    const bool outgoing_base_menu_open =
        context_.menu_state.menu_bar_open &&
        context_.menu_state.active_menu_id == MenuId::GitOutgoingBase &&
        context_.menu_state.active_menu_anchor_rect.has_value();
    draw_action_button(git_sidebar_header::StageAllButtonRect(layout.sidebar), "Stage All",
                       CanStageAllGitSidebarEntries(), ButtonTone::Neutral);
    draw_action_button(git_sidebar_header::DiscardAllButtonRect(layout.sidebar), "Discard All",
                       CanDiscardAllGitSidebarEntries(), ButtonTone::Destructive);
    DrawButtonCentered(
        text_renderer_, renderer, theme_, git_sidebar_header::RefreshButtonRect(layout.sidebar), "Refresh",
        ButtonTone::Neutral,
        ButtonVisualState{
            .enabled = true,
            .hovered = last_mouse_position_valid_ &&
                       Contains(git_sidebar_header::RefreshButtonRect(layout.sidebar), last_mouse_x_,
                                last_mouse_y_),
            .active = false,
        });

    // Branch row: the current branch (opens the switch picker) plus Sync, which
    // carries the ahead/behind counts. Both labels are precomposed in the git
    // sidebar view model — this TU must not build strings.
    if (sidebar_vm.git_sidebar != nullptr) {
      const SDL_FRect branch_rect = git_sidebar_header::BranchButtonRect(layout.sidebar);
      const SDL_FRect sync_rect = git_sidebar_header::SyncButtonRect(layout.sidebar);
      DrawButtonCentered(
          text_renderer_, renderer, theme_, branch_rect,
          TruncateLabelView(sidebar_vm.git_sidebar->branch_button_label,
                            std::max(0.0f, branch_rect.w - 8.0f)),
          ButtonTone::Neutral,
          ButtonVisualState{
              .enabled = true,
              .hovered = last_mouse_position_valid_ &&
                         Contains(branch_rect, last_mouse_x_, last_mouse_y_),
              .active = false,
          });
      DrawButtonCentered(
          text_renderer_, renderer, theme_, sync_rect,
          TruncateLabelView(sidebar_vm.git_sidebar->sync_button_label,
                            std::max(0.0f, sync_rect.w - 8.0f)),
          ButtonTone::Neutral,
          ButtonVisualState{
              .enabled = true,
              .hovered =
                  last_mouse_position_valid_ && Contains(sync_rect, last_mouse_x_, last_mouse_y_),
              .active = false,
          });
    }

    const SDL_FRect branch_row = git_sidebar_header::ActionRowRect(layout.sidebar, 1);
    float summary_y = branch_row.y + branch_row.h + 10.0f;
    if (sidebar_vm.git_sidebar != nullptr) {
      const GitSidebarViewModel& git_vm = *sidebar_vm.git_sidebar;
      const float text_width = layout.sidebar.w - kSidebarInset * 2.0f;
      // Must match the constants in WorkspaceShellSidebar.cpp so the file list starts
      // exactly below the rendered summary block.
      constexpr float kSummaryLineHeight = 17.0f;
      constexpr float kCommitButtonHeight = 22.0f;
      constexpr float kCommitButtonGap = 8.0f;
      float detail_y = summary_y;
      // The commit-readiness line is replaced by a Commit button (the discoverable way
      // to open the inline commit draft). While the draft is open we instead show the
      // commit hint. The redundant staged/unstaged/outgoing counts are gone — they are
      // already on each section header.
      if (git_vm.show_commit_button) {
        const SDL_FRect commit_rect = git_sidebar_header::CommitButtonRect(layout.sidebar);
        draw_action_button(commit_rect, "Commit", git_vm.commit_ready, ButtonTone::Accent);
        if (!git_vm.commit_ready && !git_vm.commit_blocked_reason.empty()) {
          const float reason_x = commit_rect.x + commit_rect.w + 8.0f;
          DrawVCenteredTextOn(
              text_renderer_, renderer,
              MakeRect(reason_x, commit_rect.y,
                       std::max(0.0f, layout.sidebar.x + layout.sidebar.w - kSidebarInset - reason_x),
                       commit_rect.h),
              0.0f, theme_.text_muted, theme_.surface_background,
              TruncateLabelView(git_vm.commit_blocked_reason, text_width));
        }
        detail_y += kCommitButtonHeight + kCommitButtonGap;
      } else if (!git_vm.commit_summary_line.empty()) {
        DrawTextOn(text_renderer_, renderer, layout.sidebar.x + kSidebarInset, detail_y,
                   theme_.text_muted, theme_.surface_background,
                   TruncateLabelView(git_vm.commit_summary_line, text_width));
        detail_y += kSummaryLineHeight;
      }
      // summary_lines now carries only banners (stale snapshot / refresh error); the
      // branch line lives in the status bar.
      for (const std::string& summary : git_vm.summary_lines) {
        const SDL_Color color = summary == git_vm.error_banner ? theme_.diff_deleted
                                                               : theme_.text_secondary;
        DrawTextOn(text_renderer_, renderer, layout.sidebar.x + kSidebarInset, detail_y, color,
                   theme_.surface_background, TruncateLabelView(summary, text_width));
        detail_y += kSummaryLineHeight;
      }
      summary_y = detail_y;
    } else {
      for (const std::string& summary : GitSidebarSummaryLines()) {
        DrawTextOn(text_renderer_, renderer, layout.sidebar.x + kSidebarInset, summary_y,
                   theme_.text_muted, theme_.surface_background,
                   TruncateLabelView(summary, layout.sidebar.w - kSidebarInset * 2.0f));
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
      const int body_rows = kCommitWorkflowBodyRows;
      // Single source of truth for the panel geometry; GitSidebarCommitWorkflowHeight reserves
      // exactly CommitWorkflowLayout::total_height so the file list starts flush below.
      const CommitWorkflowLayout panel = ComputeCommitWorkflowLayout(
          summary_y + 4.0f, field_x, field_w, line_height, body_rows, workflow.checks.size(),
          !workflow.status_message.empty());

      DrawTextOn(text_renderer_, renderer, field_x, panel.staged_summary_y, theme_.text_primary,
                 theme_.surface_background,
                 TruncateLabelView(workflow.staged_summary_line, field_w));

      // --- Subject: a framed single-line input. ComputeSingleLineViewMetrics gives the
      //     in-field horizontal scroll, caret x, and visible selection slice. ---
      DrawTextOn(text_renderer_, renderer, field_x, panel.subject_label_y, theme_.text_muted,
                 theme_.surface_background, "Subject:");
      const SDL_FRect subject_field = panel.subject_field;
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

      // --- Body: a framed multi-line edit area with its own caret/selection + scroll. ---
      DrawTextOn(text_renderer_, renderer, field_x, panel.body_label_y, theme_.text_muted,
                 theme_.surface_background, "Body:");
      const SDL_FRect body_field = panel.body_field;
      workflow.body_field_rect = body_field;
      workflow.body_visible_rows = body_rows;
      DrawTextFieldFrame(renderer, theme_, body_field, body_focused);
      RenderCommitBodyField(text_renderer_, theme_, renderer, workflow.body, body_field, body_rows,
                            body_focused, caret_on, &workflow.caret_rect);

      float check_y = panel.checks_y;
      for (const project::CommitPreCheck& check : workflow.checks) {
        const SDL_Color color = check.severity == project::CommitPreCheckSeverity::Blocking
                                    ? theme_.text_primary
                                    : theme_.text_muted;
        DrawTextOn(text_renderer_, renderer, layout.sidebar.x + kSidebarInset, check_y, color,
                   theme_.surface_background,
                   TruncateLabelView(check.message, layout.sidebar.w - kSidebarInset * 2.0f));
        check_y += 14.0f;
      }
      if (!workflow.status_message.empty()) {
        DrawTextOn(text_renderer_, renderer, layout.sidebar.x + kSidebarInset, panel.status_y,
                   theme_.text_muted, theme_.surface_background,
                   TruncateLabelView(workflow.status_message, layout.sidebar.w - kSidebarInset * 2.0f));
      }

      // Confirm button: the discoverable way to commit the staged changes (mirrors the
      // Ctrl+Enter shortcut). Enabled only when the draft can actually be committed.
      workflow.commit_button_rect = panel.commit_button;
      draw_action_button(panel.commit_button, "Commit",
                         commit_workflow_service_.CanExecuteCommit(workflow), ButtonTone::Accent);
    }

    // Reuse the rows already flattened during PrepareFrameOnce instead of
    // rebuilding the git view model + line specs a second time this frame.
    static const std::vector<GitSidebarLine> kEmptyGitSidebarLines;
    const std::vector<GitSidebarLine>& lines = sidebar_vm.git_sidebar_lines != nullptr
                                                   ? *sidebar_vm.git_sidebar_lines
                                                   : kEmptyGitSidebarLines;
    const auto list_layout = ComputeGitSidebarListLayout(layout.sidebar, lines.size());
    const int scroll_row = list_layout.scroll_row;
    project_state.sidebar.scroll_row = scroll_row;

    // Resolve each git entry's row view model once into an entry_index-keyed
    // table (O(total_git_rows)), replacing the previous O(visible x total_rows)
    // nested section/row scan performed for every rendered row. entry_index is
    // validated < entries.size() below, so the table is sized to match.
    std::vector<const GitSidebarRowViewModel*> row_vm_by_entry;
    if (sidebar_vm.git_sidebar != nullptr) {
      row_vm_by_entry.assign(project_state.sidebar.git.entries.size(), nullptr);
      for (const GitSidebarSectionViewModel& section : sidebar_vm.git_sidebar->sections) {
        for (const GitSidebarRowViewModel& candidate : section.rows) {
          if (candidate.entry_index >= 0 &&
              static_cast<std::size_t>(candidate.entry_index) < row_vm_by_entry.size()) {
            row_vm_by_entry[static_cast<std::size_t>(candidate.entry_index)] = &candidate;
          }
        }
      }
    }

    // Reused across rows so the per-row primary label keeps its capacity instead
    // of allocating (twice, when a review marker prefixes it) each iteration.
    std::string primary_label;
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
                            TruncateLabelView(line.label, label_width));
        continue;
      }
      if (line.kind == GitSidebarLine::Kind::Directory) {
        const bool hovered = PointerOver(row_rect);
        const SDL_Color row_background =
            hovered ? theme_.row_highlight : theme_.surface_background;
        DrawSelectableRowBackground(renderer, theme_, row_rect, theme_.surface_background, hovered,
                                    false);
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
            row_background, TruncateLabelView(line.label, label_width));
        continue;
      }
      if (line.kind == GitSidebarLine::Kind::Empty || line.entry_index < 0) {
        DrawVCenteredTextOn(text_renderer_, renderer, row_rect, 4.0f, theme_.text_disabled,
                            theme_.surface_background,
                            TruncateLabelView(line.label, row_rect.w - 8.0f));
        continue;
      }
      if (static_cast<std::size_t>(line.entry_index) >= project_state.sidebar.git.entries.size()) {
        continue;
      }

      const auto& entry = project_state.sidebar.git.entries[static_cast<std::size_t>(line.entry_index)];
      const GitSidebarRowViewModel* row_vm =
          static_cast<std::size_t>(line.entry_index) < row_vm_by_entry.size()
              ? row_vm_by_entry[static_cast<std::size_t>(line.entry_index)]
              : nullptr;
      const bool selected =
          static_cast<std::size_t>(line.entry_index) == project_state.sidebar.git.selected_index;
      const bool emphasized = selected || PointerOver(row_rect);
      DrawSelectableRowBackground(renderer, theme_, row_rect, theme_.surface_background, emphasized,
                                  selected);

      const float depth_offset = static_cast<float>(line.depth) * kTreeIndentWidth;
      const float tree_x = row_rect.x + 6.0f + depth_offset;
      const float label_x = tree_x + kTreeChevronSlotWidth + 4.0f;

      const project::GitFileStatus row_status = row_vm != nullptr ? row_vm->status : entry.status;
      // Entry actions (stage/unstage/discard) live on the right-click context
      // menu, so the row text simply runs to the row's right padding.
      const float right_edge = row_rect.x + row_rect.w - 8.0f;

      // Leading status badge before the filename (see draw_leading_git_badge): a bright,
      // status-colored M/A/D/U/! glyph, matching the file tree.
      const SDL_Color row_background =
          emphasized ? theme_.row_highlight : theme_.surface_background;
      const float name_x =
          draw_leading_git_badge(row_rect, label_x, row_status, selected, row_background);

      // The full primary text (including any "[<marker>] " branch-review prefix) is
      // precomputed in the git presentation (TD-2026-07-17A-008), so render draws it
      // directly instead of assembling it per row.
      const std::string_view primary_label = line.display_label;
      const std::string_view secondary_label;
      const SDL_Color primary_color = emphasized ? theme_.text_primary : theme_.text_secondary;
      const SDL_Color secondary_color = emphasized ? theme_.text_secondary : theme_.text_muted;
      DrawPrimarySecondaryRowText(text_renderer_, renderer, row_rect, name_x, right_edge,
                                  primary_color, secondary_color, row_background, primary_label,
                                  secondary_label, 1.0f);
    }

    draw_vertical_scrollbar(list_layout.list_rect, static_cast<float>(lines.size()),
                            list_layout.visible_units, static_cast<float>(scroll_row),
                            context_.interaction_state.drag_target == DragTarget::SidebarScrollbar);
  } else if (sidebar_mode == SidebarMode::Plugin || sidebar_mode == SidebarMode::Outline) {
    // The outline view reuses the plugin item-tree surface; only the empty-state
    // noun differs (symbols vs items).
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
      const bool emphasized = selected || PointerOver(row_rect);
      const SDL_Color row_background =
          emphasized ? theme_.row_highlight : theme_.surface_background;
      DrawSelectableRowBackground(renderer, theme_, row_rect, theme_.surface_background, emphasized,
                                  selected);

      const SDL_Color primary_color = emphasized ? theme_.text_primary : theme_.text_secondary;
      const SDL_Color secondary_color = emphasized ? theme_.text_secondary : theme_.text_muted;

      // Tree rows: host-drawn indentation by depth plus a disclosure twisty on
      // collapsible rows. Flat sidebars (depth 0, not collapsible) keep the
      // original left inset so their layout is unchanged.
      const bool tree_row = item.depth > 0 || item.collapsible;
      float label_x = row_rect.x + 6.0f;
      if (tree_row) {
        const float depth_offset = static_cast<float>(item.depth) * kTreeIndentWidth;
        const float tree_x = row_rect.x + 6.0f + depth_offset;
        label_x = tree_x + kTreeChevronSlotWidth + 4.0f;
        if (item.collapsible) {
          DrawChevron(renderer, tree_x, row_rect.y + row_rect.h * 0.5f, !item.collapsed,
                      theme_.text_muted);
        }
      }
      DrawPrimarySecondaryRowText(text_renderer_, renderer, row_rect, label_x,
                                  row_rect.x + row_rect.w - 6.0f, primary_color, secondary_color,
                                  row_background, item.label, item.detail, 0.62f);
    }

    // Placeholder text is prebuilt on refresh (SidebarCoordinator::
    // RecomputePluginSidebarPlaceholder), so the render path never materializes
    // a string per frame.
    const std::string& placeholder = project_state.sidebar.plugin.placeholder;
    if (!placeholder.empty()) {
      DrawTextOn(text_renderer_, renderer, layout.sidebar.x + kSidebarInset,
                 list_layout.row_y + 4.0f,
                 project_state.sidebar.plugin.placeholder_is_error ? theme_.diff_deleted
                                                                   : theme_.text_muted,
                 theme_.surface_background,
                 TruncateLabelView(placeholder, layout.sidebar.w - kSidebarInset * 2.0f));
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

    // The project name no longer renders here — the mode tab row owns the header, and the
    // project name is already shown on the editor tab.

    const auto& entries = project_state.directory_tree.entries();
    const auto list_layout = ComputeTreeSidebarListLayout(layout.sidebar, entries.size());
    const int scroll_row = list_layout.scroll_row;

    // File icons are opt-in. They render only when a plugin contributed an icon
    // theme, or when the user explicitly enabled the built-in defaults via
    // `sidebar.file_icons`. With no plugin and the setting off (the default) the
    // entire icon path is skipped — no Resolve(), no cache allocation, no per-row
    // draw — so the file tree is byte-for-byte identical to a host without this
    // feature. The flag is read once per sidebar render (cheap inline check).
    auto& icon_cache = project_state.file_icon_cache;
    const bool file_icons_enabled =
        file_icon_registry_.has_entries() ||
        SettingFlagEnabled(GetSettingValue("sidebar.file_icons"));
    if (!file_icons_enabled) {
      // Drop any cache left over from a previous opt-in so the feature has zero
      // footprint while off.
      if (!icon_cache.icons.empty()) {
        icon_cache.Reset();
      }
    } else {
      // Resolve file icons once per tree mutation / icon-theme reload, not per row
      // per frame. The per-row loop below then just indexes the cache, keeping the
      // hot render path allocation- and lookup-free.
      const std::uint64_t tree_revision = project_state.directory_tree.entries_revision();
      const std::uint32_t icon_revision = file_icon_registry_.revision();
      if (icon_cache.tree_revision != tree_revision || icon_cache.icon_revision != icon_revision) {
        icon_cache.icons.assign(entries.size(), std::nullopt);
        for (std::size_t i = 0; i < entries.size(); ++i) {
          if (!entries[i].is_directory) {
            icon_cache.icons[i] = file_icon_registry_.Resolve(entries[i].label);
          }
        }
        icon_cache.tree_revision = tree_revision;
        icon_cache.icon_revision = icon_revision;
      }
    }

    for (int row = 0; row < list_layout.visible_rows; ++row) {
      const int entry_index = scroll_row + row;
      if (entry_index >= static_cast<int>(entries.size())) {
        break;
      }

      const auto& entry = entries[entry_index];
      SDL_FRect row_rect = ScrollableListRowRect(list_layout, row);
      const bool selected =
          static_cast<std::size_t>(entry_index) == project_state.directory_tree.selected_index();
      // Hover lifts only the row background (the accent strip and the brighter text
      // stay reserved for the actual selection), so the tree reads as "this is what
      // I'd click" without masquerading as selected.
      const bool emphasized = selected || PointerOver(row_rect);
      const SDL_Color row_background =
          emphasized ? theme_.row_highlight : theme_.surface_background;
      DrawSelectableRowBackground(renderer, theme_, row_rect, theme_.surface_background, emphasized,
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
      // The file tree keeps the status marker at the RIGHT edge: a leading badge would
      // shove directory names out of alignment with their chevrons, which reads badly.
      // Source Control (a flat per-file list) uses the leading badge instead.
      const float marker_x = row_rect.x + row_rect.w - marker_width - 8.0f;
      const float label_width =
          has_git_marker ? std::max(20.0f, marker_x - label_x - 8.0f)
                         : std::max(20.0f, row_rect.x + row_rect.w - label_x - 8.0f);

      if (entry.is_directory) {
        DrawChevron(renderer, chevron_x, chevron_center_y, entry.expanded,
                    selected ? theme_.text_primary : theme_.text_muted);
      } else if (file_icons_enabled && icon_cache.icons[entry_index]) {
        const auto& icon = icon_cache.icons[entry_index];
        // Files have no chevron, so the chevron slot holds the type icon. Selected
        // rows tint it to the foreground so it stays legible on the highlight.
        const SDL_Color icon_color = selected ? theme_.text_primary : icon->color;
        editor::GutterIconRegistry::Draw(renderer, icon->shape, icon_color, chevron_x, row_rect.y,
                                         kTreeChevronSlotWidth, row_rect.h);
      }

      DrawVCenteredTextOn(
          text_renderer_, renderer,
          MakeRect(label_x, row_rect.y, label_width, row_rect.h), 0.0f,
          selected
              ? theme_.text_primary
              : (entry.ignored ? theme_.text_muted
                               : (entry.is_directory ? theme_.text_primary : theme_.text_secondary)),
          row_background,
          TruncateLabelView(entry.label, label_width));
      if (has_git_marker) {
        DrawVCenteredTextOn(
            text_renderer_, renderer,
            MakeRect(marker_x, row_rect.y, marker_width, row_rect.h), 0.0f,
            selected ? theme_.text_primary : GitMarkerColor(theme_, entry.git_status),
            row_background, git_marker_text);
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
  draw_sidebar_tooltip(HoveredSidebarModeTooltipLabel(layout.sidebar));
}

}  // namespace microide::workspace
