#include "TestSupport.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <cctype>
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

enum class LexState {
  Code,
  LineComment,
  BlockComment,
  StringLiteral,
  CharLiteral,
};

bool IsIdentChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

std::vector<bool> BuildCodeMask(std::string_view text) {
  std::vector<bool> is_code(text.size(), true);
  LexState state = LexState::Code;
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    const char n = (i + 1 < text.size()) ? text[i + 1] : '\0';
    switch (state) {
      case LexState::Code:
        if (c == '/' && n == '/') {
          is_code[i] = false;
          if (i + 1 < text.size()) {
            is_code[i + 1] = false;
          }
          ++i;
          state = LexState::LineComment;
          continue;
        }
        if (c == '/' && n == '*') {
          is_code[i] = false;
          if (i + 1 < text.size()) {
            is_code[i + 1] = false;
          }
          ++i;
          state = LexState::BlockComment;
          continue;
        }
        if (c == '"') {
          is_code[i] = false;
          state = LexState::StringLiteral;
          continue;
        }
        if (c == '\'') {
          is_code[i] = false;
          state = LexState::CharLiteral;
          continue;
        }
        break;
      case LexState::LineComment:
        is_code[i] = false;
        if (c == '\n') {
          state = LexState::Code;
        }
        break;
      case LexState::BlockComment:
        is_code[i] = false;
        if (c == '*' && n == '/') {
          if (i + 1 < text.size()) {
            is_code[i + 1] = false;
          }
          ++i;
          state = LexState::Code;
        }
        break;
      case LexState::StringLiteral:
        is_code[i] = false;
        if (c == '\\' && i + 1 < text.size()) {
          is_code[i + 1] = false;
          ++i;
          continue;
        }
        if (c == '"') {
          state = LexState::Code;
        }
        break;
      case LexState::CharLiteral:
        is_code[i] = false;
        if (c == '\\' && i + 1 < text.size()) {
          is_code[i + 1] = false;
          ++i;
          continue;
        }
        if (c == '\'') {
          state = LexState::Code;
        }
        break;
    }
  }
  return is_code;
}

bool MatchesCodeAt(std::string_view text,
                   const std::vector<bool>& is_code,
                   std::size_t pos,
                   std::string_view needle) {
  if (pos + needle.size() > text.size()) {
    return false;
  }
  if (pos > 0 && IsIdentChar(text[pos - 1])) {
    return false;
  }
  if (pos + needle.size() < text.size() && IsIdentChar(text[pos + needle.size()])) {
    return false;
  }
  for (std::size_t i = 0; i < needle.size(); ++i) {
    if (!is_code[pos + i] || text[pos + i] != needle[i]) {
      return false;
    }
  }
  return true;
}

std::vector<std::size_t> FindTryCatchStoViolations(std::string_view text) {
  std::vector<std::size_t> violations;
  const auto is_code = BuildCodeMask(text);
  std::size_t i = 0;
  while (i < text.size()) {
    if (!MatchesCodeAt(text, is_code, i, "try")) {
      ++i;
      continue;
    }
    std::size_t j = i + 3;
    while (j < text.size() && std::isspace(static_cast<unsigned char>(text[j]))) {
      ++j;
    }
    if (j >= text.size() || text[j] != '{' || !is_code[j]) {
      i = j;
      continue;
    }

    std::size_t depth = 1;
    std::size_t k = j + 1;
    bool has_sto = false;
    while (k < text.size() && depth > 0) {
      if (is_code[k] && k + 9 <= text.size() && text.substr(k, 9) == "std::stoi") {
        has_sto = true;
      }
      if (is_code[k] && k + 10 <= text.size() && text.substr(k, 10) == "std::stoll") {
        has_sto = true;
      }
      if (is_code[k] && k + 11 <= text.size() && text.substr(k, 11) == "std::stoull") {
        has_sto = true;
      }
      if (is_code[k] && k + 9 <= text.size() && text.substr(k, 9) == "std::stof") {
        has_sto = true;
      }
      if (is_code[k] && k + 9 <= text.size() && text.substr(k, 9) == "std::stod") {
        has_sto = true;
      }
      if (is_code[k] && text[k] == '{') {
        ++depth;
      } else if (is_code[k] && text[k] == '}') {
        --depth;
      }
      ++k;
    }
    std::size_t after = k;
    while (after < text.size() && std::isspace(static_cast<unsigned char>(text[after]))) {
      ++after;
    }
    if (has_sto && MatchesCodeAt(text, is_code, after, "catch")) {
      violations.push_back(i);
    }
    // Advance one token at a time so nested try/catch blocks are also scanned.
    ++i;
  }
  return violations;
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
    const auto offsets = FindTryCatchStoViolations(text);
    for (const std::size_t offset : offsets) {
      result.violations.push_back(Violation{
          .path = entry.path(),
          .line = LineNumberAt(text, offset),
          .message = "replace try/catch std::sto parsing with util::Parse helpers",
      });
    }
  }
  return result;
}

RuleResult CheckPluginTranslationUnitSize(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "plugin translation unit size";
  result.hard_fail = true;
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

  std::vector<std::filesystem::path> render_files;
  for (const auto& entry : std::filesystem::directory_iterator(repo_root / "src/workspace")) {
    if (!entry.is_regular_file() || entry.path().extension() != ".cpp") {
      continue;
    }
    const std::string name = entry.path().filename().string();
    if (name.starts_with("WorkspaceShellRender")) {
      render_files.push_back(entry.path());
    }
  }
  render_files.push_back(repo_root / "src/workspace/WorkspaceShellHoverPopup.cpp");
  render_files.push_back(repo_root / "src/workspace/WorkspaceShellHoverTargets.cpp");

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
          .message = "workspace coordinator translation units should stay at or below 800 lines",
      });
    }
  }
  return result;
}

RuleResult CheckViewModelBackReferences(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "view model back-references";
  result.hard_fail = true;
  const std::regex view_model_decl(R"(\b(class|struct)\s+([A-Za-z_][A-Za-z0-9_]*ViewModel)\b[^;{]*\{)");
  const std::regex forbidden_field(
      R"(([A-Za-z_][A-Za-z0-9_:<>]*)\s*[*&]\s*[A-Za-z_][A-Za-z0-9_]*\s*(=|;))");

  for (const auto& entry : std::filesystem::recursive_directory_iterator(repo_root / "src")) {
    if (!entry.is_regular_file()) {
      continue;
    }
    const std::string ext = entry.path().extension().string();
    if (ext != ".h" && ext != ".hpp" && ext != ".cpp") {
      continue;
    }
    const std::string text = ReadText(entry.path());
    for (std::sregex_iterator it(text.begin(), text.end(), view_model_decl), end; it != end; ++it) {
      const std::size_t body_start = static_cast<std::size_t>(it->position() + it->length());
      std::size_t depth = 1;
      std::size_t cursor = body_start;
      while (cursor < text.size() && depth > 0) {
        if (text[cursor] == '{') {
          ++depth;
        } else if (text[cursor] == '}') {
          --depth;
        }
        ++cursor;
      }
      if (depth != 0 || cursor <= body_start) {
        continue;
      }
      const std::string body = text.substr(body_start, cursor - body_start - 1);
      for (std::sregex_iterator field_it(body.begin(), body.end(), forbidden_field), field_end;
           field_it != field_end;
           ++field_it) {
        const std::string type_name = (*field_it)[1].str();
        if (type_name.find("WorkspaceShell") == std::string::npos &&
            type_name.find("Coordinator") == std::string::npos &&
            type_name.find("Service") == std::string::npos) {
          continue;
        }
        const std::size_t field_pos = body_start + static_cast<std::size_t>(field_it->position());
        result.violations.push_back(Violation{
            .path = entry.path(),
            .line = LineNumberAt(text, field_pos),
            .message = "view model types must not store pointers/references to shell/coordinator/service",
        });
      }
    }
  }
  return result;
}

RuleResult CheckPersistenceFileIoBoundary(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "persistence file-io boundary";
  result.hard_fail = true;
  const std::regex io_pattern(
      R"(\b(std::ifstream|std::ofstream|std::fopen|fopen|open)\s*\([^;\n]*(workspace|session|config|conversation))");

  for (const auto& entry : std::filesystem::recursive_directory_iterator(repo_root / "src")) {
    if (!entry.is_regular_file()) {
      continue;
    }
    const std::string ext = entry.path().extension().string();
    if (ext != ".h" && ext != ".hpp" && ext != ".cpp") {
      continue;
    }
    const std::string rel = std::filesystem::relative(entry.path(), repo_root).string();
    if (rel.starts_with("src/persistence/") || rel == "src/workspace/WorkspacePersistenceService.h" ||
        rel == "src/workspace/WorkspacePersistenceService.cpp" ||
        rel == "src/workspace/WorkspacePersistenceLegacyImporter.cpp" ||
        rel == "src/workspace/WorkspacePersistenceLegacyImporter.h") {
      continue;
    }
    const std::string text = ReadText(entry.path());
    AppendViolations(result, entry.path(), text, io_pattern,
                     "workspace/session/config/conversation file I/O belongs in persistence services");
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
  results.push_back(CheckCoordinatorTuSize(repo_root));
  results.push_back(CheckViewModelBackReferences(repo_root));
  results.push_back(CheckPersistenceFileIoBoundary(repo_root));
  results.push_back(CheckShellFileSize(repo_root, "src/workspace/WorkspaceShell.h", 400));
  results.push_back(CheckShellFileSize(repo_root, "src/workspace/WorkspaceShell.cpp", 600));
  results.push_back(CheckShellFileSize(repo_root, "src/workspace/WorkspaceShellTestAccess.h", 600));
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

void TestTryCatchStoScanner() {
  const std::string allowed = R"cpp(
// try { std::stoi("1"); } catch (...) {}
const char* s = "try { std::stod(x); } catch (...) {}";
try {
  const auto value = util::ParseInt("42");
  (void)value;
} catch (const std::exception&) {
}
)cpp";
  const std::string flagged = R"cpp(
try
{
  int x = std::stoi("7");
  try { (void)std::stoll("9"); } catch (...) {}
}
catch (const std::exception&) {
}
)cpp";
  Expect(FindTryCatchStoViolations(allowed).empty(),
         "scanner should ignore comments/strings and non-sto try blocks");
  Expect(FindTryCatchStoViolations(flagged).size() == 2,
         "scanner should catch nested/multiline try std::sto usage");
}

}  // namespace

void RegisterArchitectureInvariantsTests(std::vector<TestCase>& tests) {
  AddTest(tests, "ArchitectureInvariants/SoftChecks", TestArchitectureInvariants);
  AddTest(tests, "ArchitectureInvariants/TryCatchStoScanner", TestTryCatchStoScanner);
}

}  // namespace microide::tests
