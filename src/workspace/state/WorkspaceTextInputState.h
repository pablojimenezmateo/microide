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
  CommitPicker,
  QuickPick,
  CommandPalette,
  SidebarSearchQuery,
  SidebarSearchReplace,
  SidebarSearchInclude,
  SidebarSearchExclude,
  CommitSubject,
  CommitBody,
  Terminal,
  TerminalFind,
  SettingsQuery,
  SettingsValueEdit,
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
  bool swallow_next_text_input = false;
};

inline bool IsSidebarSearchFieldSurface(TextInputSurface surface) {
  return surface == TextInputSurface::SidebarSearchQuery ||
         surface == TextInputSurface::SidebarSearchReplace ||
         surface == TextInputSurface::SidebarSearchInclude ||
         surface == TextInputSurface::SidebarSearchExclude;
}

inline bool IsSingleLineTextInputSurface(TextInputSurface surface) {
  switch (surface) {
    case TextInputSurface::PromptInput:
    case TextInputSurface::FileFinder:
    case TextInputSurface::BufferSearch:
    case TextInputSurface::BufferReplaceSearch:
    case TextInputSurface::BufferReplaceReplace:
    case TextInputSurface::CommitPicker:
    case TextInputSurface::QuickPick:
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

}
