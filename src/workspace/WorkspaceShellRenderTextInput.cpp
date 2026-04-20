#include "workspace/WorkspaceShell.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "editor/DiagnosticsRender.h"
#include "workspace/WorkspaceTextSearch.h"

namespace microide::workspace {

namespace {

constexpr float kSidebarInset = 10.0f;

}  // namespace

std::optional<WorkspaceShell::TextInputVisual> WorkspaceShell::BuildActiveTextInputVisual(
    const WorkspaceLayout& layout,
    const std::optional<SDL_FRect>& active_editor_pane_rect) const {
  const TextInputSurface surface = CurrentTextInputSurface();
  const float line_height = text_renderer_.LineHeight();
  const float char_width = std::max(1.0f, text_renderer_.CharWidth());

  switch (surface) {
    case TextInputSurface::Editor: {
      if (ActiveTabIsCompare()) {
        return BuildCompareTextInputVisual(layout.editor_surface);
      }
      if (ActiveTabIsMerge()) {
        return BuildMergeTextInputVisual(layout.editor_surface);
      }
      if (!active_editor_pane_rect.has_value()) {
        return std::nullopt;
      }
      const editor::EditorViewMetrics metrics = editor::EditorViewRenderer::ComputeMetrics(
          text_renderer_, text_viewport_, *active_editor_pane_rect);
      const float cursor_x =
          metrics.text_x +
          static_cast<float>(text_viewport_.cursor_visual_column() -
                             text_viewport_.horizontal_scroll()) *
              char_width;
      const float cursor_y =
          metrics.first_line_y +
          static_cast<float>(text_viewport_.cursor_line() - text_viewport_.scroll_line()) *
              metrics.line_height;
      return TextInputVisual{
          .surface = surface,
          .area = MakeRect(cursor_x, cursor_y - 1.0f, char_width, metrics.line_height),
          .text_x = cursor_x,
          .text_y = cursor_y,
          .cursor_x = cursor_x,
          .foreground = theme_.text_primary,
          .background = theme_.row_highlight,
      };
    }
    case TextInputSurface::Command: {
      const SDL_FRect prompt_rect = BottomPanelCommandPromptRect(layout);
      const float text_x = prompt_rect.x + 6.0f;
      const float text_y = prompt_rect.y + 4.0f;
      const float cursor_x = text_x + text_renderer_.MeasureWidth("> " + command_.input);
      return TextInputVisual{
          .surface = surface,
          .area = MakeRect(text_x, text_y, std::max(1.0f, prompt_rect.w - 12.0f), line_height),
          .text_x = text_x,
          .text_y = text_y,
          .cursor_x = cursor_x,
          .foreground = theme_.text_primary,
          .background = theme_.chrome_active,
      };
    }
    case TextInputSurface::PromptInput: {
      const SDL_FRect dialog = ComputePromptSurfaceRect(layout.full);
      const SDL_FRect input_rect = ComputePromptSurfaceInputRect(dialog);
      const float text_x = input_rect.x + 6.0f;
      const float text_y = input_rect.y + 4.0f;
      const float cursor_x = text_x + text_renderer_.MeasureWidth(prompts_.surface.input);
      return TextInputVisual{
          .surface = surface,
          .area = MakeRect(text_x, text_y, std::max(1.0f, input_rect.w - 12.0f), line_height),
          .text_x = text_x,
          .text_y = text_y,
          .cursor_x = cursor_x,
          .foreground = theme_.text_primary,
          .background = theme_.surface_background,
      };
    }
    case TextInputSurface::FileFinder:
    case TextInputSurface::BufferSearch:
    case TextInputSurface::BufferReplaceSearch:
    case TextInputSurface::BufferReplaceReplace:
    case TextInputSurface::ProjectSearchOverlay:
    case TextInputSurface::CommitPicker: {
      if (!overlay_state_.visible) {
        return std::nullopt;
      }
      const SDL_FRect overlay = ComputeOverlayRect(layout.editor_area);
      const float inset = 18.0f;
      const float text_x = overlay.x + inset;
      float text_y = overlay.y + 44.0f;
      std::string prefix = "> ";
      switch (surface) {
        case TextInputSurface::BufferSearch:
          prefix += overlay_workflow_.buffer_search.query;
          break;
        case TextInputSurface::BufferReplaceSearch:
          prefix = "find: " + overlay_workflow_.buffer_search.query;
          break;
        case TextInputSurface::BufferReplaceReplace:
          text_y = overlay.y + 62.0f;
          prefix = "replace: " + overlay_workflow_.buffer_search.replace_text;
          break;
        case TextInputSurface::ProjectSearchOverlay:
          prefix += overlay_workflow_.project_search.query;
          break;
        case TextInputSurface::CommitPicker:
          text_y = overlay.y + 62.0f;
          prefix += overlay_workflow_.compare_picker.query;
          break;
        case TextInputSurface::FileFinder:
        default:
          prefix += file_finder_.query();
          break;
      }
      const float cursor_x = text_x + text_renderer_.MeasureWidth(prefix);
      return TextInputVisual{
          .surface = surface,
          .area = MakeRect(text_x, text_y, std::max(1.0f, overlay.w - inset * 2.0f), line_height),
          .text_x = text_x,
          .text_y = text_y,
          .cursor_x = cursor_x,
          .foreground = theme_.text_secondary,
          .background = theme_.overlay_background,
      };
    }
    case TextInputSurface::SidebarSearchQuery:
    case TextInputSurface::SidebarSearchReplace: {
      if (!sidebar_state_.visible || ActiveSidebarMode() != SidebarMode::Search ||
          !overlay_workflow_.project_search.editing) {
        return std::nullopt;
      }
      const float text_x = layout.sidebar.x + kSidebarInset;
      const float text_y = layout.sidebar.y +
                           (surface == TextInputSurface::SidebarSearchQuery
                                ? kProjectSearchQueryTop
                                : kProjectSearchReplaceTop);
      const std::string prefix =
          surface == TextInputSurface::SidebarSearchQuery
              ? "search> " + overlay_workflow_.project_search.edit_buffer
              : "replace> " + overlay_workflow_.project_search.edit_buffer;
      const float cursor_x = text_x + text_renderer_.MeasureWidth(prefix);
      return TextInputVisual{
          .surface = surface,
          .area = MakeRect(text_x, text_y,
                           std::max(1.0f, layout.sidebar.w - kSidebarInset * 2.0f), line_height),
          .text_x = text_x,
          .text_y = text_y,
          .cursor_x = cursor_x,
          .foreground = theme_.text_primary,
          .background = theme_.surface_background,
      };
    }
    case TextInputSurface::Terminal:
    case TextInputSurface::None:
    default:
      return std::nullopt;
  }
}

void WorkspaceShell::UpdateTextInputArea(
    SDL_Renderer* renderer,
    SDL_Window* render_window,
    const std::optional<TextInputVisual>& visual) const {
  if (render_window == nullptr) {
    return;
  }

  if (!visual.has_value()) {
    SDL_SetTextInputArea(render_window, nullptr, 0);
    return;
  }

  float window_x0 = visual->area.x;
  float window_y0 = visual->area.y;
  float window_x1 = visual->area.x + visual->area.w;
  float window_y1 = visual->area.y + visual->area.h;
  float cursor_window_x = visual->cursor_x;
  float cursor_window_y = visual->area.y;
  SDL_RenderCoordinatesToWindow(renderer, visual->area.x, visual->area.y, &window_x0, &window_y0);
  SDL_RenderCoordinatesToWindow(renderer, visual->area.x + visual->area.w,
                                visual->area.y + visual->area.h, &window_x1, &window_y1);
  SDL_RenderCoordinatesToWindow(renderer, visual->cursor_x, visual->area.y, &cursor_window_x,
                                &cursor_window_y);

  const SDL_Rect area = SDL_Rect{
      static_cast<int>(std::floor(std::min(window_x0, window_x1))),
      static_cast<int>(std::floor(std::min(window_y0, window_y1))),
      std::max(1, static_cast<int>(std::ceil(std::fabs(window_x1 - window_x0)))),
      std::max(1, static_cast<int>(std::ceil(std::fabs(window_y1 - window_y0)))),
  };
  const int cursor =
      std::max(0, static_cast<int>(std::round(cursor_window_x - static_cast<float>(area.x))));
  SDL_SetTextInputArea(render_window, &area, cursor);
}

void WorkspaceShell::RenderEditorHoverPopup(SDL_Renderer* renderer) const {
  const auto popup = ActiveEditorHoverPopupLayout();
  if (!popup.has_value()) {
    return;
  }

  const auto draw_text_on = [&](float x,
                                float y,
                                SDL_Color foreground,
                                SDL_Color background,
                                std::string_view text) {
    text_renderer_.DrawStringOn(renderer, x, y, foreground, background, text);
  };

  DrawFilledRect(renderer, popup->rect, theme_.overlay_background);
  DrawRect(renderer, popup->rect, theme_.border);
  const float text_x = popup->rect.x + 12.0f;
  const float text_width = std::max(0.0f, popup->rect.w - 24.0f);
  float text_y = popup->rect.y + 12.0f;

  if (popup->kind == EditorHoverTarget::Kind::Blame) {
    const editor::EditorBlameLine* blame_line = VisibleEditorBlameLine(popup->blame_line_index);
    if (blame_line != nullptr) {
      const auto summary_lines = WrapEditorHoverPopupText(blame_line->summary, text_width, 4);
      draw_text_on(text_x, text_y, theme_.text_primary, theme_.overlay_background,
                   text_renderer_.TruncateToWidth(blame_line->author, text_width));
      text_y += text_renderer_.LineHeight() + 2.0f;
      draw_text_on(text_x, text_y, theme_.text_secondary, theme_.overlay_background,
                   text_renderer_.TruncateToWidth(blame_line->date, text_width));
      if (!summary_lines.empty()) {
        text_y += text_renderer_.LineHeight() + 8.0f;
        for (std::size_t i = 0; i < summary_lines.size(); ++i) {
          draw_text_on(text_x, text_y, theme_.text_primary, theme_.overlay_background,
                       summary_lines[i]);
          text_y += text_renderer_.LineHeight();
          if (i + 1 < summary_lines.size()) {
            text_y += 2.0f;
          }
        }
      }
    }
  } else if (popup->kind == EditorHoverTarget::Kind::Plugin && popup->plugin_hover.has_value()) {
    const auto title_lines =
        popup->plugin_hover->title.empty()
            ? std::vector<std::string>{}
            : WrapEditorHoverPopupText(popup->plugin_hover->title, text_width, 2);
    const auto content_lines =
        popup->plugin_hover->content.empty()
            ? std::vector<std::string>{}
            : WrapEditorHoverPopupText(popup->plugin_hover->content, text_width, 6);
    for (std::size_t i = 0; i < title_lines.size(); ++i) {
      draw_text_on(text_x, text_y, theme_.text_secondary, theme_.overlay_background,
                   title_lines[i]);
      text_y += text_renderer_.LineHeight();
      if (i + 1 < title_lines.size()) {
        text_y += 2.0f;
      }
    }
    if (!title_lines.empty() && !content_lines.empty()) {
      text_y += 8.0f;
    }
    for (std::size_t i = 0; i < content_lines.size(); ++i) {
      draw_text_on(text_x, text_y, theme_.text_primary, theme_.overlay_background,
                   content_lines[i]);
      text_y += text_renderer_.LineHeight();
      if (i + 1 < content_lines.size()) {
        text_y += 2.0f;
      }
    }
  } else if (popup->diagnostic.has_value()) {
    const auto severity =
        editor::DiagnosticSeverityColor(theme_, popup->diagnostic->severity);
    std::string_view severity_label = "Diagnostic";
    switch (popup->diagnostic->severity) {
      case editor::DiagnosticSeverity::Error:
        severity_label = "Error";
        break;
      case editor::DiagnosticSeverity::Warning:
        severity_label = "Warning";
        break;
      case editor::DiagnosticSeverity::Info:
        severity_label = "Info";
        break;
      case editor::DiagnosticSeverity::Hint:
        severity_label = "Hint";
        break;
    }
    const auto message_lines =
        WrapEditorHoverPopupText(popup->diagnostic->message, text_width, 6);
    draw_text_on(text_x, text_y, severity, theme_.overlay_background,
                 text_renderer_.TruncateToWidth(severity_label, text_width));
    if (!message_lines.empty()) {
      text_y += text_renderer_.LineHeight() + 8.0f;
      for (std::size_t i = 0; i < message_lines.size(); ++i) {
        draw_text_on(text_x, text_y, theme_.text_primary, theme_.overlay_background,
                     message_lines[i]);
        text_y += text_renderer_.LineHeight();
        if (i + 1 < message_lines.size()) {
          text_y += 2.0f;
        }
      }
    }
  }

  if (popup->primary_action_rect.has_value()) {
    const bool action_hovered =
        last_mouse_position_valid_ && EditorHoverPopupPrimaryActionHovered(last_mouse_x_, last_mouse_y_);
    DrawFilledRect(renderer, *popup->primary_action_rect,
                   action_hovered ? theme_.row_highlight : theme_.surface_raised);
    DrawRect(renderer, *popup->primary_action_rect,
             action_hovered ? theme_.accent : theme_.border);
    draw_text_on(popup->primary_action_rect->x + 9.0f, popup->primary_action_rect->y + 4.0f,
                 action_hovered ? theme_.text_primary : theme_.text_secondary,
                 action_hovered ? theme_.row_highlight : theme_.surface_raised, "Copy SHA");
  }
}

void WorkspaceShell::RenderTextComposition(
    SDL_Renderer* renderer,
    const std::optional<TextInputVisual>& visual) const {
  if (!visual.has_value() || text_composition_.text.empty() ||
      text_composition_.surface != visual->surface) {
    return;
  }

  const std::string_view composition = text_composition_.text;
  const std::size_t total_codepoints = Utf8CodepointCount(composition);
  const std::size_t selection_start_codepoints =
      text_composition_.start < 0
          ? total_codepoints
          : std::min<std::size_t>(static_cast<std::size_t>(text_composition_.start),
                                  total_codepoints);
  const std::size_t selection_end_codepoints =
      text_composition_.length <= 0
          ? selection_start_codepoints
          : std::min(total_codepoints,
                     selection_start_codepoints +
                         static_cast<std::size_t>(text_composition_.length));
  const std::size_t selection_start =
      Utf8ByteOffsetForCodepointCount(composition, selection_start_codepoints);
  const std::size_t selection_end =
      Utf8ByteOffsetForCodepointCount(composition, selection_end_codepoints);
  const std::string_view prefix = composition.substr(0, selection_start);
  const std::string_view selected =
      composition.substr(selection_start, selection_end - selection_start);
  const std::string_view suffix = composition.substr(selection_end);
  const float prefix_width = text_renderer_.MeasureWidth(prefix);
  const float selected_width = text_renderer_.MeasureWidth(selected);
  const float total_width = text_renderer_.MeasureWidth(composition);

  if (!selected.empty()) {
    DrawFilledRect(renderer,
                   MakeRect(visual->cursor_x + prefix_width, visual->text_y - 1.0f, selected_width,
                            text_renderer_.LineHeight()),
                   theme_.selection_fill);
  }

  float segment_x = visual->cursor_x;
  if (!prefix.empty()) {
    text_renderer_.DrawStringOn(renderer, segment_x, visual->text_y, theme_.accent,
                                visual->background, prefix);
    segment_x += prefix_width;
  }
  if (!selected.empty()) {
    text_renderer_.DrawStringOn(renderer, segment_x, visual->text_y, theme_.text_primary,
                                theme_.selection_fill, selected);
    segment_x += selected_width;
  }
  if (!suffix.empty()) {
    text_renderer_.DrawStringOn(renderer, segment_x, visual->text_y, theme_.accent,
                                visual->background, suffix);
  }

  DrawFilledRect(renderer,
                 MakeRect(visual->cursor_x, visual->text_y + text_renderer_.LineHeight() - 1.0f,
                          total_width, 1.0f),
                 theme_.accent);
  DrawFilledRect(renderer,
                 MakeRect(visual->cursor_x + prefix_width + selected_width, visual->text_y - 1.0f,
                          1.5f, text_renderer_.LineHeight()),
                 theme_.accent);
}

}  // namespace microide::workspace
