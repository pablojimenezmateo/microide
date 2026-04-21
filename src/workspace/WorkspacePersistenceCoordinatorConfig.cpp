#include "workspace/WorkspacePersistenceCoordinator.h"

#include <algorithm>
#include <cmath>

#include "util/TextFileIO.h"
#include "workspace/WorkspaceCommandParsing.h"
#include "workspace/WorkspacePersistenceFormat.h"
#include "workspace/WorkspaceProjectPresentation.h"
#include "workspace/WorkspaceSettingsRegistry.h"

namespace microide::workspace {

namespace {

void SetStoredSetting(std::vector<std::pair<std::string, std::string>>& settings,
                      std::string id,
                      std::string value) {
  auto it = std::find_if(settings.begin(), settings.end(),
                         [&](const auto& entry) { return entry.first == id; });
  if (it != settings.end()) {
    it->second = std::move(value);
    return;
  }
  settings.emplace_back(std::move(id), std::move(value));
}

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
  if (id == "editor.soft_tabs") {
    if (const auto* spec = FindBuiltinSettingSpec(id); spec != nullptr) {
      if (const auto parsed = ParseSettingValue(*spec, value); parsed.has_value()) {
        state.editor_preferences.soft_tabs = std::get<bool>(*parsed);
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

}  // namespace

void PersistenceCoordinator::RefreshAvailableColorschemeNames() {
  available_colorscheme_names_ = render::ListAvailableThemeNames();
}

bool PersistenceCoordinator::ApplyColorscheme(std::string_view name,
                                              bool persist,
                                              bool log_feedback) {
  (void)log_feedback;
  render::Theme loaded_theme;
  std::string resolved_name;
  std::string error;
  const std::string requested_name = name.empty() ? "default" : std::string(name);
  if (!render::LoadThemeByName(requested_name, loaded_theme, &resolved_name, &error)) {
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
  if (config_path.empty()) {
    return false;
  }

  const auto text = util::ReadTextFile(config_path);
  if (!text.has_value()) {
    return false;
  }

  PersistedUserConfigState state{
      .ui_scale = ui_scale_,
      .settings = {},
      .disabled_keybinding_ids = {},
  };
  if (!ParseUserConfigText(*text, &state)) {
    return false;
  }

  context_.user_settings.clear();
  context_.disabled_keybinding_ids = state.disabled_keybinding_ids;
  for (const auto& [id, value] : state.settings) {
    if (id == "ui.scale") {
      continue;
    }
    SetStoredSetting(context_.user_settings, id, value);
  }

  return ApplyUiScale(state.ui_scale, false, false);
}

void PersistenceCoordinator::SaveUserConfig() const {
  const std::filesystem::path config_path = operations_.user_config_path();
  if (config_path.empty()) {
    return;
  }

  util::WriteTextFileAtomically(
      config_path,
      SerializeUserConfig(PersistedUserConfigState{
          .ui_scale = ui_scale_,
          .settings = context_.user_settings,
          .disabled_keybinding_ids = context_.disabled_keybinding_ids,
      }));
}

bool PersistenceCoordinator::RestoreConfigState() {
  const std::filesystem::path config_path = operations_.config_state_path();
  if (config_path.empty()) {
    return false;
  }
  const auto text = util::ReadTextFile(config_path);
  if (!text.has_value()) {
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
  };
  if (!ParseProjectConfigText(*text, &persisted_state)) {
    return false;
  }

  auto& mutable_current = CurrentProjectState();
  mutable_current.settings.clear();
  mutable_current.editor_preferences.tab_size = persisted_state.editor_tab_size;
  mutable_current.editor_preferences.indent_width = persisted_state.editor_indent_width;
  mutable_current.editor_preferences.soft_tabs = persisted_state.editor_soft_tabs;
  mutable_current.project_base_color = persisted_state.project_base_color;
  mutable_current.sidebar_policies = RuntimeSidebarPolicies(persisted_state.sidebar_policies);
  for (const auto& [id, value] : persisted_state.settings) {
    if (ApplyCanonicalProjectSetting(mutable_current, id, value)) {
      continue;
    }
    SetStoredSetting(mutable_current.settings, id, value);
  }
  operations_.apply_editor_preferences_to_all_tabs();
  ApplyColorscheme(persisted_state.colorscheme_name, false, false);
  return true;
}

void PersistenceCoordinator::SaveConfigState() const {
  const auto& state = CurrentProjectState();
  if (state.root.empty()) {
    return;
  }

  const std::filesystem::path config_path = operations_.config_state_path();
  if (config_path.empty()) {
    return;
  }
  util::WriteTextFileAtomically(
      config_path,
      SerializeProjectConfig(PersistedProjectConfigState{
          .editor_tab_size = state.editor_preferences.tab_size,
          .editor_indent_width = state.editor_preferences.indent_width,
          .editor_soft_tabs = state.editor_preferences.soft_tabs,
          .colorscheme_name = state.active_colorscheme_name,
          .project_base_color = state.project_base_color.value_or(DefaultProjectBaseColor(state.root)),
          .settings = state.settings,
          .sidebar_policies = PersistedSidebarPolicies(state.sidebar_policies),
      }));
}

}  // namespace microide::workspace
