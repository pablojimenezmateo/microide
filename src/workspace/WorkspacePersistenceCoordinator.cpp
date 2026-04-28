#include "workspace/WorkspacePersistenceCoordinator.h"

#include <utility>

#include "workspace/ProjectCatalogService.h"
#include "workspace/WorkspaceProjectCatalogCoordinator.h"
#include "workspace/WorkspaceShell.h"

namespace microide::workspace {

PersistenceCoordinator::PersistenceCoordinator(WorkspaceContext& context,
                                               render::Theme& theme,
                                               std::vector<std::string>& available_colorscheme_names,
                                               float& ui_scale,
                                               Operations operations)
    : context_(context),
      theme_(theme),
      available_colorscheme_names_(available_colorscheme_names),
      ui_scale_(ui_scale),
      operations_(std::move(operations)) {}

ProjectWorkspaceState& PersistenceCoordinator::CurrentProjectState() {
  return context_.current_project_state;
}

const ProjectWorkspaceState& PersistenceCoordinator::CurrentProjectState() const {
  return context_.current_project_state;
}

PersistenceCoordinator WorkspaceShell::MakePersistenceCoordinator() {
  return PersistenceCoordinator(
      context_,
      theme_,
      available_colorscheme_names_,
      ui_scale_,
      PersistenceCoordinator::Operations{
          .config_state_path = [this]() { return ConfigStatePath(); },
          .user_config_path = [this]() { return UserConfigPath(); },
          .project_state_directory = [this]() { return ProjectStateDirectory(); },
          .persistence_service = &persistence_service_,
          .apply_editor_preferences_to_all_tabs = [this]() { ApplyEditorPreferencesToAllTabs(); },
          .apply_editor_preferences =
              [this](editor::TextViewport& viewport) { ApplyEditorPreferences(viewport); },
          .build_compare_tab_from_commit =
              [this](const std::filesystem::path& path,
                     const project::GitCommitEntry& commit,
                     std::size_t selected_row) {
                return BuildCompareTabEntry(path, commit, selected_row);
              },
          .build_compare_tab_from_state =
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
          .refresh_merge_tab_derived_state =
              [this](MergeTabState& merge_state) { RefreshMergeTabDerivedState(merge_state); },
          .find_editor_split_node =
              [this](PersistenceCoordinator::EditorSplitNode* node,
                     const std::vector<std::size_t>& path) {
                return FindEditorSplitNode(node, path);
              },
          .normalize_editor_split_tree =
              [this](TabEntry::EditorTabState& editor_state) {
                NormalizeEditorSplitTree(editor_state);
              },
          .find_editor_view_state =
              [this](const TabEntry::EditorTabState& editor_state, std::size_t leaf_id) {
                return FindEditorViewState(editor_state, leaf_id);
              },
          .editor_view_path =
              [this](const PersistenceCoordinator::EditorViewState& view_state) {
                return EditorViewPath(view_state);
              },
          .sync_active_editor_tab = [this]() { SyncActiveEditorTab(); },
          .resolve_project_root_input =
              [this](const std::filesystem::path& project_root) {
                return ResolveProjectRootInput(project_root);
              },
          .reset_project_catalog_to_welcome_state =
              [this]() { ResetProjectCatalogToWelcomeState(); },
          .restore_project_catalog_after_removal =
              [this](std::size_t preferred_index, bool activate_restored_tab) {
                return MakeProjectCatalogService().RestoreAfterRemoval(preferred_index,
                                                                       activate_restored_tab);
              },
          .ensure_active_project_visible = [this]() { EnsureActiveProjectVisible(); },
      });
}

}  // namespace microide::workspace
