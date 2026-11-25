#pragma once

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

struct CatchUpOnActivate {
  WorkspaceContext& context;
  void operator()(const std::filesystem::path& root) const {
    if (ProjectWorkspaceState* state = context.ProjectStateForRoot(root); state != nullptr) {
      state->catch_up_on_activate = true;
    }
  }
};

}
