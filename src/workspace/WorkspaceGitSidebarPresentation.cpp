#include "workspace/WorkspaceGitSidebarPresentation.h"

namespace microide::workspace {

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

std::vector<GitSidebarLineSpec> BuildGitSidebarLineSpecs(const GitSidebarViewModel& view_model) {
  std::vector<GitSidebarLineSpec> lines;
  for (const GitSidebarSectionViewModel& section : view_model.sections) {
    lines.push_back(GitSidebarLineSpec{
        .kind = GitSidebarLineKind::Header,
        .section = section.section,
        .label = section.header_label,
    });
    if (section.rows.empty()) {
      lines.push_back(GitSidebarLineSpec{
          .kind = GitSidebarLineKind::Empty,
          .section = section.section,
          .label = section.empty_label,
      });
      continue;
    }
    for (const GitSidebarRowViewModel& row : section.rows) {
      lines.push_back(GitSidebarLineSpec{
          .kind = GitSidebarLineKind::Entry,
          .section = section.section,
          .entry_index = row.entry_index,
      });
    }
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

}  // namespace microide::workspace
