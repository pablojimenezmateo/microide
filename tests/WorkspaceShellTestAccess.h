#pragma once

#include "workspace/WorkspaceShell.h"

#include <filesystem>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace microide::workspace {

struct WorkspaceShellTestAccess {
  static void SetProjectRoot(WorkspaceShell& shell, const std::filesystem::path& root) {
    shell.project_root_ = root.lexically_normal();
    shell.directory_tree_.SetRoot(shell.project_root_);
    shell.file_index_.SetRoot(shell.project_root_);
    shell.file_finder_.SetIndex(&shell.file_index_);
    shell.sidebar_visible_ = true;
    shell.sidebar_mode_ = WorkspaceShell::SidebarMode::Tree;
    shell.focus_ = WorkspaceShell::FocusTarget::Sidebar;
  }

  static void OpenSingleEditorTab(WorkspaceShell& shell, const std::filesystem::path& path) {
    editor::TextViewport opened_view;
    if (!opened_view.OpenFile(path)) {
      throw std::runtime_error("failed to open editor fixture: " + path.string());
    }
    shell.ApplyEditorPreferences(opened_view);
    shell.text_viewport_ = opened_view;
    shell.open_tabs_.push_back(WorkspaceShell::TabEntry{
        .kind = WorkspaceShell::TabEntry::Kind::Editor,
        .path = path.lexically_normal(),
        .title = path.filename().string(),
        .editor_state = WorkspaceShell::MakeEditorTabState(opened_view),
        .compare = std::nullopt,
        .merge = std::nullopt,
    });
    shell.active_tab_index_ = 0;
    shell.focus_ = WorkspaceShell::FocusTarget::Editor;
  }

  static editor::TextViewport& ActiveEditor(WorkspaceShell& shell) { return shell.text_viewport_; }
  static WorkspaceShell::CompareTabState& ActiveCompare(WorkspaceShell& shell) {
    return shell.open_tabs_[shell.active_tab_index_].compare.value();
  }
  static WorkspaceShell::MergeTabState& ActiveMerge(WorkspaceShell& shell) {
    return shell.open_tabs_[shell.active_tab_index_].merge.value();
  }

  static void PrepareRenamePrompt(WorkspaceShell& shell,
                                  const std::filesystem::path& path,
                                  std::string input) {
    shell.OpenPromptSurface(WorkspaceShell::PromptSurfaceState::Action::RenamePath,
                            WorkspaceShell::PromptSurfaceState::Kind::TextInput, path,
                            std::move(input));
  }

  static void PrepareDeletePrompt(WorkspaceShell& shell, const std::filesystem::path& path) {
    shell.OpenPromptSurface(WorkspaceShell::PromptSurfaceState::Action::DeletePath,
                            WorkspaceShell::PromptSurfaceState::Kind::Confirm, path);
  }

  static void ConfirmPromptSurface(WorkspaceShell& shell) { shell.ConfirmPromptSurface(); }
  static bool OpenWorkingTreeComparison(WorkspaceShell& shell,
                                        const std::filesystem::path& path,
                                        const std::string& left_ref,
                                        const std::string& left_label) {
    return shell.OpenWorkingTreeComparison(path, left_ref, left_label);
  }
  static bool OpenBranchHeadComparison(WorkspaceShell& shell,
                                       const std::filesystem::path& path,
                                       const std::string& left_ref,
                                       const std::string& left_label,
                                       const std::string& right_ref,
                                       const std::string& right_label) {
    return shell.OpenBranchHeadComparison(path, left_ref, left_label, right_ref, right_label);
  }
  static bool OpenMergeEditor(WorkspaceShell& shell,
                              const std::filesystem::path& base_path,
                              const std::filesystem::path& incoming_path,
                              const std::filesystem::path& current_path,
                              const std::filesystem::path& output_path) {
    return shell.OpenMergeEditor(base_path, incoming_path, current_path, output_path);
  }
  static bool OpenProjectTab(WorkspaceShell& shell,
                             const std::filesystem::path& project_root,
                             bool restore_persistence = false,
                             bool log_feedback = false) {
    return shell.OpenProjectTab(project_root, restore_persistence, log_feedback);
  }
  static bool SwitchProject(WorkspaceShell& shell,
                            std::size_t index,
                            bool log_feedback = false) {
    return shell.SwitchProject(index, log_feedback);
  }
  static void OpenFile(WorkspaceShell& shell, const std::filesystem::path& path) {
    shell.OpenFile(path);
  }
  static bool RestoreSessionState(WorkspaceShell& shell) { return shell.RestoreSessionState(); }
  static void SaveSessionState(WorkspaceShell& shell) { shell.SaveSessionState(); }
  static bool RestoreWorkspaceSession(WorkspaceShell& shell) {
    return shell.RestoreWorkspaceSession();
  }
  static void SaveWorkspaceSession(WorkspaceShell& shell) { shell.SaveWorkspaceSession(); }

  static void ConfirmDirtyPrompt(WorkspaceShell& shell, int selected_action) {
    shell.dirty_prompt_state_.selected_action = selected_action;
    shell.ConfirmDirtyPrompt();
  }

  static bool DirtyPromptVisible(const WorkspaceShell& shell) { return shell.dirty_prompt_visible_; }
  static bool PromptSurfaceVisible(const WorkspaceShell& shell) {
    return shell.prompt_surface_visible_;
  }
  static const std::vector<WorkspaceShell::TabEntry>& OpenTabs(const WorkspaceShell& shell) {
    return shell.open_tabs_;
  }
  static const std::string& StatusMessage(const WorkspaceShell& shell) { return shell.status_message_; }
  static std::string BreadcrumbLabel(WorkspaceShell& shell) { return shell.BreadcrumbLabel(); }
  static std::size_t ProjectCount(const WorkspaceShell& shell) { return shell.projects_.size(); }
  static std::size_t ActiveProjectIndex(const WorkspaceShell& shell) {
    return shell.active_project_index_;
  }
  static const std::filesystem::path& ProjectRoot(const WorkspaceShell& shell) {
    return shell.project_root_;
  }
};

}  // namespace microide::workspace
