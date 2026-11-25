#include "workspace/debug/DebugPaneService.h"

#include <utility>
#include "util/InvokeIfSet.h"

namespace microide::workspace {

DebugPaneService::DebugPaneService(ProjectWorkspaceState& state, Operations operations)
    : state_(state), operations_(std::move(operations)) {}

void DebugPaneService::Activate(DebugPaneMode mode) {
  DebugPaneState& pane = state_.debug_pane;
  const bool was_visible = pane.visible;
  pane.visible = true;
  pane.mode = mode;
  state_.surface.focus = FocusTarget::DebugPane;
  if (!was_visible && operations_.note_layout_inputs_changed) {
    operations_.note_layout_inputs_changed();
  }
  util::InvokeIfSet(operations_.request_frame);
}

void DebugPaneService::ShowMode(DebugPaneMode mode) { Activate(mode); }

void DebugPaneService::Close() {
  DebugPaneState& pane = state_.debug_pane;
  if (!pane.visible) {
    return;
  }
  pane.visible = false;
  if (state_.surface.focus == FocusTarget::DebugPane) {
    state_.surface.focus = FocusTarget::Editor;
  }
  util::InvokeIfSet(operations_.note_layout_inputs_changed);
  util::InvokeIfSet(operations_.request_frame);
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
  Activate(DebugPaneMode::Variables);
}

}
