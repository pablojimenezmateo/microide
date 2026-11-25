#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace microide::platform {
class ProcessLauncher;
}

namespace microide::project {

std::optional<std::filesystem::path> DiscoverCompileCommandsDir(
    const std::filesystem::path& project_root);

struct ClangdCommandPlan {
  std::vector<std::string> command;
  std::string note;
};

ClangdCommandPlan PlanClangdCommand(std::vector<std::string> command,
                                    const std::filesystem::path& project_root,
                                    const platform::ProcessLauncher& launcher);

}
