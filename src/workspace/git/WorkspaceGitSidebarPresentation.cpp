#include "workspace/git/WorkspaceGitSidebarPresentation.h"

#include <algorithm>
#include <map>
#include <string>
#include <vector>

namespace microide::workspace {

namespace {

struct GitSidebarTreeNode {
  std::filesystem::path path;
  std::map<std::string, GitSidebarTreeNode> directories;
  std::vector<const GitSidebarRowViewModel*> files;
};

std::vector<std::string> ParentPathSegments(const std::filesystem::path& relative_path) {
  std::vector<std::string> segments;
  const std::filesystem::path parent = relative_path.parent_path().lexically_normal();
  for (const auto& segment : parent) {
    const std::string label = segment.generic_string();
    if (label.empty() || label == "." || label == "/") {
      continue;
    }
    segments.push_back(label);
  }
  return segments;
}

std::string FileLeafLabel(const GitSidebarRowViewModel& row) {
  if (!row.primary_label.empty()) {
    return row.primary_label;
  }
  const std::filesystem::path normalized = row.relative_path.lexically_normal();
  const std::string leaf = normalized.filename().string();
  if (!leaf.empty()) {
    return leaf;
  }
  return normalized.empty() ? "." : normalized.generic_string();
}

std::string DirectoryNodeKey(GitSidebarEntry::Section section, const std::filesystem::path& path) {
  const std::string path_key = path.lexically_normal().generic_string();
  return std::to_string(static_cast<int>(section)) + "|" + path_key;
}

void EmitGitSidebarTreeLines(const GitSidebarTreeNode& node,
                             GitSidebarEntry::Section section,
                             int depth,
                             const std::unordered_set<std::string>* collapsed_directory_keys,
                             std::vector<GitSidebarLineSpec>* lines) {
  for (const auto& [label, child] : node.directories) {
    const std::string node_key = DirectoryNodeKey(section, child.path);
    const bool collapsed =
        collapsed_directory_keys != nullptr &&
        collapsed_directory_keys->find(node_key) != collapsed_directory_keys->end();
    lines->push_back(GitSidebarLineSpec{
        .kind = GitSidebarLineKind::Directory,
        .section = section,
        .label = label,
        .display_label = label,
        .tree_node_key = node_key,
        .expanded = !collapsed,
        .depth = depth,
    });
    if (!collapsed) {
      EmitGitSidebarTreeLines(child, section, depth + 1, collapsed_directory_keys, lines);
    }
  }
  for (const GitSidebarRowViewModel* row : node.files) {
    if (row == nullptr) {
      continue;
    }
    std::string leaf = FileLeafLabel(*row);
    // Assemble the render-ready primary text once here (with the branch-review
    // "[<marker>] " prefix) so the sidebar render TU draws it directly instead of
    // rebuilding it per paint (TD-2026-07-17A-008).
    std::string display_label;
    if (!row->review_marker_label.empty()) {
      display_label.reserve(row->review_marker_label.size() + leaf.size() + 3);
      display_label += "[";
      display_label += row->review_marker_label;
      display_label += "] ";
      display_label += leaf;
    } else {
      display_label = leaf;
    }
    lines->push_back(GitSidebarLineSpec{
        .kind = GitSidebarLineKind::Entry,
        .section = section,
        .label = std::move(leaf),
        .display_label = std::move(display_label),
        .tree_node_key = {},
        .depth = depth,
        .entry_index = row->entry_index,
    });
  }
}

}  // namespace

GitSidebarEntryTextModel BuildGitSidebarEntryTextModel(const std::filesystem::path& relative_path,
                                                       bool staged) {
  const std::filesystem::path normalized_path = relative_path.lexically_normal();

  GitSidebarEntryTextModel model;
  model.primary_label = normalized_path.filename().string();
  if (model.primary_label.empty()) {
    model.primary_label = normalized_path.empty() ? "." : normalized_path.generic_string();
  }

  const std::filesystem::path parent = normalized_path.parent_path();
  if (!parent.empty() && parent != ".") {
    model.secondary_label = parent.generic_string();
  }
  if (staged) {
    if (!model.secondary_label.empty()) {
      model.secondary_label += "  ";
    }
    model.secondary_label += "[staged]";
  }
  return model;
}

std::vector<GitSidebarLineSpec> BuildGitSidebarLineSpecs(
    const GitSidebarViewModel& view_model,
    const std::unordered_set<std::string>* collapsed_directory_keys) {
  std::vector<GitSidebarLineSpec> lines;
  for (const GitSidebarSectionViewModel& section : view_model.sections) {
    if (section.show_header) {
      lines.push_back(GitSidebarLineSpec{
          .kind = GitSidebarLineKind::Header,
          .section = section.section,
          .label = section.header_label,
          .display_label = section.header_label,
          .tree_node_key = {},
      });
    }
    if (section.rows.empty()) {
      lines.push_back(GitSidebarLineSpec{
          .kind = GitSidebarLineKind::Empty,
          .section = section.section,
          .label = section.empty_label,
          .display_label = section.empty_label,
          .tree_node_key = {},
      });
      continue;
    }

    std::vector<const GitSidebarRowViewModel*> sorted_rows;
    sorted_rows.reserve(section.rows.size());
    for (const GitSidebarRowViewModel& row : section.rows) {
      sorted_rows.push_back(&row);
    }
    std::sort(sorted_rows.begin(), sorted_rows.end(),
              [](const GitSidebarRowViewModel* lhs, const GitSidebarRowViewModel* rhs) {
                if (lhs == nullptr || rhs == nullptr) {
                  return lhs != nullptr;
                }
                const std::string lhs_path = lhs->relative_path.lexically_normal().generic_string();
                const std::string rhs_path = rhs->relative_path.lexically_normal().generic_string();
                if (lhs_path != rhs_path) {
                  return lhs_path < rhs_path;
                }
                return lhs->entry_index < rhs->entry_index;
              });

    GitSidebarTreeNode root;
    for (const GitSidebarRowViewModel* row : sorted_rows) {
      if (row == nullptr) {
        continue;
      }
      const std::filesystem::path normalized = row->relative_path.lexically_normal();
      GitSidebarTreeNode* node = &root;
      // Cap tree depth. A hostile repo can contain a pathologically deep path;
      // descending one node per segment builds a chain that then overflows the
      // stack in the recursive EmitGitSidebarTreeLines. Beyond the cap the file
      // is grouped at the capped depth (a shallow display for absurd paths).
      constexpr int kMaxGitSidebarTreeDepth = 64;
      int depth = 0;
      for (const std::string& segment : ParentPathSegments(normalized)) {
        if (depth >= kMaxGitSidebarTreeDepth) {
          break;
        }
        GitSidebarTreeNode& child = node->directories[segment];
        if (child.path.empty()) {
          child.path = node->path / segment;
        }
        node = &child;
        ++depth;
      }
      node->files.push_back(row);
    }

    EmitGitSidebarTreeLines(root, section.section, 0, collapsed_directory_keys, &lines);
  }
  return lines;
}

std::optional<std::size_t> FindSelectedGitSidebarLineIndex(
    const std::vector<GitSidebarLineSpec>& lines,
    const std::size_t selected_entry_index) {
  for (std::size_t i = 0; i < lines.size(); ++i) {
    if (lines[i].kind == GitSidebarLineKind::Entry &&
        lines[i].entry_index == static_cast<int>(selected_entry_index)) {
      return i;
    }
  }
  return std::nullopt;
}

std::vector<GitSidebarLine> BuildGitSidebarLines(
    const GitSidebarViewModel& view_model,
    const std::unordered_set<std::string>* collapsed_directory_keys) {
  const std::vector<GitSidebarLineSpec> specs =
      BuildGitSidebarLineSpecs(view_model, collapsed_directory_keys);
  std::vector<GitSidebarLine> lines;
  lines.reserve(specs.size());
  for (const GitSidebarLineSpec& spec : specs) {
    lines.push_back(GitSidebarLine{
        .kind = spec.kind == GitSidebarLineKind::Header      ? GitSidebarLine::Kind::Header
                : spec.kind == GitSidebarLineKind::Directory ? GitSidebarLine::Kind::Directory
                : spec.kind == GitSidebarLineKind::Entry     ? GitSidebarLine::Kind::Entry
                                                             : GitSidebarLine::Kind::Empty,
        .section = spec.section,
        .label = spec.label,
        .display_label = spec.display_label,
        .tree_node_key = spec.tree_node_key,
        .expanded = spec.expanded,
        .depth = spec.depth,
        .entry_index = spec.entry_index,
    });
  }
  return lines;
}

std::optional<std::size_t> FindSelectedGitSidebarLineIndex(
    const std::vector<GitSidebarLine>& lines,
    const std::size_t selected_entry_index) {
  for (std::size_t i = 0; i < lines.size(); ++i) {
    if (lines[i].kind == GitSidebarLine::Kind::Entry &&
        lines[i].entry_index == static_cast<int>(selected_entry_index)) {
      return i;
    }
  }
  return std::nullopt;
}

namespace {

// Exact snapshot of every input `BuildGitSidebarViewModel` + `BuildGitSidebarLines`
// read. Comparing this field-by-field against live state (rather than hashing it)
// means the cache can never false-hit. `entries` is captured by
// (snapshot_generation, size): entry *content* is only ever assigned in
// ApplyGitRefreshSnapshot (which bumps snapshot_generation) and the no-repo
// entries.clear() (which drops size to 0), so the pair is authoritative. If a new
// git-state field starts feeding the view model, add it here AND to the equivalence
// fuzz test (GitSidebarPresentationCache/CachedMatchesUncached) will fail until it is.
struct GitSidebarPresentationKey {
  std::uint64_t snapshot_generation = 0;
  std::size_t entries_size = 0;
  std::size_t selected_index = 0;
  bool repo_available = false;
  bool refreshing = false;
  bool snapshot_stale = false;
  bool supports_mutations = false;
  bool commit_open = false;
  bool commit_in_flight = false;
  std::uint64_t review_revision = 0;
  std::string base_ref;
  std::string base_label;
  std::string error;
  std::string refresh_error;
  std::filesystem::path root;
  std::unordered_set<std::string> collapsed;
};

GitSidebarPresentationKey CaptureGitSidebarPresentationKey(
    const GitSidebarState& git, const std::filesystem::path& root,
    const compare::BranchReviewStateService& branch_review) {
  return GitSidebarPresentationKey{
      .snapshot_generation = git.snapshot_generation,
      .entries_size = git.entries.size(),
      .selected_index = git.selected_index,
      .repo_available = git.repo_available,
      .refreshing = git.refreshing,
      .snapshot_stale = git.snapshot_stale,
      .supports_mutations = git.supports_mutations,
      .commit_open = git.commit_workflow.open,
      .commit_in_flight = git.commit_workflow.operation_in_flight,
      .review_revision = branch_review.revision(),
      .base_ref = git.base_ref,
      .base_label = git.base_label,
      .error = git.error,
      .refresh_error = git.refresh_error,
      .root = root,
      .collapsed = git.collapsed_directory_keys,
  };
}

// Field-by-field equality against live state without allocating a temporary key on
// the hit path (the cheap comparisons short-circuit before the O(collapsed) set
// compare).
bool GitSidebarPresentationKeyMatches(const GitSidebarPresentationKey& key,
                                      const GitSidebarState& git,
                                      const std::filesystem::path& root,
                                      const compare::BranchReviewStateService& branch_review) {
  return key.snapshot_generation == git.snapshot_generation &&
         key.entries_size == git.entries.size() &&
         key.selected_index == git.selected_index &&
         key.repo_available == git.repo_available && key.refreshing == git.refreshing &&
         key.snapshot_stale == git.snapshot_stale &&
         key.supports_mutations == git.supports_mutations &&
         key.commit_open == git.commit_workflow.open &&
         key.commit_in_flight == git.commit_workflow.operation_in_flight &&
         key.review_revision == branch_review.revision() && key.base_ref == git.base_ref &&
         key.base_label == git.base_label && key.error == git.error &&
         key.refresh_error == git.refresh_error && key.root == root &&
         key.collapsed == git.collapsed_directory_keys;
}

struct GitSidebarPresentationCache {
  bool valid = false;
  GitSidebarPresentationKey key;
  GitSidebarPresentation presentation;
};

thread_local GitSidebarPresentationCache g_git_sidebar_presentation_cache;
// thread_local to match the cache: if a non-main thread ever builds the
// presentation it keeps its own counters/cache with no cross-thread race.
thread_local std::uint64_t g_git_sidebar_cache_hits = 0;
thread_local std::uint64_t g_git_sidebar_cache_misses = 0;

}  // namespace

const GitSidebarPresentation& CachedGitSidebarPresentation(
    const GitSidebarState& git, const std::filesystem::path& root,
    const compare::BranchReviewStateService& branch_review) {
  GitSidebarPresentationCache& cache = g_git_sidebar_presentation_cache;
  if (cache.valid && GitSidebarPresentationKeyMatches(cache.key, git, root, branch_review)) {
    ++g_git_sidebar_cache_hits;
    return cache.presentation;
  }
  ++g_git_sidebar_cache_misses;
  GitSidebarViewModel view_model = BuildGitSidebarViewModel(git, root, branch_review);
  std::vector<GitSidebarLine> lines =
      BuildGitSidebarLines(view_model, &git.collapsed_directory_keys);
  cache.key = CaptureGitSidebarPresentationKey(git, root, branch_review);
  cache.presentation =
      GitSidebarPresentation{.view_model = std::move(view_model), .lines = std::move(lines)};
  cache.valid = true;
  return cache.presentation;
}

void ResetGitSidebarPresentationCacheForTesting() {
  g_git_sidebar_presentation_cache = GitSidebarPresentationCache{};
  g_git_sidebar_cache_hits = 0;
  g_git_sidebar_cache_misses = 0;
}

std::uint64_t GitSidebarPresentationCacheHitsForTesting() { return g_git_sidebar_cache_hits; }
std::uint64_t GitSidebarPresentationCacheMissesForTesting() { return g_git_sidebar_cache_misses; }

}  // namespace microide::workspace
