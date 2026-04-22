#include "workspace/WorkspacePathMutationCoordinator.h"

#include <filesystem>
#include <utility>

#include "project/FileOperationService.h"
#include "workspace/WorkspacePathUtils.h"

namespace microide::workspace {

PathMutationCoordinator::PathMutationCoordinator(WorkspaceContext& context, Operations operations)
    : context_(context), operations_(std::move(operations)) {}

ProjectWorkspaceState& PathMutationCoordinator::CurrentProjectState() {
  return context_.current_project_state;
}

const ProjectWorkspaceState& PathMutationCoordinator::CurrentProjectState() const {
  return context_.current_project_state;
}

void PathMutationCoordinator::ConfirmPromptSurface(WorkspaceShell::DirtyPathResolution resolution) {
  if (!context_.prompts.surface_visible) {
    return;
  }

  const PromptSurfaceState state = context_.prompts.surface;
  if (state.selected_button == 1) {
    operations_.dismiss_prompt_surface(true);
    return;
  }

  if (state.kind == PromptSurfaceState::Kind::TextInput) {
    if (state.input.text.empty()) {
      return;
    }

    std::filesystem::path typed_path(state.input.text);
    if (typed_path.is_absolute()) {
      return;
    }

    std::filesystem::path destination;
    if (state.action == PromptSurfaceState::Action::RenamePath) {
      if (!ResolveDirtyTabsForPath(state.path, DirtyPromptState::Kind::RenamePath, resolution)) {
        return;
      }
      destination = (state.path.parent_path() / typed_path).lexically_normal();
    } else {
      destination = (state.path / typed_path).lexically_normal();
    }

    if (!PathEqualsOrWithin(destination, CurrentProjectState().root)) {
      return;
    }

    project::FileOperationResult result;
    if (state.action == PromptSurfaceState::Action::CreateFile) {
      result = project::FileOperationService::CreateFile(destination);
    } else if (state.action == PromptSurfaceState::Action::CreateDirectory) {
      result = project::FileOperationService::CreateDirectory(destination);
    } else {
      result = project::FileOperationService::RenamePath(state.path, destination);
    }

    if (!result.ok) {
      return;
    }

    if (context_.prompts.dirty_visible) {
      operations_.dismiss_dirty_prompt(false);
    }
    operations_.dismiss_prompt_surface(false);
    if (state.action == PromptSurfaceState::Action::CreateFile) {
      RefreshProjectViewsAfterMutation(result.resulting_path);
      operations_.open_file(result.resulting_path);
      return;
    }
    if (state.action == PromptSurfaceState::Action::CreateDirectory) {
      RefreshProjectViewsAfterMutation(result.resulting_path);
      CurrentProjectState().surface.focus = FocusTarget::Sidebar;
      return;
    }

    RetargetOpenTabsForRename(state.path, result.resulting_path,
                              resolution != WorkspaceShell::DirtyPathResolution::Discard);
    RetargetDiagnosticsForRename(state.path, result.resulting_path);
    operations_.clear_editor_blame();
    RefreshProjectViewsAfterMutation(result.resulting_path);
    CurrentProjectState().surface.focus = FocusTarget::Sidebar;
    return;
  }

  if (state.action == PromptSurfaceState::Action::DiscardGitChanges) {
    const bool discarded = operations_.discard_all_git_sidebar_entries();
    operations_.dismiss_prompt_surface(discarded ? false : true);
    if (discarded) {
      CurrentProjectState().surface.focus = FocusTarget::Sidebar;
    }
    return;
  }

  if (!ResolveDirtyTabsForPath(state.path, DirtyPromptState::Kind::DeletePath, resolution)) {
    return;
  }

  const project::FileOperationResult result = project::FileOperationService::TrashPath(state.path);
  if (!result.ok) {
    return;
  }

  const std::filesystem::path parent = state.path.parent_path();
  if (context_.prompts.dirty_visible) {
    operations_.dismiss_dirty_prompt(false);
  }
  operations_.dismiss_prompt_surface(false);
  CloseOpenTabsForPath(state.path);
  ClearDiagnosticsForPath(state.path);
  operations_.clear_editor_blame();
  RefreshProjectViewsAfterMutation(parent);
  CurrentProjectState().surface.focus = FocusTarget::Sidebar;
}

PathMutationCoordinator WorkspaceShell::MakePathMutationCoordinator() {
  return PathMutationCoordinator(
      context_,
      PathMutationCoordinator::Operations{
          .dismiss_prompt_surface = [this](bool restore_focus) { DismissPromptSurface(restore_focus); },
          .dismiss_dirty_prompt = [this](bool restore_focus) { DismissDirtyPrompt(restore_focus); },
          .open_file = [this](const std::filesystem::path& path) { OpenFile(path); },
          .clear_editor_blame = [this]() { ClearEditorBlame(); },
          .discard_all_git_sidebar_entries = [this]() { return DiscardAllGitSidebarEntries(); },
          .refresh_project_files = [this]() { RefreshProjectFiles(); },
          .reveal_selected_tree_sidebar_line = [this]() { RevealSelectedTreeSidebarLine(); },
          .refresh_project_search = [this]() { RefreshProjectSearch(); },
          .active_tab_is_editor = [this]() { return ActiveTabIsEditor(); },
          .sync_active_editor_tab = [this]() { SyncActiveEditorTab(); },
          .save_tab = [this](std::size_t index) { return SaveTab(index); },
          .close_tab = [this](std::size_t index) { CloseTab(index); },
          .refresh_problems_sidebar = [this]() { RefreshProblemsSidebar(); },
          .queue_editor_hover_refresh = [this]() { QueueEditorHoverRefresh(); },
          .request_editor_surface_redraw = [this]() { RequestEditorSurfaceRedraw(); },
          .apply_editor_preferences =
              [this](editor::TextViewport& viewport) { ApplyEditorPreferences(viewport); },
          .find_editor_view =
              [this](const TabEntry::EditorTabState& editor_state, std::size_t leaf_id) {
                return FindEditorView(editor_state, leaf_id);
              },
          .find_editor_view_state =
              [this](TabEntry::EditorTabState& editor_state, std::size_t leaf_id) {
                return FindEditorViewState(editor_state, leaf_id);
              },
          .editor_view_path =
              [this](const TabEntry::EditorTabState::EditorViewState& view_state) {
                return EditorViewPath(view_state);
              },
          .normalize_editor_split_tree =
              [this](TabEntry::EditorTabState& editor_state) {
                NormalizeEditorSplitTree(editor_state);
              },
          .editor_leaf_order =
              [this](const TabEntry::EditorTabState& editor_state) {
                return EditorLeafOrder(editor_state);
              },
          .sync_active_editor_tab_metadata = [this]() { SyncActiveEditorTabMetadata(); },
          .reset_caret_blink = [this]() { ResetCaretBlink(); },
          .invalidate_editor_blame_path =
              [this](const std::filesystem::path& path) { InvalidateEditorBlamePath(path); },
          .has_dirty_editor_tabs_for_path =
              [this](const std::filesystem::path& path, std::string* blocking_label) {
                return HasDirtyEditorTabsForPath(path, blocking_label);
              },
          .reload_clean_editor_tabs_for_path =
              [this](const std::filesystem::path& path) { ReloadCleanEditorTabsForPath(path); },
          .build_compare_tab_entry =
              [this](const std::filesystem::path& path, const CompareTabState& compare_state) {
                return BuildCompareTabEntry(path, compare_state);
              },
          .build_merge_tab_entry =
              [this](const std::filesystem::path& base_path,
                     const std::filesystem::path& incoming_path,
                     const std::filesystem::path& current_path,
                     const std::filesystem::path& output_path) {
                return BuildMergeTabEntry(base_path, incoming_path, current_path, output_path);
              },
      });
}

}  // namespace microide::workspace
