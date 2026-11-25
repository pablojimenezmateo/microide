#include "workspace/shell/WorkspaceShell.h"

#include <algorithm>
#include <cstdint>
#include <optional>

#include "render/DevicePixelSnap.h"
#include "util/Parse.h"
#include "workspace/SettingFlags.h"

namespace microide::workspace {

namespace {

constexpr Uint64 kCaretBlinkIntervalDefaultMs = 530;
constexpr Uint64 kCaretBlinkIdleStopMs = 8000;

}

bool WorkspaceShell::CaretBlinkEnabled() const {
  return SettingFlagEnabled(GetSettingValue("editor.caret_blink.enabled"), true);
}

Uint64 WorkspaceShell::CaretBlinkIntervalMs() const {
  const int parsed = util::ParseIntOr(GetSettingValue("editor.caret_blink.interval_ms"),
                                      static_cast<int>(kCaretBlinkIntervalDefaultMs));
  return static_cast<Uint64>(std::clamp(parsed, 100, 2000));
}

std::optional<Uint32> WorkspaceShell::NextCaretBlinkDelayMs() const {
  if (!ShouldBlinkCaret() || !CaretBlinkEnabled()) {
    return std::nullopt;
  }

  const Uint64 interval = CaretBlinkIntervalMs();
  const Uint64 elapsed = SDL_GetTicks() - caret_blink_epoch_ms_;
  if (elapsed >= kCaretBlinkIdleStopMs) {
    return std::nullopt;
  }
  const Uint64 remaining = interval - (elapsed % interval);
  const Uint64 until_freeze = kCaretBlinkIdleStopMs - elapsed;
  return static_cast<Uint32>(std::max<Uint64>(1, std::min(remaining, until_freeze)));
}

void WorkspaceShell::RequestSingleLineDragSurfaceRedraw(DamageMark mark,
                                                        TextInputSurface surface) {
  switch (surface) {
    case TextInputSurface::PromptInput:
      EnsureRedraw(mark, [this]() { RequestPreparedFrame(); });
      break;
    case TextInputSurface::SidebarSearchQuery:
    case TextInputSurface::SidebarSearchReplace:
    case TextInputSurface::SidebarSearchInclude:
    case TextInputSurface::SidebarSearchExclude:
      EnsureRedraw(mark, [this]() { RequestPreparedFrame(); });
      break;
    default:
      EnsureRedraw(mark, [this]() { RequestPreparedFrame(); });
      break;
  }
}

void WorkspaceShell::ResetCaretBlink() {
  caret_blink_epoch_ms_ = SDL_GetTicks();
}

bool WorkspaceShell::CaretBlinkAnimating() const {
  if (!ShouldBlinkCaret() || !CaretBlinkEnabled()) {
    return false;
  }
  return (SDL_GetTicks() - caret_blink_epoch_ms_) < kCaretBlinkIdleStopMs;
}

bool WorkspaceShell::ShouldBlinkCaret() const {
  if (context_.prompts.dirty_visible || MenuSurfaceCapturingMouse()) {
    return false;
  }

  switch (CurrentTextInputSurface()) {
    case TextInputSurface::PromptInput:
    case TextInputSurface::FileFinder:
    case TextInputSurface::BufferSearch:
    case TextInputSurface::BufferReplaceSearch:
    case TextInputSurface::BufferReplaceReplace:
    case TextInputSurface::ProjectSearchOverlay:
    case TextInputSurface::CommitPicker:
    case TextInputSurface::QuickPick:
    case TextInputSurface::CommandPalette:
    case TextInputSurface::SidebarSearchQuery:
    case TextInputSurface::SidebarSearchReplace:
    case TextInputSurface::SidebarSearchInclude:
    case TextInputSurface::SidebarSearchExclude:
    case TextInputSurface::CommitSubject:
    case TextInputSurface::CommitBody:
    case TextInputSurface::TerminalFind:
      return true;
    case TextInputSurface::SettingsQuery:
    case TextInputSurface::SettingsValueEdit:
    case TextInputSurface::DebugVariableEdit:
      return false;
    case TextInputSurface::None:
    case TextInputSurface::Editor:
    case TextInputSurface::Terminal:
      break;
  }

  if (context_.current_project_state.surface.focus == FocusTarget::Editor) {
    const editor::TextViewport* viewport = ActiveNavigableViewport();
    return viewport != nullptr && !viewport->is_placeholder();
  }

  if (context_.current_project_state.surface.focus == FocusTarget::Panel) {
    return BottomPanelShowsTerminal() && ActiveTerminalPane() != nullptr;
  }

  return false;
}

bool WorkspaceShell::CaretVisibleNow() const {
  if (!ShouldBlinkCaret()) {
    return false;
  }
  if (!CaretBlinkEnabled()) {
    return true;
  }

  const Uint64 elapsed = SDL_GetTicks() - caret_blink_epoch_ms_;
  if (elapsed >= kCaretBlinkIdleStopMs) {
    return true;
  }
  return ((elapsed / CaretBlinkIntervalMs()) % 2) == 0;
}

std::optional<SDL_FRect> WorkspaceShell::CurrentCaretDirtyRect() const {
  if (!ShouldBlinkCaret()) {
    return std::nullopt;
  }

  const auto layout = CurrentWorkspaceLayout();
  if (!layout.has_value()) {
    return std::nullopt;
  }

  if (const auto surface = CurrentTextInputSurface();
      surface == TextInputSurface::CommitSubject || surface == TextInputSurface::CommitBody) {
    const SDL_FRect caret = context_.current_project_state.sidebar.git.commit_workflow.caret_rect;
    return (caret.w > 0.0f && caret.h > 0.0f)
               ? std::optional<SDL_FRect>(MakeRect(caret.x, caret.y, std::max(1.0f, caret.w),
                                                   caret.h))
               : std::nullopt;
  }

  if (const auto surface = CurrentTextInputSurface();
      surface != TextInputSurface::None && surface != TextInputSurface::Editor &&
      surface != TextInputSurface::Terminal) {
    const auto visual = BuildActiveTextInputVisual(*layout, std::nullopt);
    return visual.has_value()
               ? std::optional<SDL_FRect>(
                     MakeRect(render::SnapToDevicePixel(visual->cursor_x, presentation_scale_x_),
                              visual->text_y - 1.0f, std::max(1.0f, text_renderer_.CharWidth()),
                              text_renderer_.LineHeight()))
               : std::nullopt;
  }

  if (context_.current_project_state.surface.focus == FocusTarget::Editor) {
    return std::nullopt;
  }
  if (context_.current_project_state.surface.focus == FocusTarget::Panel) {
    return ActiveTerminalCaretRect(*layout);
  }
  return std::nullopt;
}

std::optional<SDL_FRect> WorkspaceShell::ActiveEditorCaretRect(
    const WorkspaceLayout& layout) const {
  if (ActiveTabIsCompare()) {
    const auto visual = BuildCompareTextInputVisual(ActiveTabSurface(layout));
    return visual.has_value() ? std::optional<SDL_FRect>(visual->area) : std::nullopt;
  }
  if (ActiveTabIsMerge()) {
    const auto visual = BuildMergeTextInputVisual(ActiveTabSurface(layout));
    return visual.has_value() ? std::optional<SDL_FRect>(visual->area) : std::nullopt;
  }

  const auto panes = ComputeEditorPaneLayouts(layout);
  auto pane_it = std::find_if(panes.begin(), panes.end(),
                              [](const EditorPaneLayout& pane) { return pane.active; });
  if (pane_it == panes.end()) {
    return context_.current_project_state.focused_group().welcome_surface.viewport.is_placeholder()
               ? std::optional<SDL_FRect>(layout.editor_surface)
               : std::nullopt;
  }
  return pane_it->rect;
}

std::optional<SDL_FRect> WorkspaceShell::ActiveTerminalCaretRect(
    const WorkspaceLayout& layout) const {
  const auto* terminal_tab = ActiveTerminalPane();
  if (terminal_tab == nullptr) {
    return std::nullopt;
  }
  return TerminalCaretRectIn(ComputeBottomPanelLogLayout(layout, terminal_tab->frame_line_count),
                             terminal_tab->frame_cursor);
}

std::optional<SDL_FRect> WorkspaceShell::TerminalCaretRectIn(
    const LogSurfaceLayout& panel_layout,
    const terminal::TerminalCursorSnapshot& cursor) const {
  if (!cursor.visible ||
      cursor.row < static_cast<std::size_t>(panel_layout.scroll.vertical_scroll) ||
      cursor.row >= static_cast<std::size_t>(panel_layout.scroll.vertical_scroll +
                                             panel_layout.scroll.visible_rows)) {
    return std::nullopt;
  }
  const float char_width = std::max(1.0f, terminal_text_renderer_.CharWidth());
  const float cursor_x = panel_layout.text_x + static_cast<float>(cursor.column) * char_width;
  if (cursor_x > panel_layout.content_rect.x + panel_layout.content_rect.w - char_width) {
    return std::nullopt;
  }
  const float caret_width =
      cursor.wide ? std::min(2.0f * char_width, panel_layout.content_rect.x + panel_layout.content_rect.w - cursor_x)
                  : char_width;
  const float cursor_y =
      panel_layout.text_y +
      static_cast<float>(cursor.row -
                         static_cast<std::size_t>(panel_layout.scroll.vertical_scroll)) *
          panel_layout.line_height;
  return MakeRect(cursor_x, cursor_y - 1.0f, caret_width, panel_layout.line_height);
}

}
