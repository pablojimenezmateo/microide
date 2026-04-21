#include "workspace/WorkspaceKeybindingRegistry.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <sstream>

#include "plugin/PluginHost.h"
#include "workspace/WorkspaceCommandRegistry.h"

namespace microide::workspace {

namespace {

std::string ToLowerAscii(std::string_view text) {
  std::string lower(text);
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return lower;
}

SDL_Keymod NormalizeRelevantModifiers(SDL_Keymod modifiers) {
  SDL_Keymod normalized = SDL_KMOD_NONE;
  if ((modifiers & SDL_KMOD_CTRL) != 0) {
    normalized = static_cast<SDL_Keymod>(normalized | SDL_KMOD_CTRL);
  }
  if ((modifiers & SDL_KMOD_SHIFT) != 0) {
    normalized = static_cast<SDL_Keymod>(normalized | SDL_KMOD_SHIFT);
  }
  if ((modifiers & SDL_KMOD_ALT) != 0) {
    normalized = static_cast<SDL_Keymod>(normalized | SDL_KMOD_ALT);
  }
  if ((modifiers & SDL_KMOD_GUI) != 0) {
    normalized = static_cast<SDL_Keymod>(normalized | SDL_KMOD_GUI);
  }
  return normalized;
}

}  // namespace

std::span<const KeybindingSpec> BuiltinKeybindingSpecs() {
  static const auto kSpecs = std::to_array<KeybindingSpec>({
      // Global — available everywhere except modals
      KeybindingSpec{
          .id = "new-tab",
          .action = ActionId::Tab,
          .key = SDLK_N,
          .modifiers = SDL_KMOD_CTRL,
          .context = KeybindingContext::Global,
          .args = {},
          .arg_count = 0,
          .command_name = {},
      },
      KeybindingSpec{
          .id = "save",
          .action = ActionId::Save,
          .key = SDLK_S,
          .modifiers = SDL_KMOD_CTRL,
          .context = KeybindingContext::Global,
          .args = {},
          .arg_count = 0,
          .command_name = {},
      },
      KeybindingSpec{
          .id = "command-prompt",
          .action = ActionId::OpenCommandPrompt,
          .key = SDLK_E,
          .modifiers = SDL_KMOD_CTRL,
          .context = KeybindingContext::Global,
          .args = {},
          .arg_count = 0,
          .command_name = {},
      },
      KeybindingSpec{
          .id = "zoom-reset",
          .action = ActionId::UiScale,
          .key = SDLK_0,
          .modifiers = SDL_KMOD_CTRL,
          .context = KeybindingContext::Global,
          .args = {"reset", {}},
          .arg_count = 1,
          .command_name = {},
      },
      KeybindingSpec{
          .id = "zoom-out",
          .action = ActionId::UiScale,
          .key = SDLK_MINUS,
          .modifiers = SDL_KMOD_CTRL,
          .context = KeybindingContext::Global,
          .args = {"down", {}},
          .arg_count = 1,
          .command_name = {},
      },
      KeybindingSpec{
          .id = "zoom-in",
          .action = ActionId::UiScale,
          .key = SDLK_EQUALS,
          .modifiers = SDL_KMOD_CTRL,
          .context = KeybindingContext::Global,
          .args = {"up", {}},
          .arg_count = 1,
          .command_name = {},
      },
      KeybindingSpec{
          .id = "sidebar-toggle",
          .action = ActionId::SidebarToggle,
          .key = SDLK_F8,
          .modifiers = SDL_KMOD_NONE,
          .context = KeybindingContext::Global,
          .args = {},
          .arg_count = 0,
          .command_name = {},
      },
      KeybindingSpec{
          .id = "file-finder",
          .action = ActionId::Files,
          .key = SDLK_F6,
          .modifiers = SDL_KMOD_NONE,
          .context = KeybindingContext::Global,
          .args = {},
          .arg_count = 0,
          .command_name = {},
      },
      // Editor context
      KeybindingSpec{
          .id = "undo",
          .action = ActionId::Undo,
          .key = SDLK_Z,
          .modifiers = SDL_KMOD_CTRL,
          .context = KeybindingContext::Editor,
          .args = {},
          .arg_count = 0,
          .command_name = {},
      },
      KeybindingSpec{
          .id = "redo-y",
          .action = ActionId::Redo,
          .key = SDLK_Y,
          .modifiers = SDL_KMOD_CTRL,
          .context = KeybindingContext::Editor,
          .args = {},
          .arg_count = 0,
          .command_name = {},
      },
      KeybindingSpec{
          .id = "redo-z",
          .action = ActionId::Redo,
          .key = SDLK_Z,
          .modifiers = static_cast<SDL_Keymod>(SDL_KMOD_CTRL | SDL_KMOD_SHIFT),
          .context = KeybindingContext::Editor,
          .args = {},
          .arg_count = 0,
          .command_name = {},
      },
      KeybindingSpec{
          .id = "copy",
          .action = ActionId::CopySelection,
          .key = SDLK_C,
          .modifiers = SDL_KMOD_CTRL,
          .context = KeybindingContext::Editor,
          .args = {},
          .arg_count = 0,
          .command_name = {},
      },
      KeybindingSpec{
          .id = "cut",
          .action = ActionId::CutSelection,
          .key = SDLK_X,
          .modifiers = SDL_KMOD_CTRL,
          .context = KeybindingContext::Editor,
          .args = {},
          .arg_count = 0,
          .command_name = {},
      },
      KeybindingSpec{
          .id = "paste",
          .action = ActionId::PasteClipboard,
          .key = SDLK_V,
          .modifiers = SDL_KMOD_CTRL,
          .context = KeybindingContext::Editor,
          .args = {},
          .arg_count = 0,
          .command_name = {},
      },
      KeybindingSpec{
          .id = "select-all",
          .action = ActionId::SelectAll,
          .key = SDLK_A,
          .modifiers = SDL_KMOD_CTRL,
          .context = KeybindingContext::Editor,
          .args = {},
          .arg_count = 0,
          .command_name = {},
      },
      KeybindingSpec{
          .id = "close-tab",
          .action = ActionId::CloseActiveTab,
          .key = SDLK_W,
          .modifiers = SDL_KMOD_CTRL,
          .context = KeybindingContext::Editor,
          .args = {},
          .arg_count = 0,
          .command_name = {},
      },
      KeybindingSpec{
          .id = "find",
          .action = ActionId::Search,
          .key = SDLK_F,
          .modifiers = SDL_KMOD_CTRL,
          .context = KeybindingContext::Editor,
          .args = {},
          .arg_count = 0,
          .command_name = {},
      },
      KeybindingSpec{
          .id = "replace",
          .action = ActionId::ReplaceInBuffer,
          .key = SDLK_H,
          .modifiers = SDL_KMOD_CTRL,
          .context = KeybindingContext::Editor,
          .args = {},
          .arg_count = 0,
          .command_name = {},
      },
      KeybindingSpec{
          .id = "project-search",
          .action = ActionId::ProjectSearch,
          .key = SDLK_F,
          .modifiers = static_cast<SDL_Keymod>(SDL_KMOD_CTRL | SDL_KMOD_SHIFT),
          .context = KeybindingContext::Editor,
          .args = {},
          .arg_count = 0,
          .command_name = {},
      },
      KeybindingSpec{
          .id = "completion",
          .action = ActionId::Completion,
          .key = SDLK_SPACE,
          .modifiers = SDL_KMOD_CTRL,
          .context = KeybindingContext::Editor,
          .args = {},
          .arg_count = 0,
          .command_name = {},
      },
      KeybindingSpec{
          .id = "code-actions",
          .action = ActionId::CodeActions,
          .key = SDLK_PERIOD,
          .modifiers = SDL_KMOD_CTRL,
          .context = KeybindingContext::Editor,
          .args = {},
          .arg_count = 0,
          .command_name = {},
      },
  });
  return kSpecs;
}

const KeybindingSpec* FindBuiltinKeybinding(std::string_view id) {
  const auto specs = BuiltinKeybindingSpecs();
  const auto it = std::find_if(specs.begin(), specs.end(),
                               [id](const KeybindingSpec& spec) { return spec.id == id; });
  return it == specs.end() ? nullptr : &(*it);
}

const KeybindingSpec* FindBuiltinKeybindingByKey(SDL_Keycode key,
                                                  SDL_Keymod modifiers,
                                                  KeybindingContext context) {
  const SDL_Keymod relevant = NormalizeRelevantModifiers(modifiers);
  const auto specs = BuiltinKeybindingSpecs();
  for (const KeybindingSpec& spec : specs) {
    if (spec.key != key) {
      continue;
    }
    if (spec.modifiers != relevant) {
      continue;
    }
    if (spec.context != KeybindingContext::Global && spec.context != context) {
      continue;
    }
    return &spec;
  }
  return nullptr;
}

std::vector<ResolvedKeybinding> ResolveKeybindings(
    const plugin::PluginHost& plugin_host,
    const std::vector<std::string>& disabled_ids) {
  std::vector<ResolvedKeybinding> result;

  const auto is_disabled = [&](const std::string& id) {
    return std::find(disabled_ids.begin(), disabled_ids.end(), id) != disabled_ids.end();
  };

  for (const KeybindingSpec& spec : BuiltinKeybindingSpecs()) {
    if (is_disabled(std::string(spec.id))) {
      continue;
    }
    ResolvedKeybinding rb;
    rb.id = std::string(spec.id);
    rb.action = spec.action;
    rb.key = spec.key;
    rb.modifiers = spec.modifiers;
    rb.context = spec.context;
    for (std::size_t i = 0; i < spec.arg_count; ++i) {
      rb.args.emplace_back(spec.args[i]);
    }
    rb.command_name = spec.command_name;
    rb.from_plugin = false;
    result.push_back(std::move(rb));
  }

  for (const auto& contrib : plugin_host.ContributedKeybindings()) {
    if (is_disabled(contrib.id)) {
      continue;
    }
    SDL_Keycode key = SDLK_UNKNOWN;
    SDL_Keymod mods = SDL_KMOD_NONE;
    if (!ParseKeyChord(contrib.key_chord, &key, &mods)) {
      continue;
    }
    KeybindingContext context = KeybindingContext::Global;
    if (contrib.context == "editor") {
      context = KeybindingContext::Editor;
    } else if (contrib.context == "sidebar") {
      context = KeybindingContext::Sidebar;
    } else if (contrib.context == "terminal") {
      context = KeybindingContext::Terminal;
    }
    const ActionSpec* spec = FindWorkspaceActionByCommand(contrib.action);
    ResolvedKeybinding rb;
    rb.id = contrib.id;
    if (spec != nullptr) {
      rb.action = spec->id;
    } else {
      rb.command_name = contrib.action;
    }
    rb.key = key;
    rb.modifiers = mods;
    rb.context = context;
    rb.from_plugin = true;
    result.push_back(std::move(rb));
  }

  return result;
}

const ResolvedKeybinding* FindKeybinding(const std::vector<ResolvedKeybinding>& bindings,
                                          SDL_Keycode key,
                                          SDL_Keymod modifiers,
                                          KeybindingContext context) {
  const SDL_Keymod relevant = NormalizeRelevantModifiers(modifiers);
  for (const ResolvedKeybinding& rb : bindings) {
    if (rb.key != key) {
      continue;
    }
    if (rb.modifiers != relevant) {
      continue;
    }
    if (rb.context != KeybindingContext::Global && rb.context != context) {
      continue;
    }
    return &rb;
  }
  return nullptr;
}

bool ParseKeyChord(std::string_view chord, SDL_Keycode* key_out, SDL_Keymod* mods_out) {
  if (key_out == nullptr || mods_out == nullptr) {
    return false;
  }

  SDL_Keymod mods = SDL_KMOD_NONE;
  std::string remaining(chord);

  // Strip modifier prefixes.
  while (true) {
    std::string lower = ToLowerAscii(remaining);
    if (lower.starts_with("ctrl+")) {
      mods = static_cast<SDL_Keymod>(mods | SDL_KMOD_CTRL);
      remaining = remaining.substr(5);
    } else if (lower.starts_with("shift+")) {
      mods = static_cast<SDL_Keymod>(mods | SDL_KMOD_SHIFT);
      remaining = remaining.substr(6);
    } else if (lower.starts_with("alt+")) {
      mods = static_cast<SDL_Keymod>(mods | SDL_KMOD_ALT);
      remaining = remaining.substr(4);
    } else if (lower.starts_with("meta+") || lower.starts_with("super+") ||
               lower.starts_with("cmd+")) {
      mods = static_cast<SDL_Keymod>(mods | SDL_KMOD_GUI);
      const std::size_t plus = remaining.find('+');
      remaining = remaining.substr(plus + 1);
    } else {
      break;
    }
  }

  // Map remaining string to SDL_Keycode.
  const std::string lower_key = ToLowerAscii(remaining);

  if (lower_key.size() == 1 && std::isalpha(static_cast<unsigned char>(lower_key[0]))) {
    // Single letter: map to SDL keycode via scancode.
    const SDL_Scancode sc = SDL_GetScancodeFromName(remaining.c_str());
    if (sc == SDL_SCANCODE_UNKNOWN) {
      return false;
    }
    *key_out = SDL_GetKeyFromScancode(sc, SDL_KMOD_NONE, false);
    *mods_out = mods;
    return *key_out != SDLK_UNKNOWN;
  }

  // Function keys F1-F24.
  if (lower_key.size() >= 2 && lower_key[0] == 'f') {
    bool all_digits = true;
    for (std::size_t i = 1; i < lower_key.size(); ++i) {
      if (!std::isdigit(static_cast<unsigned char>(lower_key[i]))) {
        all_digits = false;
        break;
      }
    }
    if (all_digits) {
      const SDL_Scancode sc = SDL_GetScancodeFromName(remaining.c_str());
      if (sc != SDL_SCANCODE_UNKNOWN) {
        *key_out = SDL_GetKeyFromScancode(sc, SDL_KMOD_NONE, false);
        *mods_out = mods;
        return *key_out != SDLK_UNKNOWN;
      }
    }
  }

  // Named keys.
  static const std::pair<std::string_view, SDL_Scancode> kNamedKeys[] = {
      {"escape", SDL_SCANCODE_ESCAPE},
      {"enter", SDL_SCANCODE_RETURN},
      {"return", SDL_SCANCODE_RETURN},
      {"tab", SDL_SCANCODE_TAB},
      {"space", SDL_SCANCODE_SPACE},
      {"backspace", SDL_SCANCODE_BACKSPACE},
      {"delete", SDL_SCANCODE_DELETE},
      {"insert", SDL_SCANCODE_INSERT},
      {"home", SDL_SCANCODE_HOME},
      {"end", SDL_SCANCODE_END},
      {"pageup", SDL_SCANCODE_PAGEUP},
      {"pagedown", SDL_SCANCODE_PAGEDOWN},
      {"up", SDL_SCANCODE_UP},
      {"down", SDL_SCANCODE_DOWN},
      {"left", SDL_SCANCODE_LEFT},
      {"right", SDL_SCANCODE_RIGHT},
      {"-", SDL_SCANCODE_MINUS},
      {"=", SDL_SCANCODE_EQUALS},
      {"[", SDL_SCANCODE_LEFTBRACKET},
      {"]", SDL_SCANCODE_RIGHTBRACKET},
      {"\\", SDL_SCANCODE_BACKSLASH},
      {";", SDL_SCANCODE_SEMICOLON},
      {"'", SDL_SCANCODE_APOSTROPHE},
      {",", SDL_SCANCODE_COMMA},
      {".", SDL_SCANCODE_PERIOD},
      {"/", SDL_SCANCODE_SLASH},
      {"`", SDL_SCANCODE_GRAVE},
      {"0", SDL_SCANCODE_0},
      {"1", SDL_SCANCODE_1},
      {"2", SDL_SCANCODE_2},
      {"3", SDL_SCANCODE_3},
      {"4", SDL_SCANCODE_4},
      {"5", SDL_SCANCODE_5},
      {"6", SDL_SCANCODE_6},
      {"7", SDL_SCANCODE_7},
      {"8", SDL_SCANCODE_8},
      {"9", SDL_SCANCODE_9},
  };
  for (const auto& [name, sc] : kNamedKeys) {
    if (lower_key == name) {
      *key_out = SDL_GetKeyFromScancode(sc, SDL_KMOD_NONE, false);
      *mods_out = mods;
      return *key_out != SDLK_UNKNOWN;
    }
  }

  return false;
}

std::string FormatKeyChord(SDL_Keycode key, SDL_Keymod modifiers) {
  std::string result;
  if (modifiers & SDL_KMOD_CTRL) {
    result += "Ctrl+";
  }
  if (modifiers & SDL_KMOD_SHIFT) {
    result += "Shift+";
  }
  if (modifiers & SDL_KMOD_ALT) {
    result += "Alt+";
  }
  if (modifiers & SDL_KMOD_GUI) {
    result += "Super+";
  }
  const char* name = SDL_GetKeyName(key);
  if (name != nullptr && name[0] != '\0') {
    result += name;
  }
  return result;
}

}  // namespace microide::workspace
