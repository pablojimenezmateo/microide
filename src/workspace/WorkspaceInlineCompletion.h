#pragma once

#include <string>
#include <vector>

namespace microide::workspace {

// Inline completion: AI-powered code completion suggestion.
struct InlineCompletion {
  std::string id;
  std::string text;  // suggested completion text
  int start_line = 0;
  int start_column = 0;
  std::string provider_id;  // which AI provider generated this
};

// Inline action: explain, edit, fix, etc.
enum class InlineActionType {
  Explain,
  Edit,
  Fix,
  Refactor,
  Document,
};

struct InlineAction {
  std::string id;
  InlineActionType type;
  std::string label;
  std::string description;
  int line = 0;
  std::string provider_id;
};

// Inline completion and action registry.
class InlineCompletionRegistry {
 public:
  InlineCompletionRegistry();
  ~InlineCompletionRegistry();

  // Add inline completion suggestion.
  void AddCompletion(const InlineCompletion& completion);

  // Add inline action.
  void AddAction(const InlineAction& action);

  // Get completions for a location.
  std::vector<InlineCompletion> GetCompletions(int line, int column) const;

  // Get actions for a line.
  std::vector<InlineAction> GetActions(int line) const;

  // Clear all.
  void Clear();

 private:
  std::vector<InlineCompletion> completions_;
  std::vector<InlineAction> actions_;
};

}  // namespace microide::workspace
