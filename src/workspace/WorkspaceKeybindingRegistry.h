#pragma once

#include <SDL3/SDL.h>
#include <array>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "workspace/WorkspaceActionTypes.h"

namespace microide::plugin {
class PluginHost;
}  // namespace microide::plugin

namespace microide::workspace {

enum class KeybindingContext {
  Global,
  Editor,
  Sidebar,
  Terminal,
};

struct KeybindingSpec {
  std::string_view id;
  ActionId action = ActionId::Colorscheme;
  SDL_Keycode key = SDLK_UNKNOWN;
  SDL_Keymod modifiers = SDL_KMOD_NONE;
  KeybindingContext context = KeybindingContext::Global;
  std::array<std::string_view, 2> args{};
  std::size_t arg_count = 0;
  std::string_view command_name;
};

std::span<const KeybindingSpec> BuiltinKeybindingSpecs();
const KeybindingSpec* FindBuiltinKeybinding(std::string_view id);
const KeybindingSpec* FindBuiltinKeybindingByKey(SDL_Keycode key,
                                                  SDL_Keymod modifiers,
                                                  KeybindingContext context);

struct ResolvedKeybinding {
  std::string id;
  ActionId action = ActionId::Colorscheme;
  SDL_Keycode key = SDLK_UNKNOWN;
  SDL_Keymod modifiers = SDL_KMOD_NONE;
  KeybindingContext context = KeybindingContext::Global;
  std::vector<std::string> args;
  std::string command_name;
  bool from_plugin = false;
};

std::vector<ResolvedKeybinding> ResolveKeybindings(
    const plugin::PluginHost& plugin_host,
    const std::vector<std::string>& disabled_ids = {});
const ResolvedKeybinding* FindKeybinding(const std::vector<ResolvedKeybinding>& bindings,
                                          SDL_Keycode key,
                                          SDL_Keymod modifiers,
                                          KeybindingContext context);

// Parse "Ctrl+Shift+S" → key + modifiers. Returns false for unrecognised chords.
bool ParseKeyChord(std::string_view chord, SDL_Keycode* key, SDL_Keymod* modifiers);
std::string FormatKeyChord(SDL_Keycode key, SDL_Keymod modifiers);

}  // namespace microide::workspace
