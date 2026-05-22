#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "editor/SingleLineEditor.h"
#include "workspace/WorkspaceProjectState.h"

namespace microide::workspace {

enum class DirtyPathResolution {
  RequirePrompt,
  Save,
  Discard,
};

struct DirtyPromptState {
  enum class Kind {
    CloseTab,
    CloseTabs,
    CloseProject,
    Quit,
    RenamePath,
    DeletePath,
    ExternalFileChange,
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
    DiscardGitEntry,
    DiscardPatchPreview,
    SetGitOutgoingBaseRef,
    OpenExternalUrl,
  };

  Kind kind = Kind::None;
  Action action = Action::CreateFile;
  std::filesystem::path path;
  editor::SingleLineEditor input;
  std::string detail;
  std::string provider_id;
  std::string request_id;
  std::string tool_call_id;
  std::string tool_id;
  std::string capability_scope;
  int button_count = 2;
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
