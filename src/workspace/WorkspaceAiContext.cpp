#include "workspace/WorkspaceAiContext.h"

#include <algorithm>

namespace microide::workspace {

AiContextManager::AiContextManager() = default;
AiContextManager::~AiContextManager() = default;

void AiContextManager::SetPolicy(const ContextPolicy& policy) { policy_ = policy; }

void AiContextManager::AddItem(const ContextItem& item) {
  // Check if we're within limits before adding
  std::int64_t new_size = GetContextSize() + item.size_bytes;
  if (new_size <= policy_.max_total_bytes) {
    items_.push_back(item);
  }
}

std::vector<ContextItem> AiContextManager::GetContext() const {
  std::vector<ContextItem> result;
  std::int64_t total = 0;

  // Sort by type priority and add items respecting limits
  auto sorted = items_;
  std::stable_sort(sorted.begin(), sorted.end(),
                   [](const ContextItem& a, const ContextItem& b) {
                     // Prioritize current file and selection
                     const auto priority = [](ContextItemType t) {
                       switch (t) {
                         case ContextItemType::CurrentFile: return 0;
                         case ContextItemType::Selection: return 1;
                         case ContextItemType::Diagnostics: return 2;
                         case ContextItemType::GitDiff: return 3;
                         default: return 10;
                       }
                     };
                     return priority(a.type) < priority(b.type);
                   });

  int file_count = 0;
  for (const auto& item : sorted) {
    if (total + item.size_bytes > policy_.max_total_bytes) {
      break;
    }
    if (item.type == ContextItemType::CurrentFile ||
        item.type == ContextItemType::RelatedFiles) {
      if (file_count >= policy_.max_files) {
        continue;
      }
      file_count++;
    }
    result.push_back(item);
    total += item.size_bytes;
  }

  return result;
}

std::int64_t AiContextManager::GetContextSize() const {
  std::int64_t total = 0;
  for (const auto& item : items_) {
    total += item.size_bytes;
  }
  return total;
}

void AiContextManager::Clear() { items_.clear(); }

}  // namespace microide::workspace
