#include "workspace/BranchReviewStateBridge.h"
#include "workspace/WorkspacePersistenceCoordinator.h"

#include <algorithm>
#include <cmath>
#include <variant>

#include "workspace/CommitWorkflowState.h"
#include "workspace/SettingsStore.h"
#include "workspace/WorkspaceCommandParsing.h"
#include "workspace/WorkspaceProjectPresentation.h"
#include "workspace/WorkspaceSettingsRegistry.h"

namespace microide::workspace {

namespace {

std::vector<PersistedSidebarViewPolicy> PersistedSidebarPolicies(
    const std::vector<SidebarViewPolicy>& policies) {
  std::vector<PersistedSidebarViewPolicy> persisted;
  persisted.reserve(policies.size());
  for (const SidebarViewPolicy& policy : policies) {
    persisted.push_back(PersistedSidebarViewPolicy{
        .view_id = policy.view_id,
        .hidden = policy.hidden,
        .order = policy.order,
    });
  }
  return persisted;
}

std::vector<SidebarViewPolicy> RuntimeSidebarPolicies(
    const std::vector<PersistedSidebarViewPolicy>& policies) {
  std::vector<SidebarViewPolicy> runtime;
  runtime.reserve(policies.size());
  for (const PersistedSidebarViewPolicy& policy : policies) {
    runtime.push_back(SidebarViewPolicy{
        .view_id = policy.view_id,
        .hidden = policy.hidden,
        .order = policy.order,
    });
  }
  return runtime;
}

// One-time migration for project configs written by builds that auto-persisted the
// resolved canonical editor preferences into EVERY project's settings vector on save
// (the old SyncCanonicalProjectSettings). Now that a per-project override wins over
// the user-level default, such an auto-synced entry left at the spec default would
// silently shadow the "set as default" feature. Drop canonical editor entries whose
// value equals the spec default so user defaults take effect; genuine non-default
// per-project overrides are preserved untouched. The cleaned layer is rewritten by
// the next SaveConfigState, so this self-heals after one load.
void DropDefaultValuedCanonicalOverrides(
    std::vector<std::pair<std::string, std::string>>& settings) {
  for (std::string_view id : {"editor.tab_size", "editor.indent_width", "editor.font_size",
                              "editor.soft_tabs", "editor.wrap"}) {
    const SettingSpec* spec = FindBuiltinSettingSpec(id);
    const std::string* stored = settings_layer::Find(settings, id);
    if (spec == nullptr || stored == nullptr) {
      continue;
    }
    const auto parsed = ParseSettingValue(*spec, *stored);
    if (!parsed.has_value() ||
        SerializeSettingValue(*parsed) != SerializeSettingValue(DefaultSettingValue(*spec))) {
      continue;  // Absent, unparseable, or a genuine non-default override: keep it.
    }
    settings_layer::Erase(settings, id);
  }
}

}  // namespace

void PersistenceCoordinator::MaterializeCanonicalPreferences() {
  ProjectWorkspaceState& state = CurrentProjectState();
  const auto resolve = [&](std::string_view id) -> SettingValue {
    const SettingSpec* spec = FindBuiltinSettingSpec(id);
    if (const std::string* stored = settings_store_.Resolve(id); stored != nullptr && spec != nullptr) {
      if (auto parsed = ParseSettingValue(*spec, *stored); parsed.has_value()) {
        return *parsed;
      }
    }
    return spec != nullptr ? DefaultSettingValue(*spec) : SettingValue{std::string{}};
  };

  for (std::string_view id : {"editor.tab_size", "editor.indent_width", "editor.font_size",
                              "editor.soft_tabs", "editor.wrap"}) {
    ApplyCanonicalEditorPreference(state.editor_preferences, id, resolve(id));
  }
  // Colorscheme is deliberately NOT materialized here: its enum spec only
  // validates "default", real theme names (light, plugin/filesystem themes) never
  // pass ParseSettingValue, so the theme-change command sets active_colorscheme_name
  // directly and persists via the dedicated colorscheme_name field, not the layered
  // store. RestoreConfigState applies that field explicitly.
}

void PersistenceCoordinator::RefreshAvailableColorschemeNames() {
  available_colorscheme_names_ = render::ListAvailableThemeNames();
  if (operations_.plugin_theme_names) {
    for (std::string& name : operations_.plugin_theme_names()) {
      available_colorscheme_names_.push_back(std::move(name));
    }
    std::sort(available_colorscheme_names_.begin(), available_colorscheme_names_.end());
    available_colorscheme_names_.erase(
        std::unique(available_colorscheme_names_.begin(), available_colorscheme_names_.end()),
        available_colorscheme_names_.end());
  }
}

bool PersistenceCoordinator::ApplyColorscheme(std::string_view name,
                                              bool persist,
                                              bool log_feedback) {
  (void)log_feedback;
  render::Theme loaded_theme;
  std::string resolved_name;
  std::string error;
  const std::string requested_name = name.empty() ? "default" : std::string(name);
  // Plugin-contributed themes take precedence over built-in/filesystem themes:
  // a plugin theme id is namespaced (`"<plugin>.<id>"`) so it cannot collide with
  // a built-in name, and resolving it derives a full Theme from its style map.
  bool resolved_from_plugin = false;
  if (operations_.resolve_plugin_theme) {
    if (auto plugin_theme = operations_.resolve_plugin_theme(requested_name)) {
      loaded_theme = *plugin_theme;
      resolved_name = requested_name;
      resolved_from_plugin = true;
    }
  }
  if (!resolved_from_plugin &&
      !render::LoadThemeByName(requested_name, loaded_theme, &resolved_name, &error)) {
    return false;
  }

  auto& state = CurrentProjectState();
  theme_ = loaded_theme;
  state.active_colorscheme_name = resolved_name.empty() ? requested_name : resolved_name;
  if (!state.project_base_color.has_value() && !state.root.empty()) {
    state.project_base_color = DefaultProjectBaseColor(state.root);
  }
  if (state.project_base_color.has_value()) {
    ApplyProjectAccent(theme_, *state.project_base_color);
  }
  if (std::find(available_colorscheme_names_.begin(),
                available_colorscheme_names_.end(),
                state.active_colorscheme_name) == available_colorscheme_names_.end()) {
    available_colorscheme_names_.push_back(state.active_colorscheme_name);
    std::sort(available_colorscheme_names_.begin(), available_colorscheme_names_.end());
  }

  if (persist) {
    SaveConfigState();
  }
  return true;
}

bool PersistenceCoordinator::ApplyUiScale(float scale,
                                          bool persist,
                                          bool log_feedback) {
  (void)log_feedback;
  if (!std::isfinite(scale)) {
    return false;
  }

  ui_scale_ = std::clamp(scale, kMinUiScale, kMaxUiScale);
  if (persist) {
    SaveUserConfig();
  }
  return true;
}

bool PersistenceCoordinator::RestoreUserConfig() {
  const std::filesystem::path config_path = operations_.user_config_path();
  if (config_path.empty() || operations_.persistence_service == nullptr) {
    return false;
  }

  PersistedUserConfigState state{
      .ui_scale = ui_scale_,
      .settings = {},
      .disabled_keybinding_ids = {},
      .disabled_plugin_ids = {},
  };
  if (!operations_.persistence_service->LoadUserConfig(config_path, &state)) {
    return false;
  }

  context_.user_settings.clear();
  context_.disabled_keybinding_ids = state.disabled_keybinding_ids;
  context_.disabled_plugin_ids = state.disabled_plugin_ids;
  for (const auto& [id, value] : state.settings) {
    settings_layer::Upsert(context_.user_settings, id, value);
  }

  const bool applied_scale = ApplyUiScale(state.ui_scale, false, false);
  if (applied_scale) {
    // Keep overlay rows in sync with the canonical ui_scale field.
    settings_layer::Upsert(context_.user_settings, "ui.scale", SerializeSettingValue(ui_scale_));
  }
  // The user layer was reloaded in place; rebuild the store's resolved index.
  settings_store_.Reindex();
  return applied_scale;
}

void PersistenceCoordinator::StripTransientSettings(
    std::vector<std::pair<std::string, std::string>>& settings) const {
  if (context_.transient_setting_keys.empty()) {
    return;
  }
  settings.erase(std::remove_if(settings.begin(), settings.end(),
                                [&](const auto& entry) {
                                  return context_.transient_setting_keys.count(entry.first) != 0;
                                }),
                 settings.end());
}

void PersistenceCoordinator::SaveUserConfig() const {
  const std::filesystem::path config_path = operations_.user_config_path();
  if (config_path.empty() || operations_.persistence_service == nullptr) {
    return;
  }

  PersistedUserConfigState state{
      .ui_scale = ui_scale_,
      .settings = context_.user_settings,
      .disabled_keybinding_ids = context_.disabled_keybinding_ids,
      .disabled_plugin_ids = context_.disabled_plugin_ids,
  };
  StripTransientSettings(state.settings);
  operations_.persistence_service->SaveUserConfig(config_path, state);
}

bool PersistenceCoordinator::RestoreConfigState() {
  const std::filesystem::path config_path = operations_.config_state_path();
  if (config_path.empty() || operations_.persistence_service == nullptr) {
    return false;
  }

  const auto& current = CurrentProjectState();
  PersistedProjectConfigState persisted_state{
      .colorscheme_name = current.active_colorscheme_name,
      .project_base_color = current.project_base_color,
      .settings = {},
      .sidebar_policies = {},
      .commit_draft = std::nullopt,
      .branch_review = {},
  };
  if (!operations_.persistence_service->LoadProjectConfig(config_path, &persisted_state)) {
    return false;
  }

  auto& mutable_current = CurrentProjectState();
  mutable_current.settings.clear();
  mutable_current.project_base_color = persisted_state.project_base_color;
  mutable_current.sidebar_policies = RuntimeSidebarPolicies(persisted_state.sidebar_policies);
  mutable_current.sidebar.git.commit_workflow.loaded_persisted_draft = persisted_state.commit_draft;
  LoadBranchReviewStateFromPersisted(persisted_state.branch_review,
                                     &mutable_current.branch_review);
  // The project settings vector holds only explicit per-project overrides; the
  // canonical editor preferences are materialized from the store (project override →
  // user default → spec default) below. The colorscheme is the exception: it persists
  // in its own typed field (colorscheme_name) and is applied explicitly after
  // materialization, because its enum spec only validates "default" and real theme
  // names never round-trip through the layered store.
  for (const auto& [id, value] : persisted_state.settings) {
    settings_layer::Upsert(mutable_current.settings, id, value);
  }
  // Strip auto-synced default-valued canonical editor entries left by older builds
  // so they can no longer shadow the user-level "set as default" (see helper).
  DropDefaultValuedCanonicalOverrides(mutable_current.settings);
  // The project layer was reloaded in place; rebuild the store's resolved index
  // before materializing so the resolved values reflect the loaded overrides.
  settings_store_.Reindex();
  MaterializeCanonicalPreferences();
  operations_.apply_editor_preferences_to_all_tabs();
  // Colorscheme persists via its dedicated typed field (see
  // MaterializeCanonicalPreferences), so apply it explicitly here.
  ApplyColorscheme(persisted_state.colorscheme_name, /*persist=*/false, /*log_feedback=*/false);
  return true;
}

void PersistenceCoordinator::SaveConfigState() const {
  const auto& state = CurrentProjectState();
  if (state.root.empty()) {
    return;
  }

  const std::filesystem::path config_path = operations_.config_state_path();
  if (config_path.empty() || operations_.persistence_service == nullptr) {
    return;
  }
  PersistedProjectConfigState persisted{
      .colorscheme_name = state.active_colorscheme_name,
      .project_base_color = state.project_base_color.value_or(DefaultProjectBaseColor(state.root)),
      .settings = state.settings,
      .sidebar_policies = PersistedSidebarPolicies(state.sidebar_policies),
      .commit_draft = {},
      .branch_review = ToPersistedBranchReviewState(state.branch_review),
  };
  if (!state.sidebar.git.commit_workflow.subject.text().empty() ||
      !state.sidebar.git.commit_workflow.BodyText().empty()) {
    persisted.commit_draft = MakePersistedCommitDraft(state.sidebar.git.commit_workflow);
  }
  StripTransientSettings(persisted.settings);
  operations_.persistence_service->SaveProjectConfig(config_path, persisted);
}

}  // namespace microide::workspace
