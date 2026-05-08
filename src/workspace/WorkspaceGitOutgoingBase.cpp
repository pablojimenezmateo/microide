#include "workspace/WorkspaceGitOutgoingBase.h"

#include "project/GitCompareService.h"
#include "project/GitRepository.h"

namespace microide::workspace {

ResolvedGitOutgoingBase ResolveGitOutgoingBase(const std::filesystem::path& project_root,
                                               const OutgoingBaseChoice& choice) {
  ResolvedGitOutgoingBase resolved;
  if (project_root.empty()) {
    return resolved;
  }
  const bool repo_available = project::GitRepository(project_root).IsValid();

  switch (choice.kind) {
    case OutgoingBaseChoice::Kind::Auto: {
      const auto base_ref = project::ResolveGitBaseReference(project_root);
      if (base_ref.has_value()) {
        resolved.repo_available = true;
        resolved.base_ref = base_ref->ref;
        resolved.base_label = base_ref->label;
      } else {
        resolved.repo_available = repo_available;
      }
      return resolved;
    }
    case OutgoingBaseChoice::Kind::PreviousCommit:
      resolved.repo_available = repo_available;
      resolved.base_ref = "HEAD~1";
      resolved.base_label = "HEAD~1";
      return resolved;
    case OutgoingBaseChoice::Kind::SpecificRef:
      resolved.repo_available = repo_available;
      resolved.base_ref = choice.custom_ref;
      resolved.base_label = choice.custom_ref;
      return resolved;
  }

  return resolved;
}

}  // namespace microide::workspace
