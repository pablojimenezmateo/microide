#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

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
std::optional<std::size_t> FindSelectedGitSidebarLineIndex(
    const std::vector<GitSidebarLineSpec>& lines,
    std::size_t selected_entry_index);
GitSidebarEntryTextModel BuildGitSidebarEntryTextModel(const std::filesystem::path& relative_path,
                                                       bool staged);

}  // namespace microide::workspace
