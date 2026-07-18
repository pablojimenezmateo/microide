#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "compare/BranchReviewStateService.h"
#include "workspace/GitSidebarCommandCenter.h"
#include "workspace/WorkspaceSidebarState.h"

namespace microide::workspace {

enum class GitSidebarLineKind {
  Header,
  Directory,
  Entry,
  Empty,
};

struct GitSidebarLineSpec {
  GitSidebarLineKind kind = GitSidebarLineKind::Empty;
  GitSidebarEntry::Section section = GitSidebarEntry::Section::Changed;
  std::string label;
  // Render-ready primary text (see GitSidebarLine::display_label): entry rows carry
  // the "[<marker>] " branch-review prefix; other kinds equal `label`.
  std::string display_label;
  std::string tree_node_key;
  bool expanded = false;
  int depth = 0;
  int entry_index = -1;
};

struct GitSidebarEntryTextModel {
  std::string primary_label;
  std::string secondary_label;
};

std::vector<GitSidebarLineSpec> BuildGitSidebarLineSpecs(
    const GitSidebarViewModel& view_model,
    const std::unordered_set<std::string>* collapsed_directory_keys = nullptr);
// Flatten the git view model into the render-ready `GitSidebarLine` rows the shell
// stores and the render TU draws. Single owner of the spec->line conversion so the
// render path, hit-testing, and selection all share one build.
std::vector<GitSidebarLine> BuildGitSidebarLines(
    const GitSidebarViewModel& view_model,
    const std::unordered_set<std::string>* collapsed_directory_keys = nullptr);
std::optional<std::size_t> FindSelectedGitSidebarLineIndex(
    const std::vector<GitSidebarLineSpec>& lines,
    std::size_t selected_entry_index);
std::optional<std::size_t> FindSelectedGitSidebarLineIndex(
    const std::vector<GitSidebarLine>& lines,
    std::size_t selected_entry_index);
GitSidebarEntryTextModel BuildGitSidebarEntryTextModel(const std::filesystem::path& relative_path,
                                                       bool staged);

// The git view model plus its flattened rows, built together from one walk of state.
struct GitSidebarPresentation {
  GitSidebarViewModel view_model;
  std::vector<GitSidebarLine> lines;
};

// Revision-exact memo shared by the render path (PrepareFrameOnce) and hit-testing.
// Rebuilds only when a captured key of every git-state / branch-review input that
// the view model or lines depend on changes; pure hover/scroll repaints (no git
// state change) reuse the prior build. The key is compared field-by-field (never
// hashed), so it can never false-hit and show stale git status. The returned
// reference is valid until the next call on the same thread.
const GitSidebarPresentation& CachedGitSidebarPresentation(
    const GitSidebarState& git,
    const std::filesystem::path& root,
    const compare::BranchReviewStateService& branch_review);

// Testing hooks: reset the thread-local cache and read hit/miss counters so tests
// can assert unchanged state hits and every relevant mutation misses.
void ResetGitSidebarPresentationCacheForTesting();
std::uint64_t GitSidebarPresentationCacheHitsForTesting();
std::uint64_t GitSidebarPresentationCacheMissesForTesting();

}  // namespace microide::workspace
