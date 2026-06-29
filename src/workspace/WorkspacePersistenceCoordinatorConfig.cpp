#include "workspace/BranchReviewStateBridge.h"
#include "workspace/WorkspacePersistenceCoordinator.h"

#include <algorithm>
#include <cmath>

#include "workspace/CommitWorkflowState.h"
#include "workspace/SettingsStore.h"
#include "workspace/WorkspaceCommandParsing.h"
#include "workspace/WorkspaceProjectPresentation.h"
#include "workspace/WorkspaceSettingsRegistry.h"

namespace microide::workspace {

namespace {

bool ApplyCanonicalProjectSetting(ProjectWorkspaceState& state,
                                  std::string_view id,
                                  std::string_view value) {
  if (id == "editor.tab_size") {
    if (const auto* spec = FindBuiltinSettingSpec(id); spec != nullptr) {
      if (const auto parsed = ParseSettingValue(*spec, value); parsed.has_value()) {
        state.editor_preferences.tab_size =
            static_cast<std::size_t>(std::clamp(std::get<int>(*parsed), 1, 16));
        return true;
      }
    }
    return false;
  }
  if (id == "editor.indent_width") {
    if (const auto* spec = FindBuiltinSettingSpec(id); spec != nullptr) {
      if (const auto parsed = ParseSettingValue(*spec, value); parsed.has_value()) {
        state.editor_preferences.indent_width =
            static_cast<std::size_t>(std::clamp(std::get<int>(*parsed), 1, 16));
        return true;
      }
    }
    return false;
  }
  if (id == "editor.font_size") {
    if (const auto* spec = FindBuiltinSettingSpec(id); spec != nullptr) {
      if (const auto parsed = ParseSettingValue(*spec, value); parsed.has_value()) {
        state.editor_preferences.font_size = std::clamp(std::get<int>(*parsed), 8, 32);
        return true;
      }
    }
    return false;
  }
  if (id == "editor.soft_tabs") {
    if (const auto* spec = FindBuiltinSettingSpec(id); spec != nullptr) {
      if (const auto parsed = ParseSettingValue(*spec, value); parsed.has_value()) {
        state.editor_preferences.soft_tabs = std::get<bool>(*parsed);
        return true;
      }
    }
    return false;
  }
  if (id == "editor.wrap") {
    if (const auto* spec = FindBuiltinSettingSpec(id); spec != nullptr) {
      if (const auto parsed = ParseSettingValue(*spec, value); parsed.has_value()) {
        const std::string mode = std::get<std::string>(*parsed);
        state.editor_preferences.soft_wrap = (mode == "word");
        return true;
      }
    }
    return false;
  }
  if (id == "editor.colorscheme") {
    state.active_colorscheme_name = std::string(value);
    return true;
  }
  return false;
}

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

void SyncCanonicalProjectSettings(std::vector<std::pair<std::string, std::string>>& settings,
                                  const ProjectWorkspaceState& state) {
  settings_layer::Upsert(settings, "editor.tab_size",
                         SerializeSettingValue(static_cast<int>(state.editor_preferences.tab_size)));
  settings_layer::Upsert(
      settings, "editor.indent_width",
      SerializeSettingValue(static_cast<int>(state.editor_preferences.indent_width)));
  settings_layer::Upsert(settings, "editor.font_size",
                         SerializeSettingValue(state.editor_preferences.font_size));
  settings_layer::Upsert(settings, "editor.soft_tabs",
                         SerializeSettingValue(state.editor_preferences.soft_tabs));
  settings_layer::Upsert(settings, "editor.wrap",
                         state.editor_preferences.soft_wrap ? "word" : "off");
  settings_layer::Upsert(settings, "editor.colorscheme",
                         SerializeSettingValue(state.active_colorscheme_name));
}

}  // namespace

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
      .editor_tab_size = current.editor_preferences.tab_size,
      .editor_indent_width = current.editor_preferences.indent_width,
      .editor_soft_tabs = current.editor_preferences.soft_tabs,
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
  mutable_current.editor_preferences.tab_size = persisted_state.editor_tab_size;
  mutable_current.editor_preferences.indent_width = persisted_state.editor_indent_width;
  mutable_current.editor_preferences.soft_tabs = persisted_state.editor_soft_tabs;
  mutable_current.editor_preferences.soft_wrap = false;
  mutable_current.project_base_color = persisted_state.project_base_color;
  mutable_current.sidebar_policies = RuntimeSidebarPolicies(persisted_state.sidebar_policies);
  mutable_current.sidebar.git.commit_workflow.loaded_persisted_draft = persisted_state.commit_draft;
  LoadBranchReviewStateFromPersisted(persisted_state.branch_review,
                                     &mutable_current.branch_review);
  for (const auto& [id, value] : persisted_state.settings) {
    ApplyCanonicalProjectSetting(mutable_current, id, value);
    settings_layer::Upsert(mutable_current.settings, id, value);
  }
  operations_.apply_editor_preferences_to_all_tabs();
  ApplyColorscheme(persisted_state.colorscheme_name, false, false);
  SyncCanonicalProjectSettings(mutable_current.settings, mutable_current);
  // The project layer was reloaded in place; rebuild the store's resolved index.
  settings_store_.Reindex();
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
      .editor_tab_size = state.editor_preferences.tab_size,
      .editor_indent_width = state.editor_preferences.indent_width,
      .editor_soft_tabs = state.editor_preferences.soft_tabs,
      .colorscheme_name = state.active_colorscheme_name,
      .project_base_color = state.project_base_color.value_or(DefaultProjectBaseColor(state.root)),
      .settings = state.settings,
      .sidebar_policies = PersistedSidebarPolicies(state.sidebar_policies),
      .commit_draft = {},
      .branch_review = ToPersistedBranchReviewState(state.branch_review),
  };
  if (!state.sidebar.git.commit_workflow.subject.text().empty() ||
      !CommitWorkflowBodyText(state.sidebar.git.commit_workflow.body).empty()) {
    persisted.commit_draft = PersistedCommitDraftState{
        .head_oid = state.sidebar.git.commit_workflow.draft_context.head_oid,
        .branch_name = state.sidebar.git.commit_workflow.draft_context.branch_name,
        .subject = state.sidebar.git.commit_workflow.subject.text(),
        .body = CommitWorkflowBodyText(state.sidebar.git.commit_workflow.body),
    };
  }
  SyncCanonicalProjectSettings(persisted.settings, state);
  StripTransientSettings(persisted.settings);
  operations_.persistence_service->SaveProjectConfig(config_path, persisted);
}

}  // namespace microide::workspace
