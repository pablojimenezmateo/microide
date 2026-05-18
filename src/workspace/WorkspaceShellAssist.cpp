#include "workspace/WorkspaceShell.h"

namespace microide::workspace {

bool WorkspaceShell::EditorSnippetsSettingEnabled() const {
  return assist_service_.EditorSnippetsSettingEnabled();
}

bool WorkspaceShell::ShowCompletionOverlay(std::string* error_message) {
  return assist_service_.ShowCompletionOverlay(error_message);
}

bool WorkspaceShell::ShowInsertSnippetOverlay(std::string* error_message) {
  return assist_service_.ShowInsertSnippetOverlay(error_message);
}

bool WorkspaceShell::TrySnippetTabInEditor(bool shift_tab) {
  return assist_service_.TrySnippetTabInEditor(shift_tab);
}

bool WorkspaceShell::TrySnippetEscapeInEditor() {
  return assist_service_.TrySnippetEscapeInEditor();
}

void WorkspaceShell::NotifySnippetSessionCaretMoved() {
  assist_service_.NotifySnippetSessionCaretMoved();
}

void WorkspaceShell::ClearActiveSnippetSessionAfterUndo() {
  assist_service_.ClearActiveSnippetSessionAfterUndo();
}

bool WorkspaceShell::TrySnippetInsertTextInEditor(editor::TextViewport* viewport,
                                                  std::string_view text) {
  return assist_service_.TrySnippetInsertTextInEditor(viewport, text);
}

bool WorkspaceShell::TrySnippetBackspaceInEditor(editor::TextViewport* viewport) {
  return assist_service_.TrySnippetBackspaceInEditor(viewport);
}

bool WorkspaceShell::TrySnippetDeleteForwardInEditor(editor::TextViewport* viewport) {
  return assist_service_.TrySnippetDeleteForwardInEditor(viewport);
}

bool WorkspaceShell::ShowCodeActionsOverlay(std::string* error_message) {
  return assist_service_.ShowCodeActionsOverlay(error_message);
}

bool WorkspaceShell::GoToLspDefinition(std::string* error_message) {
  return assist_service_.GoToLspDefinition(error_message);
}

bool WorkspaceShell::FindLspReferences(std::string* error_message) {
  return assist_service_.FindLspReferences(error_message);
}

bool WorkspaceShell::ApplySelectedCompletion() {
  return assist_service_.ApplySelectedCompletion();
}

bool WorkspaceShell::ExecuteSelectedCodeAction() {
  return assist_service_.ExecuteSelectedCodeAction();
}

}  // namespace microide::workspace
