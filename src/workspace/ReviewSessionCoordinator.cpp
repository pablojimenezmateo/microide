#include "workspace/ReviewSessionCoordinator.h"

#include <string>
#include <system_error>
#include <utility>

#include "project/GitCompareService.h"
#include "project/GitStatusService.h"
#include "workspace/ReviewTabPlan.h"

namespace microide::workspace {

namespace {

// Project-relative, forward-slashed path for compact summaries; falls back to the
// generic string when the file is outside the root.
std::string DisplayPath(const std::filesystem::path& path, const std::filesystem::path& root) {
  std::error_code ec;
  const std::filesystem::path relative = std::filesystem::relative(path, root, ec);
  if (ec || relative.empty() || relative.native().rfind("..", 0) == 0) {
    return path.generic_string();
  }
  return relative.generic_string();
}

std::string BuildReviewSummary(std::string_view verb,
                               const ReviewTabPlan& plan,
                               const std::vector<std::filesystem::path>& opened,
                               std::string_view empty_message,
                               const std::filesystem::path& root) {
  std::string message(verb);
  if (opened.empty() && plan.reused.empty()) {
    message += ": ";
    message.append(empty_message);
    return message;
  }
  message += ": opened " + std::to_string(opened.size());
  message += ", reused " + std::to_string(plan.reused.size());
  message += ", closed " + std::to_string(plan.to_close.size());
  if (!plan.kept_dirty.empty()) {
    message += ", kept " + std::to_string(plan.kept_dirty.size()) + " dirty";
  }

  bool first = true;
  const auto append_file = [&](const std::filesystem::path& path) {
    message += first ? " — " : ", ";
    first = false;
    message += DisplayPath(path, root);
  };
  for (const std::filesystem::path& path : opened) {
    append_file(path);
  }
  for (const std::filesystem::path& path : plan.reused) {
    append_file(path);
  }
  return message;
}

}  // namespace

ReviewSessionCoordinator::ReviewSessionCoordinator(ProjectWorkspaceState& state,
                                                   CompareMergeService compare_merge,
                                                   Operations operations)
    : state_(state), compare_merge_(std::move(compare_merge)), operations_(std::move(operations)) {}

ReviewOpenOutcome ReviewSessionCoordinator::RunReviewSession(
    std::string_view verb,
    const std::vector<std::filesystem::path>& targets,
    const std::function<std::optional<std::filesystem::path>(const TabEntry&)>& scoped_path_of,
    const std::function<bool(const std::filesystem::path&)>& open_one,
    std::string_view empty_message) {
  // Always reveal Source Control so the review surface is in view, even when the
  // target set is empty (e.g. "no conflicts" still lands the user there).
  if (operations_.show_git_sidebar) {
    operations_.show_git_sidebar();
  }

  const std::vector<TabEntry>& tabs = state_.open_tabs;
  std::vector<ReviewTabRef> existing;
  existing.reserve(tabs.size());
  for (std::size_t i = 0; i < tabs.size(); ++i) {
    std::optional<std::filesystem::path> scoped = scoped_path_of(tabs[i]);
    if (!scoped.has_value()) {
      continue;
    }
    const bool dirty = operations_.tab_is_dirty && operations_.tab_is_dirty(i);
    existing.push_back(ReviewTabRef{std::move(*scoped), i, dirty});
  }

  const ReviewTabPlan plan = ComputeReviewTabPlan(existing, targets);

  // to_close holds only clean tabs (the planner routes dirty ones to kept_dirty),
  // so this never triggers the dirty-save prompt; indices are descending.
  if (!plan.to_close.empty() && operations_.request_close_tabs) {
    operations_.request_close_tabs(plan.to_close);
  }

  std::vector<std::filesystem::path> opened;
  opened.reserve(plan.to_open.size());
  for (const std::filesystem::path& path : plan.to_open) {
    if (open_one(path)) {
      opened.push_back(path);
    }
  }

  ReviewOpenOutcome outcome;
  outcome.ok = true;
  outcome.message = BuildReviewSummary(verb, plan, opened, empty_message, state_.root);
  return outcome;
}

ReviewOpenOutcome ReviewSessionCoordinator::OpenConflictReview() {
  const std::filesystem::path root = state_.root;

  std::vector<std::filesystem::path> targets;
  for (const project::GitWorkingTreeEntry& entry : project::CollectGitWorkingTreeEntries(root)) {
    if (!entry.conflicted) {
      continue;
    }
    targets.push_back((root / entry.relative_path).lexically_normal());
  }

  return RunReviewSession(
      "review-conflicts", targets,
      [](const TabEntry& tab) -> std::optional<std::filesystem::path> {
        // Only git-conflict merge tabs (3 stages of the same file) — never a
        // manual `merge` editor tab whose three inputs are distinct files.
        if (tab.kind == TabEntry::Kind::Merge && tab.merge.has_value() &&
            tab.merge->incoming_path == tab.merge->output_path &&
            tab.merge->current_path == tab.merge->output_path) {
          return tab.merge->output_path;
        }
        return std::nullopt;
      },
      [this](const std::filesystem::path& path) {
        return compare_merge_.OpenGitConflictMerge(path);
      },
      "no merge conflicts");
}

ReviewOpenOutcome ReviewSessionCoordinator::OpenBranchReview(const std::string& ref_arg) {
  const std::filesystem::path root = state_.root;

  std::string ref = ref_arg;
  std::string label = ref_arg;
  if (ref.empty() || ref == "origin") {
    const std::optional<project::GitBranchReference> base = project::ResolveGitBaseReference(root);
    if (!base.has_value()) {
      return {false, "review-branch: no base branch found (pass an explicit ref)"};
    }
    ref = base->ref;
    label = base->label;
  }

  std::vector<std::filesystem::path> targets;
  for (const project::GitBranchFileEntry& entry :
       project::CollectGitWorkingTreeDiffFiles(root, ref)) {
    targets.push_back((root / entry.relative_path).lexically_normal());
  }

  const std::string verb_label = "review-branch " + label;
  return RunReviewSession(
      verb_label, targets,
      [ref](const TabEntry& tab) -> std::optional<std::filesystem::path> {
        if (tab.kind == TabEntry::Kind::Compare && tab.compare.has_value() &&
            tab.compare->commit_hash == ref && tab.compare->right_ref == "WORKTREE") {
          return tab.compare->path;
        }
        return std::nullopt;
      },
      [this, ref, label](const std::filesystem::path& path) {
        return compare_merge_.OpenWorkingTreeComparison(path, ref, label);
      },
      "no differences");
}

ReviewOpenOutcome ReviewSessionCoordinator::OpenCommitReview(const std::string& ref_arg) {
  const std::filesystem::path root = state_.root;

  const std::string ref = ref_arg.empty() ? std::string("HEAD") : ref_arg;
  const std::string left_ref = ref + "~1";
  const std::string right_ref = ref;

  std::vector<std::filesystem::path> targets;
  for (const std::filesystem::path& relative : project::CollectGitCommitChangedFiles(root, ref)) {
    targets.push_back((root / relative).lexically_normal());
  }

  const std::string verb_label = "review-commit " + ref;
  return RunReviewSession(
      verb_label, targets,
      [left_ref, right_ref](const TabEntry& tab) -> std::optional<std::filesystem::path> {
        if (tab.kind == TabEntry::Kind::Compare && tab.compare.has_value() &&
            tab.compare->commit_hash == left_ref && tab.compare->right_ref == right_ref) {
          return tab.compare->path;
        }
        return std::nullopt;
      },
      [this, left_ref, right_ref](const std::filesystem::path& path) {
        return compare_merge_.OpenBranchHeadComparison(path, left_ref, left_ref, right_ref,
                                                       right_ref);
      },
      "no changes in commit");
}

}  // namespace microide::workspace
