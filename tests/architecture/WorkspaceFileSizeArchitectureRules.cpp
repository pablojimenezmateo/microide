#include "architecture/WorkspaceFileSizeArchitectureRules.h"

#include "architecture/ArchitectureFileScanner.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <regex>

namespace microide::tests::architecture {

RuleResult CheckWorkspaceShellCompanionTuCount(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "WorkspaceShell*.cpp translation-unit count";
  result.hard_fail = true;
  constexpr std::size_t kCap = 47;
  std::size_t count = 0;
  for (const auto& entry : std::filesystem::directory_iterator(repo_root / "src/workspace")) {
    if (!entry.is_regular_file() || entry.path().extension() != ".cpp") {
      continue;
    }
    const std::string name = entry.path().filename().string();
    if (name.starts_with("WorkspaceShell")) {
      ++count;
    }
  }
  if (count > kCap) {
    result.violations.push_back(Violation{
        .path = repo_root / "src/workspace",
        .line = 1,
        .message = "WorkspaceShell*.cpp companion count " + std::to_string(count) +
                   " exceeds cap " + std::to_string(kCap) +
                   "; land new shell-adjacent behavior on a service instead of a new "
                   "WorkspaceShell*.cpp file, then lower the cap when migrations succeed "
                   "(see dev-docs/project/known-tech-debt.md item #16)",
    });
  }
  return result;
}

RuleResult CheckCoordinatorTuSize(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "workspace coordinator translation unit size";
  result.hard_fail = true;
  for (const auto& entry : std::filesystem::directory_iterator(repo_root / "src/workspace")) {
    if (!entry.is_regular_file() || entry.path().extension() != ".cpp") {
      continue;
    }
    const std::string name = entry.path().filename().string();
    if (!name.starts_with("Workspace") || name.find("Coordinator") == std::string::npos) {
      continue;
    }
    const std::size_t lines = CountCodeLinesInFile(entry.path());
    if (lines > 900) {
      result.violations.push_back(Violation{
          .path = entry.path(),
          .line = 1,
          .message = "workspace coordinator translation units should stay at or below 900 "
                     "code lines (comments and blank lines excluded)",
      });
    }
  }
  return result;
}


}  // namespace microide::tests::architecture
