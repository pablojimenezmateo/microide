#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "workspace/GitSidebarCommandCenter.h"
#include "workspace/WorkspaceSidebarState.h"

namespace microide::workspace {

enum class GitSidebarLineKind {
  Header,
  Entry,
  Empty,
};

struct GitSidebarLineSpec {
  GitSidebarLineKind kind = GitSidebarLineKind::Empty;
  GitSidebarEntry::Section section = GitSidebarEntry::Section::Changed;
  std::string label;
  int entry_index = -1;
};

struct GitSidebarEntryTextModel {
  std::string primary_label;
  std::string secondary_label;
};

std::vector<GitSidebarLineSpec> BuildGitSidebarLineSpecs(const GitSidebarViewModel& view_model);
std::optional<std::size_t> FindSelectedGitSidebarLineIndex(
    const std::vector<GitSidebarLineSpec>& lines,
    std::size_t selected_entry_index);
GitSidebarEntryTextModel BuildGitSidebarEntryTextModel(const std::filesystem::path& relative_path,
                                                       bool staged);

}  // namespace microide::workspace
