#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>

#include "editor/TextViewport.h"
#include "workspace/DebugPaneRegistry.h"
#include "workspace/WorkspaceLayout.h"
#include "workspace/WorkspaceProjectState.h"
#include "workspace/WorkspaceShell.h"

namespace microide::workspace {

// Mouse interaction for the right-side debug pane: mode-row surface switching, row
// clicks (frame/thread/session focus, variable expand/edit, watch add/edit,
// breakpoint navigation/toggle), and wheel scroll. Constructed with project state
// + an Operations callback struct — never WorkspaceShell& — per the workspace
// coordinator invariant. The value-tree hit logic moved here verbatim from the
// bottom-panel coordinator; only the geometry source changed to layout.right_pane.
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
    // Enable/disable a line breakpoint from the Breakpoints panel (double-click).
    std::function<void(const std::filesystem::path&, std::size_t)> toggle_debug_breakpoint_enabled;
    // Enable/disable a function breakpoint from the Breakpoints panel (click toggles;
    // there is no source location to navigate to).
    std::function<void(std::size_t)> toggle_debug_function_breakpoint_enabled;
  };

  DebugPaneMouseCoordinator(ProjectWorkspaceState& state, Operations operations);

  bool HandleButtonDown(const SDL_Event& event, const WorkspaceLayout& layout);
  bool HandleWheel(const SDL_Event& event, const WorkspaceLayout& layout, int vertical_ticks);

 private:
  bool HandleRowClick(const SDL_Event& event, const WorkspaceLayout& layout);

  ProjectWorkspaceState& state_;
  Operations operations_;
};

}  // namespace microide::workspace
