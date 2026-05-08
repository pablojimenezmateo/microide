#include "workspace/WorkspaceShell.h"

namespace microide::workspace {

void WorkspaceShell::ConsumeLspCallbacks() {
  EnsureProjectLspManager(context_.current_project_state).DrainCallbacks();
  for (const auto& entry : context_.project_catalog.entries) {
    if (entry != nullptr) {
      EnsureProjectLspManager(*entry).DrainCallbacks();
    }
  }
  RequestFullRedraw();
}

bool WorkspaceShell::RequestInlineCompletion(std::string* error_message) {
  if (error_message != nullptr) {
    *error_message = "Inline completion is retired";
  }
  DismissInlineCompletion();
  return false;
}

bool WorkspaceShell::AcceptInlineCompletion() {
  DismissInlineCompletion();
  return false;
}

void WorkspaceShell::DismissInlineCompletion() {
  context_.current_project_state.inline_completion = InlineCompletionState{};
  RequestFocusedEditorRedraw();
}

}  // namespace microide::workspace
