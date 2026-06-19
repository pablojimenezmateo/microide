#include "workspace/DebugPaneService.h"

#include <utility>

namespace microide::workspace {

DebugPaneService::DebugPaneService(ProjectWorkspaceState& state, Operations operations)
    : state_(state), operations_(std::move(operations)) {}

void DebugPaneService::Activate(DebugPaneMode mode) {
  DebugPaneState& pane = state_.debug_pane;
  const bool was_visible = pane.visible;
  pane.visible = true;
  pane.mode = mode;
  state_.surface.focus = FocusTarget::DebugPane;
  if (!was_visible && operations_.mark_layout_dirty) {
    operations_.mark_layout_dirty();
  }
  if (operations_.request_redraw) {
    operations_.request_redraw();
  }
}

void DebugPaneService::ShowMode(DebugPaneMode mode) { Activate(mode); }

void DebugPaneService::ShowCallStack() { Activate(DebugPaneMode::CallStack); }

void DebugPaneService::ShowVariables() { Activate(DebugPaneMode::Variables); }

void DebugPaneService::ShowWatch() { Activate(DebugPaneMode::Watch); }

void DebugPaneService::ShowBreakpoints() { Activate(DebugPaneMode::Breakpoints); }

void DebugPaneService::Close() {
  DebugPaneState& pane = state_.debug_pane;
  if (!pane.visible) {
    return;
  }
  pane.visible = false;
  // Don't strand keyboard focus on a hidden surface.
  if (state_.surface.focus == FocusTarget::DebugPane) {
    state_.surface.focus = FocusTarget::Editor;
  }
  if (operations_.mark_layout_dirty) {
    operations_.mark_layout_dirty();
  }
  if (operations_.request_redraw) {
    operations_.request_redraw();
  }
}

void DebugPaneService::Toggle() {
  if (state_.debug_pane.visible) {
    Close();
  } else {
    Activate(state_.debug_pane.mode);
  }
}

void DebugPaneService::OpenOnStop() {
  if (state_.debug_pane.visible) {
    return;
  }
  // Open on the Variables inspector by default — inspecting locals is the common
  // first action at a stop; the user can switch to Call Stack when needed.
  Activate(DebugPaneMode::Variables);
}

void DebugPaneService::SetWidth(float width) {
  if (state_.debug_pane.width == width) {
    return;
  }
  state_.debug_pane.width = width;
  if (operations_.mark_layout_dirty) {
    operations_.mark_layout_dirty();
  }
  if (operations_.request_redraw) {
    operations_.request_redraw();
  }
}

}  // namespace microide::workspace
