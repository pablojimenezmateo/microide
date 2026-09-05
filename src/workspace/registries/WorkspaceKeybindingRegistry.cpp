#include "workspace/registries/WorkspaceKeybindingRegistry.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <sstream>
#include <unordered_map>

#include "plugin/PluginHost.h"
#include "util/StringUtil.h"
#include "workspace/registries/WorkspaceCommandRegistry.h"

namespace microide::workspace {

SDL_Keymod NormalizedKeyModifiers(SDL_Keymod modifiers) {
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
          .id = "settings",
          .action = ActionId::OpenSettings,
          .key = SDLK_COMMA,
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
          .key = SDLK_B,
          .modifiers = SDL_KMOD_CTRL,
          .context = KeybindingContext::Global,
          .args = {},
          .arg_count = 0,
          .command_name = {},
      },
      KeybindingSpec{
          // File > Open File… has advertised Ctrl+O since the menu was written; this
          // is the binding that makes it true. A bare `open` opens the native picker.
          .id = "open-file",
          .action = ActionId::Open,
          .key = SDLK_O,
          .modifiers = SDL_KMOD_CTRL,
          .context = KeybindingContext::Global,
          .args = {},
          .arg_count = 0,
          .command_name = {},
      },
      KeybindingSpec{
          .id = "sidebar-outline",
          .action = ActionId::SidebarShow,
          .key = SDLK_O,
          .modifiers = static_cast<SDL_Keymod>(SDL_KMOD_CTRL | SDL_KMOD_ALT),
          .context = KeybindingContext::Global,
          .args = {"outline", {}},
          .arg_count = 1,
          .command_name = {},
      },
      KeybindingSpec{
          .id = "file-finder",
          .action = ActionId::Files,
          .key = SDLK_P,
          .modifiers = SDL_KMOD_CTRL,
          .context = KeybindingContext::Global,
          .args = {},
          .arg_count = 0,
          .command_name = {},
      },
      KeybindingSpec{
          .id = "command-palette",
          .action = ActionId::OpenCommandPalette,
          .key = SDLK_P,
          .modifiers = SDL_KMOD_CTRL | SDL_KMOD_SHIFT,
          .context = KeybindingContext::Global,
          .args = {},
          .arg_count = 0,
          .command_name = {},
      },
      // Debugger execution control (Phase 3). Global; the actions are gated on
      // `debug.enabled` + session state in WorkspaceActionAvailability, so these
      // keys are inert until a session is active. F5/F10/F11/Shift+F11 mirror
      // VSCode, and F6=Pause matches VSCode as well. F8=Start fills the gap left
      // by F5 only *continuing* an already-paused session; F6/F8 were reclaimed
      // from the former file-finder/sidebar-toggle bindings.
      KeybindingSpec{
          .id = "debug-start",
          .action = ActionId::StartDebugging,
          .key = SDLK_F8,
          .modifiers = SDL_KMOD_NONE,
          .context = KeybindingContext::Global,
          .args = {},
          .arg_count = 0,
          .command_name = {},
      },
      KeybindingSpec{
          .id = "debug-pause",
          .action = ActionId::DebugPause,
          .key = SDLK_F6,
          .modifiers = SDL_KMOD_NONE,
          .context = KeybindingContext::Global,
          .args = {},
          .arg_count = 0,
          .command_name = {},
      },
      KeybindingSpec{
          .id = "debug-continue",
          .action = ActionId::DebugContinue,
          .key = SDLK_F5,
          .modifiers = SDL_KMOD_NONE,
          .context = KeybindingContext::Global,
          .args = {},
          .arg_count = 0,
          .command_name = {},
      },
      KeybindingSpec{
          .id = "debug-step-over",
          .action = ActionId::DebugStepOver,
          .key = SDLK_F10,
          .modifiers = SDL_KMOD_NONE,
          .context = KeybindingContext::Global,
          .args = {},
          .arg_count = 0,
          .command_name = {},
      },
      KeybindingSpec{
          .id = "debug-step-in",
          .action = ActionId::DebugStepIn,
          .key = SDLK_F11,
          .modifiers = SDL_KMOD_NONE,
          .context = KeybindingContext::Global,
          .args = {},
          .arg_count = 0,
          .command_name = {},
      },
      KeybindingSpec{
          .id = "debug-step-out",
          .action = ActionId::DebugStepOut,
          .key = SDLK_F11,
          .modifiers = SDL_KMOD_SHIFT,
          .context = KeybindingContext::Global,
          .args = {},
          .arg_count = 0,
          .command_name = {},
      },
      KeybindingSpec{
          .id = "debug-restart",
          .action = ActionId::DebugRestart,
          .key = SDLK_F5,
          .modifiers = static_cast<SDL_Keymod>(SDL_KMOD_CTRL | SDL_KMOD_SHIFT),
          .context = KeybindingContext::Global,
          .args = {},
          .arg_count = 0,
          .command_name = {},
      },
      KeybindingSpec{
          .id = "debug-pane-toggle",
          .action = ActionId::DebugPaneToggle,
          .key = SDLK_D,
          .modifiers = static_cast<SDL_Keymod>(SDL_KMOD_CTRL | SDL_KMOD_SHIFT),
          .context = KeybindingContext::Global,
          .args = {},
          .arg_count = 0,
          .command_name = {},
      },
      // Right-side debug pane surface switches, grouped with the toggle above
      // (Ctrl+Shift+D). Ctrl+digit is the zoom family, so these use Ctrl+Shift+digit.
      KeybindingSpec{
          .id = "debug-pane-callstack",
          .action = ActionId::DebugPaneShowCallStack,
          .key = SDLK_1,
          .modifiers = static_cast<SDL_Keymod>(SDL_KMOD_CTRL | SDL_KMOD_SHIFT),
          .context = KeybindingContext::Global,
          .args = {},
          .arg_count = 0,
          .command_name = {},
      },
      KeybindingSpec{
          .id = "debug-pane-variables",
          .action = ActionId::DebugPaneShowVariables,
          .key = SDLK_2,
          .modifiers = static_cast<SDL_Keymod>(SDL_KMOD_CTRL | SDL_KMOD_SHIFT),
          .context = KeybindingContext::Global,
          .args = {},
          .arg_count = 0,
          .command_name = {},
      },
      KeybindingSpec{
          .id = "debug-pane-watch",
          .action = ActionId::DebugPaneShowWatch,
          .key = SDLK_3,
          .modifiers = static_cast<SDL_Keymod>(SDL_KMOD_CTRL | SDL_KMOD_SHIFT),
          .context = KeybindingContext::Global,
          .args = {},
          .arg_count = 0,
          .command_name = {},
      },
      KeybindingSpec{
          .id = "debug-pane-breakpoints",
          .action = ActionId::DebugPaneShowBreakpoints,
          .key = SDLK_4,
          .modifiers = static_cast<SDL_Keymod>(SDL_KMOD_CTRL | SDL_KMOD_SHIFT),
          .context = KeybindingContext::Global,
          .args = {},
          .arg_count = 0,
          .command_name = {},
      },
      KeybindingSpec{
          .id = "debug-show-output",
          .action = ActionId::DebugShowOutput,
          .key = SDLK_5,
          .modifiers = static_cast<SDL_Keymod>(SDL_KMOD_CTRL | SDL_KMOD_SHIFT),
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
      // Editor tabs had no key chord at all: openable (Ctrl+P) and closable
      // (Ctrl+W), but reachable only with the mouse. These are VS Code's.
      KeybindingSpec{
          .id = "next-tab",
          .action = ActionId::TabSwitch,
          .key = SDLK_PAGEDOWN,
          .modifiers = SDL_KMOD_CTRL,
          .context = KeybindingContext::Editor,
          .args = {"+1", {}},
          .arg_count = 1,
          .command_name = {},
      },
      KeybindingSpec{
          .id = "previous-tab",
          .action = ActionId::TabSwitch,
          .key = SDLK_PAGEUP,
          .modifiers = SDL_KMOD_CTRL,
          .context = KeybindingContext::Editor,
          .args = {"-1", {}},
          .arg_count = 1,
          .command_name = {},
      },
      // Reordering a tab was mouse-only: `tabmove` existed as a command with a
      // relative offset, but nothing reached it from the keyboard. These are VS
      // Code's Move Editor Left / Right, and like it they clamp at the ends
      // rather than wrapping.
      KeybindingSpec{
          .id = "move-tab-right",
          .action = ActionId::TabMove,
          .key = SDLK_PAGEDOWN,
          .modifiers = SDL_KMOD_CTRL | SDL_KMOD_SHIFT,
          .context = KeybindingContext::Editor,
          .args = {"+1", {}},
          .arg_count = 1,
          .command_name = {},
      },
      KeybindingSpec{
          .id = "move-tab-left",
          .action = ActionId::TabMove,
          .key = SDLK_PAGEUP,
          .modifiers = SDL_KMOD_CTRL | SDL_KMOD_SHIFT,
          .context = KeybindingContext::Editor,
          .args = {"-1", {}},
          .arg_count = 1,
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
          // VSCode binds Ctrl+F in a focused terminal to its find widget rather
          // than passing ^F through to the shell; the bar closes on Escape, so
          // readline's forward-char stays one keystroke away.
          .id = "terminal-find",
          .action = ActionId::TerminalFind,
          .key = SDLK_F,
          .modifiers = SDL_KMOD_CTRL,
          .context = KeybindingContext::Terminal,
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
      // F3 / Shift+F3 step the buffer search from the editor, widget open or not
      // (VS Code nextMatchFindAction / previousMatchFindAction).
      KeybindingSpec{
          .id = "find-next",
          .action = ActionId::FindNext,
          .key = SDLK_F3,
          .modifiers = SDL_KMOD_NONE,
          .context = KeybindingContext::Editor,
          .args = {},
          .arg_count = 0,
          .command_name = {},
      },
      KeybindingSpec{
          .id = "find-previous",
          .action = ActionId::FindPrevious,
          .key = SDLK_F3,
          .modifiers = SDL_KMOD_SHIFT,
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
          .id = "insert-snippet",
          .action = ActionId::InsertSnippet,
          .key = SDLK_J,
          .modifiers = static_cast<SDL_Keymod>(SDL_KMOD_CTRL | SDL_KMOD_ALT),
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
      KeybindingSpec{
          .id = "format-document",
          .action = ActionId::FormatDocument,
          .key = SDLK_I,
          .modifiers = SDL_KMOD_CTRL | SDL_KMOD_SHIFT,
          .context = KeybindingContext::Editor,
          .args = {},
          .arg_count = 0,
          .command_name = {},
      },
      KeybindingSpec{
          .id = "rename-symbol",
          .action = ActionId::RenameSymbol,
          .key = SDLK_F2,
          .modifiers = SDL_KMOD_NONE,
          .context = KeybindingContext::Editor,
          .args = {},
          .arg_count = 0,
          .command_name = {},
      },
      KeybindingSpec{
          .id = "goto-definition",
          .action = ActionId::GoToDefinition,
          .key = SDLK_F12,
          .modifiers = SDL_KMOD_NONE,
          .context = KeybindingContext::Editor,
          .args = {},
          .arg_count = 0,
          .command_name = {},
      },
      KeybindingSpec{
          .id = "goto-implementation",
          .action = ActionId::GoToImplementation,
          .key = SDLK_F12,
          .modifiers = SDL_KMOD_CTRL,
          .context = KeybindingContext::Editor,
          .args = {},
          .arg_count = 0,
          .command_name = {},
      },
      KeybindingSpec{
          .id = "find-references",
          .action = ActionId::FindReferences,
          .key = SDLK_F12,
          .modifiers = SDL_KMOD_SHIFT,
          .context = KeybindingContext::Editor,
          .args = {},
          .arg_count = 0,
          .command_name = {},
      },
      KeybindingSpec{
          .id = "signature-help",
          .action = ActionId::SignatureHelp,
          .key = SDLK_SPACE,
          .modifiers = static_cast<SDL_Keymod>(SDL_KMOD_CTRL | SDL_KMOD_SHIFT),
          .context = KeybindingContext::Editor,
          .args = {},
          .arg_count = 0,
          .command_name = {},
      },
      // Editor essentials.
      KeybindingSpec{
          .id = "jump-to-matching-bracket",
          .action = ActionId::JumpToMatchingBracket,
          .key = SDLK_BACKSLASH,
          .modifiers = static_cast<SDL_Keymod>(SDL_KMOD_CTRL | SDL_KMOD_SHIFT),
          .context = KeybindingContext::Editor,
          .args = {},
          .arg_count = 0,
          .command_name = {},
      },
      KeybindingSpec{
          .id = "toggle-line-comment",
          .action = ActionId::ToggleLineComment,
          .key = SDLK_SLASH,
          .modifiers = SDL_KMOD_CTRL,
          .context = KeybindingContext::Editor,
          .args = {},
          .arg_count = 0,
          .command_name = {},
      },
      KeybindingSpec{
          .id = "toggle-block-comment",
          .action = ActionId::ToggleBlockComment,
          .key = SDLK_A,
          .modifiers = static_cast<SDL_Keymod>(SDL_KMOD_SHIFT | SDL_KMOD_ALT),
          .context = KeybindingContext::Editor,
          .args = {},
          .arg_count = 0,
          .command_name = {},
      },
      // Column (box) selection, matching VSCode's Ctrl+Shift+Alt+Arrow. Registered
      // rather than hardcoded in the key coordinator so the chord is listed in the
      // keyboard-shortcuts overlay and can be rebound. The mouse form
      // (Shift+Alt+drag) stays in WorkspaceEditorMouseCoordinator -- drags are not
      // keybindings.
      KeybindingSpec{
          .id = "column-select-up",
          .action = ActionId::ColumnSelectUp,
          .key = SDLK_UP,
          .modifiers = static_cast<SDL_Keymod>(SDL_KMOD_CTRL | SDL_KMOD_SHIFT | SDL_KMOD_ALT),
          .context = KeybindingContext::Editor,
          .args = {},
          .arg_count = 0,
          .command_name = {},
      },
      KeybindingSpec{
          .id = "column-select-down",
          .action = ActionId::ColumnSelectDown,
          .key = SDLK_DOWN,
          .modifiers = static_cast<SDL_Keymod>(SDL_KMOD_CTRL | SDL_KMOD_SHIFT | SDL_KMOD_ALT),
          .context = KeybindingContext::Editor,
          .args = {},
          .arg_count = 0,
          .command_name = {},
      },
      KeybindingSpec{
          .id = "column-select-left",
          .action = ActionId::ColumnSelectLeft,
          .key = SDLK_LEFT,
          .modifiers = static_cast<SDL_Keymod>(SDL_KMOD_CTRL | SDL_KMOD_SHIFT | SDL_KMOD_ALT),
          .context = KeybindingContext::Editor,
          .args = {},
          .arg_count = 0,
          .command_name = {},
      },
      KeybindingSpec{
          .id = "column-select-right",
          .action = ActionId::ColumnSelectRight,
          .key = SDLK_RIGHT,
          .modifiers = static_cast<SDL_Keymod>(SDL_KMOD_CTRL | SDL_KMOD_SHIFT | SDL_KMOD_ALT),
          .context = KeybindingContext::Editor,
          .args = {},
          .arg_count = 0,
          .command_name = {},
      },
      KeybindingSpec{
          .id = "move-line-up",
          .action = ActionId::MoveLineUp,
          .key = SDLK_UP,
          .modifiers = SDL_KMOD_ALT,
          .context = KeybindingContext::Editor,
          .args = {},
          .arg_count = 0,
          .command_name = {},
      },
      KeybindingSpec{
          .id = "move-line-down",
          .action = ActionId::MoveLineDown,
          .key = SDLK_DOWN,
          .modifiers = SDL_KMOD_ALT,
          .context = KeybindingContext::Editor,
          .args = {},
          .arg_count = 0,
          .command_name = {},
      },
      KeybindingSpec{
          .id = "duplicate-line",
          .action = ActionId::DuplicateLine,
          .key = SDLK_DOWN,
          .modifiers = static_cast<SDL_Keymod>(SDL_KMOD_SHIFT | SDL_KMOD_ALT),
          .context = KeybindingContext::Editor,
          .args = {},
          .arg_count = 0,
          .command_name = {},
      },
      KeybindingSpec{
          .id = "copy-line-up",
          .action = ActionId::CopyLineUp,
          .key = SDLK_UP,
          .modifiers = static_cast<SDL_Keymod>(SDL_KMOD_SHIFT | SDL_KMOD_ALT),
          .context = KeybindingContext::Editor,
          .args = {},
          .arg_count = 0,
          .command_name = {},
      },
      KeybindingSpec{
          .id = "insert-line-below",
          .action = ActionId::InsertLineBelow,
          .key = SDLK_RETURN,
          .modifiers = SDL_KMOD_CTRL,
          .context = KeybindingContext::Editor,
          .args = {},
          .arg_count = 0,
          .command_name = {},
      },
      KeybindingSpec{
          .id = "insert-line-above",
          .action = ActionId::InsertLineAbove,
          .key = SDLK_RETURN,
          .modifiers = static_cast<SDL_Keymod>(SDL_KMOD_CTRL | SDL_KMOD_SHIFT),
          .context = KeybindingContext::Editor,
          .args = {},
          .arg_count = 0,
          .command_name = {},
      },
      KeybindingSpec{
          .id = "delete-line",
          .action = ActionId::DeleteLine,
          .key = SDLK_K,
          .modifiers = static_cast<SDL_Keymod>(SDL_KMOD_CTRL | SDL_KMOD_SHIFT),
          .context = KeybindingContext::Editor,
          .args = {},
          .arg_count = 0,
          .command_name = {},
      },
      KeybindingSpec{
          .id = "add-cursor-next-match",
          .action = ActionId::AddCursorAtNextMatch,
          .key = SDLK_D,
          .modifiers = SDL_KMOD_CTRL,
          .context = KeybindingContext::Editor,
          .args = {},
          .arg_count = 0,
          .command_name = {},
      },
      KeybindingSpec{
          .id = "add-cursor-all-matches",
          .action = ActionId::AddCursorAtAllMatches,
          // Ctrl+Shift+L matches VSCode's "Select all occurrences".
          .key = SDLK_L,
          .modifiers = static_cast<SDL_Keymod>(SDL_KMOD_CTRL | SDL_KMOD_SHIFT),
          .context = KeybindingContext::Editor,
          .args = {},
          .arg_count = 0,
          .command_name = {},
      },
      KeybindingSpec{
          .id = "goto-line",
          .action = ActionId::Goto,
          .key = SDLK_G,
          .modifiers = SDL_KMOD_CTRL,
          .context = KeybindingContext::Editor,
          .args = {},
          .arg_count = 0,
          .command_name = {},
      },
      // Code folding shortcuts: fold/unfold regions use single chords; fold-all /
      // unfold-all use a minimal Ctrl+K leader sequence handled in
      // KeyInputCoordinator::HandleGlobalKeyDown (no second binding entry here).
      // VS Code parity: F9 toggles a breakpoint on the caret line, Alt+Z toggles
      // word wrap, Ctrl+] / Ctrl+[ indent / outdent, Ctrl+\ splits the editor
      // right, and Ctrl+Shift+E / G / M open the explorer, source-control and
      // problems views.
      KeybindingSpec{
          .id = "breakpoint-toggle",
          .action = ActionId::BreakpointToggle,
          .key = SDLK_F9,
          .modifiers = SDL_KMOD_NONE,
          .context = KeybindingContext::Editor,
          .args = {},
          .arg_count = 0,
          .command_name = {},
      },
      KeybindingSpec{
          .id = "wrap-toggle",
          .action = ActionId::Wrap,
          .key = SDLK_Z,
          .modifiers = SDL_KMOD_ALT,
          .context = KeybindingContext::Editor,
          .args = {},
          .arg_count = 0,
          .command_name = {},
      },
      KeybindingSpec{
          .id = "indent-lines",
          .action = ActionId::IndentLines,
          .key = SDLK_RIGHTBRACKET,
          .modifiers = SDL_KMOD_CTRL,
          .context = KeybindingContext::Editor,
          .args = {},
          .arg_count = 0,
          .command_name = {},
      },
      KeybindingSpec{
          .id = "outdent-lines",
          .action = ActionId::OutdentLines,
          .key = SDLK_LEFTBRACKET,
          .modifiers = SDL_KMOD_CTRL,
          .context = KeybindingContext::Editor,
          .args = {},
          .arg_count = 0,
          .command_name = {},
      },
      KeybindingSpec{
          .id = "split-right",
          .action = ActionId::SplitEditorRight,
          .key = SDLK_BACKSLASH,
          .modifiers = SDL_KMOD_CTRL,
          .context = KeybindingContext::Editor,
          .args = {},
          .arg_count = 0,
          .command_name = {},
      },
      KeybindingSpec{
          .id = "sidebar-explorer",
          .action = ActionId::SidebarShow,
          .key = SDLK_E,
          .modifiers = static_cast<SDL_Keymod>(SDL_KMOD_CTRL | SDL_KMOD_SHIFT),
          .context = KeybindingContext::Global,
          .args = {"tree", {}},
          .arg_count = 1,
          .command_name = {},
      },
      KeybindingSpec{
          .id = "sidebar-git",
          .action = ActionId::SidebarShow,
          .key = SDLK_G,
          .modifiers = static_cast<SDL_Keymod>(SDL_KMOD_CTRL | SDL_KMOD_SHIFT),
          .context = KeybindingContext::Global,
          .args = {"git", {}},
          .arg_count = 1,
          .command_name = {},
      },
      KeybindingSpec{
          .id = "sidebar-problems",
          .action = ActionId::SidebarShow,
          .key = SDLK_M,
          .modifiers = static_cast<SDL_Keymod>(SDL_KMOD_CTRL | SDL_KMOD_SHIFT),
          .context = KeybindingContext::Global,
          .args = {"problems", {}},
          .arg_count = 1,
          .command_name = {},
      },
      KeybindingSpec{
          .id = "next-diagnostic",
          .action = ActionId::GoToNextDiagnostic,
          .key = SDLK_F8,
          .modifiers = SDL_KMOD_ALT,
          .context = KeybindingContext::Editor,
          .args = {},
          .arg_count = 0,
          .command_name = {},
      },
      KeybindingSpec{
          .id = "previous-diagnostic",
          .action = ActionId::GoToPreviousDiagnostic,
          .key = SDLK_F8,
          .modifiers = static_cast<SDL_Keymod>(SDL_KMOD_SHIFT | SDL_KMOD_ALT),
          .context = KeybindingContext::Editor,
          .args = {},
          .arg_count = 0,
          .command_name = {},
      },
      KeybindingSpec{
          .id = "fold",
          .action = ActionId::Fold,
          .key = SDLK_LEFTBRACKET,
          .modifiers = static_cast<SDL_Keymod>(SDL_KMOD_CTRL | SDL_KMOD_SHIFT),
          .context = KeybindingContext::Editor,
          .args = {},
          .arg_count = 0,
          .command_name = {},
      },
      KeybindingSpec{
          .id = "unfold",
          .action = ActionId::Unfold,
          .key = SDLK_RIGHTBRACKET,
          .modifiers = static_cast<SDL_Keymod>(SDL_KMOD_CTRL | SDL_KMOD_SHIFT),
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
  const SDL_Keymod relevant = NormalizedKeyModifiers(modifiers);
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
  return ResolveKeybindings(plugin_host.ContributedKeybindings(), disabled_ids);
}

std::vector<ResolvedKeybinding> ResolveKeybindings(
    const std::vector<plugin::PluginHost::ContributedKeybinding>& contributed,
    const std::vector<std::string>& disabled_ids) {
  std::vector<ResolvedKeybinding> result;
  const auto builtin_specs = BuiltinKeybindingSpecs();
  result.reserve(builtin_specs.size() + contributed.size());

  const auto is_disabled = [&](std::string_view id) {
    return std::find(disabled_ids.begin(), disabled_ids.end(), id) != disabled_ids.end();
  };

  for (const KeybindingSpec& spec : builtin_specs) {
    if (is_disabled(spec.id)) {
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

  // Two bindings collide when they share a key + normalized modifiers and their
  // contexts overlap. A Global binding matches in every context (see FindKeybinding),
  // so it overlaps any context; otherwise contexts must be equal. Resolved chords
  // are indexed by (key, normalized mods) -> context bitmask so each contributed
  // binding checks in O(1) instead of rescanning every prior binding (the old
  // linear rescan made a cap-sized reload quadratic; TD-2026-07-17-019).
  const auto chord_key = [](SDL_Keycode key, SDL_Keymod normalized_mods) {
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(key)) << 16) |
           static_cast<std::uint16_t>(normalized_mods);
  };
  // The context set is packed into a uint8 bitmask; adding a 9th context would
  // silently shift bits out and make every binding past it look conflict-free.
  static_assert(static_cast<unsigned>(KeybindingContext::Terminal) < 8,
                "KeybindingContext no longer fits the uint8 conflict-index bitmask — widen "
                "the mask type in ResolveKeybindings");
  const auto context_bit = [](KeybindingContext context) {
    return static_cast<std::uint8_t>(1u << static_cast<unsigned>(context));
  };
  constexpr std::uint8_t kGlobalBit = 1u << static_cast<unsigned>(KeybindingContext::Global);
  std::unordered_map<std::uint64_t, std::uint8_t> resolved_contexts;
  resolved_contexts.reserve(result.size());
  for (const ResolvedKeybinding& existing : result) {
    resolved_contexts[chord_key(existing.key, NormalizedKeyModifiers(existing.modifiers))] |=
        context_bit(existing.context);
  }
  const auto conflicts_with_resolved = [&](SDL_Keycode key, SDL_Keymod mods,
                                           KeybindingContext context) {
    const auto it = resolved_contexts.find(chord_key(key, NormalizedKeyModifiers(mods)));
    if (it == resolved_contexts.end()) {
      return false;
    }
    return (it->second & kGlobalBit) != 0 || context == KeybindingContext::Global ||
           (it->second & context_bit(context)) != 0;
  };

  for (const auto& contrib : contributed) {
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
    // Built-ins resolve first and FindKeybinding returns the first match, so a plugin
    // chord that collides with an already-resolved binding (a built-in or an earlier
    // plugin) would be advertised in Help/Settings yet silently dispatch the winner
    // instead. Skip the shadowed contribution so the UI never shows a dead binding.
    if (conflicts_with_resolved(key, mods, context)) {
      continue;
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
    // Keep the conflict index in sync so a later plugin binding that collides
    // with this accepted one is skipped, matching the old rescan-of-result.
    resolved_contexts[chord_key(key, NormalizedKeyModifiers(mods))] |= context_bit(context);
    result.push_back(std::move(rb));
  }

  return result;
}

const ResolvedKeybinding* FindKeybinding(const std::vector<ResolvedKeybinding>& bindings,
                                          SDL_Keycode key,
                                          SDL_Keymod modifiers,
                                          KeybindingContext context) {
  const SDL_Keymod relevant = NormalizedKeyModifiers(modifiers);
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
    std::string lower = util::ToLowerAscii(remaining);
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

  // Map the key name to an SDL_Keycode BY NAME, never through a scancode. A
  // key event carries the keycode the pressed key produces under the user's
  // layout (`z` on the key labelled Z), and FindKeybinding compares keycodes, so
  // the chord has to name the same thing. The old form went
  // SDL_GetScancodeFromName("Z") -> SDL_GetKeyFromScancode: that is the PHYSICAL
  // QWERTY position, so on a QWERTZ layout a plugin's "ctrl+z" bound to the key
  // labelled Y (and "ctrl+y" to Z), and on AZERTY "ctrl+a" to the key labelled Q.
  // SDL3 keycodes for printable ASCII are the character itself (SDLK_A is 'a',
  // SDLK_MINUS is '-'), so a one-byte printable name IS its keycode; the named
  // keys are the constants; F-keys resolve by SDL's own key name.
  const std::string lower_key = util::ToLowerAscii(remaining);
  if (lower_key.size() == 1 && lower_key[0] > ' ' && lower_key[0] < 0x7F) {
    *key_out = static_cast<SDL_Keycode>(static_cast<unsigned char>(lower_key[0]));
    *mods_out = mods;
    return true;
  }

  // Function keys F1-F24, by SDL's key name ("F1").
  if (lower_key.size() >= 2 && lower_key.size() <= 3 && lower_key[0] == 'f') {
    bool all_digits = true;
    for (std::size_t i = 1; i < lower_key.size(); ++i) {
      if (!util::IsAsciiDigit(static_cast<unsigned char>(lower_key[i]))) {
        all_digits = false;
        break;
      }
    }
    if (all_digits) {
      std::string sdl_name = lower_key;
      sdl_name[0] = 'F';
      const SDL_Keycode key = SDL_GetKeyFromName(sdl_name.c_str());
      if (key != SDLK_UNKNOWN) {
        *key_out = key;
        *mods_out = mods;
        return true;
      }
    }
  }

  // Named keys.
  static const std::pair<std::string_view, SDL_Keycode> kNamedKeys[] = {
      {"escape", SDLK_ESCAPE},   {"esc", SDLK_ESCAPE},        {"enter", SDLK_RETURN},
      {"return", SDLK_RETURN},   {"tab", SDLK_TAB},           {"space", SDLK_SPACE},
      {"backspace", SDLK_BACKSPACE}, {"delete", SDLK_DELETE}, {"insert", SDLK_INSERT},
      {"home", SDLK_HOME},       {"end", SDLK_END},           {"pageup", SDLK_PAGEUP},
      {"pagedown", SDLK_PAGEDOWN}, {"up", SDLK_UP},           {"down", SDLK_DOWN},
      {"left", SDLK_LEFT},       {"right", SDLK_RIGHT},
  };
  for (const auto& [name, key] : kNamedKeys) {
    if (lower_key == name) {
      *key_out = key;
      *mods_out = mods;
      return true;
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
  // SDL names a few keys after the character they historically sent rather than
  // after the legend printed on the key. "Return" is the one that shows: the key
  // says Enter, VS Code's keybinding UI says Enter, and this is the only place
  // that spells a chord for the menus, the shortcuts overlay and Help/About --
  // so the alias belongs here and cannot drift between them.
  if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
    result += "Enter";
    return result;
  }
  const char* name = SDL_GetKeyName(key);
  if (name != nullptr && name[0] != '\0') {
    result += name;
  }
  return result;
}

}  // namespace microide::workspace
