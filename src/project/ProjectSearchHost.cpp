#include "platform/ProcessLauncher.h"
#include "project/ProjectSearchService.h"

namespace microide::project {

const ProjectSearchHost* ProjectSearchHostFor(const platform::ProcessLauncher& launcher) {
  return dynamic_cast<const ProjectSearchHost*>(&launcher);
}

}
