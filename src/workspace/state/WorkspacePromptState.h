#pragma once

#include <optional>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "compare/BranchReviewStateTypes.h"
#include "compare/CompareReviewTypes.h"
#include "editor/SingleLineEditor.h"
#include "workspace/state/WorkspaceProjectState.h"

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
  std::uint64_t tab_id = 0;
  std::vector<std::uint64_t> target_tab_ids;
  std::vector<std::uint64_t> dirty_tab_ids;
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
    ConfirmCommitWarnings,
    SetBreakpointCondition,
    SetBreakpointHitCondition,
    SetBreakpointLogMessage,
    AddWatchExpression,
    EditWatchExpression,
    EvaluateReplInput,
    GoToLine,
    RenameSymbol,
    ConfirmRenameSave,
    SaveAs,
    RemoteFolder,
    ConfirmProjectReplaceAll,
    CommandArguments,
    ConfirmCommand,
    ReviewComment,
  };

  Kind kind = Kind::None;
  Action action = Action::CreateFile;
  std::filesystem::path path;
  std::size_t target_line = 0;
  editor::SingleLineEditor input;
  std::string detail;
  std::string command_name;
  std::string provider_id;
  std::string request_id;
  std::string tool_call_id;
  std::string tool_id;
  std::string capability_scope;
  int button_count = 2;
  int selected_button = 0;
  std::vector<std::string> completions;
  compare::BranchReviewTargetIdentity review_target;
  std::optional<compare::BranchReviewHunkIdentity> review_hunk;
  std::optional<compare::BranchReviewHunkIdentity> review_replaced_hunk;
};

struct PromptState {
  bool dirty_visible = false;
  FocusTarget dirty_previous_focus = FocusTarget::Editor;
  DirtyPromptState dirty;
  bool surface_visible = false;
  FocusTarget surface_previous_focus = FocusTarget::Editor;
  PromptSurfaceState surface;
};

}
