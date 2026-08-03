#include "workspace/git/ReviewTabPlan.h"

#include <algorithm>
#include <set>

namespace microide::workspace {

ReviewTabPlan ComputeReviewTabPlan(std::span<const ReviewTabRef> existing,
                                   std::span<const std::filesystem::path> targets) {
  ReviewTabPlan plan;

  // Paths of tabs already open for this session, for fast "is this target
  // already open?" tests.
  std::set<std::filesystem::path> existing_paths;
  for (const ReviewTabRef& tab : existing) {
    existing_paths.insert(tab.path);
  }

  // Targets, both as an ordered de-duplicated open/reuse list and as a set for
  // the staleness test below.
  std::set<std::filesystem::path> target_paths;
  std::set<std::filesystem::path> seen_targets;
  for (const std::filesystem::path& target : targets) {
    target_paths.insert(target);
    if (!seen_targets.insert(target).second) {
      continue;  // collapse duplicate targets
    }
    if (existing_paths.count(target) != 0) {
      plan.reused.push_back(target);
    } else {
      plan.to_open.push_back(target);
    }
  }

  for (const ReviewTabRef& tab : existing) {
    if (target_paths.count(tab.path) != 0) {
      continue;  // still wanted
    }
    if (tab.dirty) {
      plan.kept_dirty.push_back(tab.path);
    } else {
      plan.to_close.push_back(tab.index);
    }
  }

  // Descending so callers can erase/close without shifting earlier indices.
  std::sort(plan.to_close.begin(), plan.to_close.end(), std::greater<>());
  plan.to_close.erase(std::unique(plan.to_close.begin(), plan.to_close.end()), plan.to_close.end());

  return plan;
}

}  // namespace microide::workspace
