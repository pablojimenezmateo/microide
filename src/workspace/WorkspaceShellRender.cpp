#include "workspace/WorkspaceShell.h"

#include <optional>

namespace microide::workspace {

WorkspaceRootView WorkspaceShell::MakeRootView() {
  return WorkspaceRootView(
      WorkspaceRootView::Operations{
          .prepare_render_frame =
              [this](SDL_Renderer* renderer, int width, int height) {
                PrepareRenderFrame(renderer, width, height);
              },
          .compute_layout =
              [this](int width, int height) {
                return ComputeLayout(static_cast<float>(width), static_cast<float>(height),
                                     context_.current_project_state.sidebar.visible,
                                     BottomPanelVisible(),
                                     context_.current_project_state.sidebar.width,
                                     context_.current_project_state.panel.height);
              },
          .active_terminal_line_count =
              [this]() {
                return ActiveTerminalTab() != nullptr
                           ? ActiveTerminalTab()->session.LineCount()
                           : std::size_t{0};
              },
          .reset_visible_editor_blame_overlay =
              [this]() { visible_editor_blame_overlay_.reset(); },
          .should_draw_editor_caret =
              [this]() {
                return CaretVisibleNow() &&
                       !(CurrentTextInputSurface() == TextInputSurface::Editor &&
                         !context_.text_input.composition.text.empty());
              },
          .render_frame_base =
              [this](SDL_Renderer* renderer, const WorkspaceLayout& layout) {
                RenderFrameBase(renderer, layout);
              },
          .render_active_workspace_surface =
              [this](SDL_Renderer* renderer,
                     const WorkspaceLayout& layout,
                     bool draw_editor_caret,
                     std::optional<SDL_FRect>* active_editor_pane_rect) {
                RenderActiveWorkspaceSurface(renderer, layout, draw_editor_caret,
                                            active_editor_pane_rect);
              },
          .refresh_hover_if_needed =
              [this]() {
                if (editor_hover_refresh_pending_ && last_mouse_position_valid_) {
                  UpdateEditorHover(last_mouse_x_, last_mouse_y_);
                  editor_hover_refresh_pending_ = false;
                }
              },
          .render_editor_hover_popup =
              [this](SDL_Renderer* renderer) { RenderEditorHoverPopup(renderer); },
          .render_window_chrome =
              [this](SDL_Renderer* renderer, const WorkspaceLayout& layout) {
                RenderWindowChrome(renderer, layout);
              },
          .render_sidebar_surface =
              [this](SDL_Renderer* renderer, const WorkspaceLayout& layout) {
                RenderSidebarSurface(renderer, layout);
              },
          .render_overlay_surface =
              [this](SDL_Renderer* renderer, const WorkspaceLayout& layout) {
                RenderOverlaySurface(renderer, layout);
              },
          .render_bottom_panel_surface =
              [this](SDL_Renderer* renderer,
                     const WorkspaceLayout& layout,
                     std::size_t terminal_line_count) {
                RenderBottomPanelSurface(renderer, layout, terminal_line_count);
              },
          .render_menu_popups =
              [this](SDL_Renderer* renderer, const WorkspaceLayout& layout) {
                RenderMenuPopups(renderer, layout);
              },
          .render_prompt_stack =
              [this](SDL_Renderer* renderer,
                     const WorkspaceLayout& layout,
                     const std::optional<SDL_FRect>& active_editor_pane_rect) {
                SDL_Window* render_window = SDL_GetRenderWindow(renderer);
                const auto active_text_input_visual =
                    BuildActiveTextInputVisual(layout, active_editor_pane_rect);
                RenderPromptSurface(renderer, layout, active_text_input_visual);
                RenderTextComposition(renderer, active_text_input_visual);
                UpdateTextInputArea(renderer, render_window, active_text_input_visual);
                RenderDirtyPromptSurface(renderer, layout);
              },
      });
}

void WorkspaceShell::Render(SDL_Renderer* renderer, int width, int height) {
  MakeRootView().Render(renderer, width, height);
}

void WorkspaceShell::RenderPrepared(SDL_Renderer* renderer, int width, int height) {
  MakeRootView().RenderPrepared(renderer, width, height);
}

}  // namespace microide::workspace
