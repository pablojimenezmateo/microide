#pragma once

#include <functional>
#include <filesystem>
#include <optional>
#include <vector>

#include "workspace/WorkspaceInteractionState.h"
#include "workspace/WorkspaceLayout.h"
#include "workspace/WorkspaceMenuState.h"
#include "workspace/WorkspaceProjectState.h"
#include "workspace/WorkspaceShell.h"

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
    std::function<std::optional<std::string>(float, float)> terminal_url_at_point;
    std::function<bool(std::string_view)> open_external_url;
    std::function<std::optional<TerminalSelectionPosition>(int,
                                                           int,
                                                           const std::vector<terminal::TerminalLine>&,
                                                           std::size_t)>
        terminal_selection_position_for_point;
    std::function<std::optional<WorkspaceLayout>()> current_workspace_layout;
    std::function<std::optional<TerminalSelectionPosition>(int, int)> terminal_viewport_position_for_point;
    std::function<terminal::TerminalSession::MouseButton(Uint8)> terminal_mouse_button_for_sdl;
    std::function<const std::vector<std::string>*()> output_channel_entries;
    std::function<void(const std::filesystem::path&)> open_file;
    std::function<editor::TextViewport*()> active_editor_viewport;
    std::function<std::optional<SDL_FRect>()> current_window_rect;
    std::function<float(float, float)> clamp_bottom_panel_height;
    std::function<void()> sync_primary_selection_with_terminal_selection;
    // Debug Variables panel (Phase 4): re-fetch scopes/variables when a different
    // call-stack frame is focused; lazily expand/collapse a tree row; enter inline
    // value edit on a leaf.
    std::function<void(int)> on_debug_frame_focus_changed;
    // Call Stack thread selector (Phase 7 multi-thread): switch the active thread,
    // re-resolving its frames.
    std::function<void(int)> on_debug_thread_focus_changed;
    // Call Stack session selector (Phase 8 multi-session): switch the active
    // session, re-projecting its stop.
    std::function<void(int)> on_debug_session_focus_changed;
    std::function<void(std::size_t)> toggle_debug_variable_row;
    std::function<void(std::size_t)> begin_debug_variable_edit;
    // Debug Watch panel (Phase 6): toggle a watched value's subtree; begin inline
    // setVariable edit on a watched child leaf; add/edit a watch expression
    // string (routed to a prompt on the shell side; `edit` takes its index).
    std::function<void(std::size_t)> toggle_debug_watch_row;
    std::function<void(std::size_t)> begin_debug_watch_edit;
    std::function<void()> add_debug_watch_expression;
    std::function<void(std::size_t)> edit_debug_watch_expression;
    // Debug Breakpoints panel (Phase 7): toggle an exception-breakpoint filter.
    std::function<void(const std::string&)> toggle_debug_exception_filter;
  };

  PanelMouseCoordinator(ProjectWorkspaceState& state,
                        MenuSurfaceState& menu_state,
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

 private:
  bool HandleMouseCaptureButton(const SDL_Event& event, bool pressed);

  ProjectWorkspaceState& state_;
  MenuSurfaceState& menu_state_;
  InteractionState& interaction_state_;
  Operations operations_;
};

}  // namespace microide::workspace
