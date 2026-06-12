#include "workspace/WorkspaceShell.h"

#include <algorithm>
#include <unordered_map>

#include "workspace/WorkspaceStartupOptions.h"
#include "workspace/RenderViewModelBuilder.h"
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

namespace {

void UpsertSetting(std::vector<std::pair<std::string, std::string>>& settings,
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

bool WorkspaceShell::SetSettingValue(std::string_view id, std::string value) {
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

  const auto apply_project_canonical_setting = [&]() {
    if (!parsed_builtin_value.has_value()) {
      return;
    }
    auto& prefs = context_.current_project_state.editor_preferences;
    if (id == "editor.tab_size") {
      if (const int* parsed = std::get_if<int>(&*parsed_builtin_value); parsed != nullptr) {
        prefs.tab_size = static_cast<std::size_t>(std::clamp(*parsed, 1, 16));
      }
      return;
    }
    if (id == "editor.indent_width") {
      if (const int* parsed = std::get_if<int>(&*parsed_builtin_value); parsed != nullptr) {
        prefs.indent_width = static_cast<std::size_t>(std::clamp(*parsed, 1, 16));
      }
      return;
    }
    if (id == "editor.soft_tabs") {
      if (const bool* parsed = std::get_if<bool>(&*parsed_builtin_value); parsed != nullptr) {
        prefs.soft_tabs = *parsed;
      }
      return;
    }
    if (id == "editor.wrap") {
      if (const std::string* parsed = std::get_if<std::string>(&*parsed_builtin_value);
          parsed != nullptr) {
        prefs.soft_wrap = *parsed == "word";
      }
      return;
    }
    if (id == "editor.colorscheme") {
      if (const std::string* parsed = std::get_if<std::string>(&*parsed_builtin_value);
          parsed != nullptr) {
        MakePersistenceCoordinator().ApplyColorscheme(*parsed, false, false);
      }
    }
  };

  if (builtin != nullptr && !parsed_builtin_value.has_value()) {
    return false;
  }
  if (info->scope == SettingScope::User) {
    UpsertSetting(context_.user_settings, std::string(id), std::move(value));
    if (id == "ui.scale") {
      if (const float* parsed = std::get_if<float>(&*parsed_builtin_value); parsed != nullptr) {
        MakePersistenceCoordinator().ApplyUiScale(*parsed, false, false);
      }
    }
    MakePersistenceCoordinator().SaveUserConfig();
  } else {
    apply_project_canonical_setting();
    UpsertSetting(context_.current_project_state.settings, std::string(id), std::move(value));
    MakePersistenceCoordinator().SaveConfigState();
  }
  ApplyLiveSettings();
  MarkLayoutDirty();
  RequestWindowRedraw();
  return true;
}

void WorkspaceShell::RefreshSettingsOverlayCatalog() {
  if (!settings_overlay_service_.Visible()) {
    return;
  }
  settings_overlay_service_.RebuildSettingsRows(AllSettingInfos(plugin_runtime_.Host()),
                                                context_.user_settings,
                                                context_.current_project_state.settings);
  settings_overlay_service_.RebuildHelpRows(BuildHelpRows(startup_options_, ResolvedKeybindings()));
}

void WorkspaceShell::ApplyLiveSettings() {
  if (const auto value = GetSettingValue("ui.show_status_bar"); value.has_value()) {
    layout_mode_service_.SetStatusBarVisible(*value != "false" && *value != "0" && *value != "off");
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
  if (event.button.button != SDL_BUTTON_LEFT && event.button.button != SDL_BUTTON_RIGHT) {
    if (!Contains(vm.rect, event.button.x, event.button.y)) {
      CloseSettingsOverlay();
    }
    return true;
  }
  if (!Contains(vm.rect, event.button.x, event.button.y)) {
    CloseSettingsOverlay();
    return true;
  }

  const float row_height = 24.0f;
  float list_top = vm.rect.y + 42.0f;
  if (settings_overlay_service_.Mode() == SettingsOverlayMode::Settings) {
    list_top += 16.0f;
  }
  const float list_bottom = vm.rect.y + vm.rect.h - 10.0f;
  if (event.button.y < list_top || event.button.y > list_bottom) {
    return true;
  }
  const std::size_t row =
      static_cast<std::size_t>(std::max(0.0f, event.button.y - list_top) / row_height) +
      static_cast<std::size_t>(settings_overlay_service_.ScrollRow());

  if (settings_overlay_service_.Mode() == SettingsOverlayMode::Settings &&
      row < settings_overlay_service_.SettingsRows().size()) {
    const auto& picked = settings_overlay_service_.SettingsRows()[row];
    if (const SettingSpec* spec = FindBuiltinSettingSpec(picked.id); spec != nullptr) {
      const SettingStepDirection direction =
          event.button.button == SDL_BUTTON_RIGHT ? SettingStepDirection::Backward
                                                  : SettingStepDirection::Forward;
      SetSettingValue(picked.id, NextSettingValue(*spec, picked.value, direction));
      RefreshSettingsOverlayCatalog();
      return true;
    }
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
