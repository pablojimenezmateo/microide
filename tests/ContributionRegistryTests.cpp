#include "TestSupport.h"

#include "plugin/PluginHost.h"
#include "workspace/WorkspaceKeybindingRegistry.h"
#include "workspace/WorkspaceMenuRegistry.h"
#include "workspace/WorkspacePersistenceFormat.h"
#include "workspace/WorkspaceSettingsRegistry.h"
#include "workspace/WorkspaceSidebarRegistry.h"
#include "workspace/WorkspaceStatusRegistry.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

namespace microide::tests {
namespace {

using microide::plugin::PluginHost;
using microide::workspace::BuiltinKeybindingSpecs;
using microide::workspace::BuiltinSettingSpecs;
using microide::workspace::BuiltinSidebarViewSpecs;
using microide::workspace::ContributedMenuItems;
using microide::workspace::DefaultSettingValue;
using microide::workspace::FindBuiltinKeybinding;
using microide::workspace::FindBuiltinKeybindingByKey;
using microide::workspace::FindBuiltinSettingSpec;
using microide::workspace::FindKeybinding;
using microide::workspace::FormatKeyChord;
using microide::workspace::KeybindingContext;
using microide::workspace::MenuId;
using microide::workspace::OrderedSidebarViews;
using microide::workspace::ParseKeyChord;
using microide::workspace::ParseSettingValue;
using microide::workspace::PersistedProjectConfigState;
using microide::workspace::PersistedUserConfigState;
using microide::workspace::ResolveKeybindings;
using microide::workspace::ResolveStatusItems;
using microide::workspace::SerializeSettingValue;
using microide::workspace::SettingType;
using microide::workspace::SidebarViewPolicy;

class ScopedPluginConfigHomeEnv {
 public:
  explicit ScopedPluginConfigHomeEnv(const std::filesystem::path& config_root)
      : xdg_config_home_("XDG_CONFIG_HOME", config_root.string()),
        appdata_("APPDATA", config_root.string()) {}

 private:
  ScopedEnvVar xdg_config_home_;
  ScopedEnvVar appdata_;
};

// ---------------------------------------------------------------------------
// Keybinding registry – built-ins
// ---------------------------------------------------------------------------

void TestKeybindingRegistryBuiltinsNonEmpty() {
  Expect(!BuiltinKeybindingSpecs().empty(),
         "BuiltinKeybindingSpecs should be non-empty");
}

void TestKeybindingRegistryFindById() {
  const auto* spec = FindBuiltinKeybinding("save");
  Expect(spec != nullptr, "should find built-in keybinding 'save'");
  Expect(spec->action == microide::workspace::ActionId::Save,
         "save keybinding should map to ActionId::Save");
}

void TestKeybindingRegistryFindUnknown() {
  Expect(FindBuiltinKeybinding("no-such-binding") == nullptr,
         "unknown keybinding id should return nullptr");
}

void TestParseKeyChordSingleKey() {
  SDL_Keycode key = SDLK_UNKNOWN;
  SDL_Keymod mods = SDL_KMOD_NONE;
  Expect(ParseKeyChord("F8", &key, &mods), "ParseKeyChord should succeed for F8");
  Expect(key != SDLK_UNKNOWN, "F8 should resolve to a valid keycode");
  Expect(mods == SDL_KMOD_NONE, "F8 should have no modifiers");
}

void TestParseKeyChordWithModifiers() {
  SDL_Keycode key = SDLK_UNKNOWN;
  SDL_Keymod mods = SDL_KMOD_NONE;
  Expect(ParseKeyChord("Ctrl+S", &key, &mods), "ParseKeyChord should succeed for Ctrl+S");
  Expect(key != SDLK_UNKNOWN, "Ctrl+S key should be valid");
  Expect((mods & SDL_KMOD_CTRL) != 0, "Ctrl+S should have Ctrl modifier");
}

void TestParseKeyChordMultipleModifiers() {
  SDL_Keycode key = SDLK_UNKNOWN;
  SDL_Keymod mods = SDL_KMOD_NONE;
  Expect(ParseKeyChord("Ctrl+Shift+F", &key, &mods),
         "ParseKeyChord should succeed for Ctrl+Shift+F");
  Expect((mods & SDL_KMOD_CTRL) != 0, "should have Ctrl");
  Expect((mods & SDL_KMOD_SHIFT) != 0, "should have Shift");
}

void TestParseKeyChordInvalid() {
  SDL_Keycode key = SDLK_UNKNOWN;
  SDL_Keymod mods = SDL_KMOD_NONE;
  Expect(!ParseKeyChord("NotAKey", &key, &mods),
         "ParseKeyChord should fail for unknown key name");
}

void TestFormatKeyChordRoundTrip() {
  SDL_Keycode key = SDLK_UNKNOWN;
  SDL_Keymod mods = SDL_KMOD_NONE;
  Expect(ParseKeyChord("Ctrl+S", &key, &mods), "parse must succeed");
  const std::string formatted = FormatKeyChord(key, mods);
  Expect(!formatted.empty(), "FormatKeyChord should produce a non-empty string");
}

void TestFindBuiltinKeybindingByKey() {
  const auto* save_spec = FindBuiltinKeybinding("save");
  Expect(save_spec != nullptr, "save spec should exist");
  const auto* found = FindBuiltinKeybindingByKey(save_spec->key, save_spec->modifiers,
                                                  KeybindingContext::Global);
  Expect(found != nullptr, "should find save keybinding by key");
  Expect(found->id == save_spec->id, "found spec should be save");
}

void TestFindBuiltinKeybindingByLeftCtrlKey() {
  const auto* command_prompt_spec = FindBuiltinKeybinding("command-prompt");
  Expect(command_prompt_spec != nullptr, "command prompt spec should exist");
  const auto* found = FindBuiltinKeybindingByKey(command_prompt_spec->key, SDL_KMOD_LCTRL,
                                                 KeybindingContext::Global);
  Expect(found != nullptr, "left control should match Ctrl bindings");
  Expect(found->id == command_prompt_spec->id,
         "left control should resolve the command prompt shortcut");
}

void TestResolveKeybindingsEmpty() {
  PluginHost host;
  const auto bindings = ResolveKeybindings(host);
  Expect(!bindings.empty(), "resolved keybindings should include at least built-ins");
}

void TestResolveKeybindingsDisabled() {
  PluginHost host;
  const auto all = ResolveKeybindings(host);
  const auto without_save = ResolveKeybindings(host, {"save"});
  Expect(without_save.size() < all.size(), "disabling a keybinding should reduce the count");
  const bool has_save = std::any_of(without_save.begin(), without_save.end(),
                                     [](const auto& rb) { return rb.id == "save"; });
  Expect(!has_save, "disabled keybinding should not appear in resolved list");
}

void TestResolveKeybindingsFindLeftCtrlMatch() {
  PluginHost host;
  const auto bindings = ResolveKeybindings(host);
  const auto* found = FindKeybinding(bindings, SDLK_E, SDL_KMOD_LCTRL, KeybindingContext::Global);
  Expect(found != nullptr, "resolved bindings should match left-control shortcuts");
  Expect(found->id == "command-prompt",
         "left control should resolve to the command prompt shortcut");
}

// ---------------------------------------------------------------------------
// Settings registry – built-ins
// ---------------------------------------------------------------------------

void TestSettingsRegistryBuiltinsNonEmpty() {
  Expect(!BuiltinSettingSpecs().empty(), "BuiltinSettingSpecs should be non-empty");
}

void TestSettingsRegistryFindById() {
  const auto* spec = FindBuiltinSettingSpec("editor.tab_size");
  Expect(spec != nullptr, "should find built-in setting 'editor.tab_size'");
  Expect(spec->type == SettingType::Int, "editor.tab_size should be Int type");
}

void TestSettingsRegistryWrapSpecAndEnumValues() {
  const auto* spec = FindBuiltinSettingSpec("editor.wrap");
  Expect(spec != nullptr, "should find built-in setting 'editor.wrap'");
  Expect(spec->type == SettingType::Enum, "editor.wrap should be Enum type");
  Expect(spec->enum_values.size() == 2 && spec->enum_values[0].value == "off" &&
             spec->enum_values[1].value == "word",
         "editor.wrap enum should expose 'off' and 'word' values");
}

void TestSettingsRegistryFindUnknown() {
  Expect(FindBuiltinSettingSpec("no.such.setting") == nullptr,
         "unknown setting id should return nullptr");
}

void TestParseSettingValueBool() {
  const auto* spec = FindBuiltinSettingSpec("editor.soft_tabs");
  Expect(spec != nullptr, "editor.soft_tabs spec required");
  const auto v = ParseSettingValue(*spec, "true");
  Expect(v.has_value(), "should parse 'true'");
  Expect(std::get<bool>(*v) == true, "parsed value should be true");
  const auto v2 = ParseSettingValue(*spec, "false");
  Expect(v2.has_value(), "should parse 'false'");
  Expect(std::get<bool>(*v2) == false, "parsed value should be false");
}

void TestParseSettingValueInt() {
  const auto* spec = FindBuiltinSettingSpec("editor.tab_size");
  Expect(spec != nullptr, "editor.tab_size spec required");
  const auto v = ParseSettingValue(*spec, "4");
  Expect(v.has_value(), "should parse '4'");
  Expect(std::get<int>(*v) == 4, "parsed value should be 4");
}

void TestParseSettingValueFloat() {
  const auto* spec = FindBuiltinSettingSpec("ui.scale");
  Expect(spec != nullptr, "ui.scale spec required");
  const auto v = ParseSettingValue(*spec, "1.5");
  Expect(v.has_value(), "should parse '1.5'");
  Expect(std::abs(std::get<float>(*v) - 1.5f) < 0.001f, "parsed float should be ~1.5");
}

void TestParseSettingValueInvalid() {
  const auto* spec = FindBuiltinSettingSpec("editor.tab_size");
  Expect(spec != nullptr, "editor.tab_size spec required");
  Expect(!ParseSettingValue(*spec, "not-a-number").has_value(),
         "should not parse non-numeric string as int");
}

void TestParseSettingValueWrapEnum() {
  const auto* spec = FindBuiltinSettingSpec("editor.wrap");
  Expect(spec != nullptr, "editor.wrap spec required");
  const auto word = ParseSettingValue(*spec, "word");
  Expect(word.has_value(), "editor.wrap should parse 'word'");
  Expect(std::get<std::string>(*word) == "word", "parsed editor.wrap value should be 'word'");
  Expect(!ParseSettingValue(*spec, "bounded").has_value(),
         "editor.wrap should reject unsupported enum values");
}

void TestSerializeSettingValueRoundTrip() {
  const std::string text = SerializeSettingValue(42);
  Expect(text == "42", "serialised int should be '42'");
  const std::string btext = SerializeSettingValue(true);
  Expect(btext == "true", "serialised bool should be 'true'");
}

void TestDefaultSettingValue() {
  const auto* spec = FindBuiltinSettingSpec("editor.tab_size");
  Expect(spec != nullptr, "spec required");
  const auto dv = DefaultSettingValue(*spec);
  Expect(std::get<int>(dv) == 4, "default tab_size should be 4");
}

// ---------------------------------------------------------------------------
// Plugin-contributed settings (requires Lua)
// ---------------------------------------------------------------------------

void TestPluginContributedSettingsDeclare() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp;
  const std::filesystem::path plugins_dir = temp.path() / "config" / "microide" / "plugins";
  WriteFile(plugins_dir / "contrib-settings" / "init.lua", R"(
local ide = require("microide")
return ide.plugin({
  id = "contrib.settings",
  setup = function(ctx)
    ctx.settings.declare({
      id = "my_setting",
      type = "string",
      default = "hello",
      label = "My Setting",
      scope = "project",
    })
  end,
})
)");

  PluginHost host;
  ScopedPluginConfigHomeEnv config_home(temp.path() / "config");
  host.Reload(temp.path() / "project");
  Expect(!host.ContributedSettings().empty(),
         "plugin should have contributed at least one setting");
  Expect(host.ContributedSettings().front().id == "contrib.settings.my_setting",
         "setting id should be plugin-prefixed");
  Expect(host.ContributedSettings().front().type == "string",
         "setting type should be string");
}

// ---------------------------------------------------------------------------
// Plugin-contributed menu entries
// ---------------------------------------------------------------------------

void TestPluginContributedMenuEntries() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp;
  const std::filesystem::path plugins_dir = temp.path() / "config" / "microide" / "plugins";
  WriteFile(plugins_dir / "contrib-menu" / "init.lua", R"(
local ide = require("microide")
return ide.plugin({
  id = "contrib.menu",
  setup = function(ctx)
    ctx.commands.add("contrib.menu.greet", function() end)
    ctx.menus.add({
      id = "greet",
      menu = "file",
      action = "contrib.menu.greet",
      label = "Greet",
    })
  end,
})
)");

  PluginHost host;
  ScopedPluginConfigHomeEnv config_home(temp.path() / "config");
  host.Reload(temp.path() / "project");
  Expect(!host.ContributedMenuEntries().empty(), "plugin should contribute menu entries");
  Expect(host.ContributedMenuEntries().front().menu == "file",
         "contributed entry should target 'file' menu");

  const auto file_items = ContributedMenuItems(MenuId::File, host);
  Expect(!file_items.empty(), "ContributedMenuItems for File should return plugin entries");
  Expect(file_items.front().label == "Greet", "menu item label should match");
}

// ---------------------------------------------------------------------------
// Plugin-contributed keybindings
// ---------------------------------------------------------------------------

void TestPluginContributedKeybindings() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp;
  const std::filesystem::path plugins_dir = temp.path() / "config" / "microide" / "plugins";
  WriteFile(plugins_dir / "contrib-kb" / "init.lua", R"(
local ide = require("microide")
return ide.plugin({
  id = "contrib.kb",
  setup = function(ctx)
    ctx.keybindings.add({
      id = "save-alias",
      action = "save",
      key = "Ctrl+Shift+S",
      context = "editor",
    })
  end,
})
)");

  PluginHost host;
  ScopedPluginConfigHomeEnv config_home(temp.path() / "config");
  host.Reload(temp.path() / "project");
  Expect(!host.ContributedKeybindings().empty(), "plugin should contribute keybindings");
  Expect(host.ContributedKeybindings().front().key_chord == "Ctrl+Shift+S",
         "keybinding chord should match");
  Expect(host.ContributedKeybindings().front().context == "editor",
         "keybinding context should be editor");
}

// ---------------------------------------------------------------------------
// Plugin-contributed status items
// ---------------------------------------------------------------------------

void TestPluginContributedStatusItems() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp;
  const std::filesystem::path plugins_dir = temp.path() / "config" / "microide" / "plugins";
  WriteFile(plugins_dir / "contrib-status" / "init.lua", R"(
local ide = require("microide")
return ide.plugin({
  id = "contrib.status",
  setup = function(ctx)
    ctx.status.add({
      id = "info",
      text = "ready",
      tooltip = "Plugin is ready",
      alignment = "right",
      priority = 10,
    })
  end,
})
)");

  PluginHost host;
  ScopedPluginConfigHomeEnv config_home(temp.path() / "config");
  host.Reload(temp.path() / "project");
  Expect(!host.ContributedStatusItems().empty(), "plugin should contribute status items");
  Expect(host.ContributedStatusItems().front().text == "ready", "status text should match");
  Expect(host.ContributedStatusItems().front().alignment == "right",
         "status alignment should be right");

  const auto views = ResolveStatusItems(host);
  Expect(!views.empty(), "ResolveStatusItems should return resolved items");
  Expect(views.front().text == "ready", "resolved status item text should match");
}

void TestPluginStatusItemUpdate() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp;
  const std::filesystem::path plugins_dir = temp.path() / "config" / "microide" / "plugins";
  WriteFile(plugins_dir / "update-status" / "init.lua", R"(
local ide = require("microide")
return ide.plugin({
  id = "update.status",
  setup = function(ctx)
    ctx.status.add({ id = "counter", text = "0" })
    ctx.commands.add("update.status.tick", function(c, args)
      ctx.status.update("counter", { text = "1" })
    end)
  end,
})
)");

  PluginHost host;
  ScopedPluginConfigHomeEnv config_home(temp.path() / "config");
  host.Reload(temp.path() / "project");
  Expect(host.ContributedStatusItems().front().text == "0", "initial status text should be 0");
  host.ExecuteCommand("update.status.tick", {});
  Expect(host.ContributedStatusItems().front().text == "1",
         "status text should update after command");
}

// ---------------------------------------------------------------------------
// Sidebar view ordering and visibility
// ---------------------------------------------------------------------------

void TestSidebarOrderedViewsNoPolicy() {
  PluginHost host;
  const auto ordered = OrderedSidebarViews(host, {});
  const auto all = microide::workspace::SidebarViews(host);
  Expect(ordered.size() == all.size(),
         "with no policies all views should appear");
}

void TestSidebarHideView() {
  PluginHost host;
  std::vector<SidebarViewPolicy> policies;
  policies.push_back(SidebarViewPolicy{"git", true, 0});
  const auto ordered = OrderedSidebarViews(host, policies);
  const bool has_git = std::any_of(ordered.begin(), ordered.end(),
                                    [](const auto& v) { return v.id == "git"; });
  Expect(!has_git, "hidden view should not appear in ordered list");
}

void TestSidebarReorderViews() {
  PluginHost host;
  std::vector<SidebarViewPolicy> policies;
  policies.push_back(SidebarViewPolicy{"git", false, 0});
  policies.push_back(SidebarViewPolicy{"tree", false, 1});
  const auto ordered = OrderedSidebarViews(host, policies);
  Expect(!ordered.empty(), "should have views");
  Expect(ordered.front().id == "git", "git should be first due to order=0");
}

// ---------------------------------------------------------------------------
// Persistence – settings and keybinding overrides
// ---------------------------------------------------------------------------


// ---------------------------------------------------------------------------
// Settings get callback
// ---------------------------------------------------------------------------

void TestPluginSettingsGetCallback() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp;
  const std::filesystem::path plugins_dir = temp.path() / "config" / "microide" / "plugins";
  WriteFile(plugins_dir / "settings-get" / "init.lua", R"(
local ide = require("microide")
return ide.plugin({
  id = "settings.get",
  setup = function(ctx)
    ctx.settings.declare({
      id = "my_val",
      type = "string",
      default = "default-val",
      label = "Val",
    })
    ctx.commands.add("settings.get.read", function(c, args)
      local v = ctx.settings.get("settings.get.my_val")
      ctx.log("value=" .. (v or "nil"))
    end)
  end,
})
)");

  PluginHost host;
  bool get_setting_called = false;
  PluginHost::Callbacks callbacks;
  callbacks.get_setting = [&](std::string_view id) -> std::optional<std::string> {
    get_setting_called = true;
    if (id == "settings.get.my_val") {
      return std::string("custom-value");
    }
    return std::nullopt;
  };
  host.SetCallbacks(std::move(callbacks));
  ScopedPluginConfigHomeEnv config_home(temp.path() / "config");
  host.Reload(temp.path() / "project");
  host.ExecuteCommand("settings.get.read", {});
  Expect(get_setting_called, "get_setting callback should be called");
  const bool logged_custom = std::any_of(
      host.Messages().begin(), host.Messages().end(),
      [](const std::string& m) { return m.find("custom-value") != std::string::npos; });
  Expect(logged_custom, "plugin should receive the custom value from the callback");
}

}  // namespace

void RegisterContributionRegistryTests(std::vector<TestCase>& tests) {
  AddTest(tests, "KeybindingRegistry/BuiltinsNonEmpty",
          TestKeybindingRegistryBuiltinsNonEmpty);
  AddTest(tests, "KeybindingRegistry/FindById", TestKeybindingRegistryFindById);
  AddTest(tests, "KeybindingRegistry/FindUnknown", TestKeybindingRegistryFindUnknown);
  AddTest(tests, "KeybindingRegistry/ParseKeyChordSingleKey",
          TestParseKeyChordSingleKey);
  AddTest(tests, "KeybindingRegistry/ParseKeyChordWithModifiers",
          TestParseKeyChordWithModifiers);
  AddTest(tests, "KeybindingRegistry/ParseKeyChordMultipleModifiers",
          TestParseKeyChordMultipleModifiers);
  AddTest(tests, "KeybindingRegistry/ParseKeyChordInvalid",
          TestParseKeyChordInvalid);
  AddTest(tests, "KeybindingRegistry/FormatKeyChordRoundTrip",
          TestFormatKeyChordRoundTrip);
  AddTest(tests, "KeybindingRegistry/FindByKey", TestFindBuiltinKeybindingByKey);
  AddTest(tests, "KeybindingRegistry/FindByLeftCtrlKey",
          TestFindBuiltinKeybindingByLeftCtrlKey);
  AddTest(tests, "KeybindingRegistry/ResolveEmpty", TestResolveKeybindingsEmpty);
  AddTest(tests, "KeybindingRegistry/ResolveDisabled", TestResolveKeybindingsDisabled);
  AddTest(tests, "KeybindingRegistry/ResolveLeftCtrlMatch",
          TestResolveKeybindingsFindLeftCtrlMatch);
  AddTest(tests, "SettingsRegistry/BuiltinsNonEmpty", TestSettingsRegistryBuiltinsNonEmpty);
  AddTest(tests, "SettingsRegistry/FindById", TestSettingsRegistryFindById);
  AddTest(tests, "SettingsRegistry/WrapSpecAndEnumValues",
          TestSettingsRegistryWrapSpecAndEnumValues);
  AddTest(tests, "SettingsRegistry/FindUnknown", TestSettingsRegistryFindUnknown);
  AddTest(tests, "SettingsRegistry/ParseBool", TestParseSettingValueBool);
  AddTest(tests, "SettingsRegistry/ParseInt", TestParseSettingValueInt);
  AddTest(tests, "SettingsRegistry/ParseFloat", TestParseSettingValueFloat);
  AddTest(tests, "SettingsRegistry/ParseInvalid", TestParseSettingValueInvalid);
  AddTest(tests, "SettingsRegistry/ParseWrapEnum", TestParseSettingValueWrapEnum);
  AddTest(tests, "SettingsRegistry/SerializeRoundTrip", TestSerializeSettingValueRoundTrip);
  AddTest(tests, "SettingsRegistry/DefaultValue", TestDefaultSettingValue);
  AddTest(tests, "SettingsRegistry/PluginDeclare", TestPluginContributedSettingsDeclare);
  AddTest(tests, "MenuRegistry/PluginContributions", TestPluginContributedMenuEntries);
  AddTest(tests, "KeybindingRegistry/PluginContributions",
          TestPluginContributedKeybindings);
  AddTest(tests, "StatusRegistry/PluginContributions", TestPluginContributedStatusItems);
  AddTest(tests, "StatusRegistry/Update", TestPluginStatusItemUpdate);
  AddTest(tests, "SidebarRegistry/OrderedNoPolicy", TestSidebarOrderedViewsNoPolicy);
  AddTest(tests, "SidebarRegistry/HideView", TestSidebarHideView);
  AddTest(tests, "SidebarRegistry/ReorderViews", TestSidebarReorderViews);
  AddTest(tests, "SettingsRegistry/GetCallback", TestPluginSettingsGetCallback);
}

}  // namespace microide::tests
