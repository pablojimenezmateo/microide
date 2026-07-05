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
    ConfirmCommitAmend,
    ConfirmCommitNoVerify,
    // Debugger (Phase 6): edit a breakpoint modifier on `path:target_line`.
    SetBreakpointCondition,
    SetBreakpointHitCondition,
    SetBreakpointLogMessage,
    // Debugger (Phase 6): add a watch expression, or edit the one at
    // `target_line` (reused as the watch-expression index).
    AddWatchExpression,
    EditWatchExpression,
    // Debugger (Phase 9): evaluate an expression in the active session via
    // `evaluate(context:"repl")`; the result is appended to the debug console.
    EvaluateReplInput,
    // Editor navigation: jump the active viewport to a typed `line[:column]`.
    GoToLine,
  };

  Kind kind = Kind::None;
  Action action = Action::CreateFile;
  std::filesystem::path path;
  // 0-based buffer line a breakpoint-modifier prompt targets (Set* actions).
  std::size_t target_line = 0;
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
