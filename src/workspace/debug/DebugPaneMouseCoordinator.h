#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>

#include "editor/TextViewport.h"
#include "workspace/debug/DebugPaneRegistry.h"
#include "workspace/state/WorkspaceInteractionState.h"
#include "workspace/WorkspaceLayout.h"
#include "workspace/state/WorkspaceProjectState.h"
#include "workspace/shell/WorkspaceShell.h"

namespace microide::workspace {

class DebugPaneMouseCoordinator {
 public:
  struct Operations {
    std::function<WorkspaceShell::LogSurfaceLayout(const WorkspaceLayout&, std::size_t)>
        compute_debug_pane_list_layout;
    std::function<DebugPaneModeRowLayout(const SDL_FRect&)> debug_pane_mode_row;
    std::function<std::size_t()> debug_pane_active_row_count;
    std::function<void(int, std::size_t, int)> set_debug_pane_scroll_row;
    std::function<void(DebugPaneMode)> show_debug_pane_mode;
    std::function<void(const std::filesystem::path&)> open_file;
    std::function<editor::TextViewport*()> active_editor_viewport;
    std::function<void(int)> on_debug_frame_focus_changed;
    std::function<void(int)> on_debug_thread_focus_changed;
    std::function<void(int)> on_debug_session_focus_changed;
    std::function<void(std::size_t)> toggle_debug_variable_row;
    std::function<void(std::size_t)> begin_debug_variable_edit;
    std::function<void(std::size_t)> toggle_debug_watch_row;
    std::function<void(std::size_t)> begin_debug_watch_edit;
    std::function<void()> add_debug_watch_expression;
    std::function<void(std::size_t)> edit_debug_watch_expression;
    std::function<void(const std::string&)> toggle_debug_exception_filter;
    std::function<void(const std::filesystem::path&, std::size_t)> toggle_debug_breakpoint_enabled;
    std::function<void(std::size_t)> toggle_debug_function_breakpoint_enabled;
    std::function<void(const SDL_FRect&)> open_debug_value_context_menu;
    std::function<void(const std::filesystem::path&, std::size_t, const SDL_FRect&)>
        open_breakpoint_context_menu;
  };

  DebugPaneMouseCoordinator(ProjectWorkspaceState& state, InteractionState& interaction_state,
                            Operations operations);

  bool HandleButtonDown(const SDL_Event& event, const WorkspaceLayout& layout);
  bool HandleWheel(const SDL_Event& event, const WorkspaceLayout& layout, int vertical_ticks);
  bool HandleDrag(const SDL_Event& event, const WorkspaceLayout& layout);
  void ActivateCallStackRow(std::size_t line_index, bool move_focus_to_editor);
  void ActivateBreakpointRow(std::size_t line_index, bool toggle,
                             bool move_focus_to_editor);

 private:
  bool HandleRowClick(const SDL_Event& event, const WorkspaceLayout& layout);
  bool HandleRowContextMenu(const SDL_Event& event, const WorkspaceLayout& layout);
  bool BeginScrollbarDrag(const SDL_Event& event, const WorkspaceLayout& layout);

  ProjectWorkspaceState& state_;
  InteractionState& interaction_state_;
  Operations operations_;
};

}
