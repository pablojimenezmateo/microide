#include "workspace/git/ReviewTabPlan.h"

#include <algorithm>
#include <set>

namespace microide::workspace {

ReviewTabPlan ComputeReviewTabPlan(std::span<const ReviewTabRef> existing,
                                   std::span<const std::filesystem::path> targets) {
  ReviewTabPlan plan;

  std::set<std::filesystem::path> existing_paths;
  for (const ReviewTabRef& tab : existing) {
    existing_paths.insert(tab.path);
  }

  std::set<std::filesystem::path> target_paths;
  std::set<std::filesystem::path> seen_targets;
  for (const std::filesystem::path& target : targets) {
    target_paths.insert(target);
    if (!seen_targets.insert(target).second) {
      continue;
    }
    if (existing_paths.count(target) != 0) {
      plan.reused.push_back(target);
    } else {
      plan.to_open.push_back(target);
    }
  }

  for (const ReviewTabRef& tab : existing) {
    if (target_paths.count(tab.path) != 0) {
      continue;
    }
    if (tab.dirty) {
      plan.kept_dirty.push_back(tab.path);
    } else {
      plan.to_close.push_back(tab.index);
    }
  }

  std::sort(plan.to_close.begin(), plan.to_close.end(), std::greater<>());
  plan.to_close.erase(std::unique(plan.to_close.begin(), plan.to_close.end()), plan.to_close.end());

  return plan;
}

}
