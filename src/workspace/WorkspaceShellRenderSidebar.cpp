#include "workspace/WorkspaceShellRenderPrimitives.h"

#include <algorithm>
#include <cmath>
#include <string>

#include "editor/DiagnosticsRender.h"
#include "workspace/RenderViewModelBuilder.h"
#include "workspace/WorkspaceGitSidebarPresentation.h"

namespace microide::workspace {

using namespace detail;

namespace {

constexpr float kSidebarInset = 10.0f;
constexpr float kTreeIndentWidth = 14.0f;
constexpr float kTreeChevronSlotWidth = 12.0f;

std::string BuildProjectSearchResultLabel(std::size_t line,
                                          std::size_t column,
                                          std::string_view snippet) {
  std::string label;
  label.reserve(snippet.size() + 32);
  AppendUnsigned(label, line + 1);
  label += ":";
  AppendUnsigned(label, column + 1);
  label += "  ";
  label += snippet;
  return label;
}

}  // namespace

void WorkspaceShell::RenderSidebarSurface(SDL_Renderer* renderer, const WorkspaceLayout& layout) {
  const RenderViewModelBuilder view_model_builder(context_);
  const SidebarSurfaceViewModel sidebar_vm = view_model_builder.BuildSidebarSurface();
  const TextInputSurfaceViewModel text_input_vm = view_model_builder.BuildTextInputSurface();
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
                                      bool destructive = false) {
    DrawButtonCentered(
        text_renderer_, renderer, theme_, button_rect, label,
        destructive ? ButtonTone::Destructive : ButtonTone::Accent,
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
                                          const std::string& fallback) -> std::string_view {
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
        renderer, query_rect.x + 6.0f, query_rect.y + 2.0f,
        std::max(1.0f, query_rect.w - 12.0f),
        editing_query ? theme_.text_primary : theme_.text_secondary,
        theme_.surface_background,
        sidebar_display_text(TextInputSurface::SidebarSearchQuery, sidebar_vm.query_fallback_text));
    DrawSingleLineTextTail(
        renderer, replace_rect.x + 6.0f, replace_rect.y + 2.0f,
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
                      " matches")
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

      const std::string label =
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
  } else if (sidebar_mode == SidebarMode::Chat) {
    const ChatSidebarLayout chat_layout = ComputeChatSidebarLayout(layout.sidebar);
    const Conversation* conversation = ActiveConversation();
    const bool active_request =
        conversation != nullptr && project_state.panel.chat.request_in_flight &&
        project_state.panel.chat.request_conversation_id == conversation->id;
    const std::string title =
        conversation != nullptr && !conversation->title.empty() ? conversation->title : "Chat";
    const std::string status = active_request
                                   ? project_state.panel.chat.status_text
                               : conversation != nullptr &&
                                       conversation->status == RequestStatus::Failed
                                   ? "Failed"
                               : conversation != nullptr &&
                                       conversation->status == RequestStatus::Cancelled
                                   ? "Cancelled"
                                   : "Ready";
    DrawTextOn(text_renderer_, renderer, chat_layout.header_title_rect.x, chat_layout.header_title_rect.y,
               theme_.text_primary, theme_.surface_background,
               TruncateLabel(title, chat_layout.header_title_rect.w));
    DrawTextOn(text_renderer_, renderer,
               chat_layout.header_title_rect.x + chat_layout.header_title_rect.w + 8.0f,
               chat_layout.header_title_rect.y, theme_.text_muted, theme_.surface_background,
               TruncateLabel(status, std::max(0.0f, chat_layout.header_rect.w -
                                                        chat_layout.header_title_rect.w - 12.0f)));

    DrawRect(renderer, chat_layout.rail_rect, theme_.border);
    for (std::size_t i = 0;
         i < project_state.conversations.conversations().size();
         ++i) {
      const Conversation& item = project_state.conversations.conversations()[i];
      const SDL_FRect row_rect = ChatSidebarConversationRowRect(layout.sidebar, i);
      if (row_rect.y + row_rect.h > chat_layout.rail_list_rect.y + chat_layout.rail_list_rect.h) {
        break;
      }
      const bool active = item.id == project_state.panel.chat.conversation_id;
      DrawSelectableRowBackground(renderer, theme_, row_rect, theme_.surface_background, active,
                                  active);
      SDL_Color marker = theme_.text_muted;
      if (item.status == RequestStatus::Running || item.status == RequestStatus::Streaming) {
        marker = theme_.accent;
      } else if (item.status == RequestStatus::Failed || item.status == RequestStatus::Cancelled) {
        marker = theme_.diff_deleted;
      }
      DrawFilledRect(renderer, MakeRect(row_rect.x + 4.0f, row_rect.y + 6.0f, 6.0f, 10.0f), marker);
      DrawVCenteredTextOn(text_renderer_, renderer,
                          MakeRect(row_rect.x + 14.0f, row_rect.y, row_rect.w - 18.0f, row_rect.h),
                          0.0f, active ? theme_.text_primary : theme_.text_secondary,
                          active ? theme_.row_highlight : theme_.surface_background,
                          TruncateLabel(item.title.empty() ? "Chat" : item.title, row_rect.w - 22.0f));
      if (project_state.panel.chat.focus_region == ChatPaneFocusRegion::Rail &&
          active) {
        DrawRect(renderer, row_rect, theme_.accent);
      }
    }
    draw_action_button(chat_layout.rail_new_rect, "New Chat", true);

    for (std::size_t i = 0; const ChatHeaderAction& action : BuildChatHeaderActions(layout.sidebar)) {
      draw_action_button(action.rect, action.label, action.enabled,
                         action.kind == ChatHeaderAction::Kind::DeleteConversation);
      if (project_state.panel.chat.focus_region == ChatPaneFocusRegion::Header &&
          project_state.panel.chat.header_focus_index == i) {
        DrawRect(renderer, action.rect, theme_.accent);
      }
      ++i;
    }

    const std::string auth_banner = ChatAuthBannerText(conversation);
    if (!auth_banner.empty()) {
      DrawFilledRect(renderer, chat_layout.auth_rect, theme_.surface_raised);
      DrawRect(renderer, chat_layout.auth_rect, theme_.border);
      DrawVCenteredTextOn(text_renderer_, renderer, chat_layout.auth_rect, 6.0f,
                          theme_.text_secondary, theme_.surface_raised,
                          TruncateLabel(auth_banner, chat_layout.auth_rect.w - 12.0f));
    }

    DrawRect(renderer, chat_layout.transcript_rect,
             project_state.panel.chat.focus_region == ChatPaneFocusRegion::Transcript
                 ? theme_.accent
                 : theme_.border);
    const ChatTranscriptLayout transcript = BuildChatTranscriptLayout(layout.sidebar);
    const auto list_layout =
        ComputeChatSidebarListLayout(layout.sidebar, transcript.rows.size());
    const int scroll_row = list_layout.scroll_row;
    project_state.panel.chat.scroll_row = scroll_row;

    for (int row = 0; row < list_layout.visible_rows; ++row) {
      const int line_index = scroll_row + row;
      if (line_index >= static_cast<int>(transcript.rows.size())) {
        break;
      }

      const ChatTranscriptRow& line = transcript.rows[static_cast<std::size_t>(line_index)];
      SDL_FRect row_rect = ScrollableListRowRect(list_layout, row);
      if (line.kind == ChatTranscriptRow::Kind::Spacer) {
        continue;
      }

      const bool user_line = line.role == MessageRole::User;
      const bool assistant_line = line.role == MessageRole::Assistant;
      const SDL_Color row_background =
          line.kind == ChatTranscriptRow::Kind::Placeholder
              ? theme_.surface_background
              : line.kind == ChatTranscriptRow::Kind::Code
                  ? theme_.surface_raised
                  : user_line ? theme_.surface_raised : theme_.surface_background;
      DrawFilledRect(renderer, row_rect, row_background);
      if (line.tone == ChatTranscriptRow::Tone::Quote) {
        DrawFilledRect(renderer, MakeRect(row_rect.x, row_rect.y, 2.0f, row_rect.h),
                       theme_.text_muted);
      }

      const SDL_Color base_color =
          line.kind == ChatTranscriptRow::Kind::Placeholder
              ? theme_.text_muted
          : line.kind == ChatTranscriptRow::Kind::Meta ||
                  line.kind == ChatTranscriptRow::Kind::Tool
              ? theme_.text_muted
          : line.kind == ChatTranscriptRow::Kind::Error
              ? theme_.diff_deleted
          : line.tone == ChatTranscriptRow::Tone::Heading
              ? theme_.accent
          : line.tone == ChatTranscriptRow::Tone::Quote
              ? theme_.text_muted
              : user_line ? theme_.text_primary : assistant_line ? theme_.text_secondary
                                                              : theme_.text_muted;

      float text_x = row_rect.x + 6.0f;
      if (!line.prefix.empty()) {
        DrawVCenteredTextOn(
            text_renderer_, renderer,
            MakeRect(text_x, row_rect.y, std::max(0.0f, row_rect.w - 12.0f), row_rect.h), 0.0f,
            line.tone == ChatTranscriptRow::Tone::List ? theme_.text_muted : base_color,
            row_background, line.prefix);
        text_x += text_renderer_.MeasureWidth(line.prefix);
      }

      for (const ChatTranscriptSegment& segment : line.segments) {
        SDL_Color segment_color = base_color;
        if (segment.style == ChatTextStyle::Link) {
          segment_color = theme_.accent;
        } else if (segment.style == ChatTextStyle::InlineCode) {
          segment_color = theme_.text_primary;
        } else if (segment.style == ChatTextStyle::Strong &&
                   line.kind != ChatTranscriptRow::Kind::Error) {
          segment_color = theme_.text_primary;
        } else if (segment.style == ChatTextStyle::Emphasis &&
                   line.kind != ChatTranscriptRow::Kind::Error) {
          segment_color = assistant_line ? theme_.text_primary : theme_.text_secondary;
        }
        const float segment_width = text_renderer_.MeasureWidth(segment.text);
        if (segment.style == ChatTextStyle::InlineCode && segment_width > 0.0f) {
          const SDL_FRect code_rect = MakeRect(text_x - 1.0f, row_rect.y + 2.0f,
                                               segment_width + 2.0f,
                                               std::max(1.0f, row_rect.h - 4.0f));
          DrawFilledRect(renderer, code_rect, theme_.surface_background);
          DrawRect(renderer, code_rect, theme_.border);
        }
        DrawVCenteredTextOn(
            text_renderer_, renderer,
            MakeRect(text_x, row_rect.y, std::max(0.0f, row_rect.w - (text_x - row_rect.x) - 6.0f),
                     row_rect.h),
            0.0f, segment_color, row_background, segment.text);
        if (segment.style == ChatTextStyle::Link && segment_width > 0.0f) {
          SDL_SetRenderDrawColor(renderer, segment_color.r, segment_color.g, segment_color.b,
                                 segment_color.a);
          SDL_RenderLine(renderer, text_x, row_rect.y + row_rect.h - 3.0f,
                         text_x + segment_width, row_rect.y + row_rect.h - 3.0f);
        }
        text_x += segment_width;
      }
    }

    const TextInputSurface current_surface = text_input_vm.current_surface;
    const bool chat_input_active = current_surface == TextInputSurface::ChatComposer;
    const auto visual = chat_input_active ? BuildActiveTextInputVisual(layout, std::nullopt)
                                          : std::nullopt;
    auto& composer = project_state.panel.chat.composer;
    const SDL_FRect composer_rect = ChatSidebarComposerRect(layout.sidebar);
    composer.SetViewportSize(
        std::max<std::size_t>(1, static_cast<std::size_t>((composer_rect.h - 8.0f) /
                                                          std::max(1.0f, text_renderer_.LineHeight()))),
        std::max<std::size_t>(1, static_cast<std::size_t>((composer_rect.w - 12.0f) /
                                                          std::max(1.0f, text_renderer_.CharWidth()))));
    DrawTextFieldFrame(renderer, theme_, composer_rect, chat_input_active);
    DrawRect(renderer, composer_rect,
             project_state.panel.chat.focus_region == ChatPaneFocusRegion::Composer
                 ? theme_.accent
                 : theme_.border);
    if (composer.lines().size() == 1 && composer.lines().front().empty()) {
      DrawTextOn(text_renderer_, renderer, composer_rect.x + 6.0f, composer_rect.y + 4.0f,
                 theme_.text_muted, theme_.surface_background, "Ask about this workspace");
      DrawTextOn(text_renderer_, renderer, composer_rect.x + 6.0f, composer_rect.y + 18.0f,
                 theme_.text_muted, theme_.surface_background, "Enter for newline, Ctrl+Enter to send");
    } else {
      const std::size_t first_line = composer.scroll_line();
      const std::size_t last_line =
          std::min(composer.line_count(), first_line + composer.visible_lines());
      float text_y = composer_rect.y + 4.0f;
      for (std::size_t line_index = first_line; line_index < last_line; ++line_index) {
        const editor::LayoutLine layout_line = composer.VisibleLineLayout(line_index);
        DrawTextOn(text_renderer_, renderer, composer_rect.x + 6.0f, text_y,
                   chat_input_active ? theme_.text_primary : theme_.text_secondary,
                   theme_.surface_background, layout_line.text);
        text_y += text_renderer_.LineHeight();
      }
    }

    draw_vertical_scrollbar(list_layout.list_rect, static_cast<float>(transcript.rows.size()),
                            list_layout.visible_units, static_cast<float>(scroll_row),
                            context_.interaction_state.drag_target == DragTarget::SidebarScrollbar,
                            true);
  } else if (sidebar_mode == SidebarMode::Git) {
    const bool outgoing_base_menu_open =
        context_.menu_state.menu_bar_open &&
        context_.menu_state.active_menu_id == MenuId::GitOutgoingBase &&
        context_.menu_state.active_menu_anchor_rect.has_value();
    draw_action_button(GitSidebarStageAllButtonRect(layout.sidebar), "Stage All",
                       CanStageAllGitSidebarEntries());
    draw_action_button(GitSidebarDiscardAllButtonRect(layout.sidebar), "Discard All",
                       CanDiscardAllGitSidebarEntries(), true);
    draw_action_button(GitSidebarRefreshButtonRect(layout.sidebar), "Refresh", true);

    const auto summary_lines = GitSidebarSummaryLines();
    float summary_y = GitSidebarActionRowRect(layout.sidebar).y +
                      GitSidebarActionRowRect(layout.sidebar).h + 6.0f;
    for (const std::string& summary : summary_lines) {
      DrawTextOn(text_renderer_, renderer, layout.sidebar.x + kSidebarInset, summary_y,
                 theme_.text_muted, theme_.surface_background,
                 TruncateLabel(summary, layout.sidebar.w - kSidebarInset * 2.0f));
      summary_y += 14.0f;
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
            DrawChevron(renderer, button_rect->x + button_rect->w * 0.5f,
                        button_rect->y + button_rect->h * 0.5f, true,
                        outgoing_base_menu_open || hovered ? theme_.text_primary
                                                           : theme_.text_muted);
            label_width =
                std::max(0.0f, button_rect->x - row_rect.x - 8.0f);
          }
        }
        DrawVCenteredTextOn(text_renderer_, renderer, row_rect, 4.0f, theme_.text_muted,
                            theme_.surface_background,
                            TruncateLabel(line.label, label_width));
        continue;
      }
      if (line.kind == GitSidebarLine::Kind::Empty || line.entry_index < 0) {
        DrawVCenteredTextOn(text_renderer_, renderer, row_rect, 4.0f, theme_.text_muted,
                            theme_.surface_background,
                            TruncateLabel(line.label, row_rect.w - 8.0f));
        continue;
      }

      const auto& entry = project_state.sidebar.git.entries[static_cast<std::size_t>(line.entry_index)];
      const bool selected =
          static_cast<std::size_t>(line.entry_index) == project_state.sidebar.git.selected_index;
      DrawSelectableRowBackground(renderer, theme_, row_rect, theme_.surface_background, selected,
                                  selected);

      const char git_marker = GitMarker(entry.status);
      const std::string marker_text = git_marker == ' ' ? "" : std::string(1, git_marker);
      const float marker_width =
          marker_text.empty() ? 0.0f : text_renderer_.MeasureWidth(marker_text);
      const GitSidebarEntryActionLayout actions =
          ComputeGitSidebarEntryActionLayout(row_rect, entry);
      float right_edge = actions.content_right_edge;

      const auto draw_button = [&](const SDL_FRect& button_rect,
                                   std::string_view label,
                                   ButtonTone tone) {
        DrawButtonCentered(
            text_renderer_, renderer, theme_, button_rect, label, tone,
            ButtonVisualState{
                .enabled = true,
                .hovered = false,
                .active = selected,
            });
      };

      if (actions.primary_rect.has_value()) {
        draw_button(*actions.primary_rect, entry.staged ? "Unstage" : "Stage",
                    ButtonTone::Accent);
      }
      if (actions.discard_rect.has_value()) {
        draw_button(*actions.discard_rect, "Discard", ButtonTone::Destructive);
      }

      if (!marker_text.empty()) {
        DrawVCenteredTextOn(
            text_renderer_, renderer,
            MakeRect(right_edge - marker_width, row_rect.y, marker_width, row_rect.h), 0.0f,
            GitMarkerColor(theme_, entry.status),
            selected ? theme_.row_highlight : theme_.surface_background, marker_text);
        right_edge -= marker_width + 8.0f;
      }

      const GitSidebarEntryTextModel text_model =
          BuildGitSidebarEntryTextModel(entry.relative_path, entry.staged);
      const SDL_Color row_background =
          selected ? theme_.row_highlight : theme_.surface_background;
      const SDL_Color primary_color = theme_.text_primary;
      const SDL_Color secondary_color = theme_.text_muted;
      DrawPrimarySecondaryRowText(text_renderer_, renderer, row_rect, row_rect.x + 6.0f, right_edge,
                                  primary_color, secondary_color, row_background,
                                  text_model.primary_label, text_model.secondary_label, 1.0f);
    }

    draw_vertical_scrollbar(list_layout.list_rect, static_cast<float>(lines.size()),
                            list_layout.visible_units, static_cast<float>(scroll_row),
                            context_.interaction_state.drag_target == DragTarget::SidebarScrollbar);
  } else if (sidebar_mode == SidebarMode::Problems) {
    const auto list_layout =
        ComputeProblemsSidebarListLayout(layout.sidebar, project_state.sidebar.problems.entries.size());
    const int scroll_row = list_layout.scroll_row;

    for (int row = 0; row < list_layout.visible_rows; ++row) {
      const int item_index = scroll_row + row;
      if (item_index >= static_cast<int>(project_state.sidebar.problems.entries.size())) {
        break;
      }

      const auto& item = project_state.sidebar.problems.entries[static_cast<std::size_t>(item_index)];
      SDL_FRect row_rect = ScrollableListRowRect(list_layout, row);
      const bool selected =
          static_cast<std::size_t>(item_index) == project_state.sidebar.problems.selected_index;
      const SDL_Color row_background =
          selected ? theme_.row_highlight : theme_.surface_background;
      DrawSelectableRowBackground(renderer, theme_, row_rect, theme_.surface_background, selected);

      const SDL_Color severity =
          editor::DiagnosticSeverityColor(theme_, item.diagnostic.severity);
      DrawFilledRect(renderer, MakeRect(row_rect.x, row_rect.y, 2.0f, row_rect.h), severity);

      const SDL_Color primary_color = selected ? theme_.text_primary : theme_.text_secondary;
      const SDL_Color secondary_color = selected ? theme_.text_secondary : theme_.text_muted;
      DrawPrimarySecondaryRowText(text_renderer_, renderer, row_rect, row_rect.x + 8.0f,
                                  row_rect.x + row_rect.w - 6.0f, primary_color, secondary_color,
                                  row_background, item.primary_label, item.detail_label, 0.58f);
    }

    if (project_state.sidebar.problems.entries.empty()) {
      DrawTextOn(text_renderer_, renderer, layout.sidebar.x + kSidebarInset,
                 list_layout.row_y + 4.0f, theme_.text_muted, theme_.surface_background,
                 TruncateLabel(FormatEmptyState("diagnostics"),
                               layout.sidebar.w - kSidebarInset * 2.0f));
    }

    draw_vertical_scrollbar(list_layout.list_rect,
                            static_cast<float>(project_state.sidebar.problems.entries.size()),
                            list_layout.visible_units, static_cast<float>(scroll_row),
                            context_.interaction_state.drag_target == DragTarget::SidebarScrollbar);
  } else if (sidebar_mode == SidebarMode::Tests) {
    const auto list_layout =
        ComputeTestsSidebarListLayout(layout.sidebar, project_state.sidebar.tests.entries.size());
    const int scroll_row = list_layout.scroll_row;

    for (int row = 0; row < list_layout.visible_rows; ++row) {
      const int item_index = scroll_row + row;
      if (item_index >= static_cast<int>(project_state.sidebar.tests.entries.size())) {
        break;
      }

      const auto& item =
          project_state.sidebar.tests.entries[static_cast<std::size_t>(item_index)];
      SDL_FRect row_rect = ScrollableListRowRect(list_layout, row);
      const bool selected =
          static_cast<std::size_t>(item_index) == project_state.sidebar.tests.selected_index;
      const SDL_Color row_background =
          selected ? theme_.row_highlight : theme_.surface_background;
      DrawSelectableRowBackground(renderer, theme_, row_rect, theme_.surface_background, selected,
                                  selected);

      SDL_Color status_color = theme_.text_muted;
      if (item.status == "passed") {
        status_color = theme_.diff_added;
      } else if (item.status == "failed" || item.status == "errored") {
        status_color = theme_.diff_deleted;
      } else if (item.status == "running") {
        status_color = theme_.accent;
      } else if (item.status == "skipped") {
        status_color = theme_.text_disabled;
      }

      const std::string status_glyph =
          item.status == "passed" ? "Pass"
          : item.status == "failed" ? "Fail"
          : item.status == "errored" ? "Error"
          : item.status == "running" ? "Run"
          : item.status == "skipped" ? "Skip"
                                     : "Queued";
      const float text_x = row_rect.x + 8.0f;
      const float text_y =
          row_rect.y + std::floor(std::max(0.0f, row_rect.h - text_renderer_.LineHeight()) * 0.5f);
      DrawTextOn(text_renderer_, renderer, text_x, text_y, status_color, row_background,
                 status_glyph);

      const float label_x = text_x + text_renderer_.MeasureWidth(status_glyph) + 10.0f;
      const SDL_Color primary_color = selected ? theme_.text_primary : theme_.text_secondary;
      std::string detail;
      if (!item.file.empty()) {
        detail = item.file.filename().string();
        if (item.line > 0) {
          detail.push_back(':');
          AppendUnsigned(detail, static_cast<std::size_t>(item.line));
        }
      }
      DrawPrimarySecondaryRowText(text_renderer_, renderer, row_rect, label_x,
                                  row_rect.x + row_rect.w - 6.0f, primary_color,
                                  selected ? theme_.text_secondary : theme_.text_muted,
                                  row_background, item.label, detail, 0.62f);
    }

    const std::string placeholder =
        !project_state.sidebar.tests.error.empty()
            ? "Error: " + project_state.sidebar.tests.error
            : project_state.sidebar.tests.entries.empty()
                ? FormatEmptyState("tests discovered")
                                                                           : std::string{};
    if (!placeholder.empty()) {
      DrawTextOn(text_renderer_, renderer, layout.sidebar.x + kSidebarInset,
                 list_layout.row_y + 4.0f,
                 project_state.sidebar.tests.error.empty() ? theme_.text_muted : theme_.diff_deleted,
                 theme_.surface_background,
                 TruncateLabel(placeholder, layout.sidebar.w - kSidebarInset * 2.0f));
    }

    draw_vertical_scrollbar(list_layout.list_rect,
                            static_cast<float>(project_state.sidebar.tests.entries.size()),
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
    draw_action_button(collapse_rect, "Collapse", project_state.directory_tree.CanCollapseAll());
    draw_action_button(refresh_rect, "Refresh", true);

    const std::string tree_root_label = ProjectLabel();
    const float root_label_left = sidebar_mode_rect.x + sidebar_mode_rect.w + 10.0f;
    const float root_label_right = collapse_rect.x - 10.0f;
    const float root_label_max_width = std::max(0.0f, root_label_right - root_label_left);
    const std::string root_label = TruncateLabel(tree_root_label, root_label_max_width);
    if (!root_label.empty()) {
      DrawCenteredTextOn(text_renderer_, renderer,
                         MakeRect(root_label_left, layout.sidebar.y + 4.0f, root_label_max_width,
                                  18.0f),
                         theme_.chrome_text_secondary, theme_.chrome_background, root_label);
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
      const std::string git_marker_text = has_git_marker ? std::string(1, git_marker) : "";
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

  const std::string hovered_git_sidebar_tooltip = HoveredGitSidebarTooltipLabel(layout.sidebar);
  if (!hovered_git_sidebar_tooltip.empty()) {
    const auto tooltip =
        BuildTooltipLayout(text_renderer_, hovered_git_sidebar_tooltip,
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
  }
}

}  // namespace microide::workspace
