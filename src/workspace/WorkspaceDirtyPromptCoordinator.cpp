#include "workspace/WorkspaceDirtyPromptCoordinator.h"

#include <algorithm>
#include <filesystem>
#include <utility>
#include <vector>

#include "workspace/WorkspaceShell.h"

namespace microide::workspace {

DirtyPromptCoordinator::DirtyPromptCoordinator(WorkspaceContext& context,
                                               bool& quit_requested,
                                               Operations operations)
    : context_(context),
      quit_requested_(quit_requested),
      operations_(std::move(operations)) {}

void DirtyPromptCoordinator::Confirm() {
  if (!context_.prompts.dirty_visible) {
    return;
  }

  const DirtyPromptState prompt = context_.prompts.dirty;
  if (prompt.selected_action == 2) {
    operations_.dismiss_dirty_prompt(true);
    return;
  }

  if (prompt.kind == DirtyPromptState::Kind::RenamePath ||
      prompt.kind == DirtyPromptState::Kind::DeletePath) {
    operations_.confirm_path_prompt(prompt.selected_action == 0);
    return;
  }

  switch (prompt.kind) {
    case DirtyPromptState::Kind::CloseTab:
      ConfirmCloseTab(prompt);
      return;
    case DirtyPromptState::Kind::CloseTabs:
      ConfirmCloseTabs(prompt);
      return;
    case DirtyPromptState::Kind::CloseProject:
      ConfirmCloseProject(prompt);
      return;
    case DirtyPromptState::Kind::Quit:
      ConfirmQuit(prompt);
      return;
    case DirtyPromptState::Kind::RenamePath:
    case DirtyPromptState::Kind::DeletePath:
      return;
  }
}

std::optional<std::size_t> DirtyPromptCoordinator::FindProjectIndexByRoot(
    const std::filesystem::path& root) const {
  if (root.empty()) {
    return std::nullopt;
  }
  for (std::size_t i = 0; i < context_.project_catalog.entries.size(); ++i) {
    if (context_.ProjectCatalogRoot(i) == root) {
      return i;
    }
  }
  return std::nullopt;
}

bool DirtyPromptCoordinator::SaveDirtyTabs(std::span<const std::size_t> tab_indices) {
  for (std::size_t index : tab_indices) {
    if (!operations_.save_tab(index)) {
      return false;
    }
  }
  return true;
}

bool DirtyPromptCoordinator::SwitchProjectByRoot(const std::filesystem::path& root) {
  const auto index = FindProjectIndexByRoot(root);
  return index.has_value() && operations_.switch_project(*index, false);
}

void DirtyPromptCoordinator::ConfirmCloseTab(const DirtyPromptState& prompt) {
  if (prompt.selected_action == 0 && !operations_.save_tab(prompt.tab_index)) {
    return;
  }
  operations_.dismiss_dirty_prompt(false);
  operations_.close_tab(prompt.tab_index);
}

void DirtyPromptCoordinator::ConfirmCloseTabs(const DirtyPromptState& prompt) {
  if (prompt.selected_action == 0 && !SaveDirtyTabs(prompt.dirty_tabs)) {
    return;
  }

  operations_.dismiss_dirty_prompt(false);
  std::vector<std::size_t> indices = prompt.target_tabs;
  std::sort(indices.begin(), indices.end());
  indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
  for (std::size_t i = indices.size(); i > 0; --i) {
    operations_.close_tab(indices[i - 1]);
  }
}

void DirtyPromptCoordinator::ConfirmCloseProject(const DirtyPromptState& prompt) {
  if (prompt.project_index >= context_.project_catalog.entries.size()) {
    operations_.dismiss_dirty_prompt(true);
    return;
  }

  const bool target_was_active =
      context_.HasActiveProjectCatalogEntry() &&
      prompt.project_index == context_.project_catalog.active_index;
  const std::filesystem::path original_active_root = context_.current_project_state.root;
  const std::filesystem::path target_root = context_.ProjectCatalogRoot(prompt.project_index);

  if (prompt.selected_action == 0 && !target_was_active && !context_.current_project_state.root.empty()) {
    if (!SwitchProjectByRoot(target_root)) {
      operations_.dismiss_dirty_prompt(true);
      return;
    }
  }
  if (prompt.selected_action == 0 && !SaveDirtyTabs(prompt.dirty_tabs)) {
    if (!target_was_active && !original_active_root.empty()) {
      SwitchProjectByRoot(original_active_root);
    }
    return;
  }

  operations_.dismiss_dirty_prompt(false);
  const auto target_index = FindProjectIndexByRoot(target_root);
  if (!target_index.has_value()) {
    return;
  }
  operations_.close_project(*target_index);

  if (!target_was_active && !original_active_root.empty()) {
    SwitchProjectByRoot(original_active_root);
  }
}

void DirtyPromptCoordinator::ConfirmQuit(const DirtyPromptState& prompt) {
  const std::filesystem::path original_active_root = context_.current_project_state.root;
  if (prompt.selected_action == 0) {
    std::vector<std::filesystem::path> project_roots;
    project_roots.reserve(context_.project_catalog.entries.size());
    for (std::size_t i = 0; i < context_.project_catalog.entries.size(); ++i) {
      const std::filesystem::path root = context_.ProjectCatalogRoot(i);
      if (!root.empty()) {
        project_roots.push_back(root);
      }
    }

    for (const auto& root : project_roots) {
      if (!SwitchProjectByRoot(root)) {
        continue;
      }
      if (!SaveDirtyTabs(operations_.dirty_editor_tab_indices())) {
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

  operations_.dismiss_dirty_prompt(false);
  quit_requested_ = true;
}

DirtyPromptCoordinator WorkspaceShell::MakeDirtyPromptCoordinator() {
  return DirtyPromptCoordinator(
      context_,
      quit_requested_,
      DirtyPromptCoordinator::Operations{
          .dismiss_dirty_prompt = [this](bool restore_focus) { DismissDirtyPrompt(restore_focus); },
          .confirm_path_prompt =
              [this](bool save_changes) {
                ConfirmPromptSurface(save_changes ? DirtyPathResolution::Save
                                                 : DirtyPathResolution::Discard);
              },
          .save_tab = [this](std::size_t index) { return SaveTab(index); },
          .switch_project =
              [this](std::size_t index, bool log_feedback) {
                return SwitchProject(index, log_feedback);
              },
          .close_tab = [this](std::size_t index) { CloseTab(index); },
          .close_project = [this](std::size_t index) { CloseProject(index); },
          .dirty_editor_tab_indices = [this]() { return DirtyEditorTabIndices(); },
      });
}

}  // namespace microide::workspace
