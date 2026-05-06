#pragma once

#include <filesystem>
#include <string>

#include "workspace/WorkspaceSidebarState.h"

namespace microide::workspace {

struct ResolvedGitOutgoingBase {
  bool repo_available = false;
  std::string base_ref;
  std::string base_label;
};

ResolvedGitOutgoingBase ResolveGitOutgoingBase(const std::filesystem::path& project_root,
                                               const OutgoingBaseChoice& choice);

}  // namespace microide::workspace
