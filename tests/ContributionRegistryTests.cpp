#include "TestSupport.h"

#include "editor/PluginDecorationStore.h"
#include "editor/PluginSurfaceStore.h"
#include "plugin/PluginHost.h"
#include "render/PluginDisplayList.h"

#include <variant>
#include "workspace/WorkspaceFileIconRegistry.h"
#include "workspace/WorkspaceKeybindingRegistry.h"
#include "workspace/WorkspaceMenuRegistry.h"
#include "workspace/WorkspacePersistenceFormat.h"
#include "workspace/WorkspaceSettingsRegistry.h"
#include "workspace/WorkspaceSidebarRegistry.h"
#include "workspace/WorkspaceStatusRegistry.h"
#include "workspace/WorkspaceThemeRegistry.h"

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
using microide::workspace::StatusItemTone;
using microide::workspace::WorkspaceFileIconRegistry;
using microide::workspace::WorkspaceThemeRegistry;
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

void TestPluginStatusItemEnrichment() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp;
  const std::filesystem::path plugins_dir = temp.path() / "config" / "microide" / "plugins";
  WriteFile(plugins_dir / "rich-status" / "init.lua", R"(
local ide = require("microide")
return ide.plugin({
  id = "rich.status",
  setup = function(ctx)
    ctx.status.add({
      id = "build",
      text = "building",
      icon = "dot",
      tone = "warning",
      command = "rich.status.cancel",
      progress = 0.5,
      alignment = "left",
    })
    ctx.commands.add("rich.status.cancel", function() end)
  end,
})
)");

  PluginHost host;
  ScopedPluginConfigHomeEnv config_home(temp.path() / "config");
  host.Reload(temp.path() / "project");
  const auto views = ResolveStatusItems(host);
  Expect(!views.empty(), "ResolveStatusItems should return the enriched item");
  const auto& item = views.front();
  Expect(item.icon == "dot", "icon should round-trip");
  Expect(item.tone == StatusItemTone::Warning, "tone should resolve to Warning");
  Expect(item.command == "rich.status.cancel", "command should round-trip");
  Expect(item.progress > 0.49f && item.progress < 0.51f, "progress should round-trip ~0.5");
}

// ---------------------------------------------------------------------------
// Plugin-contributed colour themes (Phase D)
// ---------------------------------------------------------------------------

void TestPluginContributedTheme() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp;
  const std::filesystem::path plugins_dir = temp.path() / "config" / "microide" / "plugins";
  WriteFile(plugins_dir / "contrib-theme" / "init.lua", R"(
local ide = require("microide")
return ide.plugin({
  id = "contrib.theme",
  setup = function(ctx)
    ctx.themes.add({
      id = "noir",
      label = "Noir",
      colors = {
        ["default"] = "#d0d0d0,#101014",
        comment = "#6a9955",
        statement = "#569cd6",
      },
    })
  end,
})
)");

  PluginHost host;
  ScopedPluginConfigHomeEnv config_home(temp.path() / "config");
  host.Reload(temp.path() / "project");
  Expect(!host.ContributedThemes().empty(), "plugin should contribute a theme");
  Expect(host.ContributedThemes().front().id == "contrib.theme.noir",
         "theme id should be host-namespaced");

  WorkspaceThemeRegistry registry;
  registry.Rebuild(host);
  Expect(registry.Contains("contrib.theme.noir"), "registry should contain the theme");
  const auto names = registry.Names();
  Expect(std::find(names.begin(), names.end(), "contrib.theme.noir") != names.end(),
         "Names() should list the contributed theme");
  Expect(registry.Label("contrib.theme.noir") == "Noir", "label should round-trip");

  const auto theme = registry.Resolve("contrib.theme.noir");
  Expect(theme.has_value(), "registry should resolve the theme");
  // The "default" group's background drives editor_background verbatim.
  Expect(theme->editor_background.r == 0x10 && theme->editor_background.g == 0x10 &&
             theme->editor_background.b == 0x14,
         "editor background should reflect the contributed default colour");
  Expect(!registry.Resolve("contrib.theme.missing").has_value(),
         "unknown theme id should resolve to nullopt");
}

// ---------------------------------------------------------------------------
// Plugin-contributed file-icon themes (Phase D)
// ---------------------------------------------------------------------------

void TestPluginFileIconTheme() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp;
  const std::filesystem::path plugins_dir = temp.path() / "config" / "microide" / "plugins";
  WriteFile(plugins_dir / "contrib-icons" / "init.lua", R"(
local ide = require("microide")
return ide.plugin({
  id = "contrib.icons",
  setup = function(ctx)
    ctx.file_icons.add({
      id = "demo",
      rules = {
        { ext = "csv", icon = "diamond", color = "#80c080" },
        { name = "Makefile", icon = "square", color = "#888888" },
      },
    })
  end,
})
)");

  PluginHost host;
  ScopedPluginConfigHomeEnv config_home(temp.path() / "config");
  host.Reload(temp.path() / "project");
  Expect(!host.ContributedFileIconThemes().empty(), "plugin should contribute a file-icon theme");

  WorkspaceFileIconRegistry registry;
  registry.Rebuild(host);

  const auto csv = registry.Resolve("data.csv");
  Expect(csv.has_value() && csv->shape == editor::GutterIconShape::Diamond,
         "plugin extension rule should map .csv to a diamond");
  const auto makefile = registry.Resolve("Makefile");
  Expect(makefile.has_value() && makefile->shape == editor::GutterIconShape::Square,
         "plugin filename rule should map Makefile to a square");
  // Built-in fallback still applies for unconfigured types.
  const auto cpp = registry.Resolve("main.cpp");
  Expect(cpp.has_value() && cpp->shape == editor::GutterIconShape::Dot,
         "built-in default should map .cpp to a dot");
  Expect(!registry.Resolve("mystery.zzz").has_value(),
         "unknown extension should resolve to no icon");
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

// ---------------------------------------------------------------------------
// Phase C: tree-capable plugin sidebar
// ---------------------------------------------------------------------------

void TestPluginTreeSidebarToggle() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp;
  const std::filesystem::path plugins_dir = temp.path() / "config" / "microide" / "plugins";
  WriteFile(plugins_dir / "tree" / "init.lua", R"lua(
local ide = require("microide")
local expanded = true
return ide.plugin({
  id = "tree",
  setup = function(ctx)
    ctx.sidebar.add({
      id = "tree",
      label = "Tree",
      snapshot = function()
        local rows = {
          { id = "root", label = "Root", depth = 0, collapsible = true, collapsed = not expanded },
        }
        if expanded then
          rows[#rows + 1] = { label = "Child", depth = 1, path = "child.txt", line = 3 }
        end
        return rows
      end,
      on_toggle = function(item)
        if item.id == "root" then expanded = not expanded end
      end,
    })
    -- A flat sidebar with no on_toggle: toggling must be a silent no-op.
    ctx.sidebar.add({
      id = "flat",
      label = "Flat",
      snapshot = function()
        return { { label = "Only", path = "only.txt" } }
      end,
    })
  end,
})
)lua");

  PluginHost host;
  ScopedPluginConfigHomeEnv config_home(temp.path() / "config");
  host.Reload(temp.path() / "project");

  std::vector<PluginHost::SidebarItem> items;
  std::string error;
  Expect(host.SnapshotSidebar("tree", &items, &error), "tree sidebar should snapshot");
  Expect(items.size() == 2, "expanded tree should expose root + child");
  Expect(items[0].id == "root" && items[0].collapsible && !items[0].collapsed &&
             items[0].depth == 0,
         "root row should be a collapsible depth-0 node");
  Expect(items[1].depth == 1, "child row should report depth 1");

  Expect(host.ToggleSidebarItem("tree", items[0], &error),
         "toggling a collapsible row should invoke on_toggle");
  Expect(host.SnapshotSidebar("tree", &items, &error), "tree sidebar should re-snapshot");
  Expect(items.size() == 1 && items[0].collapsed,
         "collapsing the root should hide its child and mark it collapsed");

  // A flat sidebar with no on_toggle reports toggle as an unhandled no-op.
  std::vector<PluginHost::SidebarItem> flat_items;
  Expect(host.SnapshotSidebar("flat", &flat_items, &error) && flat_items.size() == 1,
         "flat sidebar should snapshot its single row");
  Expect(!host.ToggleSidebarItem("flat", flat_items[0], &error) && error.empty(),
         "toggling a sidebar without on_toggle should be a silent no-op");
}

// ---------------------------------------------------------------------------
// Phase C: plugin-native language providers
// ---------------------------------------------------------------------------

void TestPluginLanguageProviders() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp;
  const std::filesystem::path plugins_dir = temp.path() / "config" / "microide" / "plugins";
  WriteFile(plugins_dir / "lang" / "init.lua", R"lua(
local ide = require("microide")
return ide.plugin({
  id = "lang",
  setup = function(ctx)
    ctx.definition.add({
      id = "defn", language_id = "lua",
      provide = function(buffer, position)
        return { { path = buffer.path, line = position.line, column = position.column } }
      end,
    })
    ctx.references.add({
      id = "refs", language_id = "lua",
      provide = function(buffer, position, include_declaration)
        local out = { { path = buffer.path, line = position.line, column = 1 } }
        if include_declaration then out[#out + 1] = { path = buffer.path, line = 1, column = 1 } end
        return out
      end,
    })
    ctx.signature_help.add({
      id = "sig", language_id = "lua",
      provide = function(_, _)
        return {
          active_signature = 0,
          signatures = {
            { label = "greet(name)", documentation = "doc", active_parameter = 0,
              parameters = { { label = "name" } } },
          },
        }
      end,
    })
    ctx.document_symbols.add({
      id = "sym", language_id = "lua",
      provide = function(_)
        return {
          { name = "Widget", kind = "class", line = 4, column = 1,
            children = { { name = "draw", kind = "method", line = 8, column = 3 } } },
          { name = "main", kind = "function", line = 40, column = 1 },
        }
      end,
    })
  end,
})
)lua");

  PluginHost host;
  ScopedPluginConfigHomeEnv config_home(temp.path() / "config");
  host.Reload(temp.path() / "project");
  const std::filesystem::path file = temp.path() / "project" / "src" / "main.lua";

  std::string error;
  const auto definitions = host.QueryDefinition("lua", file, 12, 5, &error);
  Expect(definitions.size() == 1 && definitions.front().line == 12 &&
             definitions.front().column == 5 && !definitions.front().path.empty(),
         "definition provider should return the queried position");

  const auto references = host.QueryReferences("lua", file, 12, 5, true, &error);
  Expect(references.size() == 2, "references provider should include the declaration");

  PluginHost::SignatureHelpResult signature;
  Expect(host.QuerySignatureHelp("lua", file, 12, 5, &signature, &error),
         "signature help provider should resolve");
  Expect(signature.signatures.size() == 1 && signature.signatures.front().label == "greet(name)" &&
             signature.signatures.front().parameters.size() == 1,
         "signature help should carry label and parameters");

  const auto symbols = host.QueryDocumentSymbols("lua", file, &error);
  Expect(symbols.size() == 2 && symbols.front().name == "Widget" &&
             symbols.front().children.size() == 1 &&
             symbols.front().children.front().name == "draw",
         "document symbols should preserve nesting");

  // A language with no provider yields nothing (so callers fall back to LSP).
  Expect(host.QueryDefinition("python", file, 1, 1, &error).empty(),
         "unmatched language should yield no plugin definitions");
}

}  // namespace

// ---------------------------------------------------------------------------
// Phase E: plugin content surfaces (ctx.surface.set)
// ---------------------------------------------------------------------------

void TestPluginSurfacePublish() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp;
  const std::filesystem::path plugins_dir = temp.path() / "config" / "microide" / "plugins";
  WriteFile(plugins_dir / "surface-test" / "init.lua", R"(
local ide = require("microide")
return ide.plugin({
  id = "surface.test",
  setup = function(ctx)
    ctx.commands.add("surface.test.publish", function()
      ctx.surface.set("chart", {
        title = "Chart",
        preview = "bottom",
        display_list = { width = 100, height = 50, ops = {
          { op = "rect", x = 0, y = 0, w = 10, h = 10, color = "#112233" },
          { op = "text", x = 2, y = 2, text = "hi", color = "#ffffff" },
        } },
        hit_regions = { { x = 0, y = 0, w = 100, h = 14, command = "surface.test.publish" } },
      })
    end)
    ctx.commands.add("surface.test.bad", function()
      -- Both bodies at once must be rejected by the host (no publish).
      ctx.surface.set("bad", { display_list = { ops = {} }, raster = { format = "rgba8", bytes = "x" } })
    end)
  end,
})
)");

  PluginHost host;
  std::string published_owner;
  std::string published_id;
  editor::SurfaceContent published;
  int publish_count = 0;
  PluginHost::Callbacks callbacks;
  callbacks.publish_surface = [&](std::string_view owner, std::string_view id,
                                  editor::SurfaceContent content) {
    published_owner = std::string(owner);
    published_id = std::string(id);
    published = std::move(content);
    ++publish_count;
  };
  host.SetCallbacks(std::move(callbacks));
  ScopedPluginConfigHomeEnv config_home(temp.path() / "config");
  host.Reload(temp.path() / "project");

  host.ExecuteCommand("surface.test.publish", {});
  Expect(publish_count == 1, "a valid surface.set should publish exactly once");
  Expect(published_owner == "surface.test", "publish carries the plugin id as owner");
  Expect(published_id == "chart", "publish carries the surface id");
  Expect(published.preview == editor::SurfacePreviewSlot::Bottom, "preview slot is parsed");
  Expect(published.intrinsic_width == 100.0f && published.intrinsic_height == 50.0f,
         "intrinsic size comes from the display list");
  Expect(published.hit_regions.size() == 1 && published.hit_regions[0].command == "surface.test.publish",
         "hit regions and their commands are parsed");
  const auto* list = std::get_if<render::PluginDisplayList>(&published.body);
  Expect(list != nullptr && list->ops.size() == 2, "the display list body has both ops");
  Expect(list != nullptr && list->content_hash != 0, "the display list got a content hash");

  // A spec with two bodies is rejected: the host records an error and never publishes.
  host.ExecuteCommand("surface.test.bad", {});
  Expect(publish_count == 1, "a two-body spec must not publish");
}

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
  AddTest(tests, "StatusRegistry/Enrichment", TestPluginStatusItemEnrichment);
  AddTest(tests, "ThemeRegistry/PluginContributions", TestPluginContributedTheme);
  AddTest(tests, "FileIconRegistry/PluginContributions", TestPluginFileIconTheme);
  AddTest(tests, "SidebarRegistry/OrderedNoPolicy", TestSidebarOrderedViewsNoPolicy);
  AddTest(tests, "SidebarRegistry/HideView", TestSidebarHideView);
  AddTest(tests, "SidebarRegistry/ReorderViews", TestSidebarReorderViews);
  AddTest(tests, "SettingsRegistry/GetCallback", TestPluginSettingsGetCallback);
  AddTest(tests, "SidebarRegistry/PluginTreeToggle", TestPluginTreeSidebarToggle);
  AddTest(tests, "LanguageProviders/PluginQueries", TestPluginLanguageProviders);
  AddTest(tests, "SurfaceRegistry/PluginPublish", TestPluginSurfacePublish);
}

}  // namespace microide::tests
