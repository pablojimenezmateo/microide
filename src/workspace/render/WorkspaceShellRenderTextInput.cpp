#include "workspace/render/HoverCardPaint.h"
#include "workspace/render/WorkspaceShellRenderPrimitives.h"

#include "workspace/ProjectSearchPanelLayout.h"

#include "workspace/render/RenderViewModelBuilder.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "editor/DiagnosticsRender.h"
#include "render/DevicePixelSnap.h"
#include "util/StringUtil.h"
#include "workspace/WorkspaceTextSearch.h"

namespace microide::workspace {

namespace {

bool UsesSharedSingleLineCaret(TextInputSurface surface) {
  switch (surface) {
    case TextInputSurface::PromptInput:
    case TextInputSurface::FileFinder:
    case TextInputSurface::BufferSearch:
    case TextInputSurface::BufferReplaceSearch:
    case TextInputSurface::BufferReplaceReplace:
    case TextInputSurface::CommitPicker:
    case TextInputSurface::QuickPick:
    case TextInputSurface::CommandPalette:
    case TextInputSurface::SidebarSearchQuery:
    case TextInputSurface::SidebarSearchReplace:
    case TextInputSurface::SidebarSearchInclude:
    case TextInputSurface::SidebarSearchExclude:
    case TextInputSurface::CommitSubject:
    case TextInputSurface::TerminalFind:
      return true;
    case TextInputSurface::CommitBody:
    case TextInputSurface::SettingsQuery:
    case TextInputSurface::SettingsValueEdit:
    case TextInputSurface::DebugVariableEdit:
    case TextInputSurface::None:
    case TextInputSurface::Editor:
    case TextInputSurface::Terminal:
      return false;
  }
  return false;
}

}

using namespace detail;

void WorkspaceShell::DrawSingleLineTextTail(SDL_Renderer* renderer,
                                            float x,
                                            float y,
                                            float available_width,
                                            SDL_Color foreground,
                                            SDL_Color background,
                                            std::string_view text) const {
  (void) background;
  if (available_width <= 0.0f || text.empty()) {
    return;
  }
  const std::size_t start = FittingTailStart(
      text, available_width, [this](std::string_view tail) { return text_renderer_.MeasureWidth(tail); });
  if (start < text.size()) {
    text_renderer_.DrawString(renderer, x, y, foreground, text.substr(start));
  }
}

WorkspaceShell::SingleLineViewMetrics WorkspaceShell::ComputeSingleLineViewMetrics(
    const editor::SingleLineEditor& state,
    std::string_view prefix,
    float available_width) const {
  return workspace::ComputeSingleLineViewMetrics(text_renderer_, state, prefix, available_width);
}

std::optional<WorkspaceShell::TextInputVisual> WorkspaceShell::BuildActiveTextInputVisual(
    const WorkspaceLayout& layout,
    const std::optional<SDL_FRect>& active_editor_pane_rect) const {
  EnsureClipFrameAndOverlayViewModels(layout);
  const OverlaySurfaceViewModel& overlay_vm = *clip_cached_overlay_vm_;
  if (!prepare_cached_sidebar_vm_.has_value()) {
    prepare_cached_sidebar_vm_.emplace(RenderViewModelBuilder(context_).BuildSidebarSurface());
  }
  if (!prepare_cached_text_input_vm_.has_value()) {
    prepare_cached_text_input_vm_.emplace(RenderViewModelBuilder(context_).BuildTextInputSurface());
  }
  const SidebarSurfaceViewModel& sidebar_vm = *prepare_cached_sidebar_vm_;
  const TextInputSurfaceViewModel& text_input_vm = *prepare_cached_text_input_vm_;
  const TextInputSurface surface = text_input_vm.current_surface;
  const float line_height = text_renderer_.LineHeight();
  const float char_width = std::max(1.0f, text_renderer_.CharWidth());
  const auto field_visual = [&](float text_x, float text_y, float available_width, SingleLineViewMetrics vm,
                                SDL_Color foreground) {
    return TextInputVisual{
        .surface = surface,
        .area = MakeRect(text_x, text_y, available_width, line_height),
        .text_x = text_x,
        .text_y = text_y,
        .cursor_x = text_x + vm.cursor_x,
        .foreground = foreground,
        .background = theme_.surface_background,
        .displayed_text = std::move(vm.displayed_text),
        .selection_bytes = vm.selection_bytes,
    };
  };

  switch (surface) {
    case TextInputSurface::Editor: {
      if (ActiveTabIsCompare()) {
        return BuildCompareTextInputVisual(ActiveTabSurface(layout));
      }
      if (ActiveTabIsMerge()) {
        return BuildMergeTextInputVisual(ActiveTabSurface(layout));
      }
      if (!active_editor_pane_rect.has_value()) {
        return std::nullopt;
      }
      const editor::TextViewport* viewport = ActiveEditorViewport();
      if (viewport == nullptr) {
        return std::nullopt;
      }
      const editor::EditorViewMetrics metrics = editor::EditorViewRenderer::ComputeMetrics(
          text_renderer_, *viewport, *active_editor_pane_rect, viewport->sticky_band_rows(),
          LineNumbersEnabled());
      const std::size_t visual_column = viewport->cursor_visual_column();
      const std::size_t h_scroll = viewport->horizontal_scroll();
      const std::size_t visual_row = viewport->cursor_visual_row();
      const std::size_t scroll_row = viewport->scroll_line();
      const float cursor_x =
          metrics.text_x +
          static_cast<float>(visual_column > h_scroll ? visual_column - h_scroll : 0) * char_width;
      const float cursor_y =
          metrics.first_line_y +
          static_cast<float>(visual_row > scroll_row ? visual_row - scroll_row : 0) *
              metrics.line_height;
      return TextInputVisual{
          .surface = surface,
          .area = MakeRect(cursor_x, cursor_y - 1.0f, char_width, metrics.line_height),
          .text_x = cursor_x,
          .text_y = cursor_y,
          .cursor_x = cursor_x,
          .foreground = theme_.text_primary,
          .background = theme_.row_highlight,
          .displayed_text = {},
          .selection_bytes = std::nullopt,
      };
    }
    case TextInputSurface::PromptInput: {
      const SDL_FRect dialog = ComputePromptSurfaceRect(layout.full);
      const SDL_FRect input_rect = ComputePromptSurfaceInputRect(dialog);
      const float text_x = input_rect.x + 6.0f;
      const float text_y =
          input_rect.y + std::floor((input_rect.h - text_renderer_.LineHeight()) * 0.5f);
      const float available_width = std::max(1.0f, input_rect.w - 12.0f);
      auto vm = ComputeSingleLineViewMetrics(*text_input_vm.prompt_input, "", available_width);
      return field_visual(text_x, text_y, available_width, std::move(vm), theme_.text_primary);
    }
    case TextInputSurface::BufferSearch:
    case TextInputSurface::BufferReplaceSearch:
    case TextInputSurface::BufferReplaceReplace: {
      if (!overlay_vm.visible) {
        return std::nullopt;
      }
      const bool replace_mode = surface == TextInputSurface::BufferReplaceSearch ||
                                surface == TextInputSurface::BufferReplaceReplace;
      const bool replace_field = surface == TextInputSurface::BufferReplaceReplace;
      const FindWidgetLayout fw = ComputeBufferFindWidgetLayout(ActiveTabSurface(layout), replace_mode);
      const SDL_FRect field = replace_field ? fw.replace_field : fw.search_field;
      if (field.w <= 0.0f || field.h <= 0.0f) {
        return std::nullopt;
      }
      const float text_x = field.x + 6.0f;
      const float text_y = field.y + std::floor((field.h - line_height) * 0.5f);
      const float available_width = std::max(1.0f, field.w - 12.0f);
      auto vm = ComputeSingleLineViewMetrics(
          replace_field ? *text_input_vm.buffer_search_replace
                        : *text_input_vm.buffer_search_query,
          "", available_width);
      return field_visual(text_x, text_y, available_width, std::move(vm), theme_.text_primary);
    }
    case TextInputSurface::TerminalFind: {
      if (!prepare_cached_bottom_panel_vm_.has_value() ||
          !prepare_cached_bottom_panel_vm_->find_visible ||
          prepare_cached_bottom_panel_vm_->find_query == nullptr) {
        return std::nullopt;
      }
      const SDL_FRect field = prepare_cached_bottom_panel_vm_->find.fw.search_field;
      const float text_x = field.x + 6.0f;
      const float text_y = field.y + std::floor((field.h - line_height) * 0.5f);
      const float available_width = std::max(1.0f, field.w - 12.0f);
      auto vm = ComputeSingleLineViewMetrics(*prepare_cached_bottom_panel_vm_->find_query, "",
                                             available_width);
      return field_visual(text_x, text_y, available_width, std::move(vm), theme_.text_primary);
    }
    case TextInputSurface::FileFinder:
    case TextInputSurface::CommitPicker:
    case TextInputSurface::QuickPick:
    case TextInputSurface::CommandPalette: {
      if (!overlay_vm.visible) {
        return std::nullopt;
      }
      const SDL_FRect overlay = overlay_vm.overlay_rect;
      const float inset = 18.0f;
      const float text_x = overlay.x + inset;
      const auto overlay_field_text_y = [&](float row_y) {
        const SDL_FRect field =
            MakeRect(overlay.x + 12.0f, row_y - 4.0f, std::max(0.0f, overlay.w - 24.0f), 18.0f);
        return field.y + std::floor((field.h - line_height) * 0.5f);
      };
      const float text_y = overlay_field_text_y(overlay_vm.query_row_y);
      const float available_width = std::max(1.0f, overlay.w - inset * 2.0f);
      SingleLineViewMetrics vm;
      switch (surface) {
        case TextInputSurface::CommitPicker:
          vm = ComputeSingleLineViewMetrics(*text_input_vm.commit_picker_query, "> ",
                                            available_width);
          break;
        case TextInputSurface::QuickPick:
          vm = ComputeSingleLineViewMetrics(*text_input_vm.quick_pick_query, "> ",
                                            available_width);
          break;
        case TextInputSurface::CommandPalette:
          vm = ComputeSingleLineViewMetrics(*text_input_vm.command_palette_query, "> ",
                                            available_width);
          break;
        case TextInputSurface::FileFinder:
        default:
          vm = ComputeSingleLineViewMetrics(*text_input_vm.file_finder_query, "> ",
                                            available_width);
          break;
      }
      return field_visual(text_x, text_y, available_width, std::move(vm), theme_.text_secondary);
    }
    case TextInputSurface::SidebarSearchQuery:
    case TextInputSurface::SidebarSearchReplace:
    case TextInputSurface::SidebarSearchInclude:
    case TextInputSurface::SidebarSearchExclude: {
      if (!sidebar_vm.visible || sidebar_vm.mode != SidebarMode::Search ||
          !sidebar_vm.project_search_editing) {
        return std::nullopt;
      }
      const bool scope_expanded = sidebar_vm.project_search_scope_expanded;
      SDL_FRect text_rect{};
      switch (surface) {
        case TextInputSurface::SidebarSearchReplace:
          text_rect = project_search_panel::ReplaceRect(layout.sidebar);
          break;
        case TextInputSurface::SidebarSearchInclude:
          text_rect = project_search_panel::IncludeRect(layout.sidebar, scope_expanded);
          break;
        case TextInputSurface::SidebarSearchExclude:
          text_rect = project_search_panel::ExcludeRect(layout.sidebar, scope_expanded);
          break;
        default:
          text_rect = project_search_panel::QueryRect(layout.sidebar);
          break;
      }
      if (text_rect.w <= 0.0f) {
        return std::nullopt;
      }
      const float text_x = text_rect.x + 6.0f;
      const float text_y = text_rect.y + 3.0f;
      const std::string_view prefix = "";
      const float available_width =
          std::max(1.0f, text_rect.w - 12.0f);
      auto vm = ComputeSingleLineViewMetrics(
          *text_input_vm.project_search_edit_buffer,
          prefix, available_width);
      return field_visual(text_x, text_y, available_width, std::move(vm), theme_.text_primary);
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

void WorkspaceShell::LayOutTextInputMarks(const std::optional<TextInputVisual>& visual, CardPaint& marks) const {
  marks.Clear();
  if (!visual.has_value()) {
    return;
  }
  marks.OpenOverlay(SDL_FRect{});
  if (visual->selection_bytes.has_value() && !visual->displayed_text.empty() &&
      UsesSharedSingleLineCaret(visual->surface)) {
    const auto [sel_start_byte, sel_end_byte] = *visual->selection_bytes;
    if (sel_start_byte < sel_end_byte && sel_end_byte <= visual->displayed_text.size()) {
      const std::string_view display_sv = visual->displayed_text;
      const float sel_start_x = visual->text_x + text_renderer_.MeasureWidth(display_sv.substr(0, sel_start_byte));
      const float sel_end_x = visual->text_x + text_renderer_.MeasureWidth(display_sv.substr(0, sel_end_byte));
      if (sel_end_x - sel_start_x > 0.0f) {
        marks.Fill(MakeRect(sel_start_x, visual->text_y - 1.0f, sel_end_x - sel_start_x, text_renderer_.LineHeight()),
                   theme_.selection_fill);
        marks.TextOver(sel_start_x, visual->text_y, theme_.text_primary,
                       display_sv.substr(sel_start_byte, sel_end_byte - sel_start_byte));
      }
    }
  }
  const TextCompositionState& composition_state = context_.text_input.composition;
  if (!composition_state.text.empty() && composition_state.surface == visual->surface) {
    const std::string_view composition = composition_state.text;
    const std::size_t total_codepoints = util::Utf8CodepointCount(composition);
    const std::size_t selection_start_codepoints =
        composition_state.start < 0
            ? total_codepoints
            : std::min<std::size_t>(static_cast<std::size_t>(composition_state.start), total_codepoints);
    const std::size_t selection_end_codepoints =
        composition_state.length <= 0
            ? selection_start_codepoints
            : std::min(total_codepoints,
                       selection_start_codepoints + static_cast<std::size_t>(composition_state.length));
    const std::size_t selection_start = util::Utf8ByteOffsetForCodepointCount(composition, selection_start_codepoints);
    const std::size_t selection_end = util::Utf8ByteOffsetForCodepointCount(composition, selection_end_codepoints);
    const std::string_view prefix = composition.substr(0, selection_start);
    const std::string_view selected = composition.substr(selection_start, selection_end - selection_start);
    const std::string_view suffix = composition.substr(selection_end);
    const float prefix_width = text_renderer_.MeasureWidth(prefix);
    const float selected_width = text_renderer_.MeasureWidth(selected);
    const float total_width = text_renderer_.MeasureWidth(composition);
    const float line_y = visual->cursor_y != 0.0f ? visual->cursor_y : visual->text_y;
    if (!selected.empty()) {
      marks.Fill(MakeRect(visual->cursor_x + prefix_width, line_y - 1.0f, selected_width, text_renderer_.LineHeight()),
                 theme_.selection_fill);
    }
    float segment_x = visual->cursor_x;
    if (!prefix.empty()) {
      marks.TextOver(segment_x, line_y, theme_.accent, prefix);
      segment_x += prefix_width;
    }
    if (!selected.empty()) {
      marks.TextOver(segment_x, line_y, theme_.text_primary, selected);
      segment_x += selected_width;
    }
    if (!suffix.empty()) {
      marks.TextOver(segment_x, line_y, theme_.accent, suffix);
    }
    marks.Fill(MakeRect(visual->cursor_x, visual->text_y + text_renderer_.LineHeight() - 1.0f, total_width, 1.0f),
               theme_.accent);
    marks.Fill(MakeRect(visual->cursor_x + prefix_width + selected_width, visual->text_y - 1.0f, 1.5f,
                        text_renderer_.LineHeight()),
               theme_.accent);
  }
  marks.FitOverlayToContents(text_renderer_);
}

void WorkspaceShell::LayOutTextInputCaret(const std::optional<TextInputVisual>& visual, CardPaint& caret) const {
  caret.Clear();
  if (!visual.has_value() || !context_.interaction_state.window_has_input_focus ||
      !context_.text_input.composition.text.empty() || !UsesSharedSingleLineCaret(visual->surface) ||
      !frame_state_.caret_visible) {
    return;
  }
  caret.OpenOverlay(SDL_FRect{});
  caret.Fill(MakeRect(render::SnapToDevicePixel(visual->cursor_x, presentation_scale_x_),
                      (visual->cursor_y != 0.0f ? visual->cursor_y : visual->text_y) - 1.0f, 1.5f,
                      text_renderer_.LineHeight()),
             theme_.cursor);
  caret.FitOverlayToContents(text_renderer_);
}

}
