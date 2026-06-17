#pragma once

#include <string>

namespace microide::workspace {

enum class TextInputSurface {
  None,
  Editor,
  Command,
  PromptInput,
  FileFinder,
  BufferSearch,
  BufferReplaceSearch,
  BufferReplaceReplace,
  ProjectSearchOverlay,
  CommitPicker,
  // Launch-config picker query field (Phase 9).
  LaunchConfigPicker,
  SidebarSearchQuery,
  SidebarSearchReplace,
  CommitSubject,
  CommitBody,
  Terminal,
  SettingsQuery,
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

}  // namespace microide::workspace
