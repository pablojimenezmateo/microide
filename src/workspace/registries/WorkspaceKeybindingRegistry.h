#pragma once

#include <SDL3/SDL.h>
#include <array>
#include <span>
#include <string>
#include <string_view>
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
  // Whether a Global binding still fires while a terminal holds keyboard focus
  // (VS Code's `terminal.integrated.commandsToSkipShell`). Off by default: a
  // focused terminal owns the keyboard, so Ctrl+O reaches nano as ^O instead of
  // opening a file. Set only on chords a shell has no use for (F-key debugging,
  // Ctrl+Shift+letter, zoom) and on the few the IDE must keep (Ctrl+P, Ctrl+`,
  // Ctrl+J). Terminal-context bindings always fire in a terminal.
  bool skips_terminal = false;
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
  bool skips_terminal = false;  // see KeybindingSpec::skips_terminal
};

std::vector<ResolvedKeybinding> ResolveKeybindings(
    const plugin::PluginHost& plugin_host,
    const std::vector<std::string>& disabled_ids = {});

// Pure seam taking the contributed keybindings directly. This is the per-reload
// rebuild whose cost the per-kind contribution cap bounds (TD-2026-07-17-019);
// the perf harness measures it at the cap through this overload.
std::vector<ResolvedKeybinding> ResolveKeybindings(
    const std::vector<plugin::PluginHost::ContributedKeybinding>& contributed,
    const std::vector<std::string>& disabled_ids = {});
// First binding for (key, modifiers) active in `context`. A Global binding is
// active in every context except Terminal, where it must also skip the terminal.
const ResolvedKeybinding* FindKeybinding(const std::vector<ResolvedKeybinding>& bindings,
                                          SDL_Keycode key,
                                          SDL_Keymod modifiers,
                                          KeybindingContext context);

// Parse "Ctrl+Shift+S" → key + modifiers. Returns false for unrecognised chords.
bool ParseKeyChord(std::string_view chord, SDL_Keycode* key, SDL_Keymod* modifiers);
std::string FormatKeyChord(SDL_Keycode key, SDL_Keymod modifiers);

// Normalized Ctrl/Shift/Alt/Meta mask for keybinding comparison.
SDL_Keymod NormalizedKeyModifiers(SDL_Keymod modifiers);

}  // namespace microide::workspace
