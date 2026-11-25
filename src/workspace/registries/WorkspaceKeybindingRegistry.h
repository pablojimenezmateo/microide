#pragma once

#include <SDL3/SDL.h>
#include <array>
#include <span>
#include <string>
#include <string_view>
#include <functional>
#include <vector>

#include "plugin/PluginHost.h"
#include "workspace/actions/WorkspaceActionTypes.h"

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
  bool skips_terminal = false;
  bool shares_chord = false;
  SDL_Keycode leader_key = SDLK_UNKNOWN;
  SDL_Keymod leader_modifiers = SDL_KMOD_NONE;
};

bool IsTerminalReservedChord(SDL_Keycode key, SDL_Keymod modifiers);

bool KeybindingContextsOverlap(KeybindingContext a,
                               bool a_skips_terminal,
                               KeybindingContext b,
                               bool b_skips_terminal);

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
  bool skips_terminal = false;
  bool shares_chord = false;
  SDL_Keycode leader_key = SDLK_UNKNOWN;
  SDL_Keymod leader_modifiers = SDL_KMOD_NONE;
};

std::vector<ResolvedKeybinding> ResolveKeybindings(
    const plugin::PluginHost& plugin_host,
    const std::vector<std::string>& disabled_ids = {},
    const std::vector<std::string>& skip_shell = {});

std::vector<ResolvedKeybinding> ResolveKeybindings(
    const std::vector<plugin::PluginHost::ContributedKeybinding>& contributed,
    const std::vector<std::string>& disabled_ids = {},
    const std::vector<std::string>& skip_shell = {});
struct ShadowedKeybinding {
  std::string id;
  std::string chord;
  std::string held_by;
};
std::vector<ShadowedKeybinding> ShadowedContributedKeybindings(
    const std::vector<plugin::PluginHost::ContributedKeybinding>& contributed,
    const std::vector<std::string>& disabled_ids = {},
    const std::vector<std::string>& skip_shell = {});

const ResolvedKeybinding* FindKeybinding(const std::vector<ResolvedKeybinding>& bindings,
                                          SDL_Keycode key,
                                          SDL_Keymod modifiers,
                                          KeybindingContext context,
                                          const std::function<bool(ActionId)>& action_enabled = {});
const ResolvedKeybinding* FindChordKeybinding(const std::vector<ResolvedKeybinding>& bindings,
                                               SDL_Keycode leader_key,
                                               SDL_Keymod leader_modifiers,
                                               SDL_Keycode key,
                                               SDL_Keymod modifiers,
                                               KeybindingContext context,
                                               const std::function<bool(ActionId)>& action_enabled = {});
bool IsChordLeader(const std::vector<ResolvedKeybinding>& bindings,
                   SDL_Keycode key,
                   SDL_Keymod modifiers,
                   KeybindingContext context);

bool ParseKeyChord(std::string_view chord, SDL_Keycode* key, SDL_Keymod* modifiers);
std::string FormatKeyChord(SDL_Keycode key, SDL_Keymod modifiers);
std::string FormatBindingKeys(const KeybindingSpec& binding);
std::string FormatBindingKeys(const ResolvedKeybinding& binding);

SDL_Keymod NormalizedKeyModifiers(SDL_Keymod modifiers);

}
