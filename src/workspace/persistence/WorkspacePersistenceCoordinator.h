#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string_view>

#include "render/Theme.h"
#include "workspace/persistence/PersistenceService.h"
#include "workspace/persistence/SettingsStore.h"
#include "workspace/WorkspaceContext.h"
#include "workspace/persistence/WorkspacePersistenceFormat.h"

namespace microide::workspace {

class PersistenceCoordinator {
 public:
  struct Operations {
    std::function<std::filesystem::path()> config_state_path;
    std::function<std::filesystem::path()> user_config_path;
    std::function<std::filesystem::path()> project_state_directory;
    const PersistenceService* persistence_service = nullptr;
    std::function<void()> apply_editor_preferences_to_all_tabs;
    std::function<void(editor::TextViewport&)> apply_editor_preferences;
    std::function<void(editor::TextViewport&)> apply_detected_indent_on_open;
    std::function<std::optional<TabEntry>(const std::filesystem::path&,
                                          const project::GitCommitEntry&,
                                          std::size_t)>
        build_compare_tab_from_commit;
    std::function<std::optional<TabEntry>(const std::filesystem::path&, const CompareTabState&)>
        build_compare_tab_from_state;
    std::function<std::optional<TabEntry>(const std::filesystem::path&,
                                          const std::filesystem::path&,
                                          const std::filesystem::path&,
                                          const std::filesystem::path&)>
        build_merge_tab_entry;
    std::function<void(MergeTabState&)> refresh_merge_tab_derived_state;
    std::function<std::filesystem::path(const TabEntry::EditorTabState&)> editor_view_path;
    std::function<void()> sync_active_editor_tab;
    std::function<std::filesystem::path(const std::filesystem::path&)> resolve_project_root_input;
    std::function<void()> reset_project_catalog_to_welcome_state;
    std::function<bool(std::size_t, bool)> restore_project_catalog_after_removal;
    std::function<void()> ensure_active_project_visible;
    // True when the `debug.enabled` setting is on; gates restoring the right-side
    // debug pane visible (so a pane left open in a prior session stays hidden when
    // the debugger feature is currently disabled).
    std::function<bool()> debugger_enabled;
    // Phase D plugin colour themes. `plugin_theme_names` lists contributed theme
    // ids for the colorscheme picker; `resolve_plugin_theme` derives the Theme for
    // a contributed id (nullopt when the id is not a plugin theme, so colorscheme
    // resolution falls back to built-in/filesystem `.microide` loading).
    std::function<std::vector<std::string>()> plugin_theme_names;
    std::function<std::optional<render::Theme>(std::string_view)> resolve_plugin_theme;
  };

  PersistenceCoordinator(WorkspaceContext& context,
                         render::Theme& theme,
                         std::vector<std::string>& available_colorscheme_names,
                         float& ui_scale,
                         SettingsStore& settings_store,
                         Operations operations);

  void RefreshAvailableColorschemeNames();
  bool ApplyColorscheme(std::string_view name, bool persist, bool log_feedback);
  bool ApplyUiScale(float scale, bool persist, bool log_feedback);
  bool RestoreUserConfig();
  void SaveUserConfig() const;
  bool RestoreConfigState();
  void SaveConfigState() const;
  // Recompute the current project's canonical editor preferences (tab size,
  // indent width, font size, soft tabs, wrap) and active colorscheme from the
  // layered settings store, resolving project-override → user-level default →
  // spec default. Applies the colorscheme live only when it actually changes.
  // This is the single materialization point so a user-level default applies to
  // any project that has no per-project override.
  void MaterializeCanonicalPreferences();
  bool RestoreSessionState();
  void SaveSessionState();
  bool RestoreWorkspaceSession();
  void SaveWorkspaceSession() const;

  // Test seam: override the dirty-buffer snapshot byte budget process-wide so a
  // test can exercise the over-budget omission without a 64 MiB buffer. 0 restores
  // the production budget. TD-2026-07-17A-083.
  static void SetMaxDirtySnapshotBytesForTesting(std::size_t bytes);

 private:
  std::filesystem::path SessionStatePath() const;
  std::filesystem::path WorkspaceSessionStatePath() const;
  std::filesystem::path DebugStatePath() const;
  void SaveDebugState();
  void RestoreDebugState();

  std::optional<PersistedEditorTabState> BuildPersistedCompareTabState(
      const TabEntry& tab) const;
  std::optional<PersistedEditorTabState> BuildPersistedMergeTabState(
      const TabEntry& tab) const;
  std::optional<PersistedEditorTabState> BuildPersistedEditorTabState(
      std::size_t tab_index,
      TabEntry& tab);

  ProjectWorkspaceState& CurrentProjectState();
  const ProjectWorkspaceState& CurrentProjectState() const;

  // Drop session-only setting overrides (context_.transient_setting_keys) from a
  // settings list before it is serialized, so `--set` / cold-start spec settings
  // never persist to the user's saved config.
  void StripTransientSettings(
      std::vector<std::pair<std::string, std::string>>& settings) const;

  WorkspaceContext& context_;
  render::Theme& theme_;
  std::vector<std::string>& available_colorscheme_names_;
  float& ui_scale_;
  SettingsStore& settings_store_;
  Operations operations_;
};

}  // namespace microide::workspace
