#pragma once

#include <functional>
#include <filesystem>
#include <optional>
#include <vector>

#include "workspace/state/WorkspaceInteractionState.h"
#include "workspace/WorkspaceLayout.h"
#include "workspace/state/WorkspaceMenuState.h"
#include "workspace/state/WorkspaceProjectState.h"
#include "workspace/shell/WorkspaceShell.h"

namespace microide::workspace {

class PanelMouseCoordinator {
 public:
  struct Operations {
    std::function<bool()> bottom_panel_visible;
    std::function<SDL_FRect(const WorkspaceLayout&)> bottom_panel_resize_handle_rect;
    std::function<WorkspaceShell::LogSurfaceLayout(const WorkspaceLayout&, std::size_t)>
        compute_bottom_panel_log_layout;
    std::function<std::size_t()> bottom_panel_line_count;
    std::function<void(int, std::size_t, int)> set_bottom_panel_scroll_row;
    std::function<void(MenuId, const SDL_FRect&)> open_anchored_menu;
    std::function<SDL_FRect(const WorkspaceLayout&)> bottom_panel_content_rect;
    std::function<std::optional<std::string>()> read_primary_selection_text;
    std::function<void()> clear_terminal_selection;
    std::function<void(std::string_view)> append_terminal_pending_input;
    std::function<bool(float, float)> terminal_find_mouse_down;
    std::function<std::optional<TerminalLinkHit>(float, float)> terminal_link_at_point;
    std::function<bool(std::string_view)> open_external_url;
    std::function<std::optional<TerminalSelectionPoint>(int,
                                                           int,
                                                           const std::vector<terminal::TerminalLine>&,
                                                           std::size_t)>
        terminal_selection_position_for_point;
    std::function<std::optional<WorkspaceLayout>()> current_workspace_layout;
    std::function<std::optional<TerminalSelectionPoint>(int, int)> terminal_viewport_position_for_point;
    std::function<terminal::TerminalSession::MouseButton(Uint8)> terminal_mouse_button_for_sdl;
    std::function<const std::vector<std::string>*()> output_channel_entries;
    std::function<void(const std::filesystem::path&)> open_file;
    std::function<editor::TextViewport*()> active_editor_viewport;
    std::function<std::optional<SDL_FRect>()> current_window_rect;
    std::function<float(float, float)> clamp_bottom_panel_height;
    std::function<void()> sync_primary_selection_with_terminal_selection;
    std::function<const editor::SurfaceContent*()> active_plugin_surface;
    std::function<void(const std::string&)> execute_command;
    std::function<TerminalPaneState*(const WorkspaceLayout&, float, float)> terminal_pane_at;
    std::function<bool(const WorkspaceLayout&, float, float)> activate_terminal_pane_at;
    std::function<std::optional<TerminalPaneDividerRect>(const WorkspaceLayout&, float, float)>
        terminal_pane_divider_at;
    std::function<TerminalPaneRectsLayout(const WorkspaceLayout&)> terminal_pane_rects;
    std::function<bool(std::size_t, float)> resize_terminal_pane_divider;
    std::function<bool(std::size_t)> reset_terminal_pane_divider;
  };

  PanelMouseCoordinator(ProjectWorkspaceState& state,
                        InteractionState& interaction_state,
                        Operations operations);

  bool HandleResizeButtonDown(const SDL_Event& event, const WorkspaceLayout& layout);
  bool HandleButtonDown(const SDL_Event& event, const WorkspaceLayout& layout);
  bool HandleButtonUp(const SDL_Event& event);
  bool HandleDrag(const SDL_Event& event, const WorkspaceLayout& layout);
  bool HandleMotion(const SDL_Event& event);
  bool HandleWheel(const SDL_Event& event,
                   const WorkspaceLayout& layout,
                   int vertical_ticks);

  void ScrollPanelRows(const WorkspaceLayout& layout, int row_delta);
  int ScrollSpanRows(const WorkspaceLayout& layout);

 private:
  float PanelPixelsPerScrollRow(const WorkspaceLayout& layout);
  bool HandleMouseCaptureButton(const SDL_Event& event, bool pressed);
  bool ProgramOwnsPointer(const TerminalPaneState& pane) const;
  TerminalPaneState* ActivePanelTerminalPane();

  ProjectWorkspaceState& state_;
  InteractionState& interaction_state_;
  Operations operations_;
};

}
