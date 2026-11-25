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

struct GitSidebarTreeNode {
  std::string path;
  std::map<std::string, GitSidebarTreeNode, std::less<>> directories;
  std::vector<const struct KeyedRow*> files;
};

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
                             std::vector<GitSidebarLine>* lines) {
  for (const auto& [label, child] : node.directories) {
    const std::string node_key = DirectoryNodeKey(section, child.path);
    const bool collapsed =
        collapsed_directory_keys != nullptr &&
        collapsed_directory_keys->find(node_key) != collapsed_directory_keys->end();
    lines->push_back(GitSidebarLine{
        .kind = GitSidebarLine::Kind::Directory,
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
    lines->push_back(GitSidebarLine{
        .kind = GitSidebarLine::Kind::Entry,
        .section = section,
        .label = std::move(leaf),
        .display_label = std::move(display_label),
        .tree_node_key = {},
        .depth = depth,
        .entry_index = row->entry_index,
    });
  }
}

}

GitSidebarEntryTextModel BuildGitSidebarEntryTextModel(std::string_view relative_path,
                                                       bool staged) {
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

std::vector<GitSidebarLine> BuildGitSidebarLines(
    const GitSidebarViewModel& view_model,
    const std::unordered_set<std::string>* collapsed_directory_keys) {
  std::vector<GitSidebarLine> lines;
  for (const GitSidebarSectionViewModel& section : view_model.sections) {
    if (section.show_header) {
      lines.push_back(GitSidebarLine{
          .kind = GitSidebarLine::Kind::Header,
          .section = section.section,
          .label = section.header_label,
          .display_label = section.header_label,
          .tree_node_key = {},
      });
    }
    if (section.rows.empty()) {
      lines.push_back(GitSidebarLine{
          .kind = GitSidebarLine::Kind::Empty,
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
      constexpr int kMaxGitSidebarTreeDepth = 64;
      int depth = 0;
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
  std::shared_ptr<const GitSidebarPresentation> presentation;
};

thread_local GitSidebarPresentationCache g_git_sidebar_presentation_cache;
thread_local std::uint64_t g_git_sidebar_cache_hits = 0;
thread_local std::uint64_t g_git_sidebar_cache_misses = 0;

}

std::shared_ptr<const GitSidebarPresentation> SharedGitSidebarPresentation(
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
  cache.presentation = std::make_shared<const GitSidebarPresentation>(
      GitSidebarPresentation{.view_model = std::move(view_model), .lines = std::move(lines)});
  cache.valid = true;
  return cache.presentation;
}

const GitSidebarPresentation& CachedGitSidebarPresentation(
    const GitSidebarState& git, const std::filesystem::path& root,
    const compare::BranchReviewStateService& branch_review) {
  return *SharedGitSidebarPresentation(git, root, branch_review);
}

void ResetGitSidebarPresentationCacheForTesting() {
  g_git_sidebar_presentation_cache = GitSidebarPresentationCache{};
  g_git_sidebar_cache_hits = 0;
  g_git_sidebar_cache_misses = 0;
}

std::uint64_t GitSidebarPresentationCacheHitsForTesting() { return g_git_sidebar_cache_hits; }
std::uint64_t GitSidebarPresentationCacheMissesForTesting() { return g_git_sidebar_cache_misses; }

std::string_view GitSidebarDirectoryPath(const GitSidebarLine& line) {
  const std::string_view key = line.tree_node_key;
  const std::size_t bar = key.find('|');
  return line.kind == GitSidebarLine::Kind::Directory && bar != std::string_view::npos ? key.substr(bar + 1)
                                                                                       : std::string_view();
}

}
