#include "workspace/WorkspaceSidebarCoordinator.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

#include "project/GitStatusService.h"
#include "workspace/WorkspaceMenuCoordinator.h"
#include "workspace/WorkspaceShellShared.h"
#include "workspace/WorkspaceTextSearch.h"

namespace microide::workspace {

WorkspaceShell::SidebarCoordinator::SidebarCoordinator(WorkspaceShell& shell) : shell_(shell) {}

void WorkspaceShell::SidebarCoordinator::ShowMode(SidebarMode mode, bool temporary) {
  if (mode == SidebarMode::None) {
    Close();
    return;
  }
  if (mode != SidebarMode::Tree) {
    MenuCoordinator(shell_).CloseTreeContextMenu();
  }

  if (shell_.surface_.sidebar_mode == SidebarMode::Search && mode != SidebarMode::Search) {
    shell_.StopProjectSearch();
  }

  if (temporary) {
    if (!shell_.surface_.sidebar_temporary && shell_.surface_.sidebar_visible) {
      shell_.surface_.sidebar_prev_mode = shell_.surface_.sidebar_mode;
      shell_.surface_.sidebar_prev_plugin_id = shell_.surface_.sidebar_plugin_id;
    }
  } else {
    shell_.surface_.sidebar_prev_mode = SidebarMode::None;
    shell_.surface_.sidebar_prev_plugin_id.clear();
  }

  if (mode != SidebarMode::Plugin) {
    shell_.surface_.sidebar_plugin_id.clear();
  }
  shell_.surface_.sidebar_mode = mode;
  shell_.surface_.sidebar_temporary = temporary;
  shell_.surface_.sidebar_visible = true;
  shell_.surface_.focus = FocusTarget::Sidebar;
  shell_.surface_.sidebar_scroll_row = 0;
  shell_.RequestWindowRedraw();
}

void WorkspaceShell::SidebarCoordinator::ShowTree(const std::filesystem::path& root) {
  if (!root.empty()) {
    if (!shell_.OpenProjectTab(root, true, true)) {
      return;
    }
  }

  ShowMode(SidebarMode::Tree, false);
}

void WorkspaceShell::SidebarCoordinator::ShowSearch(std::string query, bool temporary) {
  if (!query.empty() || shell_.overlay_workflow_.project_search.query.empty()) {
    shell_.overlay_workflow_.project_search.query = std::move(query);
  }
  shell_.overlay_workflow_.project_search.edit_buffer = shell_.overlay_workflow_.project_search.query;
  shell_.overlay_workflow_.project_search.editing =
      shell_.overlay_workflow_.project_search.query.empty();
  shell_.overlay_workflow_.project_search.edit_field = ProjectSearchEditField::Query;
  shell_.overlay_workflow_.project_search.selected_index = 0;
  shell_.RefreshProjectSearch();
  ShowMode(SidebarMode::Search, temporary);
}

void WorkspaceShell::SidebarCoordinator::ShowProblems() {
  RefreshProblems();
  ShowMode(SidebarMode::Problems, false);
  RevealSelectedProblemsLine();
}

void WorkspaceShell::SidebarCoordinator::ShowGit() {
  RefreshGit();
  ShowMode(SidebarMode::Git, false);
  RevealSelectedGitLine();
}

bool WorkspaceShell::SidebarCoordinator::ShowPlugin(std::string_view id, bool temporary) {
  const auto* provider = shell_.plugin_host_.FindSidebarProvider(id);
  if (provider == nullptr) {
    return false;
  }

  shell_.surface_.sidebar_plugin_id = provider->id;
  ShowMode(SidebarMode::Plugin, temporary);
  return RefreshPlugin();
}

void WorkspaceShell::SidebarCoordinator::Close() {
  const bool was_visible = shell_.surface_.sidebar_visible;
  if (shell_.surface_.sidebar_mode == SidebarMode::Search) {
    shell_.StopProjectSearch();
  }
  MenuCoordinator(shell_).CloseTreeContextMenu();

  if (shell_.surface_.sidebar_temporary && shell_.surface_.sidebar_prev_mode != SidebarMode::None) {
    RestorePrevious();
    return;
  }

  shell_.surface_.sidebar_visible = false;
  shell_.surface_.sidebar_temporary = false;
  shell_.surface_.sidebar_prev_mode = SidebarMode::None;
  shell_.surface_.sidebar_prev_plugin_id.clear();
  if (shell_.surface_.focus == FocusTarget::Sidebar) {
    shell_.surface_.focus = FocusTarget::Editor;
  }
  if (was_visible) {
    shell_.RequestWindowRedraw();
  }
}

void WorkspaceShell::SidebarCoordinator::Toggle() {
  const bool was_visible = shell_.surface_.sidebar_visible;
  if (shell_.surface_.sidebar_visible) {
    Close();
    return;
  }

  if (shell_.surface_.sidebar_mode == SidebarMode::None) {
    shell_.surface_.sidebar_mode = SidebarMode::Tree;
  }
  shell_.surface_.sidebar_visible = true;
  shell_.surface_.sidebar_temporary = false;
  shell_.surface_.sidebar_prev_plugin_id.clear();
  shell_.surface_.focus = FocusTarget::Sidebar;
  if (!was_visible) {
    shell_.RequestWindowRedraw();
  }
}

void WorkspaceShell::SidebarCoordinator::RestorePrevious() {
  if (shell_.surface_.sidebar_mode == SidebarMode::Search &&
      shell_.surface_.sidebar_prev_mode != SidebarMode::Search) {
    shell_.StopProjectSearch();
  }

  if (shell_.surface_.sidebar_prev_mode == SidebarMode::None) {
    shell_.surface_.sidebar_temporary = false;
    return;
  }

  shell_.surface_.sidebar_mode = shell_.surface_.sidebar_prev_mode;
  shell_.surface_.sidebar_plugin_id = shell_.surface_.sidebar_prev_plugin_id;
  shell_.surface_.sidebar_prev_mode = SidebarMode::None;
  shell_.surface_.sidebar_prev_plugin_id.clear();
  shell_.surface_.sidebar_temporary = false;
  shell_.surface_.sidebar_visible = true;
  shell_.surface_.focus = FocusTarget::Sidebar;
  shell_.surface_.sidebar_scroll_row = 0;
  if (shell_.surface_.sidebar_mode == SidebarMode::Plugin) {
    RefreshPlugin();
  } else if (shell_.surface_.sidebar_mode == SidebarMode::Problems) {
    RefreshProblems();
  }
  shell_.RequestSidebarRedraw();
}

void WorkspaceShell::SidebarCoordinator::RefreshProjectFiles() {
  shell_.directory_tree_.Refresh();
  RevealSelectedTreeLine();
  shell_.file_index_.Refresh();
  shell_.file_finder_.SetIndex(&shell_.file_index_);
  RefreshGit();
  RefreshProblems();
  RefreshPlugin();
  if (shell_.surface_.sidebar_visible) {
    shell_.RequestSidebarRedraw();
  }
}

void WorkspaceShell::SidebarCoordinator::RefreshGit() {
  const std::filesystem::path previous_path =
      shell_.git_sidebar_.selected_index < shell_.git_sidebar_.entries.size()
          ? shell_.git_sidebar_.entries[shell_.git_sidebar_.selected_index].path
          : std::filesystem::path{};
  const GitSidebarEntry::Section previous_section =
      shell_.git_sidebar_.selected_index < shell_.git_sidebar_.entries.size()
          ? shell_.git_sidebar_.entries[shell_.git_sidebar_.selected_index].section
          : GitSidebarEntry::Section::Modified;

  shell_.git_sidebar_.entries.clear();
  shell_.git_sidebar_.base_ref.clear();
  shell_.git_sidebar_.base_label.clear();
  shell_.git_sidebar_.repo_available = false;
  shell_.git_sidebar_.selected_index = 0;
  if (shell_.project_root_.empty()) {
    return;
  }

  const auto working_entries = project::CollectGitWorkingTreeEntries(shell_.project_root_);
  for (const auto& entry : working_entries) {
    shell_.git_sidebar_.entries.push_back(GitSidebarEntry{
        .section = GitSidebarEntry::Section::Modified,
        .path = (shell_.project_root_ / entry.relative_path).lexically_normal(),
        .relative_path = entry.relative_path,
        .status = entry.conflicted ? project::GitFileStatus::Conflicted : entry.status,
        .conflicted = entry.conflicted,
        .staged = entry.staged,
    });
  }

  const auto base_ref = project::ResolveGitBaseReference(shell_.project_root_);
  if (base_ref.has_value()) {
    shell_.git_sidebar_.repo_available = true;
    shell_.git_sidebar_.base_ref = base_ref->ref;
    shell_.git_sidebar_.base_label = base_ref->label;
    const auto outgoing_entries =
        project::CollectGitBranchOutgoingFiles(shell_.project_root_, shell_.git_sidebar_.base_ref);
    for (const auto& entry : outgoing_entries) {
      shell_.git_sidebar_.entries.push_back(GitSidebarEntry{
          .section = GitSidebarEntry::Section::Outgoing,
          .path = (shell_.project_root_ / entry.relative_path).lexically_normal(),
          .relative_path = entry.relative_path,
          .status = entry.status,
      });
    }
  } else {
    shell_.git_sidebar_.repo_available = std::filesystem::exists(shell_.project_root_ / ".git");
  }

  for (std::size_t i = 0; i < shell_.git_sidebar_.entries.size(); ++i) {
    if (shell_.git_sidebar_.entries[i].path == previous_path &&
        shell_.git_sidebar_.entries[i].section == previous_section) {
      shell_.git_sidebar_.selected_index = i;
      RevealSelectedGitLine();
      if (shell_.surface_.sidebar_visible && shell_.surface_.sidebar_mode == SidebarMode::Git) {
        shell_.RequestSidebarRedraw();
      }
      return;
    }
  }

  RevealSelectedGitLine();
  if (shell_.surface_.sidebar_visible && shell_.surface_.sidebar_mode == SidebarMode::Git) {
    shell_.RequestSidebarRedraw();
  }
}

bool WorkspaceShell::SidebarCoordinator::RefreshProblems() {
  std::optional<editor::PublishedDiagnostic> previous_diagnostic;
  if (shell_.problems_sidebar_.selected_index < shell_.problems_sidebar_.entries.size()) {
    previous_diagnostic =
        shell_.problems_sidebar_.entries[shell_.problems_sidebar_.selected_index].diagnostic;
  }

  shell_.problems_sidebar_.entries.clear();
  shell_.problems_sidebar_.selected_index = 0;
  for (const auto& diagnostic : shell_.diagnostics_store_.SnapshotAll()) {
    ProblemsSidebarEntry entry;
    const std::string collapsed_message = CollapseWhitespace(diagnostic.message);
    entry.diagnostic = diagnostic;
    entry.primary_label = collapsed_message.empty() ? "Diagnostic" : collapsed_message;
    entry.detail_label =
        RelativePathLabel(shell_.project_root_, diagnostic.path) + ":" +
        std::to_string(diagnostic.range.start.line + 1) + ":" +
        std::to_string(diagnostic.range.start.column + 1);
    if (!diagnostic.owner.empty()) {
      entry.detail_label += " | " + diagnostic.owner;
    }
    shell_.problems_sidebar_.entries.push_back(std::move(entry));
  }

  if (previous_diagnostic.has_value()) {
    for (std::size_t i = 0; i < shell_.problems_sidebar_.entries.size(); ++i) {
      if (shell_.problems_sidebar_.entries[i].diagnostic == *previous_diagnostic) {
        shell_.problems_sidebar_.selected_index = i;
        break;
      }
    }
  }

  RevealSelectedProblemsLine();
  if (shell_.surface_.sidebar_visible && shell_.surface_.sidebar_mode == SidebarMode::Problems) {
    shell_.RequestSidebarRedraw();
  }
  return !shell_.problems_sidebar_.entries.empty();
}

bool WorkspaceShell::SidebarCoordinator::RefreshPlugin() {
  shell_.plugin_sidebar_.items.clear();
  shell_.plugin_sidebar_.error.clear();
  shell_.plugin_sidebar_.selected_index = 0;
  if (shell_.surface_.sidebar_plugin_id.empty()) {
    if (shell_.surface_.sidebar_visible && shell_.surface_.sidebar_mode == SidebarMode::Plugin) {
      shell_.RequestSidebarRedraw();
    }
    return false;
  }
  if (shell_.plugin_host_.FindSidebarProvider(shell_.surface_.sidebar_plugin_id) == nullptr) {
    shell_.surface_.sidebar_plugin_id.clear();
    if (shell_.surface_.sidebar_mode == SidebarMode::Plugin) {
      shell_.surface_.sidebar_mode = SidebarMode::Tree;
      if (shell_.surface_.sidebar_visible) {
        shell_.RequestSidebarRedraw();
      }
    }
    return false;
  }

  std::string error_message;
  if (!shell_.plugin_host_.SnapshotSidebar(shell_.surface_.sidebar_plugin_id,
                                           &shell_.plugin_sidebar_.items, &error_message)) {
    shell_.plugin_sidebar_.error = std::move(error_message);
    if (shell_.surface_.sidebar_visible && shell_.surface_.sidebar_mode == SidebarMode::Plugin) {
      shell_.RequestSidebarRedraw();
    }
    return false;
  }
  if (!shell_.plugin_sidebar_.items.empty()) {
    shell_.plugin_sidebar_.selected_index = std::min(
        shell_.plugin_sidebar_.selected_index, shell_.plugin_sidebar_.items.size() - 1);
  }
  RevealSelectedPluginLine();
  if (shell_.surface_.sidebar_visible && shell_.surface_.sidebar_mode == SidebarMode::Plugin) {
    shell_.RequestSidebarRedraw();
  }
  return true;
}

void WorkspaceShell::SidebarCoordinator::RevealSelectedTreeLine() {
  const auto& entries = shell_.directory_tree_.entries();
  if (shell_.directory_tree_.selected_index() >= entries.size()) {
    return;
  }

  const auto layout_state = shell_.CurrentWorkspaceLayout();
  if (!layout_state.has_value()) {
    return;
  }
  const WorkspaceLayout layout = *layout_state;
  if (layout.sidebar.h <= 0.0f) {
    return;
  }

  const auto list_layout = shell_.ComputeTreeSidebarListLayout(layout.sidebar, entries.size());
  shell_.surface_.sidebar_scroll_row = RevealScrollableListIndex(
      list_layout, static_cast<int>(shell_.directory_tree_.selected_index()));
}

void WorkspaceShell::SidebarCoordinator::RevealSelectedGitLine() {
  const auto selected_line = shell_.SelectedGitSidebarLineIndex();
  if (!selected_line.has_value()) {
    return;
  }

  const auto layout_state = shell_.CurrentWorkspaceLayout();
  if (!layout_state.has_value()) {
    return;
  }
  const WorkspaceLayout layout = *layout_state;
  if (layout.sidebar.h <= 0.0f) {
    return;
  }
  const auto lines = shell_.BuildGitSidebarLines();
  const auto list_layout = shell_.ComputeGitSidebarListLayout(layout.sidebar, lines.size());
  shell_.surface_.sidebar_scroll_row =
      RevealScrollableListIndex(list_layout, static_cast<int>(*selected_line));
}

void WorkspaceShell::SidebarCoordinator::RevealSelectedProblemsLine() {
  if (shell_.problems_sidebar_.entries.empty() ||
      shell_.problems_sidebar_.selected_index >= shell_.problems_sidebar_.entries.size()) {
    return;
  }
  const auto layout_state = shell_.CurrentWorkspaceLayout();
  if (!layout_state.has_value()) {
    return;
  }
  const WorkspaceLayout layout = *layout_state;
  if (layout.sidebar.h <= 0.0f) {
    return;
  }
  const auto list_layout = shell_.ComputeProblemsSidebarListLayout(
      layout.sidebar, shell_.problems_sidebar_.entries.size());
  shell_.surface_.sidebar_scroll_row = RevealScrollableListIndex(
      list_layout, static_cast<int>(shell_.problems_sidebar_.selected_index));
}

void WorkspaceShell::SidebarCoordinator::RevealSelectedPluginLine() {
  if (shell_.plugin_sidebar_.items.empty() ||
      shell_.plugin_sidebar_.selected_index >= shell_.plugin_sidebar_.items.size()) {
    return;
  }
  const auto layout_state = shell_.CurrentWorkspaceLayout();
  if (!layout_state.has_value()) {
    return;
  }
  const WorkspaceLayout layout = *layout_state;
  if (layout.sidebar.h <= 0.0f) {
    return;
  }
  const auto list_layout =
      shell_.ComputePluginSidebarListLayout(layout.sidebar, shell_.plugin_sidebar_.items.size());
  shell_.surface_.sidebar_scroll_row = RevealScrollableListIndex(
      list_layout, static_cast<int>(shell_.plugin_sidebar_.selected_index));
}

void WorkspaceShell::SidebarCoordinator::MoveGitSelection(int delta) {
  if (shell_.git_sidebar_.entries.empty() || delta == 0) {
    return;
  }
  const int current = static_cast<int>(shell_.git_sidebar_.selected_index);
  const int max_index = static_cast<int>(shell_.git_sidebar_.entries.size()) - 1;
  shell_.git_sidebar_.selected_index =
      static_cast<std::size_t>(std::clamp(current + delta, 0, max_index));
  RevealSelectedGitLine();
}

void WorkspaceShell::SidebarCoordinator::MoveProblemsSelection(int delta) {
  if (shell_.problems_sidebar_.entries.empty() || delta == 0) {
    return;
  }
  const int current = static_cast<int>(shell_.problems_sidebar_.selected_index);
  const int max_index = static_cast<int>(shell_.problems_sidebar_.entries.size()) - 1;
  shell_.problems_sidebar_.selected_index =
      static_cast<std::size_t>(std::clamp(current + delta, 0, max_index));
  RevealSelectedProblemsLine();
}

void WorkspaceShell::SidebarCoordinator::MovePluginSelection(int delta) {
  if (shell_.plugin_sidebar_.items.empty() || delta == 0) {
    return;
  }
  const int current = static_cast<int>(shell_.plugin_sidebar_.selected_index);
  const int max_index = static_cast<int>(shell_.plugin_sidebar_.items.size()) - 1;
  shell_.plugin_sidebar_.selected_index =
      static_cast<std::size_t>(std::clamp(current + delta, 0, max_index));
  RevealSelectedPluginLine();
}

bool WorkspaceShell::SidebarCoordinator::OpenGitEntry(std::size_t entry_index) {
  if (entry_index >= shell_.git_sidebar_.entries.size()) {
    return false;
  }
  const auto& entry = shell_.git_sidebar_.entries[entry_index];
  bool opened = false;
  if (entry.section == GitSidebarEntry::Section::Modified) {
    if (entry.conflicted) {
      opened = shell_.OpenGitConflictMerge(entry.path);
    } else {
      opened = shell_.OpenWorkingTreeComparison(entry.path, "HEAD", "HEAD");
    }
  } else {
    if (shell_.git_sidebar_.base_ref.empty()) {
      return false;
    }
    opened = shell_.OpenBranchHeadComparison(
        entry.path, shell_.git_sidebar_.base_ref,
        shell_.git_sidebar_.base_label.empty() ? shell_.git_sidebar_.base_ref
                                               : shell_.git_sidebar_.base_label,
        "HEAD", "HEAD");
  }
  if (opened && shell_.surface_.sidebar_visible &&
      shell_.surface_.sidebar_mode == SidebarMode::Git) {
    shell_.RequestSidebarRedraw();
  }
  return opened;
}

bool WorkspaceShell::SidebarCoordinator::OpenProblemItem() {
  if (shell_.problems_sidebar_.entries.empty() ||
      shell_.problems_sidebar_.selected_index >= shell_.problems_sidebar_.entries.size()) {
    return false;
  }
  const auto& entry = shell_.problems_sidebar_.entries[shell_.problems_sidebar_.selected_index];
  if (entry.diagnostic.path.empty()) {
    return false;
  }
  shell_.OpenFile(entry.diagnostic.path);
  shell_.text_viewport_.MoveCursorTo(entry.diagnostic.range.start.line,
                                     entry.diagnostic.range.start.column);
  if (shell_.surface_.sidebar_temporary) {
    shell_.RestorePreviousSidebar();
  }
  shell_.surface_.focus = FocusTarget::Editor;
  return true;
}

bool WorkspaceShell::SidebarCoordinator::OpenPluginItem() {
  if (shell_.plugin_sidebar_.items.empty() ||
      shell_.plugin_sidebar_.selected_index >= shell_.plugin_sidebar_.items.size()) {
    return false;
  }
  const auto& item = shell_.plugin_sidebar_.items[shell_.plugin_sidebar_.selected_index];
  std::string error_message;
  const bool confirmed = shell_.plugin_host_.ConfirmSidebarItem(
      shell_.surface_.sidebar_plugin_id, item, &error_message);
  if (!confirmed && !error_message.empty()) {
    shell_.plugin_sidebar_.error = std::move(error_message);
  }
  if (confirmed && shell_.surface_.sidebar_temporary) {
    shell_.RestorePreviousSidebar();
  }
  if (confirmed && !item.path.empty()) {
    shell_.surface_.focus = FocusTarget::Editor;
  }
  return confirmed;
}

bool WorkspaceShell::SidebarCoordinator::CanStageAllGitEntries() const {
  return std::any_of(shell_.git_sidebar_.entries.begin(), shell_.git_sidebar_.entries.end(),
                     [](const auto& entry) {
                       return entry.section == GitSidebarEntry::Section::Modified && !entry.staged;
                     });
}

bool WorkspaceShell::SidebarCoordinator::CanDiscardAllGitEntries() const {
  return std::any_of(shell_.git_sidebar_.entries.begin(), shell_.git_sidebar_.entries.end(),
                     [](const auto& entry) {
                       return entry.section == GitSidebarEntry::Section::Modified;
                     });
}

bool WorkspaceShell::SidebarCoordinator::StageAllGitEntries() {
  if (!CanStageAllGitEntries()) {
    return false;
  }
  std::vector<std::filesystem::path> affected_paths;
  affected_paths.reserve(shell_.git_sidebar_.entries.size());
  for (const auto& entry : shell_.git_sidebar_.entries) {
    if (entry.section != GitSidebarEntry::Section::Modified || entry.staged) {
      continue;
    }
    affected_paths.push_back(entry.path.lexically_normal());
  }
  std::sort(affected_paths.begin(), affected_paths.end());
  affected_paths.erase(std::unique(affected_paths.begin(), affected_paths.end()),
                       affected_paths.end());
  if (!project::GitStageAll(shell_.project_root_)) {
    return false;
  }
  for (const auto& path : affected_paths) {
    shell_.InvalidateEditorBlamePath(path);
  }
  RefreshProjectFiles();
  return true;
}

void WorkspaceShell::SidebarCoordinator::OpenDiscardAllGitPrompt() {
  if (!CanDiscardAllGitEntries()) {
    return;
  }
  shell_.OpenPromptSurface(PromptSurfaceState::Action::DiscardGitChanges,
                           PromptSurfaceState::Kind::Confirm, shell_.project_root_);
}

bool WorkspaceShell::SidebarCoordinator::DiscardAllGitEntries() {
  if (!CanDiscardAllGitEntries()) {
    return false;
  }

  std::string blocking_label;
  if (shell_.HasDirtyEditorTabsForPath(shell_.project_root_, &blocking_label)) {
    return false;
  }

  std::vector<std::filesystem::path> affected_paths;
  affected_paths.reserve(shell_.git_sidebar_.entries.size());
  for (const auto& entry : shell_.git_sidebar_.entries) {
    if (entry.section != GitSidebarEntry::Section::Modified) {
      continue;
    }
    affected_paths.push_back(entry.path.lexically_normal());
  }
  std::sort(affected_paths.begin(), affected_paths.end());
  affected_paths.erase(std::unique(affected_paths.begin(), affected_paths.end()),
                       affected_paths.end());

  if (!project::GitDiscardAll(shell_.project_root_)) {
    return false;
  }

  for (const auto& path : affected_paths) {
    shell_.InvalidateEditorBlamePath(path);
    ReconcileOpenTabsAfterPathDiscard(path);
  }
  RefreshProjectFiles();
  return true;
}

bool WorkspaceShell::SidebarCoordinator::StageGitEntry(std::size_t entry_index) {
  if (entry_index >= shell_.git_sidebar_.entries.size()) {
    return false;
  }
  const auto& entry = shell_.git_sidebar_.entries[entry_index];
  if (entry.section != GitSidebarEntry::Section::Modified || entry.staged) {
    return false;
  }
  if (!project::GitStagePath(shell_.project_root_, entry.path)) {
    return false;
  }
  shell_.InvalidateEditorBlamePath(entry.path);
  RefreshProjectFiles();
  return true;
}

bool WorkspaceShell::SidebarCoordinator::UnstageGitEntry(std::size_t entry_index) {
  if (entry_index >= shell_.git_sidebar_.entries.size()) {
    return false;
  }
  const auto& entry = shell_.git_sidebar_.entries[entry_index];
  if (entry.section != GitSidebarEntry::Section::Modified || !entry.staged) {
    return false;
  }
  if (!project::GitUnstagePath(shell_.project_root_, entry.path)) {
    return false;
  }
  shell_.InvalidateEditorBlamePath(entry.path);
  RefreshProjectFiles();
  return true;
}

bool WorkspaceShell::SidebarCoordinator::DiscardGitEntry(std::size_t entry_index) {
  if (entry_index >= shell_.git_sidebar_.entries.size()) {
    return false;
  }
  const auto& entry = shell_.git_sidebar_.entries[entry_index];
  if (entry.section != GitSidebarEntry::Section::Modified) {
    return false;
  }

  std::string blocking_label;
  if (shell_.HasDirtyEditorTabsForPath(entry.path, &blocking_label)) {
    return false;
  }
  if (!project::GitDiscardPath(shell_.project_root_, entry.path)) {
    return false;
  }
  shell_.InvalidateEditorBlamePath(entry.path);
  ReconcileOpenTabsAfterPathDiscard(entry.path);
  RefreshProjectFiles();
  return true;
}

void WorkspaceShell::SidebarCoordinator::ReconcileOpenTabsAfterPathDiscard(
    const std::filesystem::path& path) {
  const std::filesystem::path normalized_path = path.lexically_normal();
  std::error_code error;
  if (std::filesystem::exists(normalized_path, error) && !error) {
    shell_.ReloadCleanEditorTabsForPath(normalized_path);
    return;
  }
  shell_.CloseOpenTabsForPath(normalized_path);
}

void WorkspaceShell::ShowSidebarMode(SidebarMode mode, bool temporary) {
  SidebarCoordinator(*this).ShowMode(mode, temporary);
}

void WorkspaceShell::ShowTreeSidebar(const std::filesystem::path& root) {
  SidebarCoordinator(*this).ShowTree(root);
}

void WorkspaceShell::ShowSearchSidebar(std::string query, bool temporary) {
  SidebarCoordinator(*this).ShowSearch(std::move(query), temporary);
}

void WorkspaceShell::ShowProblemsSidebar() {
  SidebarCoordinator(*this).ShowProblems();
}

void WorkspaceShell::ShowGitSidebar() {
  SidebarCoordinator(*this).ShowGit();
}

bool WorkspaceShell::ShowPluginSidebar(std::string_view id, bool temporary) {
  return SidebarCoordinator(*this).ShowPlugin(id, temporary);
}

void WorkspaceShell::CloseSidebar() {
  SidebarCoordinator(*this).Close();
}

void WorkspaceShell::ToggleSidebar() {
  SidebarCoordinator(*this).Toggle();
}

void WorkspaceShell::RestorePreviousSidebar() {
  SidebarCoordinator(*this).RestorePrevious();
}

void WorkspaceShell::RefreshProjectFiles() {
  SidebarCoordinator(*this).RefreshProjectFiles();
}

void WorkspaceShell::RefreshGitSidebar() {
  SidebarCoordinator(*this).RefreshGit();
}

bool WorkspaceShell::RefreshProblemsSidebar() {
  return SidebarCoordinator(*this).RefreshProblems();
}

bool WorkspaceShell::RefreshPluginSidebar() {
  return SidebarCoordinator(*this).RefreshPlugin();
}

void WorkspaceShell::RevealSelectedGitSidebarLine() {
  SidebarCoordinator(*this).RevealSelectedGitLine();
}

void WorkspaceShell::RevealSelectedProblemsSidebarLine() {
  SidebarCoordinator(*this).RevealSelectedProblemsLine();
}

void WorkspaceShell::RevealSelectedTreeSidebarLine() {
  SidebarCoordinator(*this).RevealSelectedTreeLine();
}

void WorkspaceShell::RevealSelectedPluginSidebarLine() {
  SidebarCoordinator(*this).RevealSelectedPluginLine();
}

void WorkspaceShell::MoveGitSidebarSelection(int delta) {
  SidebarCoordinator(*this).MoveGitSelection(delta);
}

void WorkspaceShell::MoveProblemsSidebarSelection(int delta) {
  SidebarCoordinator(*this).MoveProblemsSelection(delta);
}

void WorkspaceShell::MovePluginSidebarSelection(int delta) {
  SidebarCoordinator(*this).MovePluginSelection(delta);
}

bool WorkspaceShell::OpenGitSidebarEntry(std::size_t entry_index) {
  return SidebarCoordinator(*this).OpenGitEntry(entry_index);
}

bool WorkspaceShell::OpenSelectedProblemSidebarItem() {
  return SidebarCoordinator(*this).OpenProblemItem();
}

bool WorkspaceShell::OpenSelectedPluginSidebarItem() {
  return SidebarCoordinator(*this).OpenPluginItem();
}

bool WorkspaceShell::CanStageAllGitSidebarEntries() const {
  return SidebarCoordinator(*const_cast<WorkspaceShell*>(this)).CanStageAllGitEntries();
}

bool WorkspaceShell::CanDiscardAllGitSidebarEntries() const {
  return SidebarCoordinator(*const_cast<WorkspaceShell*>(this)).CanDiscardAllGitEntries();
}

bool WorkspaceShell::StageAllGitSidebarEntries() {
  return SidebarCoordinator(*this).StageAllGitEntries();
}

void WorkspaceShell::OpenDiscardAllGitSidebarPrompt() {
  SidebarCoordinator(*this).OpenDiscardAllGitPrompt();
}

bool WorkspaceShell::DiscardAllGitSidebarEntries() {
  return SidebarCoordinator(*this).DiscardAllGitEntries();
}

bool WorkspaceShell::StageGitSidebarEntry(std::size_t entry_index) {
  return SidebarCoordinator(*this).StageGitEntry(entry_index);
}

bool WorkspaceShell::UnstageGitSidebarEntry(std::size_t entry_index) {
  return SidebarCoordinator(*this).UnstageGitEntry(entry_index);
}

bool WorkspaceShell::DiscardGitSidebarEntry(std::size_t entry_index) {
  return SidebarCoordinator(*this).DiscardGitEntry(entry_index);
}

void WorkspaceShell::ReconcileOpenTabsAfterPathDiscard(const std::filesystem::path& path) {
  SidebarCoordinator(*this).ReconcileOpenTabsAfterPathDiscard(path);
}

}  // namespace microide::workspace
