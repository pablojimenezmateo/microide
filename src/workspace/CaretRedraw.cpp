#include "workspace/WorkspaceShell.h"

#include <algorithm>
#include <cstdint>
#include <optional>

#include "util/Parse.h"
#include "workspace/SettingFlags.h"

namespace microide::workspace {

namespace {

constexpr Uint64 kCaretBlinkIntervalDefaultMs = 530;
// After this many ms with no caret-blink-resetting input (typing, navigation,
// focus changes, etc.) the caret freezes in its visible phase and we stop
// scheduling blink-only wake-ups. This cuts the idle wake/partial-redraw
// rate to ~0 during long inactivity, matching the dominant terminal
// convention and many editors' "stop-blink" timeout. The next input event
// resets `caret_blink_epoch_ms_` via `ResetCaretBlink()` and resumes blink.
constexpr Uint64 kCaretBlinkIdleStopMs = 8000;

}  // namespace

bool WorkspaceShell::CaretBlinkEnabled() const {
  return SettingFlagEnabled(GetSettingValue("editor.caret_blink.enabled"), true);
}

Uint64 WorkspaceShell::CaretBlinkIntervalMs() const {
  const int parsed = util::ParseIntOr(GetSettingValue("editor.caret_blink.interval_ms"),
                                      static_cast<int>(kCaretBlinkIntervalDefaultMs));
  return static_cast<Uint64>(std::clamp(parsed, 100, 2000));
}

std::optional<Uint32> WorkspaceShell::NextCaretBlinkDelayMs() const {
  // A disabled blink shows a solid caret and schedules no blink-only wake-ups.
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
  if (context_.prompts.dirty_visible || context_.menu_state.menu_bar_open ||
      context_.menu_state.tree_context_menu.open) {
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
    case TextInputSurface::LaunchConfigPicker:
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
      // The Settings overlay renders its own static (non-blinking) caret; it does
      // not participate in the shared caret-blink machinery.
    case TextInputSurface::DebugVariableEdit:
      // The Variables value field likewise renders its own static caret in the
      // bottom-panel TU, so it stays out of the blink machinery (no idle wake-ups).
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
    return BottomPanelShowsTerminal() && ActiveTerminalTab() != nullptr;
  }

  return false;
}

bool WorkspaceShell::CaretVisibleNow() const {
  if (!ShouldBlinkCaret()) {
    return false;
  }
  // Blink disabled: a focused caret is always solid.
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
    // The commit fields are rendered by the sidebar panel, which caches the focused field's
    // caret rect each frame; reuse it so blink-only wake-ups repaint a tight region.
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
               ? std::optional<SDL_FRect>(MakeRect(visual->cursor_x, visual->text_y - 1.0f,
                                                   std::max(1.0f, text_renderer_.CharWidth()),
                                                   text_renderer_.LineHeight()))
               : std::nullopt;
  }

  if (context_.current_project_state.surface.focus == FocusTarget::Editor) {
    return ActiveEditorCaretRect(*layout);
  }
  if (context_.current_project_state.surface.focus == FocusTarget::Panel) {
    return ActiveTerminalCaretRect(*layout);
  }
  return std::nullopt;
}

std::optional<SDL_FRect> WorkspaceShell::ActiveEditorCaretRect(
    const WorkspaceLayout& layout) const {
  if (ActiveTabIsCompare()) {
    const auto visual = BuildCompareTextInputVisual(layout.editor_surface);
    return visual.has_value() ? std::optional<SDL_FRect>(visual->area) : std::nullopt;
  }
  if (ActiveTabIsMerge()) {
    const auto visual = BuildMergeTextInputVisual(layout.editor_surface);
    return visual.has_value() ? std::optional<SDL_FRect>(visual->area) : std::nullopt;
  }

  const auto panes = ComputeEditorPaneLayouts(layout.editor_surface);
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
  const auto* terminal_tab = ActiveTerminalTab();
  if (terminal_tab == nullptr) {
    return std::nullopt;
  }
  const std::size_t line_count = terminal_tab->session.LineCount();
  return TerminalCaretRectIn(ComputeBottomPanelLogLayout(layout, line_count),
                             terminal_tab->session.CursorSnapshot());
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
  const float cursor_y =
      panel_layout.text_y +
      static_cast<float>(cursor.row -
                         static_cast<std::size_t>(panel_layout.scroll.vertical_scroll)) *
          panel_layout.line_height;
  return MakeRect(cursor_x, cursor_y - 1.0f, char_width, panel_layout.line_height);
}

}  // namespace microide::workspace
