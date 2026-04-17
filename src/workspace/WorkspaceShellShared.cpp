#include "workspace/WorkspaceShellShared.h"

namespace microide::workspace {

GitSidebarEntryTextModel BuildGitSidebarEntryTextModel(const std::filesystem::path& relative_path,
                                                       bool staged) {
  const std::filesystem::path normalized_path = relative_path.lexically_normal();

  GitSidebarEntryTextModel model;
  model.primary_label = normalized_path.filename().string();
  if (model.primary_label.empty()) {
    model.primary_label = normalized_path.empty() ? "." : normalized_path.string();
  }

  const std::filesystem::path parent = normalized_path.parent_path();
  if (!parent.empty() && parent != ".") {
    model.secondary_label = parent.string();
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
    const std::vector<GitSidebarSection>& entry_sections,
    bool git_repo_available,
    std::string_view git_base_ref,
    std::string_view git_base_label) {
  std::vector<GitSidebarLineSpec> lines;
  std::size_t modified_count = 0;
  std::size_t outgoing_count = 0;
  for (GitSidebarSection section : entry_sections) {
    if (section == GitSidebarSection::Modified) {
      ++modified_count;
    } else {
      ++outgoing_count;
    }
  }

  lines.push_back(GitSidebarLineSpec{
      .kind = GitSidebarLineKind::Header,
      .section = GitSidebarSection::Modified,
      .label = "Changes (" + std::to_string(modified_count) + ")",
  });
  if (modified_count == 0) {
    lines.push_back(GitSidebarLineSpec{
        .kind = GitSidebarLineKind::Empty,
        .section = GitSidebarSection::Modified,
        .label = git_repo_available ? "Working tree is clean" : "Not a git repository",
    });
  } else {
    for (std::size_t i = 0; i < entry_sections.size(); ++i) {
      if (entry_sections[i] != GitSidebarSection::Modified) {
        continue;
      }
      lines.push_back(GitSidebarLineSpec{
          .kind = GitSidebarLineKind::Entry,
          .section = GitSidebarSection::Modified,
          .label = {},
          .entry_index = static_cast<int>(i),
      });
    }
  }

  const std::string outgoing_header =
      git_base_label.empty()
          ? "Outgoing files (" + std::to_string(outgoing_count) + ")"
          : "Outgoing files (" + std::to_string(outgoing_count) + ")  " + std::string(git_base_label);
  lines.push_back(GitSidebarLineSpec{
      .kind = GitSidebarLineKind::Header,
      .section = GitSidebarSection::Outgoing,
      .label = outgoing_header,
  });
  if (outgoing_count == 0) {
    lines.push_back(GitSidebarLineSpec{
        .kind = GitSidebarLineKind::Empty,
        .section = GitSidebarSection::Outgoing,
        .label = git_base_ref.empty() ? "Base branch unavailable" : "No outgoing files",
    });
  } else {
    for (std::size_t i = 0; i < entry_sections.size(); ++i) {
      if (entry_sections[i] != GitSidebarSection::Outgoing) {
        continue;
      }
      lines.push_back(GitSidebarLineSpec{
          .kind = GitSidebarLineKind::Entry,
          .section = GitSidebarSection::Outgoing,
          .label = {},
          .entry_index = static_cast<int>(i),
      });
    }
  }

  return lines;
}

std::optional<std::size_t> FindSelectedGitSidebarLineIndex(
    const std::vector<GitSidebarLineSpec>& lines,
    std::size_t selected_entry_index) {
  for (std::size_t i = 0; i < lines.size(); ++i) {
    if (lines[i].kind == GitSidebarLineKind::Entry &&
        lines[i].entry_index == static_cast<int>(selected_entry_index)) {
      return i;
    }
  }
  return std::nullopt;
}

std::vector<int> BuildProjectSearchResultLineMap(
    const std::vector<project::ProjectSearchResult>& results) {
  std::vector<int> line_map;
  line_map.reserve(results.size() * 2);

  std::filesystem::path current_path;
  for (std::size_t i = 0; i < results.size(); ++i) {
    const auto& result = results[i];
    if (result.relative_path != current_path) {
      current_path = result.relative_path;
      line_map.push_back(-1);
    }
    line_map.push_back(static_cast<int>(i));
  }

  return line_map;
}

int FindProjectSearchResultLine(const std::vector<int>& line_map, std::size_t result_index) {
  for (std::size_t line = 0; line < line_map.size(); ++line) {
    if (line_map[line] == static_cast<int>(result_index)) {
      return static_cast<int>(line);
    }
  }
  return 0;
}

}  // namespace microide::workspace
