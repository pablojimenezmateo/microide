#include "workspace/WorkspaceShellRenderPrimitives.h"

#include "workspace/RenderViewModelBuilder.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "editor/DiagnosticsRender.h"
#include "util/SingleLineText.h"
#include "util/StringUtil.h"
#include "workspace/WorkspaceTextSearch.h"

namespace microide::workspace {

namespace {

constexpr float kSidebarInset = 10.0f;

}  // namespace

using namespace detail;

float WorkspaceShell::MeasureSingleLineTextTail(std::string_view text,
                                                float available_width) const {
  if (available_width <= 0.0f || text.empty()) {
    return 0.0f;
  }
  if (text_renderer_.MeasureWidth(text) <= available_width) {
    return text_renderer_.MeasureWidth(text);
  }

  std::size_t byte_offset = 0;
  while (byte_offset < text.size()) {
    const std::string_view suffix = text.substr(byte_offset);
    const float suffix_width = text_renderer_.MeasureWidth(suffix);
    if (suffix_width <= available_width) {
      return suffix_width;
    }
    byte_offset += util::Utf8SequenceLength(text, byte_offset);
  }
  return 0.0f;
}

void WorkspaceShell::DrawSingleLineTextTail(SDL_Renderer* renderer,
                                            float x,
                                            float y,
                                            float available_width,
                                            SDL_Color foreground,
                                            SDL_Color background,
                                            std::string_view text) const {
  if (available_width <= 0.0f || text.empty()) {
    return;
  }
  if (text_renderer_.MeasureWidth(text) <= available_width) {
    text_renderer_.DrawStringOn(renderer, x, y, foreground, background, text);
    return;
  }

  std::size_t byte_offset = 0;
  while (byte_offset < text.size()) {
    const std::string_view suffix = text.substr(byte_offset);
    if (text_renderer_.MeasureWidth(suffix) <= available_width) {
      text_renderer_.DrawStringOn(renderer, x, y, foreground, background, suffix);
      return;
    }
    byte_offset += util::Utf8SequenceLength(text, byte_offset);
  }
}

WorkspaceShell::SingleLineViewMetrics WorkspaceShell::ComputeSingleLineViewMetrics(
    const util::SingleLineTextState& state,
    std::string_view prefix,
    float available_width) const {
  const std::string full_text = std::string(prefix) + state.text;
  const std::size_t cursor_byte =
      std::min(prefix.size() + state.cursor, full_text.size());

  // Measure each codepoint from 0..cursor_byte exactly once, storing (start, width).
  // Walking backward through this array to find view_start avoids re-measuring.
  struct CharEntry {
    std::size_t start;
    float width;
  };
  std::vector<CharEntry> before_cursor;
  before_cursor.reserve(64);
  for (std::size_t pos = 0; pos < cursor_byte;) {
    const std::size_t next = util::NextUtf8Boundary(full_text, pos);
    before_cursor.push_back(
        {pos, text_renderer_.MeasureWidth(std::string_view(full_text).substr(pos, next - pos))});
    pos = next;
  }

  // Walk backward through stored widths to find the leftmost byte that still lets
  // [view_start..cursor] fit in available_width — no extra MeasureWidth calls needed.
  float cursor_x = 0.0f;
  std::size_t view_start_idx = before_cursor.size();
  for (auto i = before_cursor.size(); i > 0; --i) {
    if (cursor_x + before_cursor[i - 1].width > available_width) {
      break;
    }
    cursor_x += before_cursor[i - 1].width;
    view_start_idx = i - 1;
  }
  const std::size_t view_start =
      before_cursor.empty() ? 0 : before_cursor[view_start_idx].start;

  // Walk forward from cursor_byte, measuring each codepoint once, until full.
  float right_accum = cursor_x;
  std::size_t view_end = cursor_byte;
  while (view_end < full_text.size()) {
    const std::size_t next = util::NextUtf8Boundary(full_text, view_end);
    const float char_w = text_renderer_.MeasureWidth(
        std::string_view(full_text).substr(view_end, next - view_end));
    if (right_accum + char_w > available_width) {
      break;
    }
    right_accum += char_w;
    view_end = next;
  }

  std::optional<std::pair<std::size_t, std::size_t>> selection_bytes;
  if (const auto sel = util::SingleLineSelection(state); sel.has_value()) {
    const std::size_t sel_start_full = prefix.size() + sel->start;
    const std::size_t sel_end_full = prefix.size() + sel->end;
    if (sel_start_full < view_end && sel_end_full > view_start) {
      const std::size_t clamped_start = std::max(sel_start_full, view_start) - view_start;
      const std::size_t clamped_end = std::min(sel_end_full, view_end) - view_start;
      if (clamped_start < clamped_end) {
        selection_bytes = {clamped_start, clamped_end};
      }
    }
  }

  return SingleLineViewMetrics{
      .displayed_text =
          std::string(std::string_view(full_text).substr(view_start, view_end - view_start)),
      .cursor_x = cursor_x,
      .selection_bytes = selection_bytes,
  };
}

std::optional<WorkspaceShell::TextInputVisual> WorkspaceShell::BuildActiveTextInputVisual(
    const WorkspaceLayout& layout,
    const std::optional<SDL_FRect>& active_editor_pane_rect) const {
  const RenderViewModelBuilder view_model_builder(context_);
  const OverlaySurfaceViewModel overlay_vm = view_model_builder.BuildOverlaySurface();
  const SidebarSurfaceViewModel sidebar_vm = view_model_builder.BuildSidebarSurface();
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
      const editor::TextViewport* viewport = ActiveEditorViewport();
      if (viewport == nullptr) {
        return std::nullopt;
      }
      const editor::EditorViewMetrics metrics = editor::EditorViewRenderer::ComputeMetrics(
          text_renderer_, *viewport, *active_editor_pane_rect);
      const float cursor_x =
          metrics.text_x +
          static_cast<float>(viewport->cursor_visual_column() - viewport->horizontal_scroll()) *
              char_width;
      const float cursor_y =
          metrics.first_line_y +
          static_cast<float>(viewport->cursor_line() - viewport->scroll_line()) *
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
    case TextInputSurface::Command: {
      const SDL_FRect prompt_rect = BottomPanelCommandPromptRect(layout);
      const float text_x = prompt_rect.x + 6.0f;
      const float text_y = prompt_rect.y + 4.0f;
      const float available_width = std::max(1.0f, prompt_rect.w - 12.0f);
      auto vm = ComputeSingleLineViewMetrics(
          context_.current_project_state.panel.command.input, "> ", available_width);
      return TextInputVisual{
          .surface = surface,
          .area = MakeRect(text_x, text_y, available_width, line_height),
          .text_x = text_x,
          .text_y = text_y,
          .cursor_x = text_x + vm.cursor_x,
          .foreground = theme_.text_primary,
          .background = theme_.surface_background,
          .displayed_text = std::move(vm.displayed_text),
          .selection_bytes = vm.selection_bytes,
      };
    }
    case TextInputSurface::ChatComposer: {
      const SDL_FRect prompt_rect =
          sidebar_vm.visible && sidebar_vm.mode == SidebarMode::Chat
              ? ChatSidebarComposerRect(layout.sidebar)
              : BottomPanelCommandPromptRect(layout);
      auto& composer =
          const_cast<editor::TextViewport&>(context_.current_project_state.panel.chat.composer);
      const std::size_t visible_lines = std::max<std::size_t>(
          1, static_cast<std::size_t>(std::floor(std::max(1.0f, prompt_rect.h - 8.0f) / line_height)));
      const std::size_t visible_columns = std::max<std::size_t>(
          1, static_cast<std::size_t>(std::floor(std::max(1.0f, prompt_rect.w - 12.0f) / char_width)));
      composer.SetViewportSize(visible_lines, visible_columns);
      const editor::LayoutLine layout_line = composer.VisibleLineLayout(composer.cursor_line());
      const float text_x = prompt_rect.x + 6.0f;
      const float text_y = prompt_rect.y + 4.0f;
      const float cursor_x = text_x + static_cast<float>(layout_line.caret_column) * char_width;
      const float cursor_y =
          text_y + static_cast<float>(composer.cursor_line() - composer.scroll_line()) * line_height;
      return TextInputVisual{
          .surface = surface,
          .area = MakeRect(text_x, text_y, std::max(1.0f, prompt_rect.w - 12.0f),
                           std::max(1.0f, prompt_rect.h - 8.0f)),
          .text_x = text_x,
          .text_y = text_y,
          .cursor_x = cursor_x,
          .cursor_y = cursor_y,
          .foreground = theme_.text_primary,
          .background = theme_.surface_background,
          .displayed_text = {},
          .selection_bytes = std::nullopt,
      };
    }
    case TextInputSurface::PromptInput: {
      const SDL_FRect dialog = ComputePromptSurfaceRect(layout.full);
      const SDL_FRect input_rect = ComputePromptSurfaceInputRect(dialog);
      const float text_x = input_rect.x + 6.0f;
      const float text_y = input_rect.y + 4.0f;
      const float available_width = std::max(1.0f, input_rect.w - 12.0f);
      auto vm = ComputeSingleLineViewMetrics(
          context_.prompts.surface.input, "", available_width);
      return TextInputVisual{
          .surface = surface,
          .area = MakeRect(text_x, text_y, available_width, line_height),
          .text_x = text_x,
          .text_y = text_y,
          .cursor_x = text_x + vm.cursor_x,
          .foreground = theme_.text_primary,
          .background = theme_.surface_background,
          .displayed_text = std::move(vm.displayed_text),
          .selection_bytes = vm.selection_bytes,
      };
    }
    case TextInputSurface::FileFinder:
    case TextInputSurface::BufferSearch:
    case TextInputSurface::BufferReplaceSearch:
    case TextInputSurface::BufferReplaceReplace:
    case TextInputSurface::ProjectSearchOverlay:
    case TextInputSurface::CommitPicker: {
      if (!overlay_vm.visible) {
        return std::nullopt;
      }
      const SDL_FRect overlay = ComputeOverlayRect(layout.editor_area);
      const float inset = 18.0f;
      const float text_x = overlay.x + inset;
      float text_y = overlay.y + 44.0f;
      const float available_width = std::max(1.0f, overlay.w - inset * 2.0f);
      SingleLineViewMetrics vm;
      switch (surface) {
        case TextInputSurface::BufferSearch:
          vm = ComputeSingleLineViewMetrics(
              context_.current_project_state.overlay.workflow.buffer_search.query,
              "> ", available_width);
          break;
        case TextInputSurface::BufferReplaceSearch:
          vm = ComputeSingleLineViewMetrics(
              context_.current_project_state.overlay.workflow.buffer_search.query,
              "find: ", available_width);
          break;
        case TextInputSurface::BufferReplaceReplace:
          text_y = overlay.y + 62.0f;
          vm = ComputeSingleLineViewMetrics(
              context_.current_project_state.overlay.workflow.buffer_search.replace_text,
              "replace: ", available_width);
          break;
        case TextInputSurface::ProjectSearchOverlay:
          vm = ComputeSingleLineViewMetrics(
              context_.current_project_state.overlay.workflow.project_search.query,
              "> ", available_width);
          break;
        case TextInputSurface::CommitPicker:
          text_y = overlay.y + 62.0f;
          vm = ComputeSingleLineViewMetrics(
              context_.current_project_state.overlay.workflow.compare_picker.query,
              "> ", available_width);
          break;
        case TextInputSurface::FileFinder:
        default:
          vm = ComputeSingleLineViewMetrics(
              context_.current_project_state.file_finder.query_state(),
              "> ", available_width);
          break;
      }
      return TextInputVisual{
          .surface = surface,
          .area = MakeRect(text_x, text_y, available_width, line_height),
          .text_x = text_x,
          .text_y = text_y,
          .cursor_x = text_x + vm.cursor_x,
          .foreground = theme_.text_secondary,
          .background = theme_.surface_background,
          .displayed_text = std::move(vm.displayed_text),
          .selection_bytes = vm.selection_bytes,
      };
    }
    case TextInputSurface::SidebarSearchQuery:
    case TextInputSurface::SidebarSearchReplace: {
      if (!sidebar_vm.visible || sidebar_vm.mode != SidebarMode::Search ||
          !sidebar_vm.project_search_editing) {
        return std::nullopt;
      }
      const SDL_FRect text_rect =
          surface == TextInputSurface::SidebarSearchQuery ? ProjectSearchQueryRect(layout.sidebar)
                                                          : ProjectSearchReplaceRect(layout.sidebar);
      const float text_x = text_rect.x + 6.0f;
      const float text_y = text_rect.y + 2.0f;
      const std::string_view prefix =
          surface == TextInputSurface::SidebarSearchQuery ? "search> " : "replace> ";
      const float available_width =
          std::max(1.0f, text_rect.w - 12.0f);
      auto vm = ComputeSingleLineViewMetrics(
          context_.current_project_state.overlay.workflow.project_search.edit_buffer,
          prefix, available_width);
      return TextInputVisual{
          .surface = surface,
          .area = MakeRect(text_x, text_y, available_width, line_height),
          .text_x = text_x,
          .text_y = text_y,
          .cursor_x = text_x + vm.cursor_x,
          .foreground = theme_.text_primary,
          .background = theme_.surface_background,
          .displayed_text = std::move(vm.displayed_text),
          .selection_bytes = vm.selection_bytes,
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

void WorkspaceShell::RenderSingleLineTextSelection(
    SDL_Renderer* renderer,
    const std::optional<TextInputVisual>& visual) const {
  if (!visual.has_value() || !visual->selection_bytes.has_value() ||
      visual->displayed_text.empty()) {
    return;
  }

  switch (visual->surface) {
    case TextInputSurface::PromptInput:
    case TextInputSurface::Command:
    case TextInputSurface::ChatComposer:
    case TextInputSurface::FileFinder:
    case TextInputSurface::BufferSearch:
    case TextInputSurface::BufferReplaceSearch:
    case TextInputSurface::BufferReplaceReplace:
    case TextInputSurface::ProjectSearchOverlay:
    case TextInputSurface::CommitPicker:
    case TextInputSurface::SidebarSearchQuery:
    case TextInputSurface::SidebarSearchReplace:
      break;
    case TextInputSurface::None:
    case TextInputSurface::Editor:
    case TextInputSurface::Terminal:
      return;
  }

  const auto [sel_start_byte, sel_end_byte] = *visual->selection_bytes;
  if (sel_start_byte >= sel_end_byte || sel_end_byte > visual->displayed_text.size()) {
    return;
  }

  const std::string_view display_sv = visual->displayed_text;
  const float sel_start_x =
      visual->text_x +
      text_renderer_.MeasureWidth(display_sv.substr(0, sel_start_byte));
  const float sel_end_x =
      visual->text_x +
      text_renderer_.MeasureWidth(display_sv.substr(0, sel_end_byte));
  const float sel_width = sel_end_x - sel_start_x;

  if (sel_width <= 0.0f) {
    return;
  }

  DrawFilledRect(renderer,
                 MakeRect(sel_start_x, visual->text_y - 1.0f, sel_width,
                          text_renderer_.LineHeight()),
                 theme_.selection_fill);
  text_renderer_.DrawStringOn(
      renderer, sel_start_x, visual->text_y, theme_.text_primary, theme_.selection_fill,
      display_sv.substr(sel_start_byte, sel_end_byte - sel_start_byte));
}

void WorkspaceShell::RenderActiveTextInputCaret(
    SDL_Renderer* renderer,
    const std::optional<TextInputVisual>& visual) const {
  if (!visual.has_value() || !context_.interaction_state.window_has_input_focus ||
      !context_.text_input.composition.text.empty()) {
    return;
  }

  switch (visual->surface) {
    case TextInputSurface::PromptInput:
    case TextInputSurface::Command:
    case TextInputSurface::ChatComposer:
    case TextInputSurface::FileFinder:
    case TextInputSurface::BufferSearch:
    case TextInputSurface::BufferReplaceSearch:
    case TextInputSurface::BufferReplaceReplace:
    case TextInputSurface::ProjectSearchOverlay:
    case TextInputSurface::CommitPicker:
    case TextInputSurface::SidebarSearchQuery:
    case TextInputSurface::SidebarSearchReplace:
      break;
    case TextInputSurface::None:
    case TextInputSurface::Editor:
    case TextInputSurface::Terminal:
      return;
  }

  if (!CaretVisibleNow()) {
    return;
  }

  DrawFilledRect(renderer,
                 MakeRect(visual->cursor_x, (visual->cursor_y != 0.0f ? visual->cursor_y : visual->text_y) - 1.0f, 1.5f,
                          text_renderer_.LineHeight()),
                 theme_.cursor);
}

void WorkspaceShell::RenderTextComposition(
    SDL_Renderer* renderer,
    const std::optional<TextInputVisual>& visual) const {
  if (!visual.has_value() || context_.text_input.composition.text.empty() ||
      context_.text_input.composition.surface != visual->surface) {
    return;
  }

  const std::string_view composition = context_.text_input.composition.text;
  const std::size_t total_codepoints = util::Utf8CodepointCount(composition);
  const std::size_t selection_start_codepoints =
      context_.text_input.composition.start < 0
          ? total_codepoints
          : std::min<std::size_t>(static_cast<std::size_t>(context_.text_input.composition.start),
                                  total_codepoints);
  const std::size_t selection_end_codepoints =
      context_.text_input.composition.length <= 0
          ? selection_start_codepoints
          : std::min(total_codepoints,
                     selection_start_codepoints +
                         static_cast<std::size_t>(context_.text_input.composition.length));
  const std::size_t selection_start =
      util::Utf8ByteOffsetForCodepointCount(composition, selection_start_codepoints);
  const std::size_t selection_end =
      util::Utf8ByteOffsetForCodepointCount(composition, selection_end_codepoints);
  const std::string_view prefix = composition.substr(0, selection_start);
  const std::string_view selected =
      composition.substr(selection_start, selection_end - selection_start);
  const std::string_view suffix = composition.substr(selection_end);
  const float prefix_width = text_renderer_.MeasureWidth(prefix);
  const float selected_width = text_renderer_.MeasureWidth(selected);
  const float total_width = text_renderer_.MeasureWidth(composition);

  if (!selected.empty()) {
    DrawFilledRect(renderer,
                   MakeRect(visual->cursor_x + prefix_width,
                            (visual->cursor_y != 0.0f ? visual->cursor_y : visual->text_y) - 1.0f,
                            selected_width,
                            text_renderer_.LineHeight()),
                   theme_.selection_fill);
  }

  float segment_x = visual->cursor_x;
  if (!prefix.empty()) {
    text_renderer_.DrawStringOn(renderer, segment_x,
                                visual->cursor_y != 0.0f ? visual->cursor_y : visual->text_y,
                                theme_.accent,
                                visual->background, prefix);
    segment_x += prefix_width;
  }
  if (!selected.empty()) {
    text_renderer_.DrawStringOn(
        renderer, segment_x, visual->cursor_y != 0.0f ? visual->cursor_y : visual->text_y,
        theme_.text_primary, theme_.selection_fill, selected);
    segment_x += selected_width;
  }
  if (!suffix.empty()) {
    text_renderer_.DrawStringOn(renderer, segment_x,
                                visual->cursor_y != 0.0f ? visual->cursor_y : visual->text_y,
                                theme_.accent,
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
