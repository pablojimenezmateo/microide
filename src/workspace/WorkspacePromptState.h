#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "workspace/WorkspaceProjectState.h"

namespace microide::workspace {

struct DirtyPromptState {
  enum class Kind {
    CloseTab,
    CloseTabs,
    CloseProject,
    Quit,
    RenamePath,
    DeletePath,
  };

  Kind kind = Kind::CloseTab;
  std::size_t tab_index = 0;
  std::size_t project_index = 0;
  std::vector<std::size_t> target_tabs;
  std::vector<std::size_t> dirty_tabs;
  std::size_t dirty_count = 0;
  std::filesystem::path path;
  int selected_action = 0;
};

struct PromptSurfaceState {
  enum class Kind {
    None,
    TextInput,
    Confirm,
  };

  enum class Action {
    CreateFile,
    CreateDirectory,
    RenamePath,
    DeletePath,
    DiscardGitChanges,
  };

  Kind kind = Kind::None;
  Action action = Action::CreateFile;
  std::filesystem::path path;
  std::string input;
  int selected_button = 0;
};

struct PromptState {
  bool dirty_visible = false;
  FocusTarget dirty_previous_focus = FocusTarget::Editor;
  DirtyPromptState dirty;
  bool surface_visible = false;
  FocusTarget surface_previous_focus = FocusTarget::Editor;
  PromptSurfaceState surface;
};

}  // namespace microide::workspace
