#pragma once

#include <string>
#include <vector>

#include "platform/Subprocess.h"

namespace microide::project {

platform::SubprocessResult RunSubprocess(const std::vector<std::string>& command,
                                         const platform::SubprocessOptions& options = {});

}  // namespace microide::project
