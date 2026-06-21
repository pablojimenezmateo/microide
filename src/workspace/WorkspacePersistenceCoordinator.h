#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string_view>

#include "render/Theme.h"
#include "workspace/PersistenceService.h"
#include "workspace/SettingsStore.h"
#include "workspace/WorkspaceContext.h"
#include "workspace/WorkspacePersistenceFormat.h"

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
  bool RestoreSessionState();
  void SaveSessionState();
  bool RestoreWorkspaceSession();
  void SaveWorkspaceSession() const;

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
