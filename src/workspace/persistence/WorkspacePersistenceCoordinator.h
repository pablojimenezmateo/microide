#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

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
    std::function<std::vector<MergeTrackedConflict>(compare::MergeModel&, const std::vector<std::string>&)>
        merge_conflicts_for_result;
    std::function<std::filesystem::path(const TabEntry::EditorTabState&)> editor_view_path;
    std::function<void()> sync_active_editor_tab;
    std::function<std::filesystem::path(const std::filesystem::path&)> resolve_project_root_input;
    std::function<void()> reset_project_catalog_to_welcome_state;
    std::function<bool(std::size_t, bool)> restore_project_catalog_after_removal;
    std::function<void()> ensure_active_project_visible;
    std::function<bool()> debugger_enabled;
    std::function<TerminalLayoutRecord()> capture_terminal_layout;
    std::function<void(const PersistedProjectSessionState&)> session_saved;
    std::function<std::vector<std::string>()> host_view_collapsed;
    std::function<void(const std::vector<std::string>&)> restore_host_view_collapsed;
    std::function<std::vector<std::string>()> plugin_theme_names;
    std::function<std::optional<render::Theme>(std::string_view)> resolve_plugin_theme;
  };

  PersistenceCoordinator(WorkspaceContext& context,
                         render::Theme& theme,
                         std::vector<std::string>& available_colorscheme_names,
                         render::ThemeNameCatalog& theme_name_catalog,
                         float& ui_scale,
                         SettingsStore& settings_store,
                         Operations operations);

  void RefreshAvailableColorschemeNames();
  const std::vector<std::string>& AvailableColorschemeNames() const { return available_colorscheme_names_; }
  bool ApplyColorscheme(std::string_view name, bool persist, bool log_feedback);
  bool ApplyUiScale(float scale, bool persist, bool log_feedback);
  bool RestoreUserConfig();
  void SaveUserConfig() const;
  bool RestoreConfigState();
  void SaveConfigState() const;
  void MaterializeCanonicalPreferences();
  bool RestoreSessionState();
  void SaveSessionState();
  bool SaveTerminalLayout(const std::filesystem::path& root, TerminalLayoutRecord layout);
  bool RestoreWorkspaceSession();
  void SaveWorkspaceSession() const;

  static void SetMaxDirtySnapshotBytesForTesting(std::size_t bytes);

 private:
  std::filesystem::path SessionStatePath() const;
  std::filesystem::path WorkspaceSessionStatePath() const;
  std::filesystem::path DebugStatePath() const;
  void SaveDebugState();
  void RestoreDebugState();

  using RestoredDirtyViews = std::unordered_map<std::string, editor::TextViewport>;
  void RestoreEditorGroups(const PersistedProjectSessionState& persisted_session);
  std::optional<TabEntry> RebuildTab(const PersistedEditorTabState& persisted_tab,
                                     bool should_eager_hydrate, RestoredDirtyViews& dirty_views);
  editor::TextViewport RestoreDirtyView(const PersistedEditorTabState& persisted_tab,
                                        const std::filesystem::path& view_path,
                                        RestoredDirtyViews& dirty_views);
  void RestoreSessionLayout(PersistedProjectSessionState& persisted_session);

  std::optional<TabEntry> RestoreCompareTab(const PersistedEditorTabState& persisted_tab,
                                            const std::filesystem::path& root,
                                            const editor::TextViewport* unsaved_right);
  std::optional<TabEntry> RestoreMergeTab(const PersistedEditorTabState& persisted_tab,
                                          const std::filesystem::path& root);
  void RestoreTypedMergeResult(const PersistedEditorTabState& persisted_tab,
                               const std::filesystem::path& output_path, MergeTabState& merge);
  std::optional<PersistedEditorTabState> BuildPersistedCompareTabState(
      const TabEntry& tab) const;
  std::optional<PersistedEditorTabState> BuildPersistedMergeTabState(
      const TabEntry& tab) const;
  std::optional<PersistedEditorTabState> BuildPersistedEditorTabState(
      std::size_t tab_index,
      TabEntry& tab);

  ProjectWorkspaceState& CurrentProjectState();
  const ProjectWorkspaceState& CurrentProjectState() const;

  void StripTransientSettings(
      std::vector<std::pair<std::string, std::string>>& settings) const;

  WorkspaceContext& context_;
  render::Theme& theme_;
  std::vector<std::string>& available_colorscheme_names_;
  render::ThemeNameCatalog& theme_name_catalog_;
  float& ui_scale_;
  SettingsStore& settings_store_;
  Operations operations_;
};

}
