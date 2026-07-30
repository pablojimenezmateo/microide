#pragma once

#include <string>

namespace microide::workspace {

enum class TextInputSurface {
  None,
  Editor,
  PromptInput,
  FileFinder,
  BufferSearch,
  BufferReplaceSearch,
  BufferReplaceReplace,
  ProjectSearchOverlay,
  CommitPicker,
  // Launch-config picker query field (Phase 9).
  LaunchConfigPicker,
  // Command palette query field (fuzzy command search).
  CommandPalette,
  SidebarSearchQuery,
  SidebarSearchReplace,
  // "files to include" / "files to exclude" scope glob fields in the search
  // sidebar. Present only while the panel's scope section is expanded.
  SidebarSearchInclude,
  SidebarSearchExclude,
  CommitSubject,
  CommitBody,
  Terminal,
  // Query field of the terminal panel's find bar. Claimed only while the bar
  // holds focus, so an unfocused bar leaves typing to the terminal underneath.
  TerminalFind,
  SettingsQuery,
  // Inline edit of a String setting's value in the Settings overlay. Routes to
  // SettingsOverlayService::ValueEditor(); commits on Return, cancels on Esc.
  SettingsValueEdit,
  // Inline edit of a value in the debug Variables panel (Phase 4). Routes to
  // `debug_variables.EditBuffer()`; the panel renders its own static caret.
  DebugVariableEdit,
};

struct TextCompositionState {
  TextInputSurface surface = TextInputSurface::None;
  std::string text;
  int start = -1;
  int length = -1;
};

struct TextInputState {
  TextInputSurface active_surface = TextInputSurface::None;
  TextCompositionState composition;
};

// True for the four single-line fields in the search sidebar's header (query,
// replace, and the two scope glob boxes). They share caret handling, focus
// routing, and text dispatch, so the predicate lives here rather than being
// re-spelled as a four-case fallthrough at every consumer.
inline bool IsSidebarSearchFieldSurface(TextInputSurface surface) {
  return surface == TextInputSurface::SidebarSearchQuery ||
         surface == TextInputSurface::SidebarSearchReplace ||
         surface == TextInputSurface::SidebarSearchInclude ||
         surface == TextInputSurface::SidebarSearchExclude;
}

// True for every single-line text field in the shell: the surfaces that own
// Ctrl+A / Ctrl+C / Ctrl+X / Ctrl+V and that Select All and Paste must report as
// available while focused.
//
// This was spelled out as a hand-written list at four call sites, and the lists
// had already diverged: the key handler included CommitSubject and TerminalFind,
// the Select All and Paste availability rules did not. The result was Ctrl+A
// working in the commit subject and the terminal find bar while Edit > Select All
// sat greyed out over the same field.
//
// `Editor`, `Terminal` and `CommitBody` are deliberately absent: those are
// multi-line surfaces with their own handling, not single-line fields.
inline bool IsSingleLineTextInputSurface(TextInputSurface surface) {
  switch (surface) {
    case TextInputSurface::PromptInput:
    case TextInputSurface::FileFinder:
    case TextInputSurface::BufferSearch:
    case TextInputSurface::BufferReplaceSearch:
    case TextInputSurface::BufferReplaceReplace:
    case TextInputSurface::ProjectSearchOverlay:
    case TextInputSurface::CommitPicker:
    case TextInputSurface::LaunchConfigPicker:
    case TextInputSurface::CommandPalette:
    case TextInputSurface::SidebarSearchQuery:
    case TextInputSurface::SidebarSearchReplace:
    case TextInputSurface::SidebarSearchInclude:
    case TextInputSurface::SidebarSearchExclude:
    case TextInputSurface::CommitSubject:
    case TextInputSurface::TerminalFind:
    case TextInputSurface::SettingsQuery:
    case TextInputSurface::SettingsValueEdit:
    case TextInputSurface::DebugVariableEdit:
      return true;
    case TextInputSurface::None:
    case TextInputSurface::Editor:
    case TextInputSurface::CommitBody:
    case TextInputSurface::Terminal:
      return false;
  }
  return false;
}

}  // namespace microide::workspace
