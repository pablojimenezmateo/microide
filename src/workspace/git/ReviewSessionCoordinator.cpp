#include "workspace/git/ReviewSessionCoordinator.h"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "project/GitCompareService.h"
#include "project/GitStatusService.h"
#include "util/PathMatch.h"
#include "workspace/git/ReviewTabPlan.h"

namespace microide::workspace {

namespace {

// Project-relative, forward-slashed path for compact summaries; falls back to the
// generic string when the file is outside the root. Purely lexical: this is a display
// label, and std::filesystem::relative touches the filesystem (canonicalization) so it
// can fail/slow on deleted paths, symlinks, or inaccessible mounts (TD-2026-07-17A-010).
std::string DisplayPath(const std::filesystem::path& path, const std::filesystem::path& root) {
  if (std::optional<std::string> relative = util::RelativePathWithin(path, root)) {
    return std::move(*relative);
  }
  return path.generic_string();
}

std::string BuildReviewSummary(std::string_view verb,
                               const ReviewTabPlan& plan,
                               const std::vector<std::filesystem::path>& opened,
                               std::size_t not_opened,
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
  if (not_opened > 0) {
    message += ", " + std::to_string(not_opened) + " more not opened (review cap reached)";
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

  const std::vector<TabEntry>& tabs = state_.focused_group().open_tabs;
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

  // Cap the number of tabs a single review opens: past the cap, stop building
  // compare/merge models entirely and report the remainder as truncated instead of
  // spending the shell thread opening thousands of tabs. Only the *attempts* count
  // toward the cap, so a run that fails to open a few files still reaches the ceiling.
  std::vector<std::filesystem::path> opened;
  const std::size_t open_budget =
      std::min(plan.to_open.size(), kMaxReviewSessionOpenTabs);
  opened.reserve(open_budget);

  std::size_t attempted = 0;
  for (const std::filesystem::path& path : plan.to_open) {
    if (attempted >= kMaxReviewSessionOpenTabs) {
      break;
    }
    ++attempted;
    if (open_one(path)) {
      opened.push_back(path);
    }
  }
  const std::size_t not_opened = plan.to_open.size() - attempted;

  ReviewOpenOutcome outcome;
  outcome.ok = true;
  outcome.message =
      BuildReviewSummary(verb, plan, opened, not_opened, empty_message, state_.root);
  return outcome;
}

ReviewOpenOutcome ReviewSessionCoordinator::Dispatch(
    std::function<void()> gather,
    std::function<ReviewOpenOutcome(ReviewSessionCoordinator&)> apply) {
  if (!operations_.run_off_shell_thread) {
    gather();
    return apply(*this);
  }
  operations_.run_off_shell_thread(
      std::move(gather),
      [state = &state_, compare_merge = compare_merge_, operations = operations_,
       apply = std::move(apply)]() {
        ReviewSessionCoordinator coordinator(*state, compare_merge, operations);
        ReviewOpenOutcome outcome = apply(coordinator);
        if (operations.report_outcome) {
          operations.report_outcome(std::move(outcome));
        }
      });
  return ReviewOpenOutcome{.ok = true, .message = {}, .pending = true};
}

namespace {

// The files a review prefetches blobs for, gathered before the shell knows which of
// them already have a tab. Twice the open cap covers a rerun that reuses up to a
// cap's worth of tabs; anything past it is read per file when opened.
std::vector<std::filesystem::path> PrefetchSet(const std::vector<std::filesystem::path>& targets) {
  const std::size_t count =
      std::min(targets.size(), ReviewSessionCoordinator::kMaxReviewSessionOpenTabs * 2);
  return {targets.begin(), targets.begin() + static_cast<std::ptrdiff_t>(count)};
}

struct Gathered {
  // nullopt: git could not be asked (not installed, failed, truncated output).
  std::optional<std::vector<std::filesystem::path>> targets;
  project::GitRevisionBlobCache prefetched;
  // review-branch: the resolved base, or an error to report instead.
  std::string ref;
  std::string label;
  std::string error;
  // review-commit: the commit's changed files, project-relative.
  std::vector<std::filesystem::path> review_files;
};

}  // namespace

ReviewOpenOutcome ReviewSessionCoordinator::OpenConflictReview() {
  const std::filesystem::path root = state_.root;
  const platform::ProcessLauncher* launcher = &state_.launcher();
  auto gathered = std::make_shared<Gathered>();
  return Dispatch(
      [root, launcher, gathered] {
        const std::optional<std::vector<project::GitWorkingTreeEntry>> entries =
            project::CollectGitWorkingTreeEntries(root, *launcher);
        if (!entries.has_value()) {
          return;
        }
        std::vector<std::filesystem::path> targets;
        for (const project::GitWorkingTreeEntry& entry : *entries) {
          if (entry.conflicted) {
            targets.push_back((root / entry.relative_path).lexically_normal());
          }
        }
        // Three index stages per conflicted file -- the worst per-file spawn count
        // in the app before the bulk read.
        gathered->prefetched.Prefetch(root, *launcher, {":1", ":2", ":3"}, PrefetchSet(targets));
        gathered->targets = std::move(targets);
      },
      [gathered](ReviewSessionCoordinator& self) -> ReviewOpenOutcome {
        if (!gathered->targets.has_value()) {
          // git could not be asked -- it is not installed, it failed, or its output
          // was truncated. Saying "no conflicts to review" here would be a claim
          // about the user's repository that nothing established, and the one they
          // would act on by considering the merge finished.
          return ReviewOpenOutcome{.ok = false,
                                   .message = "Could not read the working tree from git"};
        }
        return self.RunReviewSession(
            "review-conflicts", *gathered->targets,
            [](const TabEntry& tab) -> std::optional<std::filesystem::path> {
              // Only git-conflict merge tabs (3 stages of the same file) -- never a
              // manual `merge` editor tab whose three inputs are distinct files.
              if (tab.kind == TabEntry::Kind::Merge && tab.merge.has_value() &&
                  tab.merge->incoming_path == tab.merge->output_path &&
                  tab.merge->current_path == tab.merge->output_path) {
                return tab.merge->output_path;
              }
              return std::nullopt;
            },
            [&self, gathered](const std::filesystem::path& path) {
              return self.compare_merge_.OpenGitConflictMerge(path, &gathered->prefetched);
            },
            "no merge conflicts");
      });
}

ReviewOpenOutcome ReviewSessionCoordinator::OpenBranchReview(const std::string& ref_arg) {
  const std::filesystem::path root = state_.root;
  const platform::ProcessLauncher* launcher = &state_.launcher();
  auto gathered = std::make_shared<Gathered>();
  return Dispatch(
      [root, launcher, gathered, ref_arg] {
        gathered->ref = ref_arg;
        gathered->label = ref_arg;
        if (ref_arg.empty() || ref_arg == "origin") {
          const std::optional<project::GitBranchReference> base =
              project::ResolveGitBaseReference(root, *launcher);
          if (!base.has_value()) {
            gathered->error = "review-branch: no base branch found (pass an explicit ref)";
            return;
          }
          gathered->ref = base->ref;
          gathered->label = base->label;
        }
        std::vector<std::filesystem::path> targets;
        for (const project::GitBranchFileEntry& entry :
             project::CollectGitWorkingTreeDiffFiles(root, *launcher, gathered->ref)) {
          targets.push_back((root / entry.relative_path).lexically_normal());
        }
        // Only the left side is a revision; the right is the working tree on disk.
        gathered->prefetched.Prefetch(root, *launcher, {gathered->ref}, PrefetchSet(targets));
        gathered->targets = std::move(targets);
      },
      [gathered](ReviewSessionCoordinator& self) -> ReviewOpenOutcome {
        if (!gathered->error.empty()) {
          return ReviewOpenOutcome{.ok = false, .message = gathered->error};
        }
        const std::string ref = gathered->ref;
        const std::string label = gathered->label;
        return self.RunReviewSession(
            "review-branch " + label, gathered->targets.value_or(std::vector<std::filesystem::path>{}),
            [ref](const TabEntry& tab) -> std::optional<std::filesystem::path> {
              if (tab.kind == TabEntry::Kind::Compare && tab.compare.has_value() &&
                  tab.compare->commit_hash == ref && tab.compare->right_ref == "WORKTREE") {
                return tab.compare->path;
              }
              return std::nullopt;
            },
            [&self, gathered, ref, label](const std::filesystem::path& path) {
              return self.compare_merge_.OpenWorkingTreeComparison(path, ref, label,
                                                                   &gathered->prefetched);
            },
            "no differences");
      });
}

ReviewOpenOutcome ReviewSessionCoordinator::OpenCommitReview(const std::string& ref_arg) {
  const std::filesystem::path root = state_.root;
  const platform::ProcessLauncher* launcher = &state_.launcher();
  const std::string ref = ref_arg.empty() ? std::string("HEAD") : ref_arg;
  const std::string left_ref = ref + "~1";
  const std::string right_ref = ref;
  auto gathered = std::make_shared<Gathered>();
  return Dispatch(
      [root, launcher, gathered, ref, left_ref, right_ref] {
        // The commit's own changed files, project-relative. This is BOTH the set of
        // tabs to open and the review file list each of those tabs navigates --
        // deriving the latter inside the open would spend a `git diff` spawn per
        // tab to answer the same question, and would answer it with
        // `<commit>~1...HEAD` (everything since the commit) instead of the commit.
        gathered->review_files = project::CollectGitCommitChangedFiles(root, *launcher, ref);
        std::vector<std::filesystem::path> targets;
        targets.reserve(gathered->review_files.size());
        for (const std::filesystem::path& relative : gathered->review_files) {
          targets.push_back((root / relative).lexically_normal());
        }
        gathered->prefetched.Prefetch(root, *launcher, {left_ref, right_ref}, PrefetchSet(targets));
        gathered->targets = std::move(targets);
      },
      [gathered, ref, left_ref, right_ref](ReviewSessionCoordinator& self) -> ReviewOpenOutcome {
        return self.RunReviewSession(
            "review-commit " + ref, gathered->targets.value_or(std::vector<std::filesystem::path>{}),
            [left_ref, right_ref](const TabEntry& tab) -> std::optional<std::filesystem::path> {
              if (tab.kind == TabEntry::Kind::Compare && tab.compare.has_value() &&
                  tab.compare->commit_hash == left_ref && tab.compare->right_ref == right_ref) {
                return tab.compare->path;
              }
              return std::nullopt;
            },
            [&self, gathered, left_ref, right_ref](const std::filesystem::path& path) {
              return self.compare_merge_.OpenBranchHeadComparison(
                  path, left_ref, left_ref, right_ref, right_ref, &gathered->prefetched,
                  &gathered->review_files);
            },
            "no changes in commit");
      });
}

}  // namespace microide::workspace
