#include "workspace/WorkspacePersistenceCoordinator.h"

#include <algorithm>
#include <cmath>

#include "util/TextFileIO.h"
#include "workspace/WorkspaceCommandParsing.h"
#include "workspace/WorkspacePersistenceFormat.h"
#include "workspace/WorkspaceProjectPresentation.h"

namespace microide::workspace {

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

  PersistedUserConfigState state{.ui_scale = ui_scale_};
  if (!ParseUserConfigText(*text, &state)) {
    return false;
  }

  return ApplyUiScale(state.ui_scale, false, false);
}

void PersistenceCoordinator::SaveUserConfig() const {
  const std::filesystem::path config_path = operations_.user_config_path();
  if (config_path.empty()) {
    return;
  }

  util::WriteTextFileAtomically(
      config_path, SerializeUserConfig(PersistedUserConfigState{.ui_scale = ui_scale_}));
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
  };
  if (!ParseProjectConfigText(*text, &persisted_state)) {
    return false;
  }

  auto& mutable_current = CurrentProjectState();
  mutable_current.editor_preferences.tab_size = persisted_state.editor_tab_size;
  mutable_current.editor_preferences.indent_width = persisted_state.editor_indent_width;
  mutable_current.editor_preferences.soft_tabs = persisted_state.editor_soft_tabs;
  mutable_current.project_base_color = persisted_state.project_base_color;
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
      }));
}

}  // namespace microide::workspace
