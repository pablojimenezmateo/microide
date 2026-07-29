#pragma once

#include <functional>
#include <optional>
#include <string_view>

#include "workspace/WorkspaceShell.h"

namespace microide::workspace {

class EditorMouseCoordinator {
 public:
  struct Operations {
    std::function<std::vector<WorkspaceShell::EditorPaneLayout>(const SDL_FRect&)>
        compute_editor_pane_layouts;
    std::function<editor::TextViewport*()> active_editor_viewport;
    // Resolves the viewport backing a specific split pane (by group), without
    // changing which group is focused. Used so a wheel scroll targets the pane
    // under the pointer rather than only the focused pane.
    std::function<editor::TextViewport*(const WorkspaceShell::EditorPaneLayout&)>
        viewport_for_pane;
    std::function<ScrollSurfaceLayout(const SDL_FRect&,
                                      const editor::TextViewport&,
                                      const editor::EditorViewMetrics&)>
        compute_editor_scroll_layout;
    std::function<std::optional<std::string>()> read_primary_selection_text;
    std::function<void()> reset_caret_blink;
    std::function<void()> request_active_editable_last_change_redraw;
    std::function<void(std::size_t, std::size_t)> request_active_editable_blame_neighborhood_redraw;
    std::function<void()> request_tab_strip_redraw;
    std::function<void()> request_focused_editor_redraw;
    std::function<TabEntry::EditorTabState*()> active_editor_tab;
    std::function<void()> clear_drag_state;
    std::function<std::optional<std::string>(std::string_view id)> get_setting_value;
    std::function<editor::FoldingModel*()> ensure_active_folding_model_fresh;
    // Dispatches a non-blocking external-change banner action (Reload/Overwrite/Keep).
    std::function<void(EditorBannerAction, const std::filesystem::path&)> editor_banner_action;
    // Notifies that a breakpoint was toggled on `path` (live re-send to an
    // active debug session). Only invoked when `debug.enabled` is ON.
    std::function<void(const std::filesystem::path&)> on_breakpoint_toggled;
    // Opens the breakpoint-gutter context menu for `path:line` (Phase 6).
    // Only invoked on a gutter right-click when `debug.enabled` is ON.
    std::function<void(const std::filesystem::path&, std::size_t, const SDL_FRect&)>
        open_breakpoint_context_menu;
  };

  EditorMouseCoordinator(ProjectWorkspaceState& state,
                         InteractionState& interaction_state,
                         render::TextRenderer& text_renderer,
                         Operations operations);

  bool HandleButtonDown(const SDL_Event& event, const WorkspaceLayout& layout);
  // Right-click on the breakpoint gutter → open the breakpoint context menu
  // (Phase 6). Returns true only when it opened the menu; has no other side
  // effects, so the caller can fall through to the editor context menu when it
  // declines. Gated on `debug.enabled`.
  bool HandleGutterContextMenu(const SDL_Event& event, const WorkspaceLayout& layout);
  bool HandleDrag(const SDL_Event& event, const WorkspaceLayout& layout);
  bool HandleSelectionMotion(const SDL_Event& event, const WorkspaceLayout& layout);
  bool HandleWheel(const SDL_Event& event,
                   const WorkspaceLayout& layout,
                   int vertical_ticks,
                   int horizontal_ticks);

 private:
  ProjectWorkspaceState& state_;
  InteractionState& interaction_state_;
  render::TextRenderer& text_renderer_;
  Operations operations_;
};

}  // namespace microide::workspace
