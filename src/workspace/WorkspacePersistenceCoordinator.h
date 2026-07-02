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
    // False when this window must not write the canonical project/workspace
    // session (a detached editor-tab child that shares the parent's project root
    // and would otherwise clobber the parent's saved session). Null / true means
    // persist normally.
    std::function<bool()> session_persist_enabled;
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
  bool RestoreSessionState();
  void SaveSessionState();
  bool RestoreWorkspaceSession();
  void SaveWorkspaceSession() const;

  // Tab-handoff seams (detach / reattach). A handoff payload is an ordinary
  // PersistedProjectSessionState written/read through PersistenceService, so the
  // dirty-buffer snapshot and compare/merge fields ride along for free and no new
  // on-disk format is introduced.
  //
  // Hydrate the current project from a session file at an arbitrary path (a
  // detached window's handoff), replacing the current tabs. Mirrors
  // RestoreSessionState but skips the canonical-path / debug-state coupling.
  bool RestoreSessionFromFile(const std::filesystem::path& session_path);
  // Append the handoff's tabs into the focused editor group without disturbing
  // the tabs already open (the reattach path). Activates the last appended tab.
  void AppendTabsFromSession(const PersistedProjectSessionState& session);
  // Load a handoff session file and append its tabs (the reattach entry point).
  // Returns false when the file cannot be read.
  bool AppendTabsFromFile(const std::filesystem::path& session_path);
  // Build a handoff payload: BuildPersistedProjectSession captures the whole
  // project (all groups, for project-detach); BuildSingleTabHandoff captures one
  // tab from one group (for editor/compare/merge tab-detach).
  PersistedProjectSessionState BuildPersistedProjectSession();
  PersistedProjectSessionState BuildSingleTabHandoff(std::size_t group_index,
                                                     std::size_t tab_index);
  // Write a handoff payload to `out_path` through the PersistedRecord writer.
  bool WriteHandoffFile(const std::filesystem::path& out_path,
                        const PersistedProjectSessionState& session) const;

 private:
  // Apply a decoded session payload to the current project (reset + rebuild tabs
  // + restore layout). Shared by RestoreSessionState and RestoreSessionFromFile.
  void ApplyPersistedSessionState(const PersistedProjectSessionState& session);
  // Seed a session payload's non-tab fields from the live project so a partial /
  // handoff record still yields a usable window.
  PersistedProjectSessionState SessionDefaultsFromCurrentState() const;
  // Rebuild one persisted tab into a runtime TabEntry (editor/compare/merge).
  // `should_eager_hydrate` opens an editor tab immediately vs leaving it deferred.
  std::optional<TabEntry> RebuildPersistedTab(const PersistedEditorTabState& persisted_tab,
                                              bool should_eager_hydrate);
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
