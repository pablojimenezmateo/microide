#pragma once

#include <SDL3/SDL.h>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "project/ProjectSearchService.h"
#include "render/Theme.h"

namespace microide::workspace {

struct WorkspaceTabTextModel {
  std::string display_title;
  std::string tooltip_label;
};

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
    std::string_view git_base_ref,
    std::string_view git_base_label);
std::optional<std::size_t> FindSelectedGitSidebarLineIndex(
    const std::vector<GitSidebarLineSpec>& lines,
    std::size_t selected_entry_index);
std::vector<int> BuildProjectSearchResultLineMap(
    const std::vector<project::ProjectSearchResult>& results);
int FindProjectSearchResultLine(const std::vector<int>& line_map, std::size_t result_index);

std::string ProjectStateDirectoryName(const std::filesystem::path& project_root);
GitSidebarEntryTextModel BuildGitSidebarEntryTextModel(const std::filesystem::path& relative_path,
                                                       bool staged);
WorkspaceTabTextModel BuildWorkspaceTabTextModel(const std::filesystem::path& project_root,
                                                 const std::filesystem::path& path,
                                                 std::string_view fallback_title,
                                                 bool dirty);
std::string BuildEditorBreadcrumbLabel(const std::filesystem::path& project_root,
                                       const std::filesystem::path& path,
                                       bool placeholder);
std::string BuildCompareBreadcrumbLabel(const std::filesystem::path& project_root,
                                        const std::filesystem::path& path,
                                        std::string_view left_label,
                                        std::string_view right_label);
std::string BuildMergeBreadcrumbLabel(const std::filesystem::path& project_root,
                                      const std::filesystem::path& output_path,
                                      std::string_view incoming_label,
                                      std::string_view current_label);
std::optional<SDL_Color> ParseProjectColor(std::string_view text);
std::string FormatProjectColor(SDL_Color color);
SDL_Color DefaultProjectBaseColor(const std::filesystem::path& project_root);
void ApplyProjectAccent(render::Theme& theme, SDL_Color accent);

}  // namespace microide::workspace
