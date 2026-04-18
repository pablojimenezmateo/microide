#include "workspace/WorkspaceGitSidebarPresentation.h"

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
          : "Outgoing files (" + std::to_string(outgoing_count) + ")  " +
                std::string(git_base_label);
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

}  // namespace microide::workspace
