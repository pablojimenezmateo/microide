#include "workspace/WorkspaceGitSidebarPresentation.h"

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
    lines->push_back(GitSidebarLineSpec{
        .kind = GitSidebarLineKind::Entry,
        .section = section,
        .label = FileLeafLabel(*row),
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
    lines.push_back(GitSidebarLineSpec{
        .kind = GitSidebarLineKind::Header,
        .section = section.section,
        .label = section.header_label,
        .tree_node_key = {},
    });
    if (section.rows.empty()) {
      lines.push_back(GitSidebarLineSpec{
          .kind = GitSidebarLineKind::Empty,
          .section = section.section,
          .label = section.empty_label,
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
      for (const std::string& segment : ParentPathSegments(normalized)) {
        GitSidebarTreeNode& child = node->directories[segment];
        if (child.path.empty()) {
          child.path = node->path / segment;
        }
        node = &child;
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

}  // namespace microide::workspace
