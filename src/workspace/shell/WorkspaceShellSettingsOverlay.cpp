#include "workspace/shell/WorkspaceShell.h"

#include "workspace/services/TerminalPanelService.h"

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <limits>
#include <unordered_map>

#include "workspace/WorkspaceStartupOptions.h"
#include "workspace/WorkspaceUiText.h"
#include "workspace/render/RenderViewModelBuilder.h"
#include "workspace/SettingFlags.h"
#include "workspace/actions/WorkspaceActionCoordinator.h"
#include "workspace/registries/WorkspaceCommandRegistry.h"
#include "workspace/registries/WorkspaceKeybindingRegistry.h"
#include "workspace/persistence/WorkspacePersistenceCoordinator.h"
#include "workspace/registries/WorkspaceSettingsRegistry.h"
#include "util/Parse.h"

#ifndef MICROIDE_VERSION
#define MICROIDE_VERSION "dev"
#endif

namespace microide::workspace {

constexpr const char* kPluginToggleRowPrefix = "plugin.toggle:";

namespace {

bool IsCanonicalPreferenceId(std::string_view id) {
  return id == "editor.tab_size" || id == "editor.indent_width" ||
         id == "editor.font_size" || id == "editor.soft_tabs" || id == "editor.wrap" ||
         id == "editor.colorscheme";
}

std::vector<SettingsOverlayRow> BuildPluginToggleRows(const plugin::PluginHost& host) {
  std::vector<SettingsOverlayRow> rows;
  const std::vector<plugin::PluginHost::LoadedPlugin> plugins = host.LoadedPlugins();
  if (plugins.empty()) {
    SettingsOverlayRow row;
    row.id = "plugin.empty";
    row.label = "No plugins installed";
    row.value_display = "—";
    row.description = "Install plugins into ~/.config/microide/plugins/<plugin-id>/init.lua.";
    row.detail = "User / plugin";
    row.scope_label = "User";
    row.group = "Plugins";
    row.type = SettingType::String;
    row.scope = SettingScope::User;
    row.control_kind = SettingsControlKind::None;
    row.resettable = false;
    row.editable = false;
    rows.push_back(std::move(row));
    return rows;
  }
  for (const plugin::PluginHost::LoadedPlugin& plugin : plugins) {
    SettingsOverlayRow row;
    row.id = std::string(kPluginToggleRowPrefix) + plugin.id;
    row.label = plugin.id;
    row.value = plugin.enabled ? "true" : "false";
    row.value_display = row.value;
    row.description = plugin.enabled ? "Plugin is enabled." : "Plugin is disabled.";
    row.detail = "User / plugin";
    row.scope_label = "User";
    row.group = "Plugins";
    row.type = SettingType::Bool;
    row.scope = SettingScope::User;
    row.control_kind = SettingsControlKind::Checkbox;
    row.resettable = false;
    row.editable = true;
    rows.push_back(std::move(row));
  }
  return rows;
}

void AppendStartupHelpRows(std::vector<HelpAboutRow>& rows,
                           const WorkspaceStartupOptions& startup_options) {
  if (startup_options.safe_mode) {
    rows.push_back(HelpAboutRow{
        .label = "Startup mode",
        .detail =
            "Safe mode: plugins disabled, plugin syntax disabled, session restore skipped",
    });
    return;
  }
  if (startup_options.disable_plugins) {
    rows.push_back(HelpAboutRow{
        .label = "Startup mode",
        .detail = "Plugins disabled: user-scope plugins and plugin syntax are not loaded",
    });
  }
}

std::unordered_map<ActionId, std::string> BuildActionChordLookup(
    const std::vector<ResolvedKeybinding>& keybindings) {
  std::unordered_map<ActionId, std::string> chord_by_action;
  for (const ResolvedKeybinding& binding : keybindings) {
    if (binding.key == SDLK_UNKNOWN) {
      continue;
    }
    const std::string chord = FormatBindingKeys(binding);
    if (chord.empty()) {
      continue;
    }
    std::string& line = chord_by_action[binding.action];
    if (line.find(chord) == std::string::npos) {
      AppendHintSegment(line, chord);
    }
  }
  return chord_by_action;
}

std::vector<HelpAboutRow> BuildHelpRows(const WorkspaceStartupOptions& startup_options,
                                        const std::vector<ResolvedKeybinding>& keybindings) {
  std::vector<HelpAboutRow> rows;
  rows.push_back(HelpAboutRow{.label = "microide", .detail = "Desktop IDE"});
  rows.push_back(HelpAboutRow{.label = "Version", .detail = MICROIDE_VERSION});
  AppendStartupHelpRows(rows, startup_options);
  rows.push_back(HelpAboutRow{.label = "Git sidebar (focused)",
                              .detail = "Enter default view · d diff · s stage · u unstage · "
                                         "x discard (confirm) · m merge · c commit · r refresh · "
                                         "o open file"});
  rows.push_back(HelpAboutRow{.label = "Search sidebar (focused)",
                              .detail = "Enter open match · j/k move · / query · = replace · "
                                         "i include · x exclude · r rerun · R replace all (confirm) · "
                                         "c count all"});
  const std::unordered_map<ActionId, std::string> chord_by_action =
      BuildActionChordLookup(keybindings);
  for (const ActionSpec& spec : WorkspaceCommandSpecs()) {
    if (spec.command_name.empty()) {
      continue;
    }
    std::string detail =
        std::string(spec.command_usage.empty() ? spec.command_name : spec.command_usage);
    if (const auto it = chord_by_action.find(spec.id); it != chord_by_action.end()) {
      detail = it->second + "  ·  " + detail;
    }
    rows.push_back(HelpAboutRow{
        .label = std::string(spec.label),
        .detail = std::move(detail),
    });
  }
  return rows;
}

enum class SettingStepDirection {
  Forward,
  Backward,
};

int ClampSteppedInt(int value, int min_value, int max_value, int step, SettingStepDirection direction) {
  if (min_value > max_value) {
    std::swap(min_value, max_value);
  }
  const std::int64_t delta = direction == SettingStepDirection::Forward ? step : -step;
  const std::int64_t stepped = static_cast<std::int64_t>(value) + delta;
  return static_cast<int>(std::clamp<std::int64_t>(stepped, min_value, max_value));
}

std::string NextSettingValue(const SettingSpec& spec,
                             std::string_view current,
                             SettingStepDirection direction,
                             std::span<const std::string> colorschemes) {
  switch (spec.type) {
    case SettingType::Bool:
      return SettingFlagEnabled(std::optional<std::string>(std::string(current))) ? "false"
                                                                                  : "true";
    case SettingType::Enum: {
      std::vector<std::string_view> values;
      if (spec.enum_source == SettingEnumSource::Colorschemes && !colorschemes.empty()) {
        values.assign(colorschemes.begin(), colorschemes.end());
      } else {
        for (const SettingEnumValue& value : spec.enum_values) {
          values.push_back(value.value);
        }
      }
      if (values.empty()) {
        return std::string(current);
      }
      const auto at = std::find(values.begin(), values.end(), current);
      if (at == values.end()) {
        return std::string(direction == SettingStepDirection::Forward ? values.front() : values.back());
      }
      const std::size_t i = static_cast<std::size_t>(at - values.begin());
      return std::string(direction == SettingStepDirection::Forward
                             ? values[(i + 1) % values.size()]
                             : values[(i + values.size() - 1) % values.size()]);
    }
    case SettingType::Int: {
      const auto parsed = util::ParseInt(current);
      int value = parsed.value_or(spec.default_int);
      const bool bounded = spec.min_int != std::numeric_limits<int>::min() ||
                           spec.max_int != std::numeric_limits<int>::max();
      if (bounded) {
        value = ClampSteppedInt(value, spec.min_int, spec.max_int, spec.int_step, direction);
      } else {
        value = ClampSteppedInt(value, spec.default_int - 20, spec.default_int + 20, 1, direction);
      }
      return std::to_string(value);
    }
    case SettingType::Float: {
      const auto parsed = util::ParseFloat(current);
      const float stepped = parsed.value_or(spec.default_float) +
                            (direction == SettingStepDirection::Forward ? 0.25f : -0.25f);
      return SerializeSettingValue(std::clamp(stepped, spec.min_float, spec.max_float));
    }
    case SettingType::String:
      return std::string(current);
  }
  return std::string(current);
}

}

bool WorkspaceShell::SetSettingValue(std::string_view id, std::string value, bool persist) {
  return WriteSettingValue(id, std::move(value), false, persist);
}

bool WorkspaceShell::SetSettingAsUserDefault(std::string_view id, std::string value, bool persist) {
  return WriteSettingValue(id, std::move(value), true, persist);
}

bool WorkspaceShell::WriteSettingValue(std::string_view id, std::string value,
                                       bool as_user_default, bool persist) {
  const std::optional<SettingScope> scope = FindSettingScope(id, plugin_runtime_.Host());
  if (!scope.has_value()) {
    return false;
  }
  const SettingSpec* builtin = FindBuiltinSettingSpec(id);
  std::optional<SettingValue> parsed_builtin_value;
  if (builtin != nullptr) {
    parsed_builtin_value = ParseSettingValue(*builtin, value);
    if (!parsed_builtin_value.has_value()) {
      return false;
    }
    if (builtin->enum_source == SettingEnumSource::Colorschemes) {
      MakePersistenceCoordinator().RefreshAvailableColorschemeNames();
      if (std::find(available_colorscheme_names_.begin(), available_colorscheme_names_.end(), value) ==
          available_colorscheme_names_.end()) {
        return false;
      }
    }
  }

  std::string stored_value =
      builtin != nullptr ? SerializeSettingValue(*parsed_builtin_value) : std::move(value);

  const bool write_user_layer = *scope == SettingScope::User || as_user_default;

  if (persist) {
    context_.transient_setting_keys.erase(std::string(id));
  } else {
    context_.transient_setting_keys.insert(std::string(id));
  }

  if (write_user_layer) {
    if (id == "ui.scale") {
      if (const float* parsed = std::get_if<float>(&*parsed_builtin_value); parsed != nullptr) {
        MakePersistenceCoordinator().ApplyUiScale(*parsed, false, false);
      }
    }
    settings_store_.SetUser(id, std::move(stored_value));
    ApplyCanonicalPreferenceSideEffects(id);
    if (persist) {
      MakePersistenceCoordinator().SaveUserConfig();
    }
  } else {
    settings_store_.SetProject(id, std::move(stored_value));
    ApplyCanonicalPreferenceSideEffects(id);
    if (persist) {
      MakePersistenceCoordinator().SaveConfigState();
    }
  }
  ApplyLiveSettings();
  NoteLayoutInputsChanged();
  RequestPreparedFrame();
  return true;
}

void WorkspaceShell::ApplyStartupSettingOverrides() {
  for (const auto& [id, value] : startup_options_.setting_overrides) {
    if (!SetSettingValue(id, value, false)) {
      SDL_Log("--set: unknown setting or invalid value for \"%s\"", id.c_str());
    }
  }
}

void WorkspaceShell::RefreshSettingsOverlayCatalog() {
  if (!settings_overlay_service_.Visible()) {
    return;
  }
  MakePersistenceCoordinator().RefreshAvailableColorschemeNames();
  settings_overlay_service_.RebuildSettingsRows(AllSettingInfos(plugin_runtime_.Host(), available_colorscheme_names_),
                                                context_.user_settings,
                                                context_.current_project_state.settings,
                                                BuildPluginToggleRows(plugin_runtime_.Host()));
  settings_overlay_service_.RebuildHelpRows(BuildHelpRows(startup_options_, ResolvedKeybindings()));
}

void WorkspaceShell::TogglePluginEnabled(std::string_view plugin_id) {
  auto& disabled = context_.disabled_plugin_ids;
  const auto it = std::find(disabled.begin(), disabled.end(), plugin_id);
  if (it == disabled.end()) {
    disabled.emplace_back(plugin_id);
  } else {
    disabled.erase(it);
  }
  MakePersistenceCoordinator().SaveUserConfig();
  ReloadPluginsForCurrentProject(PluginReloadRequest{});
  RefreshSettingsOverlayCatalog();
  RequestPreparedFrame();
}

void WorkspaceShell::ApplyLiveSettings() {
  MaybeStartControlChannel();

  const std::uint64_t settings_revision = settings_store_.Revision();
  if (settings_revision == last_applied_settings_revision_) {
    return;
  }
  last_applied_settings_revision_ = settings_revision;

  if (const auto family = GetSettingValue("editor.font_family"); family.has_value()) {
    if (text_renderer_.SetFontFamily(*family)) {
      NoteLayoutInputsChanged();
    }
  }
  if (const int scrollback =
          std::clamp(util::ParseIntOr(GetSettingValue("terminal.scrollback_lines"), 2000), 200,
                     100000);
      scrollback != last_applied_terminal_scrollback_) {
    last_applied_terminal_scrollback_ = scrollback;
    for (auto& tab : context_.current_project_state.terminal_tabs) {
      if (tab == nullptr) {
        continue;
      }
      for (auto& pane : tab->panes) {
        if (pane != nullptr) {
          pane->session.SetMaxScrollbackLines(static_cast<std::size_t>(scrollback));
        }
      }
    }
  }
  if (const auto value = GetSettingValue("ui.show_status_bar"); value.has_value()) {
    layout_mode_service_.SetStatusBarVisible(SettingFlagEnabled(value));
  }
  if (const auto value = GetSettingValue("ui.layout_compact_breakpoint_px"); value.has_value()) {
    if (const auto parsed = util::ParseFloat(*value); parsed.has_value()) {
      layout_mode_service_.SetCompactBreakpointPx(std::clamp(*parsed, 600.0f, 2000.0f));
    }
  }
  if (const auto value = GetSettingValue("ui.layout_mode"); value.has_value()) {
    if (*value == "compact") {
      layout_mode_service_.SetUserOverride(LayoutModeInputs::Override::Compact);
    } else if (*value == "regular") {
      layout_mode_service_.SetUserOverride(LayoutModeInputs::Override::Regular);
    } else {
      layout_mode_service_.SetUserOverride(LayoutModeInputs::Override::Auto);
    }
  }
  if (const bool follow =
          SettingFlagEnabled(GetSettingValue("project.follow_out_of_root_symlinks"), false);
      follow != last_applied_follow_out_of_root_symlinks_) {
    last_applied_follow_out_of_root_symlinks_ = follow;
    context_.current_project_state.directory_tree.SetFollowOutOfRootSymlinks(follow);
    context_.current_project_state.file_index.SetFollowOutOfRootSymlinks(follow);
    context_.current_project_state.directory_tree.Refresh();
    RequestFileIndexRefresh();
    RequestPreparedFrame();
  }
  if (std::string files_exclude = GetSettingValue("project.files_exclude").value_or(std::string());
      files_exclude != last_applied_files_exclude_) {
    last_applied_files_exclude_ = files_exclude;
    std::vector<std::string> globs = ParseExcludeGlobs(files_exclude);
    context_.current_project_state.directory_tree.SetExcludeGlobs(globs);
    context_.current_project_state.file_index.SetExcludeGlobs(globs);
    context_.current_project_state.directory_tree.Refresh();
    RequestFileIndexRefresh();
    StartFileIndexWatcherForCurrentProject();
    RequestPreparedFrame();
  }

  LiveSettingsEditorSnapshot snapshot;
  snapshot.project_root = context_.current_project_state.root.lexically_normal();
  snapshot.tab_size = context_.current_project_state.editor_preferences.tab_size;
  snapshot.indent_width = context_.current_project_state.editor_preferences.indent_width;
  snapshot.font_size = context_.current_project_state.editor_preferences.font_size;
  snapshot.soft_tabs = context_.current_project_state.editor_preferences.soft_tabs;
  snapshot.soft_wrap = context_.current_project_state.editor_preferences.soft_wrap;
  const WorkspaceLanguageContract::SettingGetter get_setting = [this](std::string_view id) {
    return GetSettingValue(id);
  };
  snapshot.language_contract_inputs = WorkspaceLanguageContract::SettingsFingerprint(get_setting);
  snapshot.save_trim_trailing_whitespace =
      GetSettingValue("editor.save.trim_trailing_whitespace");
  snapshot.save_ensure_final_newline =
      GetSettingValue("editor.save.ensure_final_newline");
  const bool snapshot_matches =
      last_live_settings_editor_snapshot_.has_value() &&
      last_live_settings_editor_snapshot_->project_root == snapshot.project_root &&
      last_live_settings_editor_snapshot_->tab_size == snapshot.tab_size &&
      last_live_settings_editor_snapshot_->indent_width == snapshot.indent_width &&
      last_live_settings_editor_snapshot_->font_size == snapshot.font_size &&
      last_live_settings_editor_snapshot_->soft_tabs == snapshot.soft_tabs &&
      last_live_settings_editor_snapshot_->soft_wrap == snapshot.soft_wrap &&
      last_live_settings_editor_snapshot_->language_contract_inputs ==
          snapshot.language_contract_inputs &&
      last_live_settings_editor_snapshot_->save_trim_trailing_whitespace ==
          snapshot.save_trim_trailing_whitespace &&
      last_live_settings_editor_snapshot_->save_ensure_final_newline ==
          snapshot.save_ensure_final_newline;
  if (!snapshot_matches) {
    const bool contract_affecting_changed =
        !last_live_settings_editor_snapshot_.has_value() ||
        last_live_settings_editor_snapshot_->language_contract_inputs !=
            snapshot.language_contract_inputs;
    if (contract_affecting_changed) {
      language_contract_.Refresh(plugin_runtime_.Host(), get_setting);
    }
    ApplyEditorPreferencesToAllTabs(contract_affecting_changed);
    last_live_settings_editor_snapshot_ = snapshot;
  }

  lsp_service_.ReconcileFeatureSettings();
}

bool WorkspaceShell::ResetSettingValue(std::string_view id) {
  const auto info = FindSettingInfo(id, plugin_runtime_.Host());
  if (!info.has_value()) {
    return false;
  }
  return ResetSettingInScope(id, info->scope);
}

bool WorkspaceShell::ResetSettingInScope(std::string_view id, SettingScope scope) {
  if (scope == SettingScope::User) {
    settings_store_.ResetUser(id);
  } else {
    settings_store_.ResetProject(id);
  }
  if (id == "ui.scale") {
    if (const auto parsed = util::ParseFloat(GetSettingValue("ui.scale").value_or("1.0"));
        parsed.has_value()) {
      MakePersistenceCoordinator().ApplyUiScale(*parsed, false, false);
    }
  }
  ApplyCanonicalPreferenceSideEffects(id);
  if (scope == SettingScope::User) {
    MakePersistenceCoordinator().SaveUserConfig();
  } else {
    MakePersistenceCoordinator().SaveConfigState();
  }
  ApplyLiveSettings();
  NoteLayoutInputsChanged();
  RequestPreparedFrame();
  return true;
}

void WorkspaceShell::ApplyCanonicalPreferenceSideEffects(std::string_view id) {
  if (!IsCanonicalPreferenceId(id)) {
    return;
  }
  auto& coordinator = MakePersistenceCoordinator();
  coordinator.MaterializeCanonicalPreferences();
  if (id == "editor.colorscheme") {
    const std::string* stored = settings_store_.Resolve(id);
    coordinator.ApplyColorscheme(stored != nullptr ? *stored : std::string("default"),
                                 false, false);
  }
}

bool WorkspaceShell::LineNumbersEnabled() const {
  return SettingFlagEnabled(GetSettingValue("editor.line_numbers"), true);
}

bool WorkspaceShell::SettingWritesToUserDefault(std::string_view id) const {
  const SettingSpec* spec = FindBuiltinSettingSpec(id);
  if (spec == nullptr || spec->scope != SettingScope::Project) {
    return false;
  }
  const bool project_override = settings_store_.FindInProjectLayer(id) != nullptr;
  const bool user_default = settings_store_.FindInUserLayer(id) != nullptr;
  return !project_override && user_default;
}

bool WorkspaceShell::WriteSettingRespectingScope(std::string_view id, std::string value) {
  if (SettingWritesToUserDefault(id)) {
    return SetSettingAsUserDefault(id, std::move(value));
  }
  return SetSettingValue(id, std::move(value));
}

void WorkspaceShell::ToggleSettingScope(std::string_view id) {
  const SettingSpec* spec = FindBuiltinSettingSpec(id);
  if (spec == nullptr || spec->scope != SettingScope::Project) {
    return;
  }
  const std::string current =
      GetSettingValue(id).value_or(SerializeSettingValue(DefaultSettingValue(*spec)));
  const bool project_override = settings_store_.FindInProjectLayer(id) != nullptr;
  const bool user_default = settings_store_.FindInUserLayer(id) != nullptr;
  const bool target_project = project_override || !user_default;
  if (target_project) {
    SetSettingAsUserDefault(id, current);
    ResetSettingInScope(id, SettingScope::Project);
  } else {
    SetSettingValue(id, current);
  }
}

const std::vector<std::string>& WorkspaceShell::CachedFontFamilies() {
  if (!font_families_cached_) {
    cached_font_families_ = text_renderer_.AvailableFontFamilies();
    font_families_cached_ = true;
  }
  return cached_font_families_;
}

void WorkspaceShell::BeginSettingValueEdit(std::string_view id) {
  settings_overlay_service_.SetFocusedPane(SettingsPane::Values);
  const SettingSpec* spec = FindBuiltinSettingSpec(id);
  if (spec != nullptr && spec->suggests_fonts) {
    settings_overlay_service_.BeginFontValueEdit(std::string(id), CachedFontFamilies());
  } else {
    settings_overlay_service_.BeginValueEdit(std::string(id), GetSettingValue(id).value_or(""));
  }
  InvalidateCursorKindFingerprint();
  RequestPreparedFrame();
}

void WorkspaceShell::MoveSettingsFontPicker(int delta) {
  if (!settings_overlay_service_.EditingFonts()) {
    return;
  }
  settings_overlay_service_.MovePickerHighlight(delta);
  InvalidateCursorKindFingerprint();
  RequestPreparedFrame();
}

void WorkspaceShell::ApplySettingsFontPickerIndex(int dropdown_index) {
  if (!settings_overlay_service_.EditingFonts()) {
    return;
  }
  const std::string id = settings_overlay_service_.EditingRowId();
  if (dropdown_index == settings_overlay_service_.PickerChooseFileIndex()) {
    settings_overlay_service_.CancelValueEdit();
    OpenNativeFontFilePicker(id);
    InvalidateCursorKindFingerprint();
    RequestPreparedFrame();
    return;
  }
  const auto& filtered = settings_overlay_service_.FilteredFontFamilies();
  if (dropdown_index < 0 || dropdown_index >= static_cast<int>(filtered.size())) {
    return;
  }
  const std::string value(filtered[dropdown_index]);
  settings_overlay_service_.CancelValueEdit();
  WriteSettingRespectingScope(id, value);
  RefreshSettingsOverlayCatalog();
  InvalidateCursorKindFingerprint();
  RequestPreparedFrame();
}

void WorkspaceShell::CommitSettingValueEdit() {
  if (!settings_overlay_service_.EditingValue()) {
    return;
  }
  const std::string id = settings_overlay_service_.EditingRowId();
  if (settings_overlay_service_.EditingFonts()) {
    const int highlight = settings_overlay_service_.PickerHighlight();
    if (highlight >= 0) {
      ApplySettingsFontPickerIndex(highlight);
      return;
    }
    const std::string typed = settings_overlay_service_.ValueEditText();
    settings_overlay_service_.CancelValueEdit();
    if (!typed.empty()) {
      WriteSettingRespectingScope(id, typed);
    }
    RefreshSettingsOverlayCatalog();
    InvalidateCursorKindFingerprint();
    RequestPreparedFrame();
    return;
  }
  const std::string value = settings_overlay_service_.ValueEditText();
  settings_overlay_service_.CancelValueEdit();
  WriteSettingRespectingScope(id, value);
  RefreshSettingsOverlayCatalog();
  InvalidateCursorKindFingerprint();
  RequestPreparedFrame();
}

void WorkspaceShell::CancelSettingValueEdit() {
  if (!settings_overlay_service_.EditingValue()) {
    return;
  }
  settings_overlay_service_.CancelValueEdit();
  InvalidateCursorKindFingerprint();
  RequestPreparedFrame();
}

void WorkspaceShell::StepSetting(std::string_view id, bool forward) {
  if (id.rfind(kPluginToggleRowPrefix, 0) == 0) {
    TogglePluginEnabled(id.substr(std::string_view(kPluginToggleRowPrefix).size()));
    return;
  }
  const SettingStepDirection direction =
      forward ? SettingStepDirection::Forward : SettingStepDirection::Backward;
  if (const SettingSpec* spec = FindBuiltinSettingSpec(id); spec != nullptr) {
    const std::string current =
        GetSettingValue(id).value_or(SerializeSettingValue(DefaultSettingValue(*spec)));
    WriteSettingRespectingScope(id, NextSettingValue(*spec, current, direction, available_colorscheme_names_));
    return;
  }
  const auto info = FindSettingInfo(id, plugin_runtime_.Host());
  if (!info.has_value()) {
    return;
  }
  const std::string current =
      GetSettingValue(id).value_or(SerializeSettingValue(info->default_value));
  if (info->type == SettingType::Bool) {
    const bool on = SettingFlagEnabled(std::optional<std::string>(current));
    SetSettingValue(id, on ? "false" : "true");
    return;
  }
  if (info->type == SettingType::Enum && !info->enum_values.empty()) {
    const auto& values = info->enum_values;
    std::size_t index = 0;
    for (std::size_t i = 0; i < values.size(); ++i) {
      if (values[i] == current) {
        index = i;
        break;
      }
    }
    const std::size_t next = forward ? (index + 1) % values.size()
                                     : (index + values.size() - 1) % values.size();
    SetSettingValue(id, values[next]);
  }
}

void WorkspaceShell::ScrollSettingsOverlayRows(int delta) {
  const auto layout_state = CurrentWorkspaceLayout();
  if (!layout_state.has_value()) {
    return;
  }
  const SettingsOverlayViewModel vm = RenderViewModelBuilder(context_).BuildSettingsOverlay(
      *layout_state, settings_overlay_service_, text_renderer_);
  settings_overlay_service_.SetScrollRow(
      std::clamp(settings_overlay_service_.ScrollRow() + delta, 0, vm.max_scroll));
}

void WorkspaceShell::EnsureSettingsSelectionVisible() {
  const auto layout_state = CurrentWorkspaceLayout();
  if (!layout_state.has_value()) {
    return;
  }
  {
    const SettingsOverlayViewModel vm = RenderViewModelBuilder(context_).BuildSettingsOverlay(
        *layout_state, settings_overlay_service_, text_renderer_);
    const int selected_category = settings_overlay_service_.SelectedCategory();
    int category_scroll = vm.category_scroll_row;
    if (selected_category < category_scroll) {
      category_scroll = selected_category;
    } else if (selected_category >= category_scroll + vm.category_visible_rows) {
      category_scroll = selected_category - vm.category_visible_rows + 1;
    }
    settings_overlay_service_.SetCategoryScrollRow(
        std::clamp(category_scroll, 0, vm.category_max_scroll));
  }
  const int selected = settings_overlay_service_.SelectedRow();
  if (selected < settings_overlay_service_.ScrollRow()) {
    settings_overlay_service_.SetScrollRow(selected);
  }
  const SettingsOverlayViewModel vm = RenderViewModelBuilder(context_).BuildSettingsOverlay(
      *layout_state, settings_overlay_service_, text_renderer_);
  std::size_t selected_index = vm.rows.size();
  for (std::size_t i = 0; i < vm.rows.size(); ++i) {
    if (vm.rows[i].row_in_category == selected) {
      selected_index = i;
      break;
    }
  }
  if (selected_index == vm.rows.size()) {
    return;
  }
  const float pane_h = vm.right_pane_rect.h;
  float acc = 0.0f;
  int target_scroll = static_cast<int>(selected_index);
  for (int i = static_cast<int>(selected_index); i >= 0; --i) {
    acc += vm.rows[static_cast<std::size_t>(i)].advance_height;
    if (acc > pane_h) {
      break;
    }
    target_scroll = i;
  }
  const int scroll = std::clamp(std::max(vm.scroll_row, target_scroll), 0, vm.max_scroll);
  settings_overlay_service_.SetScrollRow(scroll);
}

void WorkspaceShell::OpenSettingsOverlay() {
  settings_overlay_service_.OpenSettings();
  RefreshSettingsOverlayCatalog();
  InvalidateCursorKindFingerprint();
  RequestPreparedFrame();
}

void WorkspaceShell::OpenHelpAboutOverlay() {
  settings_overlay_service_.OpenHelpAbout();
  RefreshSettingsOverlayCatalog();
  InvalidateCursorKindFingerprint();
  RequestPreparedFrame();
}

void WorkspaceShell::CloseSettingsOverlay() {
  settings_overlay_service_.Close();
  if (context_.interaction_state.drag_target == DragTarget::SettingsScrollbar ||
      context_.interaction_state.drag_target == DragTarget::SettingsCategoryScrollbar ||
      context_.interaction_state.drag_target == DragTarget::SettingsPickerScrollbar) {
    context_.interaction_state.drag_target = DragTarget::None;
  }
  InvalidateCursorKindFingerprint();
  RequestPreparedFrame();
}

bool WorkspaceShell::HandleSettingsOverlayButtonDown(const SDL_Event& event,
                                                     const WorkspaceLayout& layout) {
  if (!settings_overlay_service_.Visible()) {
    return false;
  }

  const SettingsOverlayViewModel vm =
      RenderViewModelBuilder(context_).BuildSettingsOverlay(layout, settings_overlay_service_,
                                                            text_renderer_);
  const float mx = event.button.x;
  const float my = event.button.y;

  if (!SettingsOverlayContains(vm, mx, my)) {
    CloseSettingsOverlay();
    return true;
  }

  const bool left = event.button.button == SDL_BUTTON_LEFT;
  const bool right = event.button.button == SDL_BUTTON_RIGHT;
  if (!left && !right) {
    return true;
  }
  const auto begin_scrollbar_drag = [&](const ScrollbarGeometry& bar, DragTarget target,
                                        int max_scroll, auto&& set_scroll) {
    context_.interaction_state.drag_target = target;
    context_.interaction_state.drag_scrollbar_offset =
        ScrollbarGrabOffset(bar, my, true);
    set_scroll(std::clamp(static_cast<int>(std::lround(ScrollUnitsForPointer(
                              bar, my, context_.interaction_state.drag_scrollbar_offset))),
                          0, max_scroll));
    RequestPreparedFrame();
  };
  if (left && vm.value_picker.visible) {
    if (vm.value_picker.scrollbar.has_value() &&
        Contains(vm.value_picker.scrollbar->track, mx, my)) {
      begin_scrollbar_drag(*vm.value_picker.scrollbar, DragTarget::SettingsPickerScrollbar,
                           vm.value_picker.max_scroll,
                           [this](int row) { settings_overlay_service_.SetPickerScroll(row); });
      return true;
    }
    for (const SettingsPickerItemViewModel& item : vm.value_picker.items) {
      if (Contains(item.rect, mx, my)) {
        ApplySettingsFontPickerIndex(item.dropdown_index);
        return true;
      }
    }
    if (Contains(vm.value_picker.rect, mx, my)) {
      return true;
    }
  }

  if (left && vm.scrollbar.has_value() && Contains(vm.scrollbar->track, mx, my)) {
    begin_scrollbar_drag(*vm.scrollbar, DragTarget::SettingsScrollbar, vm.max_scroll,
                         [this](int row) { settings_overlay_service_.SetScrollRow(row); });
    return true;
  }

  if (vm.mode != SettingsOverlayMode::Settings) {
    return true;
  }

  if (settings_overlay_service_.EditingValue()) {
    bool click_in_editor = false;
    for (const SettingsRowViewModel& row : vm.rows) {
      if (row.control.editing && Contains(row.control.value_rect, mx, my)) {
        click_in_editor = true;
        break;
      }
    }
    if (!click_in_editor) {
      settings_overlay_service_.CancelValueEdit();
    }
  }

  if (Contains(vm.filter_rect, mx, my)) {
    settings_overlay_service_.SetFocusedPane(SettingsPane::Filter);
    InvalidateCursorKindFingerprint();
    RequestPreparedFrame();
    return true;
  }

  if (left && vm.category_scrollbar.has_value() &&
      Contains(vm.category_scrollbar->track, mx, my)) {
    begin_scrollbar_drag(
        *vm.category_scrollbar, DragTarget::SettingsCategoryScrollbar, vm.category_max_scroll,
        [this](int row) { settings_overlay_service_.SetCategoryScrollRow(row); });
    return true;
  }

  for (std::size_t i = 0; i < vm.categories.size(); ++i) {
    if (SettingsRowShownIn(vm.left_pane_rect, vm.categories[i].rect) &&
        Contains(vm.categories[i].rect, mx, my)) {
      settings_overlay_service_.SetFocusedPane(SettingsPane::Categories);
      settings_overlay_service_.SetSelectedCategory(static_cast<int>(i));
      RefreshSettingsOverlayCatalog();
      RequestPreparedFrame();
      return true;
    }
  }

  for (const SettingsRowViewModel& row : vm.rows) {
    if (!SettingsRowShownIn(vm.right_pane_rect, row.row_rect) ||
        !Contains(SettingsRowVisibleRect(vm.right_pane_rect, row.row_rect), mx, my)) {
      continue;
    }
    settings_overlay_service_.SetFocusedPane(SettingsPane::Values);
    settings_overlay_service_.SetSelectedRow(row.row_in_category);

    if (row.scope_rect.w > 0.0f && Contains(row.scope_rect, mx, my)) {
      ToggleSettingScope(row.id);
    } else if (row.resettable && Contains(row.reset_rect, mx, my)) {
      if (row.scope_rect.w > 0.0f) {
        ResetSettingInScope(row.id, row.scope_is_project ? SettingScope::Project
                                                         : SettingScope::User);
      } else {
        ResetSettingValue(row.id);
      }
    } else if (Contains(row.control.checkbox_rect, mx, my)) {
      StepSetting(row.id, true);
    } else if (Contains(row.control.dec_rect, mx, my)) {
      StepSetting(row.id, false);
    } else if (Contains(row.control.inc_rect, mx, my)) {
      StepSetting(row.id, true);
    } else if (Contains(row.control.value_rect, mx, my)) {
      if (row.control.kind == SettingsControlKind::Segmented) {
        StepSetting(row.id, left);
      } else if (row.control.kind == SettingsControlKind::TextEdit && left) {
        BeginSettingValueEdit(row.id);
      }
    } else if (right) {
      StepSetting(row.id, false);
    }
    RefreshSettingsOverlayCatalog();
    RequestPreparedFrame();
    return true;
  }

  return true;
}

}
