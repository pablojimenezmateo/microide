#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace microide::workspace {

// Context item: a piece of information to include in AI requests.
enum class ContextItemType {
  CurrentFile,
  Selection,
  Diagnostics,
  SearchResults,
  GitDiff,
  ProjectStructure,
  RecentFiles,
  RelatedFiles,
};

struct ContextItem {
  std::string id;
  ContextItemType type;
  std::string content;
  std::int64_t size_bytes = 0;
};

// Context collection policy: limits to prevent overwhelming AI with context.
struct ContextPolicy {
  std::int64_t max_total_bytes = 100 * 1024;  // 100KB default
  int max_files = 10;
  bool include_diagnostics = true;
  bool include_git_context = true;
  bool include_project_structure = false;
};

// AI context: bounded context collection and management.
class AiContextManager {
 public:
  AiContextManager();
  ~AiContextManager();

  // Set context policy.
  void SetPolicy(const ContextPolicy& policy);

  // Add a context item.
  void AddItem(const ContextItem& item);

  // Get collected context (respects policy limits).
  std::vector<ContextItem> GetContext() const;

  // Get current context size in bytes.
  std::int64_t GetContextSize() const;

  // Clear context.
  void Clear();

  // Set cancellation callback.
  using OnCancel = std::function<void()>;
  void SetCancelCallback(OnCancel callback) { on_cancel_ = std::move(callback); }

  // Request cancellation.
  void RequestCancel() {
    if (on_cancel_) on_cancel_();
  }

 private:
  std::vector<ContextItem> items_;
  ContextPolicy policy_;
  OnCancel on_cancel_;
};

}  // namespace microide::workspace
