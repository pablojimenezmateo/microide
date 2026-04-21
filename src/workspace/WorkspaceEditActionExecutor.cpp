#include "workspace/WorkspaceActionCoordinator.h"

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "workspace/WorkspaceActionRequests.h"

namespace microide::workspace {

ActionCoordinator::DispatchResult ActionCoordinator::ExecuteEdit(ActionId id,
                                                                 const std::vector<std::string>& args,
                                                                 ActionSource source,
                                                                 std::string* rejection_feedback) {
  (void)source;
  const auto reject = [&](std::string feedback) {
    if (rejection_feedback != nullptr) {
      *rejection_feedback = std::move(feedback);
    }
    return DispatchResult::Rejected;
  };

  switch (id) {
    case ActionId::Completion: {
      std::string error_message;
      if (!context_.ShowCompletionOverlay(&error_message)) {
        return reject(error_message.empty() ? "No completions available" : error_message);
      }
      return DispatchResult::Handled;
    }
    case ActionId::CodeActions: {
      std::string error_message;
      if (!context_.ShowCodeActionsOverlay(&error_message)) {
        return reject(error_message.empty() ? "No code actions available" : error_message);
      }
      return DispatchResult::Handled;
    }
    case ActionId::Goto:
    case ActionId::Jump: {
      if (context_.ActiveTabIsCompare() || context_.ActiveTabIsMerge()) {
        return DispatchResult::Handled;
      }
      const std::optional<LineNavigationRequest> request =
          BuildLineNavigationRequest(args, id == ActionId::Jump);
      if (!request.has_value()) {
        return DispatchResult::Handled;
      }

      if (id == ActionId::Goto && request->requested_line == 0) {
        return DispatchResult::Handled;
      }

      context_.ExecuteLineNavigation(*request, id == ActionId::Jump);
      return DispatchResult::Handled;
    }
    case ActionId::SelectAll:
      context_.SelectAll();
      return DispatchResult::Handled;
    case ActionId::Undo:
      context_.Undo();
      return DispatchResult::Handled;
    case ActionId::Redo:
      context_.Redo();
      return DispatchResult::Handled;
    case ActionId::CopySelection: {
      const std::string text = context_.CopySelectionText();
      if (!text.empty()) {
        context_.WriteClipboardText(text);
        context_.WritePrimarySelectionText(text);
      }
      return DispatchResult::Handled;
    }
    case ActionId::CopyLastTerminalCommand: {
      const std::optional<std::string> text = context_.LastTerminalCommandText();
      if (text.has_value()) {
        context_.WriteClipboardText(*text);
      }
      return DispatchResult::Handled;
    }
    case ActionId::CopySelectionWithContext: {
      const std::optional<std::string> text = context_.SelectionTextWithContext();
      if (text.has_value()) {
        context_.WriteClipboardText(*text);
        context_.WritePrimarySelectionText(*text);
      }
      return DispatchResult::Handled;
    }
    case ActionId::CutSelection: {
      context_.CutSelection();
      return DispatchResult::Handled;
    }
    case ActionId::PasteClipboard: {
      context_.PasteClipboard();
      return DispatchResult::Handled;
    }
    case ActionId::InlineCompletion: {
      std::string error_message;
      if (!context_.RequestInlineCompletion(&error_message)) {
        return reject(error_message.empty() ? "Inline completion unavailable" : error_message);
      }
      return DispatchResult::Handled;
    }
    case ActionId::TestsDiscover: {
      std::string error_message;
      if (!context_.DiscoverTestsForActiveBuffer(&error_message)) {
        return reject(error_message.empty() ? "Test discovery failed" : error_message);
      }
      return DispatchResult::Handled;
    }
    default:
      return DispatchResult::Unhandled;
  }
}

}  // namespace microide::workspace
