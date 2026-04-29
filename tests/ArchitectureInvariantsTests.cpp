#include "TestSupport.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace microide::tests {
namespace {

struct Violation {
  std::filesystem::path path;
  std::size_t line = 0;
  std::string message;
};

struct RuleResult {
  std::string label;
  bool hard_fail = false;
  std::vector<Violation> violations;
};

std::filesystem::path RepoRoot() {
  return std::filesystem::path(MICROIDE_TEST_SOURCE_DIR).parent_path();
}

std::string ReadText(const std::filesystem::path& path) {
  std::ifstream stream(path);
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  return buffer.str();
}

std::size_t LineNumberAt(std::string_view text, std::size_t offset) {
  std::size_t line = 1;
  for (std::size_t i = 0; i < offset && i < text.size(); ++i) {
    if (text[i] == '\n') {
      ++line;
    }
  }
  return line;
}

void AppendViolations(RuleResult& result,
                      const std::filesystem::path& path,
                      const std::string& text,
                      const std::regex& pattern,
                      std::string_view message) {
  for (std::sregex_iterator it(text.begin(), text.end(), pattern), end; it != end; ++it) {
    result.violations.push_back(Violation{
        .path = path,
        .line = LineNumberAt(text, static_cast<std::size_t>(it->position())),
        .message = std::string(message),
    });
  }
}

RuleResult CheckWorkspaceFriends(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "workspace friend declarations";
  result.hard_fail = true;
  const std::regex pattern(R"(\bfriend\s+(class|struct)\b)");
  for (const auto& entry :
       std::filesystem::recursive_directory_iterator(repo_root / "src/workspace")) {
    if (!entry.is_regular_file() || (entry.path().extension() != ".h" && entry.path().extension() != ".cpp")) {
      continue;
    }
    const std::string text = ReadText(entry.path());
    AppendViolations(result, entry.path(), text, pattern,
                     "workspace code should not declare friend class or friend struct");
  }
  return result;
}

RuleResult CheckCoordinatorShellConstructors(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "coordinator constructors taking WorkspaceShell";
  result.hard_fail = true;
  for (const auto& entry :
       std::filesystem::directory_iterator(repo_root / "src/workspace")) {
    if (!entry.is_regular_file() || entry.path().extension() != ".h") {
      continue;
    }
    const std::string filename = entry.path().filename().string();
    if (filename.find("Coordinator") == std::string::npos || !filename.starts_with("Workspace")) {
      continue;
    }
    const std::string stem = entry.path().stem().string();
    const std::regex pattern(stem + R"(\s*\([^;{}]*WorkspaceShell\s*[&*])");
    const std::string text = ReadText(entry.path());
    AppendViolations(result, entry.path(), text, pattern,
                     "coordinator constructors should not take WorkspaceShell");
  }
  return result;
}

RuleResult CheckThrowingStoParsers(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "try/std::sto parsing";
  for (const auto& entry : std::filesystem::recursive_directory_iterator(repo_root / "src")) {
    if (!entry.is_regular_file() || entry.path().extension() != ".cpp") {
      continue;
    }
    const std::string text = ReadText(entry.path());
    std::size_t search = 0;
    while ((search = text.find("try", search)) != std::string::npos) {
      const std::size_t brace = text.find('{', search);
      if (brace == std::string::npos) {
        break;
      }
      const std::size_t catch_pos = text.find("catch", brace);
      if (catch_pos == std::string::npos) {
        break;
      }
      if (text.substr(search, catch_pos - search).find("std::sto") != std::string::npos) {
        result.violations.push_back(Violation{
            .path = entry.path(),
            .line = LineNumberAt(text, search),
            .message = "replace try/catch std::sto parsing with util::Parse helpers",
        });
      }
      search = catch_pos + 5;
    }
  }
  return result;
}

RuleResult CheckPluginTranslationUnitSize(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "plugin translation unit size";
  for (const auto& entry : std::filesystem::directory_iterator(repo_root / "src/plugin")) {
    if (!entry.is_regular_file() || entry.path().extension() != ".cpp") {
      continue;
    }
    std::ifstream stream(entry.path());
    std::size_t lines = 0;
    std::string line;
    while (std::getline(stream, line)) {
      ++lines;
    }
    if (lines > 800) {
      result.violations.push_back(Violation{
          .path = entry.path(),
          .line = 1,
          .message = "plugin translation units should stay at or below 800 lines",
      });
    }
  }
  return result;
}

RuleResult CheckShellFileSize(const std::filesystem::path& repo_root,
                              std::string_view relative_path,
                              std::size_t limit) {
  RuleResult result;
  result.label = std::string(relative_path) + " size";
  result.hard_fail = true;
  const std::filesystem::path path = repo_root / relative_path;
  std::ifstream stream(path);
  std::size_t lines = 0;
  std::string line;
  while (std::getline(stream, line)) {
    ++lines;
  }
  if (lines > limit) {
    result.violations.push_back(Violation{
        .path = path,
        .line = 1,
        .message = std::string(relative_path) + " should stay at or below " +
                   std::to_string(limit) + " lines",
    });
  }
  return result;
}

RuleResult CheckRenderSurfaceStateAccess(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "render surface view-model-only access";
  result.hard_fail = true;

  const std::vector<std::filesystem::path> render_files = {
      repo_root / "src/workspace/WorkspaceShellRenderFrame.cpp",
      repo_root / "src/workspace/WorkspaceShellRenderOverlay.cpp",
      repo_root / "src/workspace/WorkspaceShellRenderTextInput.cpp",
      repo_root / "src/workspace/WorkspaceShellRenderSidebar.cpp",
      repo_root / "src/workspace/WorkspaceShellRenderBottomPanel.cpp",
      repo_root / "src/workspace/WorkspaceShellHoverPopup.cpp",
      repo_root / "src/workspace/WorkspaceShellHoverTargets.cpp",
  };

  const std::regex direct_state_pattern(R"(context_\.current_project_state)");
  const std::regex current_surface_pattern(R"(\bCurrentTextInputSurface\s*\()");
  for (const auto& path : render_files) {
    const std::string text = ReadText(path);
    AppendViolations(result, path, text, direct_state_pattern,
                     "render surface files should read project state through render view models");
    AppendViolations(result, path, text, current_surface_pattern,
                     "render surface files should use view-model-provided text-input surface");
  }

  return result;
}

void ReportRule(const RuleResult& result) {
  if (result.violations.empty()) {
    return;
  }
  std::cerr << "ArchitectureInvariants warning: " << result.label << '\n';
  for (const Violation& violation : result.violations) {
    std::cerr << "  " << std::filesystem::relative(violation.path, RepoRoot()).string() << ':'
              << violation.line << ": " << violation.message << '\n';
  }
}

void TestArchitectureInvariants() {
  const std::filesystem::path repo_root = RepoRoot();
  std::vector<RuleResult> results;
  results.push_back(CheckWorkspaceFriends(repo_root));
  results.push_back(CheckCoordinatorShellConstructors(repo_root));
  results.push_back(CheckThrowingStoParsers(repo_root));
  results.push_back(CheckPluginTranslationUnitSize(repo_root));
  results.push_back(CheckShellFileSize(repo_root, "src/workspace/WorkspaceShell.h", 400));
  results.push_back(CheckShellFileSize(repo_root, "src/workspace/WorkspaceShell.cpp", 600));
  results.push_back(CheckRenderSurfaceStateAccess(repo_root));

  bool hard_failure = false;
  for (const RuleResult& result : results) {
    ReportRule(result);
    if (result.hard_fail && !result.violations.empty()) {
      hard_failure = true;
    }
  }

  Expect(!hard_failure, "hard-fail architecture invariants should have no violations");
}

}  // namespace

void RegisterArchitectureInvariantsTests(std::vector<TestCase>& tests) {
  AddTest(tests, "ArchitectureInvariants/SoftChecks", TestArchitectureInvariants);
}

}  // namespace microide::tests
