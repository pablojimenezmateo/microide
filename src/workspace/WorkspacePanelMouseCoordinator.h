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
    // Hands a left press to the terminal find bar. Returns true when the bar took
    // it (a button or its chrome); a press anywhere else blurs the bar so the
    // keyboard goes back to the terminal, exactly like the in-file find widget.
    std::function<bool(float, float)> terminal_find_mouse_down;
    std::function<std::optional<std::string>(float, float)> terminal_url_at_point;
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
    // The debug pane's callbacks used to live here too, back when this
    // coordinator owned the Variables/Watch/Call Stack/Breakpoints hit testing.
    // That moved to DebugPaneMouseCoordinator, which declares and uses its own
    // copies; these were left behind, wired to live lambdas and never called.
    // Plugin surface preview (TD-2026-07-16-60/61): resolve the active preview's
    // content (null when none), and dispatch a hit-region command through the
    // host's validated command runner (the same path code-lens clicks use).
    std::function<const editor::SurfaceContent*()> active_plugin_surface;
    std::function<void(const std::string&)> execute_command;
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

  // The bottom panel's scroll model, shared by the wheel and the keyboard.
  // `row_delta` counts text rows toward the end of the content; the plugin-surface
  // preview scrolls by pixels and converts. Both clamp, so passing ScrollSpanRows()
  // reliably lands on either end — which is what Home/End do.
  void ScrollPanelRows(const WorkspaceLayout& layout, int row_delta);
  int ScrollSpanRows(const WorkspaceLayout& layout);

 private:
  // Pixel height of one scroll row, used to convert the shared row delta for the
  // pixel-scrolled plugin surface preview.
  float PanelPixelsPerScrollRow(const WorkspaceLayout& layout);
  bool HandleMouseCaptureButton(const SDL_Event& event, bool pressed);
  // Active terminal tab when the bottom panel is showing a terminal, else null.
  // Consolidates the "panel is a terminal AND a live tab exists" guard and routes
  // it through the bounds-checked ProjectWorkspaceState::active_terminal_tab()
  // accessor so no caller indexes terminal_tabs by a possibly-stale index.
  TerminalTabState* ActivePanelTerminalTab();

  ProjectWorkspaceState& state_;
  InteractionState& interaction_state_;
  Operations operations_;
};

}  // namespace microide::workspace
