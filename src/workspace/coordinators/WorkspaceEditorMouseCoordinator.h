#pragma once

#include <functional>
#include <optional>
#include <string_view>

#include "workspace/shell/WorkspaceShell.h"

namespace microide::workspace {

class DebugService;

class EditorMouseCoordinator {
 public:
  struct Services {
    DebugService& debug;
  };

  struct Operations {
    std::function<WorkspaceShell::EditorPaneLayouts(const WorkspaceLayout&)>
        compute_editor_pane_layouts;
    std::function<editor::TextViewport*()> active_editor_viewport;
    std::function<editor::TextViewport*(const WorkspaceShell::EditorPaneLayout&)>
        viewport_for_pane;
    std::function<ScrollSurfaceLayout(const SDL_FRect&,
                                      const editor::TextViewport&,
                                      const editor::EditorViewMetrics&)>
        compute_editor_scroll_layout;
    std::function<std::optional<std::string>()> read_primary_selection_text;
    std::function<void()> reset_caret_blink;
    std::function<void()> note_active_editable_edit;
    std::function<void()> request_frame;
    std::function<void()> clear_drag_state;
    std::function<std::optional<std::string>(std::string_view id)> get_setting_value;
    std::function<editor::FoldingModel*()> ensure_active_folding_model_fresh;
    std::function<void(EditorBannerAction, const std::filesystem::path&)> editor_banner_action;
    std::function<void(const std::filesystem::path&, std::size_t, const SDL_FRect&)>
        open_breakpoint_context_menu;
    std::function<void()> go_to_definition;
  };

  EditorMouseCoordinator(ProjectWorkspaceState& state,
                         InteractionState& interaction_state,
                         render::TextRenderer& text_renderer,
                         Services services,
                         Operations operations);

  bool HandleButtonDown(const SDL_Event& event, const WorkspaceLayout& layout);
  bool HandleGutterContextMenu(const SDL_Event& event, const WorkspaceLayout& layout);
  bool HandleDrag(const SDL_Event& event, const WorkspaceLayout& layout);
  bool HandleSelectionMotion(const SDL_Event& event, const WorkspaceLayout& layout);
  bool FinishTextDrag(bool copy);
  bool HandleWheel(const SDL_Event& event,
                   const WorkspaceLayout& layout,
                   int vertical_ticks,
                   int horizontal_ticks);

 private:
  ProjectWorkspaceState& state_;
  InteractionState& interaction_state_;
  render::TextRenderer& text_renderer_;
  Services services_;
  Operations operations_;
};

}
