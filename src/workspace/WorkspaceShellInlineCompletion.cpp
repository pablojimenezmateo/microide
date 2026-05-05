#include "workspace/WorkspaceShell.h"

#include <algorithm>
#include <sstream>

namespace microide::workspace {

namespace {

std::string GenerateRuntimeMessageId(std::string_view prefix) {
  static std::uint64_t counter = 1;
  return std::string(prefix) + "-" + std::to_string(counter++);
}

}  // namespace

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
    error_message->clear();
  }
  editor::TextViewport* viewport = ActiveEditableViewport();
  if (viewport == nullptr) {
    return false;
  }
  const std::vector<std::string> provider_ids =
      ai_provider_runtime_service_.ProviderIdsForWorkflow(AiRuntimeWorkflow::InlineCompletion);
  if (provider_ids.empty()) {
    if (error_message != nullptr) {
      *error_message = "No inline completion provider runtime is available";
    }
    return false;
  }
  const std::string provider_id = provider_ids.front();
  const AiProviderSpec* provider = ai_provider_registry_.FindProvider(provider_id);
  if (provider == nullptr) {
    if (error_message != nullptr) {
      *error_message = "Unknown inline completion provider runtime";
    }
    return false;
  }

  inline_completion_registry_.Clear();
  context_.current_project_state.inline_completion.visible = false;
  context_.current_project_state.inline_completion.request_in_flight = true;
  context_.current_project_state.inline_completion.start_line = viewport->cursor_line();
  context_.current_project_state.inline_completion.start_column = viewport->cursor_column();
  context_.current_project_state.inline_completion.provider_id = provider->id;
  context_.current_project_state.inline_completion.text.clear();
  context_.current_project_state.inline_completion.error.clear();
  context_.current_project_state.inline_completion.pending_provider_id.clear();
  context_.current_project_state.inline_completion.pending_request_id.clear();

  std::ostringstream prompt;
  prompt << "Complete the code at line " << (viewport->cursor_line() + 1) << ", column "
         << (viewport->cursor_column() + 1) << ". Return only the completion text.\n\n";
  const auto& lines = viewport->lines();
  for (std::size_t i = 0; i < lines.size(); ++i) {
    prompt << lines[i] << '\n';
  }
  const std::string request_id = GenerateRuntimeMessageId("runtime-inline");
  context_.current_project_state.inline_completion.pending_provider_id = provider->id;
  context_.current_project_state.inline_completion.pending_request_id = request_id;
  if (!ai_provider_runtime_service_.StartRequest(
          AiRuntimeRequest{
              .workflow = AiRuntimeWorkflow::InlineCompletion,
              .provider_id = provider->id,
              .request_id = request_id,
              .messages = {AiRuntimeMessage{.role = "user", .content = prompt.str()}},
              .model_id = {},
              .system_prompt = {},
              .tool_mode = "no_tools",
              .tools = {},
          },
          AiRuntimeLaunchContext{
              .cwd = context_.current_project_state.root,
              .secret = ResolveProviderApiKey(*provider),
          },
          error_message)) {
    context_.current_project_state.inline_completion.request_in_flight = false;
    context_.current_project_state.inline_completion.pending_provider_id.clear();
    context_.current_project_state.inline_completion.pending_request_id.clear();
    if (error_message != nullptr) {
      *error_message = error_message->empty()
                           ? "Failed to send inline completion request to provider runtime"
                           : *error_message;
    }
    return false;
  }
  return true;
}

bool WorkspaceShell::AcceptInlineCompletion() {
  if (!context_.current_project_state.inline_completion.visible ||
      context_.current_project_state.inline_completion.text.empty()) {
    return false;
  }
  editor::TextViewport* viewport = ActiveEditableViewport();
  if (viewport == nullptr) {
    return false;
  }

  const bool was_dirty = viewport->dirty();
  const std::size_t cursor_before_line = viewport->cursor_line();
  std::vector<std::string> before_lines;
  std::optional<editor::SelectionRange> selection_before;
  std::optional<editor::TextPosition> cursor_before;
  if (auto* merge_tab = ActiveMergeTab();
      merge_tab != nullptr && viewport == &merge_tab->result_viewport) {
    before_lines = viewport->lines();
    selection_before = viewport->selection_range();
    cursor_before = editor::TextPosition{viewport->cursor_line(), viewport->cursor_column()};
  }
  viewport->ReplaceRange(
      editor::SelectionRange{
          .start = editor::TextPosition{
              context_.current_project_state.inline_completion.start_line,
              context_.current_project_state.inline_completion.start_column,
          },
          .end = editor::TextPosition{viewport->cursor_line(), viewport->cursor_column()},
      },
      context_.current_project_state.inline_completion.text);
  if (auto* compare_tab = ActiveCompareTab();
      compare_tab != nullptr && viewport == &compare_tab->right_viewport) {
    RefreshCompareTabDerivedState(*compare_tab);
    SyncCompareSelectionFromViewport(*compare_tab, true);
  }
  if (auto* merge_tab = ActiveMergeTab();
      merge_tab != nullptr && viewport == &merge_tab->result_viewport) {
    UpdateMergeTrackingAfterViewportEdit(*merge_tab, before_lines, selection_before, *cursor_before);
  }
  ResetCaretBlink();
  RequestActiveEditableLastChangeRedraw();
  if (viewport->dirty() != was_dirty) {
    RequestActiveEditableBlameNeighborhoodRedraw(cursor_before_line, viewport->cursor_line());
    RequestTabStripRedraw();
  }
  DismissInlineCompletion();
  RequestFocusedEditorRedraw();
  return true;
}

void WorkspaceShell::DismissInlineCompletion() {
  context_.current_project_state.inline_completion = InlineCompletionState{};
  RequestFocusedEditorRedraw();
}

}  // namespace microide::workspace
