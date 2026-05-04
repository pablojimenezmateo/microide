#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace microide::workspace {

enum class GitSidebarSection {
  Modified,
  Outgoing,
};

enum class GitSidebarLineKind {
  Header,
  Entry,
  Empty,
};

struct GitSidebarLineSpec {
  GitSidebarLineKind kind = GitSidebarLineKind::Empty;
  GitSidebarSection section = GitSidebarSection::Modified;
  std::string label;
  int entry_index = -1;
};

struct GitSidebarEntryTextModel {
  std::string primary_label;
  std::string secondary_label;
};

std::vector<GitSidebarLineSpec> BuildGitSidebarLineSpecs(
    const std::vector<GitSidebarSection>& entry_sections,
    bool git_repo_available,
    bool refreshing,
    std::string_view git_base_ref,
    std::string_view git_base_label);
std::optional<std::size_t> FindSelectedGitSidebarLineIndex(
    const std::vector<GitSidebarLineSpec>& lines,
    std::size_t selected_entry_index);
GitSidebarEntryTextModel BuildGitSidebarEntryTextModel(const std::filesystem::path& relative_path,
                                                       bool staged);

}  // namespace microide::workspace
