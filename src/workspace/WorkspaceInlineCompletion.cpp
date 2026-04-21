#include "workspace/WorkspaceInlineCompletion.h"

namespace microide::workspace {

InlineCompletionRegistry::InlineCompletionRegistry() = default;
InlineCompletionRegistry::~InlineCompletionRegistry() = default;

void InlineCompletionRegistry::AddCompletion(const InlineCompletion& completion) {
  completions_.push_back(completion);
}

void InlineCompletionRegistry::AddAction(const InlineAction& action) {
  actions_.push_back(action);
}

std::vector<InlineCompletion> InlineCompletionRegistry::GetCompletions(int line,
                                                                       int column) const {
  std::vector<InlineCompletion> result;
  for (const auto& completion : completions_) {
    if (completion.start_line == line && completion.start_column <= column) {
      result.push_back(completion);
    }
  }
  return result;
}

std::vector<InlineAction> InlineCompletionRegistry::GetActions(int line) const {
  std::vector<InlineAction> result;
  for (const auto& action : actions_) {
    if (action.line == line) {
      result.push_back(action);
    }
  }
  return result;
}

void InlineCompletionRegistry::Clear() {
  completions_.clear();
  actions_.clear();
}

}  // namespace microide::workspace
