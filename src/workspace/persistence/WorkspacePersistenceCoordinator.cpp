#include "workspace/persistence/WorkspacePersistenceCoordinator.h"

#include <utility>

#include "workspace/services/ProjectCatalogService.h"
#include "workspace/SettingFlags.h"
#include "workspace/coordinators/WorkspaceProjectCatalogCoordinator.h"
#include "workspace/shell/WorkspaceShell.h"

namespace microide::workspace {

PersistenceCoordinator::PersistenceCoordinator(WorkspaceContext& context,
                                               render::Theme& theme,
                                               std::vector<std::string>& available_colorscheme_names,
                                               float& ui_scale,
                                               SettingsStore& settings_store,
                                               Operations operations)
    : context_(context),
      theme_(theme),
      available_colorscheme_names_(available_colorscheme_names),
      ui_scale_(ui_scale),
      settings_store_(settings_store),
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
      settings_store_,
      PersistenceCoordinator::Operations{
          .config_state_path = [this]() { return ConfigStatePath(); },
          .user_config_path = [this]() { return UserConfigPath(); },
          .project_state_directory = [this]() { return ProjectStateDirectory(); },
          .persistence_service = &persistence_service_,
          .apply_editor_preferences_to_all_tabs = [this]() { ApplyEditorPreferencesToAllTabs(); },
          .apply_editor_preferences =
              [this](editor::TextViewport& viewport) { ApplyEditorPreferences(viewport); },
          .apply_detected_indent_on_open =
              [this](editor::TextViewport& viewport) { ApplyDetectedIndentOnOpen(viewport); },
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
          .editor_view_path =
              [this](const TabEntry::EditorTabState& editor_state) {
                return EditorViewPath(editor_state);
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
          .ensure_active_project_visible = [this]() { tab_strip_chrome_.EnsureActiveProjectVisible(); },
          .debugger_enabled =
              [this]() { return SettingFlagEnabled(GetSettingValue("debug.enabled")); },
          .plugin_theme_names = [this]() { return theme_registry_.Names(); },
          .resolve_plugin_theme =
              [this](std::string_view id) { return theme_registry_.Resolve(id); },
      });
}

}  // namespace microide::workspace
