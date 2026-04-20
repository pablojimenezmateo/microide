#include "workspace/WorkspaceDirtyPromptCoordinator.h"

#include <algorithm>
#include <filesystem>
#include <vector>

namespace microide::workspace {

DirtyPromptCoordinator::DirtyPromptCoordinator(WorkspaceShell& shell) : shell_(shell) {}

void DirtyPromptCoordinator::Confirm() {
  if (!shell_.prompts_.dirty_visible) {
    return;
  }

  const WorkspaceShell::DirtyPromptState prompt = shell_.prompts_.dirty;
  if (prompt.selected_action == 2) {
    shell_.DismissDirtyPrompt(true);
    return;
  }

  if (prompt.kind == WorkspaceShell::DirtyPromptState::Kind::RenamePath ||
      prompt.kind == WorkspaceShell::DirtyPromptState::Kind::DeletePath) {
    shell_.ConfirmPromptSurface(prompt.selected_action == 0
                                    ? WorkspaceShell::DirtyPathResolution::Save
                                    : WorkspaceShell::DirtyPathResolution::Discard);
    return;
  }

  switch (prompt.kind) {
    case WorkspaceShell::DirtyPromptState::Kind::CloseTab:
      ConfirmCloseTab(prompt);
      return;
    case WorkspaceShell::DirtyPromptState::Kind::CloseTabs:
      ConfirmCloseTabs(prompt);
      return;
    case WorkspaceShell::DirtyPromptState::Kind::CloseProject:
      ConfirmCloseProject(prompt);
      return;
    case WorkspaceShell::DirtyPromptState::Kind::Quit:
      ConfirmQuit(prompt);
      return;
    case WorkspaceShell::DirtyPromptState::Kind::RenamePath:
    case WorkspaceShell::DirtyPromptState::Kind::DeletePath:
      return;
  }
}

std::optional<std::size_t> DirtyPromptCoordinator::FindProjectIndexByRoot(
    const std::filesystem::path& root) const {
  if (root.empty()) {
    return std::nullopt;
  }
  for (std::size_t i = 0; i < shell_.project_catalog_.entries.size(); ++i) {
    if (shell_.ProjectCatalogRoot(i) == root) {
      return i;
    }
  }
  return std::nullopt;
}

bool DirtyPromptCoordinator::SaveDirtyTabs(std::span<const std::size_t> tab_indices) {
  for (std::size_t index : tab_indices) {
    if (!shell_.SaveTab(index)) {
      return false;
    }
  }
  return true;
}

bool DirtyPromptCoordinator::SwitchProjectByRoot(const std::filesystem::path& root) {
  const auto index = FindProjectIndexByRoot(root);
  return index.has_value() && shell_.SwitchProject(*index, false);
}

void DirtyPromptCoordinator::ConfirmCloseTab(const WorkspaceShell::DirtyPromptState& prompt) {
  if (prompt.selected_action == 0 && !shell_.SaveTab(prompt.tab_index)) {
    return;
  }
  shell_.DismissDirtyPrompt(false);
  shell_.CloseTab(prompt.tab_index);
}

void DirtyPromptCoordinator::ConfirmCloseTabs(const WorkspaceShell::DirtyPromptState& prompt) {
  if (prompt.selected_action == 0 && !SaveDirtyTabs(prompt.dirty_tabs)) {
    return;
  }

  shell_.DismissDirtyPrompt(false);
  std::vector<std::size_t> indices = prompt.target_tabs;
  std::sort(indices.begin(), indices.end());
  indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
  for (std::size_t i = indices.size(); i > 0; --i) {
    shell_.CloseTab(indices[i - 1]);
  }
}

void DirtyPromptCoordinator::ConfirmCloseProject(
    const WorkspaceShell::DirtyPromptState& prompt) {
  if (prompt.project_index >= shell_.project_catalog_.entries.size()) {
    shell_.DismissDirtyPrompt(true);
    return;
  }

  const bool target_was_active =
      shell_.HasActiveProjectCatalogEntry() &&
      prompt.project_index == shell_.project_catalog_.active_index;
  const std::filesystem::path original_active_root = shell_.project_root_;
  const std::filesystem::path target_root = shell_.ProjectCatalogRoot(prompt.project_index);

  if (prompt.selected_action == 0 && !target_was_active && !shell_.project_root_.empty()) {
    if (!SwitchProjectByRoot(target_root)) {
      shell_.DismissDirtyPrompt(true);
      return;
    }
  }
  if (prompt.selected_action == 0 && !SaveDirtyTabs(prompt.dirty_tabs)) {
    if (!target_was_active && !original_active_root.empty()) {
      SwitchProjectByRoot(original_active_root);
    }
    return;
  }

  shell_.DismissDirtyPrompt(false);
  const auto target_index = FindProjectIndexByRoot(target_root);
  if (!target_index.has_value()) {
    return;
  }
  shell_.CloseProject(*target_index);

  if (!target_was_active && !original_active_root.empty()) {
    SwitchProjectByRoot(original_active_root);
  }
}

void DirtyPromptCoordinator::ConfirmQuit(const WorkspaceShell::DirtyPromptState& prompt) {
  const std::filesystem::path original_active_root = shell_.project_root_;
  if (prompt.selected_action == 0) {
    std::vector<std::filesystem::path> project_roots;
    project_roots.reserve(shell_.project_catalog_.entries.size());
    for (std::size_t i = 0; i < shell_.project_catalog_.entries.size(); ++i) {
      const std::filesystem::path root = shell_.ProjectCatalogRoot(i);
      if (!root.empty()) {
        project_roots.push_back(root);
      }
    }

    for (const auto& root : project_roots) {
      if (!SwitchProjectByRoot(root)) {
        continue;
      }
      if (!SaveDirtyTabs(shell_.DirtyEditorTabIndices())) {
        if (!original_active_root.empty()) {
          SwitchProjectByRoot(original_active_root);
        }
        return;
      }
    }

    if (!original_active_root.empty()) {
      SwitchProjectByRoot(original_active_root);
    }
  }

  shell_.DismissDirtyPrompt(false);
  shell_.quit_requested_ = true;
}

}  // namespace microide::workspace
