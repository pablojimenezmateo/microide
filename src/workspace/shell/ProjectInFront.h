#pragma once

// The three callbacks a background git completion uses to tell work for the
// project in front from work for one the user switched away from meanwhile
// (GitOperationService, CommitWorkflowService and PatchApplyService::Callbacks:
// is_in_front, project_label, defer_to_project). Shell TUs only.

#include <filesystem>
#include <string>

#include "workspace/WorkspaceContext.h"

namespace microide::workspace {

struct ProjectInFront {
  WorkspaceContext& context;
  bool operator()(const std::filesystem::path& root) const { return context.current_project_state.root == root; }
};

struct ProjectLabelOf {
  WorkspaceContext& context;
  std::string operator()(const std::filesystem::path& root) const {
    const ProjectWorkspaceState* state = context.ProjectStateForRoot(root);
    return state != nullptr ? state->id.Label() : root.filename().string();
  }
};

// Marks the project to catch up (git state, the tree, clean buffers, compare tabs)
// when it is next activated. A project closed meanwhile has nothing to catch up.
struct CatchUpOnActivate {
  WorkspaceContext& context;
  void operator()(const std::filesystem::path& root) const {
    if (ProjectWorkspaceState* state = context.ProjectStateForRoot(root); state != nullptr) {
      state->catch_up_on_activate = true;
    }
  }
};

}  // namespace microide::workspace
