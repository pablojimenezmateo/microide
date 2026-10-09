#include "platform/ProcessLauncher.h"
#include "project/ProjectSearchService.h"

namespace microide::project {

// Its own unit so the search engine links without the launcher (the search bench).
const ProjectSearchHost* ProjectSearchHostFor(const platform::ProcessLauncher& launcher) {
  // A sideways cast, as GitMetadataFor does: a remote launcher opts in by
  // implementing the interface.
  return dynamic_cast<const ProjectSearchHost*>(&launcher);
}

}  // namespace microide::project
