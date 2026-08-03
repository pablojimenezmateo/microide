#pragma once

#include <functional>

#include "workspace/state/WorkspaceProjectState.h"

namespace microide::workspace {

// Owns transitions for the right-side debug pane (visibility, active surface,
// width). Constructed with a ProjectWorkspaceState reference plus an Operations
// callback struct — never WorkspaceShell& — per the workspace coordinator
// invariant. Mirrors SidebarService/SidebarCoordinator but is far leaner: the
// pane hosts a fixed, debug-owned set of surfaces with no plugin plumbing.
class DebugPaneService {
 public:
  struct Operations {
    std::function<void()> request_redraw;
    std::function<void()> mark_layout_dirty;
  };

  DebugPaneService(ProjectWorkspaceState& state, Operations operations);

  // Show a specific surface (makes the pane visible and focuses it).
  void ShowMode(DebugPaneMode mode);

  // Hide the pane (e.g. on session teardown). Moves focus back to the editor if
  // the pane currently holds it.
  void Close();
  // Toggle pane visibility (Debug menu / keybinding).
  void Toggle();

  // Auto-open on the first stop: make the pane visible on Call Stack, but only
  // when it is not already visible, so stepping doesn't yank the user off the
  // Variables/Watch surface they were inspecting.
  void OpenOnStop();

  // Set the pane width (resize drag). The frame path re-clamps against the
  // current window + sidebar width, so this stores the requested value.

 private:
  void Activate(DebugPaneMode mode);

  ProjectWorkspaceState& state_;
  Operations operations_;
};

}  // namespace microide::workspace
