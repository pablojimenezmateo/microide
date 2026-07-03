#include "workspace/WorkspaceShell.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

#include "workspace/WorkspaceStartupOptions.h"
#include "workspace/RenderViewModelBuilder.h"
#include "workspace/SettingFlags.h"
#include "workspace/WorkspaceActionCoordinator.h"
#include "workspace/WorkspaceCommandRegistry.h"
#include "workspace/WorkspaceKeybindingRegistry.h"
#include "workspace/WorkspacePersistenceCoordinator.h"
#include "workspace/WorkspaceSettingsRegistry.h"
#include "util/Parse.h"

// Set by CMake (target_compile_definitions) for the app target. The test target
// compiles these sources without it, so fall back to a development marker.
#ifndef MICROIDE_VERSION
#define MICROIDE_VERSION "dev"
#endif

namespace microide::workspace {

// Synthetic settings-row id prefix for per-plugin enable toggles ("plugin.toggle:<id>").
constexpr const char* kPluginToggleRowPrefix = "plugin.toggle:";

namespace {

// Canonical settings are materialized into the editor_preferences / colorscheme
// cache rather than read from the store on the hot path, so a write of one of
// these ids must re-run MaterializeCanonicalPreferences to keep the cache aligned
// with the resolved (project → user-default → spec) value.
bool IsCanonicalPreferenceId(std::string_view id) {
  return id == "editor.tab_size" || id == "editor.indent_width" ||
         id == "editor.font_size" || id == "editor.soft_tabs" || id == "editor.wrap" ||
         id == "editor.colorscheme";
}

// One synthetic Checkbox row per discovered plugin, for the Settings "Plugins" pane.
// When no plugins are installed, a single display-only row keeps the section visible so
// the capability stays discoverable.
std::vector<SettingsOverlayRow> BuildPluginToggleRows(const plugin::PluginHost& host) {
  std::vector<SettingsOverlayRow> rows;
  const std::vector<plugin::PluginHost::LoadedPlugin> plugins = host.LoadedPlugins();
  if (plugins.empty()) {
    SettingsOverlayRow row;
    // Note: id does NOT use kPluginToggleRowPrefix, so it is never treated as a toggle.
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

// Maps each action to its currently bound key chord, honoring user remaps and
// plugin bindings (the registry is already override-resolved). A Global binding
// wins over a context-specific one so the help list shows the chord that works
// everywhere; key-less command bindings are skipped.
std::unordered_map<ActionId, std::string> BuildActionChordLookup(
    const std::vector<ResolvedKeybinding>& keybindings) {
  std::unordered_map<ActionId, std::string> chord_by_action;
  for (const ResolvedKeybinding& binding : keybindings) {
    if (binding.key == SDLK_UNKNOWN) {
      continue;
    }
    std::string chord = FormatKeyChord(binding.key, binding.modifiers);
    if (chord.empty()) {
      continue;
    }
    const auto [it, inserted] = chord_by_action.try_emplace(binding.action, std::move(chord));
    if (!inserted && binding.context == KeybindingContext::Global) {
      it->second = FormatKeyChord(binding.key, binding.modifiers);
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
                              .detail = "Enter default view | d diff | s stage | u unstage | "
                                         "x discard (confirm) | m merge | c commit | r refresh | "
                                         "o open file"});
  const std::unordered_map<ActionId, std::string> chord_by_action =
      BuildActionChordLookup(keybindings);
  for (const ActionSpec& spec : WorkspaceCommandSpecs()) {
    if (spec.command_name.empty()) {
      continue;
    }
    std::string detail =
        std::string(spec.command_usage.empty() ? spec.command_name : spec.command_usage);
    if (const auto it = chord_by_action.find(spec.id); it != chord_by_action.end()) {
      // Prefix the bound key so chords align at the start of the detail column.
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

int WrapSteppedInt(int value, int min_value, int max_value, int step, SettingStepDirection direction) {
  if (min_value > max_value) {
    std::swap(min_value, max_value);
  }
  const int delta = direction == SettingStepDirection::Forward ? step : -step;
  value += delta;
  if (value > max_value) {
    return min_value;
  }
  if (value < min_value) {
    return max_value;
  }
  return value;
}

std::string NextSettingValue(const SettingSpec& spec,
                             std::string_view current,
                             SettingStepDirection direction) {
  switch (spec.type) {
    case SettingType::Bool:
      return (current == "true" || current == "1" || current == "on") ? "false" : "true";
    case SettingType::Enum: {
      if (spec.enum_values.empty()) {
        return std::string(current);
      }
      for (std::size_t i = 0; i < spec.enum_values.size(); ++i) {
        if (spec.enum_values[i].value == current) {
          if (direction == SettingStepDirection::Forward) {
            return std::string(spec.enum_values[(i + 1) % spec.enum_values.size()].value);
          }
          return std::string(
              spec.enum_values[(i + spec.enum_values.size() - 1) % spec.enum_values.size()].value);
        }
      }
      return direction == SettingStepDirection::Forward
                 ? std::string(spec.enum_values.front().value)
                 : std::string(spec.enum_values.back().value);
    }
    case SettingType::Int: {
      const auto parsed = util::ParseInt(current);
      int value = parsed.value_or(spec.default_int);
      if (spec.id == "ui.layout_compact_breakpoint_px") {
        value = WrapSteppedInt(value, 600, 2000, 20, direction);
      } else if (spec.id == "editor.hover_delay_ms") {
        value = WrapSteppedInt(value, 0, 2000, 50, direction);
      } else if (spec.id == "editor.tab_size" || spec.id == "editor.indent_width") {
        value = WrapSteppedInt(value, 1, 16, 1, direction);
      } else if (spec.id == "editor.font_size" || spec.id == "terminal.font_size") {
        value = WrapSteppedInt(value, 8, 32, 1, direction);
      } else {
        value = WrapSteppedInt(value, spec.default_int - 20, spec.default_int + 20, 1, direction);
      }
      return std::to_string(value);
    }
    case SettingType::Float: {
      const auto parsed = util::ParseFloat(current);
      float value =
          parsed.value_or(spec.default_float) + (direction == SettingStepDirection::Forward ? 0.1f : -0.1f);
      if (value > 2.0f) {
        value = 0.75f;
      } else if (value < 0.75f) {
        value = 2.0f;
      }
      return SerializeSettingValue(value);
    }
    case SettingType::String:
      return std::string(current);
  }
  return std::string(current);
}

}  // namespace

bool WorkspaceShell::SetSettingValue(std::string_view id, std::string value, bool persist) {
  return WriteSettingValue(id, std::move(value), /*as_user_default=*/false, persist);
}

bool WorkspaceShell::SetSettingAsUserDefault(std::string_view id, std::string value, bool persist) {
  return WriteSettingValue(id, std::move(value), /*as_user_default=*/true, persist);
}

bool WorkspaceShell::WriteSettingValue(std::string_view id, std::string value,
                                       bool as_user_default, bool persist) {
  const auto info = FindSettingInfo(id, plugin_runtime_.Host());
  if (!info.has_value()) {
    return false;
  }
  const SettingSpec* builtin = FindBuiltinSettingSpec(id);
  std::optional<SettingValue> parsed_builtin_value;
  if (builtin != nullptr) {
    parsed_builtin_value = ParseSettingValue(*builtin, value);
    if (!parsed_builtin_value.has_value()) {
      return false;
    }
  }

  // A user-scoped setting already lives in the user layer; "set as default" only
  // changes where a project-scoped write lands (user layer instead of project).
  const bool write_user_layer = info->scope == SettingScope::User || as_user_default;

  // Track / untrack the transient marker before the value is moved-from. A later
  // persisting write of the same id promotes it back to durable storage.
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
    settings_store_.SetUser(id, std::move(value));
    // Canonical preferences are materialized from the resolved layers so a new
    // user-level default takes effect on any project without a project override.
    ApplyCanonicalPreferenceSideEffects(id);
    if (persist) {
      MakePersistenceCoordinator().SaveUserConfig();
    }
  } else {
    settings_store_.SetProject(id, std::move(value));
    ApplyCanonicalPreferenceSideEffects(id);
    if (persist) {
      MakePersistenceCoordinator().SaveConfigState();
    }
  }
  ApplyLiveSettings();
  MarkLayoutDirty();
  RequestWindowRedraw();
  return true;
}

void WorkspaceShell::ApplyStartupSettingOverrides() {
  for (const auto& [id, value] : startup_options_.setting_overrides) {
    if (!SetSettingValue(id, value, /*persist=*/false)) {
      SDL_Log("--set: unknown setting or invalid value for \"%s\"", id.c_str());
    }
  }
}

void WorkspaceShell::RefreshSettingsOverlayCatalog() {
  if (!settings_overlay_service_.Visible()) {
    return;
  }
  settings_overlay_service_.RebuildSettingsRows(AllSettingInfos(plugin_runtime_.Host()),
                                                context_.user_settings,
                                                context_.current_project_state.settings,
                                                BuildPluginToggleRows(plugin_runtime_.Host()));
  settings_overlay_service_.RebuildHelpRows(BuildHelpRows(startup_options_, ResolvedKeybindings()));
}

void WorkspaceShell::TogglePluginEnabled(std::string_view plugin_id) {
  auto& disabled = context_.disabled_plugin_ids;
  const auto it = std::find(disabled.begin(), disabled.end(), plugin_id);
  if (it == disabled.end()) {
    disabled.emplace_back(plugin_id);  // was enabled -> now disabled
  } else {
    disabled.erase(it);  // was disabled -> now enabled
  }
  MakePersistenceCoordinator().SaveUserConfig();
  ReloadPluginsForCurrentProject(PluginReloadRequest{});
  RefreshSettingsOverlayCatalog();
  RequestWindowRedraw();
}

void WorkspaceShell::ApplyLiveSettings() {
  // Start/stop the control channel when `control.enabled` is toggled at runtime.
  // The wake-event plumbing is already bound; only the listener is gated.
  MaybeStartControlChannel();
  // Font family is renderer-global and not part of the per-tab editor snapshot,
  // so apply it here. ApplyLiveSettings runs every frame, so only relayout when
  // the typeface actually changed (SetFontFamily returns false when unchanged).
  if (const auto family = GetSettingValue("editor.font_family"); family.has_value()) {
    if (text_renderer_.SetFontFamily(*family)) {
      MarkLayoutDirty();
    }
  }
  // Push terminal scrollback to live sessions only when the resolved cap changes
  // (this hook runs every frame, so avoid touching every terminal's mutex).
  if (const int scrollback = static_cast<int>(TerminalScrollbackLines());
      scrollback != last_applied_terminal_scrollback_) {
    last_applied_terminal_scrollback_ = scrollback;
    for (auto& tab : context_.current_project_state.terminal_tabs) {
      if (tab != nullptr) {
        tab->session.SetMaxScrollbackLines(static_cast<std::size_t>(scrollback));
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

  LiveSettingsEditorSnapshot snapshot;
  snapshot.project_root = context_.current_project_state.root.lexically_normal();
  snapshot.tab_size = context_.current_project_state.editor_preferences.tab_size;
  snapshot.indent_width = context_.current_project_state.editor_preferences.indent_width;
  snapshot.font_size = context_.current_project_state.editor_preferences.font_size;
  snapshot.soft_tabs = context_.current_project_state.editor_preferences.soft_tabs;
  snapshot.soft_wrap = context_.current_project_state.editor_preferences.soft_wrap;
  snapshot.auto_close_enabled = GetSettingValue("editor.brackets.auto_close.enabled");
  snapshot.surround_enabled = GetSettingValue("editor.brackets.surround.enabled");
  snapshot.smart_indent_enabled = GetSettingValue("editor.indent.smart.enabled");
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
      last_live_settings_editor_snapshot_->auto_close_enabled == snapshot.auto_close_enabled &&
      last_live_settings_editor_snapshot_->surround_enabled == snapshot.surround_enabled &&
      last_live_settings_editor_snapshot_->smart_indent_enabled == snapshot.smart_indent_enabled &&
      last_live_settings_editor_snapshot_->save_trim_trailing_whitespace ==
          snapshot.save_trim_trailing_whitespace &&
      last_live_settings_editor_snapshot_->save_ensure_final_newline ==
          snapshot.save_ensure_final_newline;
  if (!snapshot_matches) {
    // Keep per-tab editor runtime knobs (save normalization and language-pair
    // toggles) aligned with effective settings after relevant settings change.
    ApplyEditorPreferencesToAllTabs();
    last_live_settings_editor_snapshot_ = snapshot;
  }
}

bool WorkspaceShell::ResetSettingValue(std::string_view id) {
  const auto info = FindSettingInfo(id, plugin_runtime_.Host());
  if (!info.has_value()) {
    return false;
  }
  return ResetSettingInScope(id, info->scope);
}

bool WorkspaceShell::ResetSettingInScope(std::string_view id, SettingScope scope) {
  // Callers (ResetSettingValue, ToggleSettingScope) already validate the id before
  // delegating here, so no second catalog lookup is needed.
  // Drop the stored override from the chosen layer. Canonical preferences are then
  // re-materialized from the remaining layers (a project reset may surface a user
  // default; a user-default reset falls back to the spec default). Everything else
  // is re-read live by ApplyLiveSettings + redraw.
  if (scope == SettingScope::User) {
    settings_store_.ResetUser(id);
  } else {
    settings_store_.ResetProject(id);
  }
  ApplyCanonicalPreferenceSideEffects(id);
  if (scope == SettingScope::User) {
    MakePersistenceCoordinator().SaveUserConfig();
  } else {
    MakePersistenceCoordinator().SaveConfigState();
  }
  ApplyLiveSettings();
  MarkLayoutDirty();
  RequestWindowRedraw();
  return true;
}

void WorkspaceShell::ApplyCanonicalPreferenceSideEffects(std::string_view id) {
  if (!IsCanonicalPreferenceId(id)) {
    return;
  }
  auto coordinator = MakePersistenceCoordinator();
  // Keep the editor-preferences cache aligned with the resolved (project →
  // user-default → spec) value.
  coordinator.MaterializeCanonicalPreferences();
  // Colorscheme is materialized into the theme, not editor_preferences, so a
  // write/reset must apply it explicitly to update the live theme (this happens
  // before persistence so SaveConfigState records the newly-active name).
  if (id == "editor.colorscheme") {
    coordinator.ApplyColorscheme(GetSettingValue(id).value_or("default"),
                                 /*persist=*/false, /*log_feedback=*/false);
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
    // Currently "This Project": promote the value to the shared user-level
    // default and drop the per-project override so the row follows the default.
    SetSettingAsUserDefault(id, current);
    ResetSettingInScope(id, SettingScope::Project);
  } else {
    // Currently "Default": pin the current value as a per-project override.
    SetSettingValue(id, current);
  }
}

void WorkspaceShell::BeginSettingValueEdit(std::string_view id) {
  settings_overlay_service_.SetFocusedPane(SettingsPane::Values);
  settings_overlay_service_.BeginValueEdit(std::string(id), GetSettingValue(id).value_or(""));
  InvalidateCursorKindFingerprint();
  RequestOverlayRedraw();
}

void WorkspaceShell::CommitSettingValueEdit() {
  if (!settings_overlay_service_.EditingValue()) {
    return;
  }
  const std::string id = settings_overlay_service_.EditingRowId();
  const std::string value = settings_overlay_service_.ValueEditText();
  settings_overlay_service_.CancelValueEdit();
  WriteSettingRespectingScope(id, value);
  RefreshSettingsOverlayCatalog();
  InvalidateCursorKindFingerprint();
  RequestOverlayRedraw();
}

void WorkspaceShell::CancelSettingValueEdit() {
  if (!settings_overlay_service_.EditingValue()) {
    return;
  }
  settings_overlay_service_.CancelValueEdit();
  InvalidateCursorKindFingerprint();
  RequestOverlayRedraw();
}

void WorkspaceShell::StepSetting(std::string_view id, bool forward) {
  // Plugin enable/disable toggles are synthetic rows, not real settings.
  if (id.rfind(kPluginToggleRowPrefix, 0) == 0) {
    TogglePluginEnabled(id.substr(std::string_view(kPluginToggleRowPrefix).size()));
    return;
  }
  const SettingStepDirection direction =
      forward ? SettingStepDirection::Forward : SettingStepDirection::Backward;
  if (const SettingSpec* spec = FindBuiltinSettingSpec(id); spec != nullptr) {
    const std::string current =
        GetSettingValue(id).value_or(SerializeSettingValue(DefaultSettingValue(*spec)));
    WriteSettingRespectingScope(id, NextSettingValue(*spec, current, direction));
    return;
  }
  // Plugin-contributed setting without a built-in spec: support generic Bool /
  // Enum cycling; otherwise it is display-only.
  const auto info = FindSettingInfo(id, plugin_runtime_.Host());
  if (!info.has_value()) {
    return;
  }
  const std::string current =
      GetSettingValue(id).value_or(SerializeSettingValue(info->default_value));
  if (info->type == SettingType::Bool) {
    const bool on = current == "true" || current == "1" || current == "on";
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

void WorkspaceShell::EnsureSettingsSelectionVisible() {
  const auto layout_state = CurrentWorkspaceLayout();
  if (!layout_state.has_value()) {
    return;
  }
  const SettingsOverlayViewModel vm =
      RenderViewModelBuilder(context_).BuildSettingsOverlay(*layout_state, settings_overlay_service_);
  const int selected = settings_overlay_service_.SelectedRow();
  int scroll = settings_overlay_service_.ScrollRow();
  if (selected < scroll) {
    scroll = selected;
  } else if (selected >= scroll + vm.visible_rows) {
    scroll = selected - vm.visible_rows + 1;
  }
  settings_overlay_service_.SetScrollRow(std::clamp(scroll, 0, vm.max_scroll));
}

void WorkspaceShell::OpenSettingsOverlay() {
  settings_overlay_service_.OpenSettings();
  RefreshSettingsOverlayCatalog();
  InvalidateCursorKindFingerprint();
  RequestOverlayRedraw();
}

void WorkspaceShell::OpenHelpAboutOverlay() {
  settings_overlay_service_.OpenHelpAbout();
  RefreshSettingsOverlayCatalog();
  InvalidateCursorKindFingerprint();
  RequestOverlayRedraw();
}

void WorkspaceShell::CloseSettingsOverlay() {
  settings_overlay_service_.Close();
  if (context_.interaction_state.drag_target == DragTarget::SettingsScrollbar) {
    context_.interaction_state.drag_target = DragTarget::None;
  }
  InvalidateCursorKindFingerprint();
  RequestOverlayRedraw();
}

bool WorkspaceShell::HandleSettingsOverlayButtonDown(const SDL_Event& event,
                                                     const WorkspaceLayout& layout) {
  if (!settings_overlay_service_.Visible()) {
    return false;
  }

  const SettingsOverlayViewModel vm =
      RenderViewModelBuilder(context_).BuildSettingsOverlay(layout, settings_overlay_service_);
  const float mx = event.button.x;
  const float my = event.button.y;

  // Any click outside the surface dismisses the modal.
  if (!Contains(vm.rect, mx, my)) {
    CloseSettingsOverlay();
    return true;
  }

  const bool left = event.button.button == SDL_BUTTON_LEFT;
  const bool right = event.button.button == SDL_BUTTON_RIGHT;
  if (!left && !right) {
    return true;  // consume other buttons inside the modal
  }
  if (vm.mode != SettingsOverlayMode::Settings) {
    return true;  // Help / About is read-only
  }

  // A click anywhere but inside the active inline value editor commits nothing and
  // cancels the edit, so keyboard focus never strands on a hidden editor.
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

  // Scrollbar: clicking the track jumps and begins a drag (tracked via motion).
  if (left && vm.scrollbar.has_value() && Contains(vm.scrollbar->track, mx, my)) {
    context_.interaction_state.drag_target = DragTarget::SettingsScrollbar;
    context_.interaction_state.drag_scrollbar_offset =
        Contains(vm.scrollbar->thumb, mx, my) ? my - vm.scrollbar->thumb.y
                                              : vm.scrollbar->thumb.h * 0.5f;
    settings_overlay_service_.SetScrollRow(std::clamp(
        static_cast<int>(std::lround(ScrollUnitsForPointer(
            *vm.scrollbar, my, context_.interaction_state.drag_scrollbar_offset))),
        0, vm.max_scroll));
    RequestOverlayRedraw();
    return true;
  }

  // Filter box: focus it so typing filters.
  if (Contains(vm.filter_rect, mx, my)) {
    settings_overlay_service_.SetFocusedPane(SettingsPane::Filter);
    InvalidateCursorKindFingerprint();
    RequestOverlayRedraw();
    return true;
  }

  // Left rail: pick a category.
  for (std::size_t i = 0; i < vm.categories.size(); ++i) {
    if (Contains(vm.categories[i].rect, mx, my)) {
      settings_overlay_service_.SetFocusedPane(SettingsPane::Categories);
      settings_overlay_service_.SetSelectedCategory(static_cast<int>(i));
      RefreshSettingsOverlayCatalog();
      RequestOverlayRedraw();
      return true;
    }
  }

  // Value rows: controls, reset, or row selection.
  for (const SettingsRowViewModel& row : vm.rows) {
    if (!Contains(row.row_rect, mx, my)) {
      continue;
    }
    settings_overlay_service_.SetFocusedPane(SettingsPane::Values);
    settings_overlay_service_.SetSelectedRow(row.row_in_category);

    if (row.scope_rect.w > 0.0f && Contains(row.scope_rect, mx, my)) {
      ToggleSettingScope(row.id);
    } else if (row.resettable && Contains(row.reset_rect, mx, my)) {
      // Reset the layer the scope chip currently targets (project override vs
      // user default); non-scope-selectable rows reset their declared scope.
      if (row.scope_rect.w > 0.0f) {
        ResetSettingInScope(row.id, row.scope_is_project ? SettingScope::Project
                                                         : SettingScope::User);
      } else {
        ResetSettingValue(row.id);
      }
    } else if (Contains(row.control.checkbox_rect, mx, my)) {
      StepSetting(row.id, true);  // checkbox toggle (forward == toggle for Bool)
    } else if (Contains(row.control.dec_rect, mx, my)) {
      StepSetting(row.id, false);
    } else if (Contains(row.control.inc_rect, mx, my)) {
      StepSetting(row.id, true);
    } else if (Contains(row.control.value_rect, mx, my)) {
      // Segmented value cycles on click; TextEdit opens the inline editor; the
      // stepper value field only selects.
      if (row.control.kind == SettingsControlKind::Segmented) {
        StepSetting(row.id, left);
      } else if (row.control.kind == SettingsControlKind::TextEdit && left) {
        BeginSettingValueEdit(row.id);
      }
    } else if (right) {
      // Right-click anywhere on a row steps backward as a power-user shortcut.
      StepSetting(row.id, false);
    }
    RefreshSettingsOverlayCatalog();
    RequestOverlayRedraw();
    return true;
  }

  return true;
}

bool WorkspaceShell::HandleStatusBarButtonDown(const SDL_Event& event,
                                               const WorkspaceLayout& layout) {
  if (event.button.button != SDL_BUTTON_LEFT || layout.status_bar.w <= 0.0f ||
      !Contains(layout.status_bar, event.button.x, event.button.y)) {
    return false;
  }

  const StatusBarViewModel vm =
      RenderViewModelBuilder(context_).BuildStatusBar(layout, status_bar_service_);
  const float padding = 12.0f;
  const float gap = 14.0f;
  float left_x = vm.rect.x + padding;
  for (const StatusBarSegmentViewModel& seg : vm.left_segments) {
    const float width = text_renderer_.MeasureWidth(seg.text);
    const SDL_FRect row = MakeRect(left_x, vm.rect.y, width, vm.rect.h);
    if (seg.clickable && Contains(row, event.button.x, event.button.y)) {
      if (seg.id == StatusBarSegmentId::Project || seg.id == StatusBarSegmentId::Branch) {
        ActionCoordinator(MakeActionContext())
            .Execute(ActionId::SidebarShow, {"git"}, ActionSource::Menu);
      } else if (seg.id == StatusBarSegmentId::Indent || seg.id == StatusBarSegmentId::Encoding) {
        OpenSettingsOverlay();
      }
      return true;
    }
    left_x += width + gap;
  }

  float right_x = vm.rect.x + vm.rect.w - padding;
  for (auto it = vm.right_segments.rbegin(); it != vm.right_segments.rend(); ++it) {
    const float width = text_renderer_.MeasureWidth(it->text);
    right_x -= width;
    const SDL_FRect row = MakeRect(right_x, vm.rect.y, width, vm.rect.h);
    if (it->clickable && Contains(row, event.button.x, event.button.y)) {
      switch (it->id) {
        case StatusBarSegmentId::LineColumn:
          ActionCoordinator(MakeActionContext()).Execute(ActionId::Goto, {}, ActionSource::Menu);
          break;
        case StatusBarSegmentId::Lsp:
          // ShowOutputChannel was removed with the user-facing output panel.
          // The status segment now reads as informational only.
          break;
        case StatusBarSegmentId::LayoutMode: {
          const bool compact = layout_mode_service_.CurrentMode() != LayoutMode::Compact;
          SetSettingValue("ui.layout_mode", compact ? "compact" : "regular");
          break;
        }
        default:
          break;
      }
      return true;
    }
    right_x -= gap;
  }
  return true;
}

}  // namespace microide::workspace
