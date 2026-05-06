#include "project/SubprocessHelper.h"

namespace microide::project {

platform::SubprocessResult RunSubprocess(const std::vector<std::string>& command,
                                         const platform::SubprocessOptions& options) {
  return platform::RunSubprocess(command, options);
}

}  // namespace microide::project
