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
  SidebarSearchQuery,
  SidebarSearchReplace,
  Terminal,
  ChatComposer,
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
