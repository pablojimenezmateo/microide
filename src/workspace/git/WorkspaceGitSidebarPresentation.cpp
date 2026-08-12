#include "workspace/git/WorkspaceGitSidebarPresentation.h"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "util/PathMatch.h"

namespace microide::workspace {

namespace {

// The tree is keyed on '/'-separated generic text, not on `std::filesystem::path`.
// Every path in a row arrives normalized (`MakeGitRepositoryPathIdentity` does it
// on ingress), and a normalized generic path IS its text — so the whole grouping
// is substring work over one string per row instead of `parent_path()`,
// `lexically_normal()` and `generic_string()` temporaries per ancestor level
// (TD-2026-08-06-159; same shape as the status-refresh fix in a43cbb10).
struct GitSidebarTreeNode {
  std::string path;  // generic, '/'-joined, no leading or trailing separator
  std::map<std::string, GitSidebarTreeNode, std::less<>> directories;
  std::vector<const struct KeyedRow*> files;
};

// A row paired with its sort/group key, computed ONCE. The comparator used to
// derive this key from scratch on both sides of every comparison, which made the
// sort O(n log n) path normalizations for an O(n) amount of distinct text.
//
// The key is a VIEW into the row's own `relative_path`, which is already the
// normalized generic text git reported (TD-2026-08-11-183) — so grouping a
// 3,000-file refresh no longer copies 3,000 strings out of it. The rows live in the
// caller's view model and outlive every use of these views.
struct KeyedRow {
  const GitSidebarRowViewModel* row = nullptr;
  std::string_view key;
};

std::string FileLeafLabel(const GitSidebarRowViewModel& row, std::string_view normalized_key) {
  if (!row.primary_label.empty()) {
    return std::string(row.primary_label);
  }
  const std::size_t slash = normalized_key.find_last_of('/');
  const std::string_view leaf =
      slash == std::string_view::npos ? normalized_key : normalized_key.substr(slash + 1);
  if (!leaf.empty()) {
    return std::string(leaf);
  }
  return normalized_key.empty() ? "." : std::string(normalized_key);
}

std::string DirectoryNodeKey(GitSidebarEntry::Section section, std::string_view path) {
  std::string key = std::to_string(static_cast<int>(section));
  key.reserve(key.size() + 1 + path.size());
  key.push_back('|');
  key.append(path);
  return key;
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
  for (const KeyedRow* keyed : node.files) {
    if (keyed == nullptr || keyed->row == nullptr) {
      continue;
    }
    const GitSidebarRowViewModel* row = keyed->row;
    std::string leaf = FileLeafLabel(*row, keyed->key);
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

GitSidebarEntryTextModel BuildGitSidebarEntryTextModel(std::string_view relative_path,
                                                       bool staged) {
  // The input is the normalized generic text git reported, so the leaf and its
  // parent are one `find_last_of('/')`. This used to run `lexically_normal()` plus
  // `filename()` plus `parent_path()` per changed file — roughly fifteen
  // allocations each, on the path that rebuilds the whole git panel
  // (TD-2026-08-11-183).
  GitSidebarEntryTextModel model;
  const std::size_t slash = relative_path.find_last_of('/');
  const std::string_view leaf =
      slash == std::string_view::npos ? relative_path : relative_path.substr(slash + 1);
  if (!leaf.empty()) {
    model.primary_label.assign(leaf);
  } else {
    model.primary_label = relative_path.empty() ? "." : std::string(relative_path);
  }

  const std::string_view parent =
      slash == std::string_view::npos ? std::string_view{} : relative_path.substr(0, slash);
  if (!parent.empty() && parent != ".") {
    model.secondary_label.assign(parent);
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

    std::vector<KeyedRow> sorted_rows;
    sorted_rows.reserve(section.rows.size());
    for (const GitSidebarRowViewModel& row : section.rows) {
      sorted_rows.push_back(KeyedRow{.row = &row, .key = row.relative_path});
    }
    std::sort(sorted_rows.begin(), sorted_rows.end(),
              [](const KeyedRow& lhs, const KeyedRow& rhs) {
                if (lhs.key != rhs.key) {
                  return lhs.key < rhs.key;
                }
                return lhs.row->entry_index < rhs.row->entry_index;
              });

    GitSidebarTreeNode root;
    for (const KeyedRow& keyed : sorted_rows) {
      GitSidebarTreeNode* node = &root;
      // Cap tree depth. A hostile repo can contain a pathologically deep path;
      // descending one node per segment builds a chain that then overflows the
      // stack in the recursive EmitGitSidebarTreeLines. Beyond the cap the file
      // is grouped at the capped depth (a shallow display for absurd paths).
      constexpr int kMaxGitSidebarTreeDepth = 64;
      int depth = 0;
      // Every '/'-separated component except the last names an ancestor
      // directory. An empty component is a leading '/' or a trailing one and
      // names nothing, matching what the path-iterator form skipped.
      const std::string_view key = keyed.key;
      const std::size_t last_slash = key.find_last_of('/');
      std::size_t begin = 0;
      while (last_slash != std::string_view::npos && begin < last_slash &&
             depth < kMaxGitSidebarTreeDepth) {
        const std::size_t slash = key.find('/', begin);
        const std::string_view segment = key.substr(begin, slash - begin);
        begin = slash + 1;
        if (segment.empty() || segment == "." || segment == "/") {
          continue;
        }
        const auto existing = node->directories.find(segment);
        GitSidebarTreeNode& child = existing != node->directories.end()
                                        ? existing->second
                                        : node->directories[std::string(segment)];
        if (child.path.empty()) {
          child.path = node->path;
          child.path.reserve(node->path.size() + 1 + segment.size());
          if (!child.path.empty()) child.path.push_back('/');
          child.path.append(segment);
        }
        node = &child;
        ++depth;
      }
      node->files.push_back(&keyed);
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
