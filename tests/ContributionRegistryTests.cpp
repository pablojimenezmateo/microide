#include "TestSupport.h"

#include "editor/PluginDecorationStore.h"
#include "editor/PluginSurfaceStore.h"
#include "plugin/PluginHost.h"
#include "plugin/PluginRegistryInterop.h"
#include "render/PluginDisplayList.h"

#include <cstddef>
#include <unordered_map>

#include <variant>
#include "workspace/registries/WorkspaceFileIconRegistry.h"
#include "workspace/registries/WorkspaceKeybindingRegistry.h"
#include "workspace/registries/WorkspaceMenuRegistry.h"
#include "workspace/persistence/WorkspacePersistenceFormat.h"
#include "workspace/registries/WorkspaceSettingsRegistry.h"
#include "workspace/registries/WorkspaceSidebarRegistry.h"
#include "workspace/registries/WorkspaceStatusRegistry.h"
#include "workspace/registries/WorkspaceThemeRegistry.h"

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
using microide::workspace::StatusItemCache;
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

void TestKeybindingRegistryNoOverlaps() {
  // Two built-in bindings collide when they share the same key + modifiers AND
  // their contexts can be active at the same time: identical contexts, or either
  // is Global (a Global binding matches in every context via FindKeybinding).
  // Bindings in two different non-Global contexts (e.g. Editor vs Terminal)
  // never fire in the same focus state, so they do not collide.
  const auto specs = BuiltinKeybindingSpecs();
  constexpr SDL_Keymod kRelevant = static_cast<SDL_Keymod>(
      SDL_KMOD_CTRL | SDL_KMOD_SHIFT | SDL_KMOD_ALT | SDL_KMOD_GUI);
  const auto contexts_overlap = [](KeybindingContext a, KeybindingContext b) {
    return a == b || a == KeybindingContext::Global || b == KeybindingContext::Global;
  };
  for (std::size_t i = 0; i < specs.size(); ++i) {
    for (std::size_t j = i + 1; j < specs.size(); ++j) {
      const auto& a = specs[i];
      const auto& b = specs[j];
      const bool same_chord =
          a.key == b.key && (a.modifiers & kRelevant) == (b.modifiers & kRelevant);
      const bool clash = same_chord && contexts_overlap(a.context, b.context);
      Expect(!clash, (std::string("keybinding overlap: '") + std::string(a.id) + "' and '" +
                      std::string(b.id) + "' share a chord in overlapping contexts")
                         .c_str());
    }
  }
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

// A chord names a KEYCODE — what the key produces under the user's layout —
// because that is what a key event carries and what FindKeybinding compares.
// The parser used to go through the physical QWERTY scancode, so on QWERTZ a
// plugin's "ctrl+z" bound to the key labelled Y. Pin the mapping to the keycode
// constants (on this test's QWERTY keymap both forms agree, so the pin is the
// contract, not a layout repro).
void TestParseKeyChordResolvesKeycodesByName() {
  SDL_Keycode key = SDLK_UNKNOWN;
  SDL_Keymod mods = SDL_KMOD_NONE;
  Expect(ParseKeyChord("ctrl+z", &key, &mods) && key == SDLK_Z, "z is SDLK_Z");
  Expect(ParseKeyChord("Ctrl+Y", &key, &mods) && key == SDLK_Y, "Y is SDLK_Y, case-insensitive");
  Expect(ParseKeyChord("alt+1", &key, &mods) && key == SDLK_1 && (mods & SDL_KMOD_ALT) != 0,
         "a digit is its keycode");
  Expect(ParseKeyChord("ctrl+-", &key, &mods) && key == SDLK_MINUS, "'-' is SDLK_MINUS");
  Expect(ParseKeyChord("ctrl+=", &key, &mods) && key == SDLK_EQUALS, "'=' is SDLK_EQUALS");
  Expect(ParseKeyChord("ctrl+[", &key, &mods) && key == SDLK_LEFTBRACKET, "'[' is its keycode");
  Expect(ParseKeyChord("ctrl+/", &key, &mods) && key == SDLK_SLASH, "'/' is SDLK_SLASH");
  Expect(ParseKeyChord("ctrl+`", &key, &mods) && key == SDLK_GRAVE, "'`' is SDLK_GRAVE");
  Expect(ParseKeyChord("F12", &key, &mods) && key == SDLK_F12, "F12 is SDLK_F12");
  Expect(ParseKeyChord("shift+enter", &key, &mods) && key == SDLK_RETURN &&
             (mods & SDL_KMOD_SHIFT) != 0,
         "enter is SDLK_RETURN");
  Expect(ParseKeyChord("escape", &key, &mods) && key == SDLK_ESCAPE, "escape is SDLK_ESCAPE");
  Expect(ParseKeyChord("ctrl+pagedown", &key, &mods) && key == SDLK_PAGEDOWN,
         "pagedown is SDLK_PAGEDOWN");
  Expect(ParseKeyChord("super+space", &key, &mods) && key == SDLK_SPACE &&
             (mods & SDL_KMOD_GUI) != 0,
         "space is SDLK_SPACE and super is the GUI modifier");
  Expect(!ParseKeyChord("ctrl+nosuchkey", &key, &mods), "an unknown key name fails");
  Expect(!ParseKeyChord("ctrl+", &key, &mods), "a trailing modifier with no key fails");
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
  const auto* palette_spec = FindBuiltinKeybinding("command-palette");
  Expect(palette_spec != nullptr, "command palette spec should exist");
  const auto* found = FindBuiltinKeybindingByKey(
      palette_spec->key, SDL_KMOD_LCTRL | SDL_KMOD_LSHIFT, KeybindingContext::Global);
  Expect(found != nullptr, "left control/shift should match Ctrl+Shift bindings");
  Expect(found->id == palette_spec->id,
         "left modifiers should resolve the command palette shortcut");
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
  const auto* found = FindKeybinding(bindings, SDLK_P, SDL_KMOD_LCTRL | SDL_KMOD_LSHIFT,
                                     KeybindingContext::Global);
  Expect(found != nullptr, "resolved bindings should match left-modifier shortcuts");
  Expect(found->id == "command-palette",
         "left modifiers should resolve to the command palette shortcut");
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

// A plugin chord that collides with an already-resolved binding (a built-in, or an
// earlier plugin) must be skipped in ResolveKeybindings: built-ins resolve first and
// FindKeybinding returns the first match, so a shadowed plugin binding would be shown
// in Help/Settings yet silently run the winner. Regression for inventory J2.
void TestResolveKeybindingsSkipsBuiltinCollision() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp;
  const std::filesystem::path plugins_dir = temp.path() / "config" / "microide" / "plugins";
  WriteFile(plugins_dir / "collide-kb" / "init.lua", R"(
local ide = require("microide")
return ide.plugin({
  id = "collide.kb",
  setup = function(ctx)
    -- Collides with the built-in Ctrl+S "save" (a Global binding matches the editor
    -- context), so this contribution must NOT win.
    ctx.keybindings.add({ id = "shadow-save", action = "save", key = "Ctrl+S", context = "editor" })
    -- A free chord: no built-in owns Ctrl+Alt+Y, so this one survives.
    ctx.keybindings.add({ id = "free", action = "save", key = "Ctrl+Alt+Y", context = "editor" })
  end,
})
)");

  PluginHost host;
  ScopedPluginConfigHomeEnv config_home(temp.path() / "config");
  host.Reload(temp.path() / "project");

  const auto bindings = ResolveKeybindings(host);

  const auto* ctrl_s =
      FindKeybinding(bindings, SDLK_S, SDL_KMOD_CTRL, KeybindingContext::Editor);
  Expect(ctrl_s != nullptr, "Ctrl+S should resolve");
  Expect(!ctrl_s->from_plugin && ctrl_s->id == "save",
         "a colliding plugin Ctrl+S must not shadow the built-in save binding");
  const bool shadow_present = std::any_of(bindings.begin(), bindings.end(), [](const auto& rb) {
    return rb.id == "collide.kb.shadow-save";
  });
  Expect(!shadow_present, "the colliding plugin binding must be dropped, not advertised");

  const auto* free_chord = FindKeybinding(
      bindings, SDLK_Y, static_cast<SDL_Keymod>(SDL_KMOD_CTRL | SDL_KMOD_ALT),
      KeybindingContext::Editor);
  Expect(free_chord != nullptr && free_chord->from_plugin,
         "a non-colliding plugin chord must still resolve as a plugin binding");
}

// When two plugins register the same chord, only the first resolved one wins; the
// second collides with an already-resolved plugin binding and is skipped. Regression
// for inventory J2 ("two plugins registering the same chord → second skipped").
void TestResolveKeybindingsSkipsSecondPluginSameChord() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp;
  const std::filesystem::path plugins_dir = temp.path() / "config" / "microide" / "plugins";
  WriteFile(plugins_dir / "kb-a" / "init.lua", R"(
local ide = require("microide")
return ide.plugin({
  id = "kb.a",
  setup = function(ctx)
    ctx.keybindings.add({ id = "u", action = "save", key = "Ctrl+Alt+U", context = "editor" })
  end,
})
)");
  WriteFile(plugins_dir / "kb-b" / "init.lua", R"(
local ide = require("microide")
return ide.plugin({
  id = "kb.b",
  setup = function(ctx)
    ctx.keybindings.add({ id = "u", action = "save", key = "Ctrl+Alt+U", context = "editor" })
  end,
})
)");

  PluginHost host;
  ScopedPluginConfigHomeEnv config_home(temp.path() / "config");
  host.Reload(temp.path() / "project");

  const auto bindings = ResolveKeybindings(host);
  const int u_matches = static_cast<int>(std::count_if(bindings.begin(), bindings.end(),
                                                       [](const auto& rb) {
                                                         return rb.key == SDLK_U && rb.from_plugin;
                                                       }));
  Expect(u_matches == 1,
         "two plugins registering the same chord must resolve to exactly one binding");
}

// ---------------------------------------------------------------------------
// Plugin-contributed status items
// ---------------------------------------------------------------------------

// registry_interop::ApplyStatusItemUpdate now keeps an id->position cache so a
// runtime ctx.status.update resolves in O(1). This exercises the cache directly:
// updates land on the right row, the lazy rebuild triggers when the order vector's
// size changes (register/teardown), and a same-size structural swap still resolves.
void TestStatusItemUpdateIndexCache() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  namespace registry_interop = microide::plugin::registry_interop;
  using ContributedStatusItem = PluginHost::ContributedStatusItem;

  std::vector<ContributedStatusItem> order = {
      ContributedStatusItem{.id = "p.a", .text = "a0"},
      ContributedStatusItem{.id = "p.b", .text = "b0"},
      ContributedStatusItem{.id = "p.c", .text = "c0"},
  };
  std::unordered_map<std::string, std::size_t> index;

  const auto text_update = [](const std::string& id, const std::string& text) {
    registry_interop::StatusItemUpdate u;
    u.full_id = id;
    u.has_text = true;
    u.text = text;
    return u;
  };

  Expect(registry_interop::ApplyStatusItemUpdate(text_update("p.b", "b1"), &order, &index),
         "update finds an existing item");
  Expect(order[1].text == "b1", "the right row is updated");
  Expect(!registry_interop::ApplyStatusItemUpdate(text_update("p.missing", "x"), &order, &index),
         "an unknown id reports no match");

  // Simulate a teardown: the order vector shrinks. The next update must rebuild the
  // stale index (size mismatch) and still resolve the surviving row.
  order.erase(order.begin());  // drop "p.a"; order is now [p.b, p.c]
  Expect(registry_interop::ApplyStatusItemUpdate(text_update("p.c", "c1"), &order, &index),
         "update resolves after a structural shrink");
  Expect(order[1].text == "c1", "the surviving row updates at its new position");
  Expect(!registry_interop::ApplyStatusItemUpdate(text_update("p.a", "x"), &order, &index),
         "the removed id no longer resolves");

  // Simulate a same-size swap (one id replaced by another): the id-verify fallback
  // must rebuild and resolve rather than trust a stale slot.
  order[0].id = "p.d";
  order[0].text = "d0";
  Expect(registry_interop::ApplyStatusItemUpdate(text_update("p.d", "d1"), &order, &index),
         "a same-size id swap still resolves via the id-verify fallback");
  Expect(order[0].text == "d1", "the swapped-in row updates");
}

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

// Two status items with equal alignment+priority must keep their registration
// order (stable_sort), not reorder nondeterministically between revisions.
void TestPluginStatusItemsEqualPriorityKeepRegistrationOrder() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp;
  const std::filesystem::path plugins_dir = temp.path() / "config" / "microide" / "plugins";
  WriteFile(plugins_dir / "contrib-order" / "init.lua", R"(
local ide = require("microide")
return ide.plugin({
  id = "contrib.order",
  setup = function(ctx)
    ctx.status.add({ id = "first",  text = "first",  alignment = "right", priority = 10 })
    ctx.status.add({ id = "second", text = "second", alignment = "right", priority = 10 })
  end,
})
)");

  PluginHost host;
  ScopedPluginConfigHomeEnv config_home(temp.path() / "config");
  host.Reload(temp.path() / "project");

  const auto& contribs = host.ContributedStatusItems();
  Expect(contribs.size() == 2, "both equal-priority status items should be contributed");
  const auto views = ResolveStatusItems(host);
  Expect(views.size() == 2, "both equal-priority status items should resolve");
  // stable_sort: equal alignment+priority items preserve the contribution order
  // (deterministic, no jitter) rather than reordering under an unstable sort.
  Expect(views[0].id == contribs[0].id && views[1].id == contribs[1].id,
         "equal alignment+priority items must keep contribution order (stable_sort, no jitter)");
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

// The cache-aware resolve must reuse one sorted build until the host's status
// items change, then rebuild after a status update bumps StatusItemsRevision().
void TestStatusItemCacheInvalidatesOnRevisionBump() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp;
  const std::filesystem::path plugins_dir = temp.path() / "config" / "microide" / "plugins";
  WriteFile(plugins_dir / "cache-status" / "init.lua", R"(
local ide = require("microide")
return ide.plugin({
  id = "cache.status",
  setup = function(ctx)
    ctx.status.add({ id = "counter", text = "0" })
    ctx.commands.add("cache.status.tick", function()
      ctx.status.update("counter", { text = "1" })
    end)
  end,
})
)");

  PluginHost host;
  ScopedPluginConfigHomeEnv config_home(temp.path() / "config");
  host.Reload(temp.path() / "project");

  StatusItemCache cache;
  const auto& first = ResolveStatusItems(host, cache);
  Expect(first.size() == 1 && first.front().text == "0", "cached resolve sees the initial item");
  const auto& second = ResolveStatusItems(host, cache);
  Expect(&first == &second, "an unchanged host returns the cached view by reference");

  host.ExecuteCommand("cache.status.tick", {});  // ctx.status.update bumps the revision.
  const auto& third = ResolveStatusItems(host, cache);
  Expect(third.size() == 1 && third.front().text == "1",
         "a status update invalidates the cache and rebuilds the resolved view");
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

// The ordering must be STABLE: views the policy set says nothing about all share
// order = INT_MAX, and their relative order is the registration order the sidebar
// rail is expected to show. The sort was a `std::stable_sort`, which takes a
// `_Temporary_buffer` — one heap allocation per call, on a path that runs once per
// painted frame — and is now a `std::sort` keyed on (order, insertion index).
// This is the assertion that key has to earn (TD-2026-08-14-229).
void TestSidebarUnpolicedViewsKeepRegistrationOrder() {
  PluginHost host;
  const auto baseline = OrderedSidebarViews(host, {});
  Expect(baseline.size() > 1, "the built-in view set should have more than one view");
  // Pin the LAST view to the front. Everything else stays unpoliced, so the rest
  // must follow in exactly the order they had with no policies at all.
  std::vector<SidebarViewPolicy> policies;
  policies.push_back(SidebarViewPolicy{std::string(baseline.back().id), false, 0});
  const auto ordered = OrderedSidebarViews(host, policies);
  Expect(ordered.size() == baseline.size(), "pinning a view must not drop any");
  Expect(ordered.front().id == baseline.back().id, "the pinned view should sort first");
  for (std::size_t i = 0; i + 1 < baseline.size(); ++i) {
    Expect(ordered[i + 1].id == baseline[i].id,
           "unpoliced views must keep their registration order behind the pinned one");
  }
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

// The snapshot settings cache (TD-2026-07-17A-076) must reuse the resolved
// values while the host settings revision is unchanged, and re-resolve exactly
// when it advances. A plugin reading the same setting across two commands should
// see the OLD value while the revision is pinned (cache reuse, no re-resolve) and
// the NEW value after the revision bumps (invalidation), never a stale read.
void TestPluginSettingsSnapshotCacheInvalidation() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  TemporaryDirectory temp;
  const std::filesystem::path plugins_dir = temp.path() / "config" / "microide" / "plugins";
  WriteFile(plugins_dir / "settings-rev" / "init.lua", R"(
local ide = require("microide")
return ide.plugin({
  id = "settings.rev",
  setup = function(ctx)
    ctx.settings.declare({
      id = "my_val",
      type = "string",
      default = "default-val",
      label = "Val",
    })
    ctx.commands.add("settings.rev.read", function(c, args)
      local v = ctx.settings.get("settings.rev.my_val")
      ctx.log("value=" .. (v or "nil"))
    end)
  end,
})
)");

  PluginHost host;
  std::string current_value = "first";
  std::uint64_t revision = 1;
  int resolve_calls = 0;
  PluginHost::Callbacks callbacks;
  callbacks.get_setting = [&](std::string_view id) -> std::optional<std::string> {
    if (id == "settings.rev.my_val") {
      ++resolve_calls;
      return current_value;
    }
    return std::nullopt;
  };
  callbacks.settings_revision = [&]() { return revision; };
  host.SetCallbacks(std::move(callbacks));
  ScopedPluginConfigHomeEnv config_home(temp.path() / "config");
  host.Reload(temp.path() / "project");

  const auto last_logged_value = [&]() -> std::string {
    for (auto it = host.Messages().rbegin(); it != host.Messages().rend(); ++it) {
      const auto pos = it->find("value=");
      if (pos != std::string::npos) {
        return it->substr(pos + 6);
      }
    }
    return "<none>";
  };

  host.ExecuteCommand("settings.rev.read", {});
  Expect(last_logged_value() == "first", "plugin should read the resolved value 'first'");
  const int calls_after_first = resolve_calls;

  // Change the underlying value WITHOUT bumping the revision: the cache must serve
  // the previously-resolved value and must not re-invoke get_setting.
  current_value = "second";
  host.ExecuteCommand("settings.rev.read", {});
  Expect(last_logged_value() == "first",
         "with the revision pinned, the cached resolved value must be reused (no stale re-resolve)");
  Expect(resolve_calls == calls_after_first,
         "an unchanged revision must not re-resolve the setting through get_setting");

  // Bump the revision: the cache invalidates and the new value is resolved.
  revision = 2;
  host.ExecuteCommand("settings.rev.read", {});
  Expect(last_logged_value() == "second",
         "advancing the settings revision must invalidate the cache and resolve the new value");
  Expect(resolve_calls > calls_after_first,
         "a bumped revision must re-resolve the setting through get_setting");
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

void TestPluginDocumentSymbolsBoundedAgainstAdversarialArray() {
#if !MICROIDE_HAS_LUA_PLUGINS
  return;
#endif
  // Regression: the document-symbol harvest ran an unbounded for(;;) with a
  // metamethod-invoking lua_geti, so a returned array whose __index yields a
  // non-nil value for every integer key would spin the worker thread forever.
  // The harvest now bounds by lua_rawlen + lua_rawgeti, so a real two-element
  // array wrapped in such a metatable must terminate and return exactly the
  // two real entries.
  TemporaryDirectory temp;
  const std::filesystem::path plugins_dir = temp.path() / "config" / "microide" / "plugins";
  WriteFile(plugins_dir / "sym" / "init.lua", R"lua(
local ide = require("microide")
return ide.plugin({
  id = "sym",
  setup = function(ctx)
    ctx.document_symbols.add({
      id = "adversarial", language_id = "lua",
      provide = function(_)
        local real = {
          { name = "Alpha", kind = "class", line = 1, column = 1 },
          { name = "Beta", kind = "function", line = 2, column = 1 },
        }
        -- __index returns a fresh nameless table for every out-of-range key, so a
        -- naive lua_geti walk would never hit nil.
        return setmetatable(real, { __index = function() return {} end })
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
  const auto symbols = host.QueryDocumentSymbols("lua", file, &error);
  Expect(symbols.size() == 2, "harvest must stop at the array length, not the metatable");
  Expect(symbols.front().name == "Alpha" && symbols.back().name == "Beta",
         "harvest should return exactly the real array entries");
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
  AddTest(tests, "KeybindingRegistry/NoOverlaps", TestKeybindingRegistryNoOverlaps);
  AddTest(tests, "KeybindingRegistry/FindById", TestKeybindingRegistryFindById);
  AddTest(tests, "KeybindingRegistry/FindUnknown", TestKeybindingRegistryFindUnknown);
  AddTest(tests, "KeybindingRegistry/ParseKeyChordSingleKey",
          TestParseKeyChordSingleKey);
  AddTest(tests, "KeybindingRegistry/ParseKeyChordWithModifiers",
          TestParseKeyChordWithModifiers);
  AddTest(tests, "KeybindingRegistry/ParseKeyChordResolvesKeycodesByName",
          TestParseKeyChordResolvesKeycodesByName);
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
  AddTest(tests, "KeybindingRegistry/ResolveSkipsBuiltinCollision",
          TestResolveKeybindingsSkipsBuiltinCollision);
  AddTest(tests, "KeybindingRegistry/ResolveSkipsSecondPluginSameChord",
          TestResolveKeybindingsSkipsSecondPluginSameChord);
  AddTest(tests, "StatusRegistry/PluginContributions", TestPluginContributedStatusItems);
  AddTest(tests, "StatusRegistry/StatusItemUpdateIndexCache", TestStatusItemUpdateIndexCache);
  AddTest(tests, "StatusRegistry/EqualPriorityKeepsRegistrationOrder",
          TestPluginStatusItemsEqualPriorityKeepRegistrationOrder);
  AddTest(tests, "StatusRegistry/Update", TestPluginStatusItemUpdate);
  AddTest(tests, "StatusRegistry/CacheInvalidatesOnRevisionBump",
          TestStatusItemCacheInvalidatesOnRevisionBump);
  AddTest(tests, "StatusRegistry/Enrichment", TestPluginStatusItemEnrichment);
  AddTest(tests, "ThemeRegistry/PluginContributions", TestPluginContributedTheme);
  AddTest(tests, "FileIconRegistry/PluginContributions", TestPluginFileIconTheme);
  AddTest(tests, "SidebarRegistry/OrderedNoPolicy", TestSidebarOrderedViewsNoPolicy);
  AddTest(tests, "SidebarRegistry/HideView", TestSidebarHideView);
  AddTest(tests, "SidebarRegistry/UnpolicedViewsKeepRegistrationOrder",
          TestSidebarUnpolicedViewsKeepRegistrationOrder);
  AddTest(tests, "SidebarRegistry/ReorderViews", TestSidebarReorderViews);
  AddTest(tests, "SettingsRegistry/GetCallback", TestPluginSettingsGetCallback);
  AddTest(tests, "SettingsRegistry/SnapshotCacheInvalidation",
          TestPluginSettingsSnapshotCacheInvalidation);
  AddTest(tests, "SidebarRegistry/PluginTreeToggle", TestPluginTreeSidebarToggle);
  AddTest(tests, "LanguageProviders/PluginQueries", TestPluginLanguageProviders);
  AddTest(tests, "LanguageProviders/DocumentSymbolsBoundedAgainstAdversarialArray",
          TestPluginDocumentSymbolsBoundedAgainstAdversarialArray);
  AddTest(tests, "SurfaceRegistry/PluginPublish", TestPluginSurfacePublish);
}

}  // namespace microide::tests
