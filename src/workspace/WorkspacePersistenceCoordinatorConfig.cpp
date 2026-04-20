#include "workspace/WorkspacePersistenceCoordinator.h"

#include <algorithm>
#include <cmath>

#include "util/TextFileIO.h"
#include "workspace/WorkspaceCommandParsing.h"
#include "workspace/WorkspacePersistenceFormat.h"
#include "workspace/WorkspaceProjectPresentation.h"

namespace microide::workspace {

void PersistenceCoordinator::RefreshAvailableColorschemeNames() {
  shell_.available_colorscheme_names_ = render::ListAvailableThemeNames();
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

  shell_.theme_ = loaded_theme;
  shell_.active_colorscheme_name_ = resolved_name.empty() ? requested_name : resolved_name;
  if (!shell_.project_base_color_.has_value() && !shell_.project_root_.empty()) {
    shell_.project_base_color_ = DefaultProjectBaseColor(shell_.project_root_);
  }
  if (shell_.project_base_color_.has_value()) {
    ApplyProjectAccent(shell_.theme_, *shell_.project_base_color_);
  }
  if (std::find(shell_.available_colorscheme_names_.begin(),
                shell_.available_colorscheme_names_.end(),
                shell_.active_colorscheme_name_) == shell_.available_colorscheme_names_.end()) {
    shell_.available_colorscheme_names_.push_back(shell_.active_colorscheme_name_);
    std::sort(shell_.available_colorscheme_names_.begin(),
              shell_.available_colorscheme_names_.end());
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

  shell_.ui_scale_ = std::clamp(scale, kMinUiScale, kMaxUiScale);
  if (persist) {
    SaveUserConfig();
  }
  return true;
}

bool PersistenceCoordinator::RestoreUserConfig() {
  const std::filesystem::path config_path = shell_.UserConfigPath();
  if (config_path.empty()) {
    return false;
  }

  const auto text = util::ReadTextFile(config_path);
  if (!text.has_value()) {
    return false;
  }

  PersistedUserConfigState state{.ui_scale = shell_.ui_scale_};
  if (!ParseUserConfigText(*text, &state)) {
    return false;
  }

  return ApplyUiScale(state.ui_scale, false, false);
}

void PersistenceCoordinator::SaveUserConfig() const {
  const std::filesystem::path config_path = shell_.UserConfigPath();
  if (config_path.empty()) {
    return;
  }

  util::WriteTextFileAtomically(
      config_path, SerializeUserConfig(PersistedUserConfigState{.ui_scale = shell_.ui_scale_}));
}

bool PersistenceCoordinator::RestoreConfigState() {
  const std::filesystem::path config_path = shell_.ConfigStatePath();
  if (config_path.empty()) {
    return false;
  }
  const auto text = util::ReadTextFile(config_path);
  if (!text.has_value()) {
    return false;
  }

  PersistedProjectConfigState state{
      .editor_tab_size = shell_.editor_preferences_.tab_size,
      .editor_indent_width = shell_.editor_preferences_.indent_width,
      .editor_soft_tabs = shell_.editor_preferences_.soft_tabs,
      .colorscheme_name = shell_.active_colorscheme_name_,
      .project_base_color = shell_.project_base_color_,
  };
  if (!ParseProjectConfigText(*text, &state)) {
    return false;
  }

  shell_.editor_preferences_.tab_size = state.editor_tab_size;
  shell_.editor_preferences_.indent_width = state.editor_indent_width;
  shell_.editor_preferences_.soft_tabs = state.editor_soft_tabs;
  shell_.project_base_color_ = state.project_base_color;
  shell_.ApplyEditorPreferencesToAllTabs();
  ApplyColorscheme(state.colorscheme_name, false, false);
  return true;
}

void PersistenceCoordinator::SaveConfigState() const {
  if (shell_.project_root_.empty()) {
    return;
  }

  const std::filesystem::path config_path = shell_.ConfigStatePath();
  if (config_path.empty()) {
    return;
  }
  util::WriteTextFileAtomically(
      config_path,
      SerializeProjectConfig(PersistedProjectConfigState{
          .editor_tab_size = shell_.editor_preferences_.tab_size,
          .editor_indent_width = shell_.editor_preferences_.indent_width,
          .editor_soft_tabs = shell_.editor_preferences_.soft_tabs,
          .colorscheme_name = shell_.active_colorscheme_name_,
          .project_base_color =
              shell_.project_base_color_.value_or(DefaultProjectBaseColor(shell_.project_root_)),
      }));
}

}  // namespace microide::workspace
