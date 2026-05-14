#include "TestSupport.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <cctype>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
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
  return std::filesystem::path(MICROIDE_TEST_SOURCE_DIR).lexically_normal().parent_path();
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

std::vector<std::size_t> FindCodeLiteralOccurrences(std::string_view text,
                                                    std::string_view literal) {
  std::vector<std::size_t> offsets;
  if (literal.empty()) {
    return offsets;
  }
  const auto is_code = BuildCodeMask(text);
  std::size_t pos = text.find(literal);
  while (pos != std::string::npos) {
    bool in_code = true;
    for (std::size_t i = 0; i < literal.size(); ++i) {
      if (pos + i >= is_code.size() || !is_code[pos + i]) {
        in_code = false;
        break;
      }
    }
    if (in_code) {
      offsets.push_back(pos);
    }
    pos = text.find(literal, pos + 1);
  }
  return offsets;
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

void AppendCodeMaskRegexViolations(RuleResult& result,
                                   const std::filesystem::path& path,
                                   const std::string& text,
                                   const std::regex& pattern,
                                   std::string_view message) {
  const auto is_code = BuildCodeMask(text);
  for (std::sregex_iterator it(text.begin(), text.end(), pattern), end; it != end; ++it) {
    const std::size_t start = static_cast<std::size_t>(it->position());
    const std::size_t len = static_cast<std::size_t>(it->length());
    bool in_code = true;
    for (std::size_t i = 0; i < len; ++i) {
      if (start + i >= is_code.size() || !is_code[start + i]) {
        in_code = false;
        break;
      }
    }
    if (!in_code) {
      continue;
    }
    result.violations.push_back(Violation{
        .path = path,
        .line = LineNumberAt(text, start),
        .message = std::string(message),
    });
  }
}

// Build a per-byte mask flagging positions inside `#ifdef MICROIDE_TESTING` blocks.
// Test-only backdoor access (e.g. `friend struct TestAccess`) is an accepted exception
// to the workspace no-friend policy; everything outside those guards is still policed.
std::vector<bool> BuildTestingGuardMask(const std::string& text) {
  std::vector<bool> mask(text.size(), false);
  std::size_t depth = 0;
  std::size_t cursor = 0;
  while (cursor < text.size()) {
    const std::size_t line_end = text.find('\n', cursor);
    const std::size_t end = (line_end == std::string::npos) ? text.size() : line_end;
    std::size_t scan = cursor;
    while (scan < end && std::isspace(static_cast<unsigned char>(text[scan]))) {
      ++scan;
    }
    if (scan < end && text[scan] == '#') {
      ++scan;
      while (scan < end && std::isspace(static_cast<unsigned char>(text[scan]))) {
        ++scan;
      }
      const std::string_view line(text.data() + scan, end - scan);
      if (line.starts_with("ifdef MICROIDE_TESTING") ||
          line.starts_with("if defined(MICROIDE_TESTING)")) {
        ++depth;
      } else if (depth > 0 && line.starts_with("endif")) {
        --depth;
      }
    }
    if (depth > 0) {
      for (std::size_t i = cursor; i < end; ++i) {
        mask[i] = true;
      }
    }
    if (line_end == std::string::npos) {
      break;
    }
    cursor = line_end + 1;
  }
  return mask;
}

RuleResult CheckWorkspaceFriends(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "workspace friend declarations";
  result.hard_fail = true;
  const std::regex pattern(R"(\bfriend\s+(class|struct)\b)");
  for (const auto& entry :
       std::filesystem::recursive_directory_iterator(repo_root / "src/workspace")) {
    if (!entry.is_regular_file()) {
      continue;
    }
    const auto ext = entry.path().extension();
    if (ext != ".h" && ext != ".cpp" && ext != ".inc") {
      continue;
    }
    const std::string text = ReadText(entry.path());
    const auto testing_mask = BuildTestingGuardMask(text);
    for (std::sregex_iterator it(text.begin(), text.end(), pattern), end; it != end; ++it) {
      const std::size_t pos = static_cast<std::size_t>(it->position());
      if (pos < testing_mask.size() && testing_mask[pos]) {
        continue;
      }
      result.violations.push_back(Violation{
          .path = entry.path(),
          .line = LineNumberAt(text, pos),
          .message = "workspace code should not declare friend class or friend struct",
      });
    }
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

RuleResult CheckRenderSurfaceGeometryAccess(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "render surface geometry access";
  result.hard_fail = true;

  const std::array<std::string_view, 6> render_tus = {
      "src/workspace/WorkspaceShellRenderOverlay.cpp",
      "src/workspace/WorkspaceShellRenderTextInput.cpp",
      "src/workspace/WorkspaceShellRenderSidebar.cpp",
      "src/workspace/WorkspaceShellRenderBottomPanel.cpp",
      "src/workspace/WorkspaceShellHoverPopup.cpp",
      "src/workspace/WorkspaceShellHoverTargets.cpp",
  };
  const std::regex compute_layout_pattern(R"(\bComputeLayout\s*\()");
  const std::regex direct_window_size_pattern(R"(context_\.window_size\b)");
  for (const std::string_view relative_path : render_tus) {
    const std::filesystem::path path = repo_root / relative_path;
    if (!std::filesystem::exists(path)) {
      continue;
    }
    const std::string text = ReadText(path);
    AppendViolations(result, path, text, compute_layout_pattern,
                     "render geometry must come from FrameToken/prepared layout, not ComputeLayout");
    AppendViolations(result, path, text, direct_window_size_pattern,
                     "render geometry must not read context_.window_size directly");
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

RuleResult CheckPluginDrainBeforeTeardown(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "plugin drain-before-teardown";
  result.hard_fail = true;
  for (const auto& entry : std::filesystem::directory_iterator(repo_root / "src/plugin")) {
    if (!entry.is_regular_file()) {
      continue;
    }
    const std::string ext = entry.path().extension().string();
    if (ext != ".cpp" && ext != ".inc") {
      continue;
    }
    const std::string text = ReadText(entry.path());
    const std::vector<bool> is_code = BuildCodeMask(text);
    // Target only the public-API teardown call sites (`impl_->TearDownPlugins(...)`),
    // not the definition or the inner leaf helper. Those leaf calls are reached
    // exclusively through these call sites, so guarding here is sufficient.
    const std::regex teardown_pattern(R"(impl_->\s*TearDownPlugins\s*\()");
    for (std::sregex_iterator it(text.begin(), text.end(), teardown_pattern), end; it != end;
         ++it) {
      const auto teardown_pos = static_cast<std::size_t>(it->position());
      if (teardown_pos < is_code.size() && !is_code[teardown_pos]) {
        continue;
      }
      // Drain seam call must appear within the previous 12 lines and after any
      // earlier teardown call in the same translation unit. The window is small
      // enough to keep the check tight without parsing function boundaries.
      std::size_t scan_start = teardown_pos;
      std::size_t lines_back = 0;
      while (scan_start > 0 && lines_back < 12) {
        --scan_start;
        if (text[scan_start] == '\n') {
          ++lines_back;
        }
      }
      const std::string_view window(text.data() + scan_start, teardown_pos - scan_start);
      const bool drain_seen = window.find("DrainAsyncProcessWorkers") != std::string_view::npos ||
                              window.find("DrainAndJoinWorkers") != std::string_view::npos;
      if (!drain_seen) {
        result.violations.push_back(Violation{
            .path = entry.path(),
            .line = LineNumberAt(text, teardown_pos),
            .message = "TearDownPlugins must be preceded by a drain seam call "
                       "(DrainAsyncProcessWorkers / DrainAndJoinWorkers) within the same path",
        });
      }
    }
  }
  return result;
}

RuleResult CheckSinglePluginReloadPerActivation(const std::filesystem::path& repo_root) {
  // The reactivation branch of ProjectCatalogService::ActivateProjectState SHALL NOT
  // call reload_plugins_for_current_project / ReloadPluginsForCurrentProject. The
  // first-activation branch routes through initialize_current_project, which already
  // performs exactly one reload internally. Reintroducing a direct call here would
  // restore the back-to-back reload regression the change was created to fix.
  RuleResult result;
  result.label = "single plugin reload per ActivateProjectState";
  result.hard_fail = true;
  const std::filesystem::path service_cpp = repo_root / "src/workspace/ProjectCatalogService.cpp";
  if (!std::filesystem::exists(service_cpp)) {
    return result;
  }
  const std::string text = ReadText(service_cpp);
  const std::vector<bool> is_code = BuildCodeMask(text);
  const std::regex activate_pattern(R"(ProjectCatalogService::ActivateProjectState\s*\([^)]*\)\s*\{)");
  std::smatch match;
  if (!std::regex_search(text, match, activate_pattern)) {
    return result;
  }
  const std::size_t body_start = static_cast<std::size_t>(match.position()) + match.length() - 1;
  // Walk braces to find the matching close.
  std::size_t depth = 0;
  std::size_t body_end = text.size();
  for (std::size_t i = body_start; i < text.size(); ++i) {
    if (i < is_code.size() && !is_code[i]) {
      continue;
    }
    if (text[i] == '{') {
      ++depth;
    } else if (text[i] == '}') {
      --depth;
      if (depth == 0) {
        body_end = i;
        break;
      }
    }
  }
  const std::regex reload_pattern(
      R"((reload_plugins_for_current_project|ReloadPluginsForCurrentProject)\s*\()");
  for (std::sregex_iterator it(text.begin() + static_cast<std::ptrdiff_t>(body_start),
                                text.begin() + static_cast<std::ptrdiff_t>(body_end),
                                reload_pattern),
       end;
       it != end; ++it) {
    const std::size_t pos = body_start + static_cast<std::size_t>(it->position());
    if (pos < is_code.size() && !is_code[pos]) {
      continue;
    }
    result.violations.push_back(Violation{
        .path = service_cpp,
        .line = LineNumberAt(text, pos),
        .message = "ActivateProjectState must not call reload_plugins_for_current_project; "
                   "first init goes through initialize_current_project, reactivation through "
                   "refresh_plugin_surfaces_for_reactivation",
    });
  }
  return result;
}

RuleResult CheckCompareRenderStructuralGate(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "compare render structural gate";
  result.hard_fail = true;
  const std::regex active_compare_pattern(R"(\bActiveTabIsCompare\s*\()");
  const std::regex direct_state_pattern(R"(context_\.current_project_state)");

  for (const auto& entry : std::filesystem::directory_iterator(repo_root / "src/workspace")) {
    if (!entry.is_regular_file() || entry.path().extension() != ".cpp") {
      continue;
    }
    const std::string name = entry.path().filename().string();
    if (!name.starts_with("WorkspaceShellCompareRender")) {
      continue;
    }
    const std::string text = ReadText(entry.path());
    AppendViolations(result, entry.path(), text, active_compare_pattern,
                     "compare render translation units must consume structural view-model gates, "
                     "not ActiveTabIsCompare()");
    AppendViolations(result, entry.path(), text, direct_state_pattern,
                     "compare render translation units must not read context_.current_project_state");
  }
  return result;
}

RuleResult CheckPerClipRenderPathDoesNotRunFramePrep(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "per-clip render path avoids frame prep";
  result.hard_fail = true;

  const std::filesystem::path app_cpp = repo_root / "src/app/Application.cpp";
  const std::filesystem::path shell_render_cpp = repo_root / "src/workspace/WorkspaceShellRender.cpp";
  const std::string app_text = ReadText(app_cpp);
  const std::string shell_render_text = ReadText(shell_render_cpp);
  const std::vector<bool> app_is_code = BuildCodeMask(app_text);
  const std::vector<bool> shell_is_code = BuildCodeMask(shell_render_text);

  const std::regex partial_loop_pattern(
      R"(for\s*\(\s*const SDL_Rect& clip_rect\s*:\s*dirty_region_analysis\.merged_clip_rects\s*\)\s*\{)");
  std::smatch partial_loop_match;
  if (std::regex_search(app_text, partial_loop_match, partial_loop_pattern)) {
    const std::size_t loop_start =
        static_cast<std::size_t>(partial_loop_match.position()) + partial_loop_match.length() - 1;
    std::size_t depth = 0;
    std::size_t loop_end = app_text.size();
    for (std::size_t i = loop_start; i < app_text.size(); ++i) {
      if (i < app_is_code.size() && !app_is_code[i]) {
        continue;
      }
      if (app_text[i] == '{') {
        ++depth;
      } else if (app_text[i] == '}') {
        --depth;
        if (depth == 0) {
          loop_end = i;
          break;
        }
      }
    }
    const std::string loop_body = app_text.substr(loop_start, loop_end - loop_start + 1);
    if (loop_body.find("PrepareFrameOnce(") != std::string::npos ||
        loop_body.find("PrepareRenderFrame(") != std::string::npos) {
      result.violations.push_back(Violation{
          .path = app_cpp,
          .line = LineNumberAt(app_text, loop_start),
          .message = "partial-clip render loop must not invoke frame prep; prepare once before the loop",
      });
    }
  }

  const std::regex render_clip_pattern(
      R"(WorkspaceShell::RenderClip\s*\([^)]*\)\s*\{)");
  std::smatch render_clip_match;
  if (std::regex_search(shell_render_text, render_clip_match, render_clip_pattern)) {
    const std::size_t body_start =
        static_cast<std::size_t>(render_clip_match.position()) + render_clip_match.length() - 1;
    std::size_t depth = 0;
    std::size_t body_end = shell_render_text.size();
    for (std::size_t i = body_start; i < shell_render_text.size(); ++i) {
      if (i < shell_is_code.size() && !shell_is_code[i]) {
        continue;
      }
      if (shell_render_text[i] == '{') {
        ++depth;
      } else if (shell_render_text[i] == '}') {
        --depth;
        if (depth == 0) {
          body_end = i;
          break;
        }
      }
    }
    const std::string body =
        shell_render_text.substr(body_start, body_end - body_start + 1);
    const std::array<std::string_view, 5> forbidden{
        "PrepareFrameOnce(",
        "PrepareRenderFrame(",
        "ComputeLayout(",
        "NormalizeEditorSplitTree(",
        "RenderViewModelBuilder(",
    };
    for (const std::string_view needle : forbidden) {
      if (body.find(needle) == std::string::npos) {
        continue;
      }
      result.violations.push_back(Violation{
          .path = shell_render_cpp,
          .line = LineNumberAt(shell_render_text, body_start),
          .message = "RenderClip must not run frame prep/layout/normalization/view-model construction",
      });
      break;
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
    const std::string rel = entry.path().lexically_normal().lexically_relative(
                                repo_root.lexically_normal()).generic_string();
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

// Task 4.1: No synchronous blocking-wait primitives in workspace code.
// Blocking on a subprocess from the main thread stalls the event loop.
// All git/lint subprocesses must be dispatched through ProjectBackgroundExecutor.
RuleResult CheckNoSynchronousSubprocessWaitInWorkspace(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "synchronous subprocess wait in workspace";
  result.hard_fail = true;
  // Catches direct use of raw blocking-wait primitives. High-level RunSubprocess() wrappers are
  // pre-existing and tracked separately; new workspace code must use ProjectBackgroundExecutor.
  const std::regex pattern(
      R"(\bSubprocess::Wait\s*\(|\bwaitpid\s*\(|\bWaitForSingleObject\s*\(|\bWaitForMultipleObjects\s*\()");
  for (const auto& entry :
       std::filesystem::recursive_directory_iterator(repo_root / "src/workspace")) {
    if (!entry.is_regular_file() ||
        (entry.path().extension() != ".h" && entry.path().extension() != ".cpp")) {
      continue;
    }
    const std::string text = ReadText(entry.path());
    AppendViolations(result, entry.path(), text, pattern,
                     "workspace code must not block on subprocess wait; use ProjectBackgroundExecutor");
  }
  return result;
}

// Task 4.2: LSP textDocument/didOpen and textDocument/didChange must not be sent
// synchronously from EditorTabService::ActivateTab. The hydration path must dispatch
// notifications asynchronously so the tab is visible before LSP acknowledges.
RuleResult CheckLspDidOpenIsNonBlocking(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "LSP didOpen/didChange synchronous send from ActivateTab";
  result.hard_fail = true;
  // Scan Activate() in TabCoordinator for direct LSP didOpen/didChange dispatch.
  // Activation should only hydrate UI state and schedule async work.
  const std::filesystem::path target_path = repo_root / "src/workspace/WorkspaceTabCoordinator.cpp";
  if (!std::filesystem::exists(target_path)) {
    return result;
  }
  const std::string text = ReadText(target_path);

  const std::string activate_signature = "void TabCoordinator::Activate(std::size_t index)";
  const std::size_t activate_pos = text.find(activate_signature);
  if (activate_pos == std::string::npos) {
    return result;
  }
  const std::size_t body_start = text.find('{', activate_pos);
  if (body_start == std::string::npos) {
    return result;
  }

  std::size_t body_end = body_start;
  int depth = 0;
  for (; body_end < text.size(); ++body_end) {
    if (text[body_end] == '{') {
      ++depth;
    } else if (text[body_end] == '}') {
      --depth;
      if (depth == 0) {
        break;
      }
    }
  }
  if (body_end <= body_start || body_end >= text.size()) {
    return result;
  }

  const std::string activate_body = text.substr(body_start + 1, body_end - body_start - 1);
  const std::regex sync_lsp_dispatch_pattern(
      R"(\b(DidOpen|DidChange|DidChangeIncremental|EnsureLspDocumentOpen|NotifyLspBufferOpen)\s*\()");
  std::smatch match;
  if (std::regex_search(activate_body, match, sync_lsp_dispatch_pattern)) {
    result.violations.push_back(
        Violation{target_path, 1,
                  "TabCoordinator::Activate must not synchronously dispatch LSP didOpen/didChange"});
  }
  return result;
}

RuleResult CheckNoLegacyPersistenceSymbols(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "legacy persistence symbols";
  result.hard_fail = true;
  const std::array<std::string_view, 8> forbidden = {
      "WorkspacePersistenceLegacyFormat", "EncodeSessionNodePath", "DecodeSessionNodePath",
      "ParseUserConfigText",             "ParseProjectConfigText", "ParseProjectSessionText",
      "ParseWorkspaceSessionText",       "WorkspacePersistenceLegacyImporter",
  };

  for (const auto& root_dir : {repo_root / "src", repo_root / "tests", repo_root / "tools"}) {
    if (!std::filesystem::exists(root_dir)) {
      continue;
    }
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root_dir)) {
      if (!entry.is_regular_file()) {
        continue;
      }
      const std::string ext = entry.path().extension().string();
      if (ext != ".h" && ext != ".hpp" && ext != ".cpp" && ext != ".cc" && ext != ".cxx" &&
          ext != ".inc") {
        continue;
      }
      const std::string text = ReadText(entry.path());
      for (const std::string_view symbol : forbidden) {
        for (const std::size_t pos : FindCodeLiteralOccurrences(text, symbol)) {
          result.violations.push_back(Violation{
              .path = entry.path(),
              .line = LineNumberAt(text, pos),
              .message = "remove legacy persistence symbol: " + std::string(symbol),
          });
        }
      }
    }
  }
  return result;
}

// Catches the anti-pattern where workspace code posts work to the background executor
// and then immediately blocks on the resulting future — defeating the offload while
// looking asynchronous.
RuleResult CheckNoExecutorPostThenFutureGetInWorkspace(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "workspace executor-post-then-future-get anti-pattern";
  result.hard_fail = true;
  const std::regex pattern(
      R"(project_background_executor_\s*\.\s*Post\s*\([\s\S]{0,2000}?\.\s*get\s*\(\s*\))");
  for (const auto& entry :
       std::filesystem::recursive_directory_iterator(repo_root / "src/workspace")) {
    if (!entry.is_regular_file() || entry.path().extension() != ".cpp") {
      continue;
    }
    const std::string text = ReadText(entry.path());
    AppendCodeMaskRegexViolations(
        result, entry.path(), text, pattern,
        "do not Post() to ProjectBackgroundExecutor and immediately wait on the future "
        "from the caller — either run inline or restructure to async completion");
  }
  return result;
}

RuleResult CheckNoSynchronousSubprocessInWorkspace(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "synchronous subprocess call in workspace";
  result.hard_fail = true;
  const std::regex pattern(R"(\bplatform::RunSubprocess\s*\()");
  for (const auto& entry :
       std::filesystem::recursive_directory_iterator(repo_root / "src/workspace")) {
    if (!entry.is_regular_file() || entry.path().extension() != ".cpp") {
      continue;
    }
    const std::string text = ReadText(entry.path());
    AppendCodeMaskRegexViolations(
        result, entry.path(), text, pattern,
        "workspace code must not run synchronous subprocesses; use ProjectBackgroundExecutor");
  }
  return result;
}

RuleResult CheckRenderTuDoesNotMaterializeStrings(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "render translation units do not materialize search fallback strings";
  result.hard_fail = true;
  const std::filesystem::path sidebar_path = repo_root / "src/workspace/WorkspaceShellRenderSidebar.cpp";
  const std::string text = ReadText(sidebar_path);
  AppendViolations(
      result, sidebar_path, text, std::regex(R"(std::string\s*\(\s*"search>\s*"\s*\)\s*\+)"),
      "render code must use RenderViewModelBuilder query_fallback_text");
  AppendViolations(
      result, sidebar_path, text, std::regex(R"(std::string\s*\(\s*"replace>\s*"\s*\)\s*\+)"),
      "render code must use RenderViewModelBuilder replace_fallback_text");
  AppendViolations(
      result, sidebar_path, text, std::regex(R"("search>\s*"\s*\+)"),
      "render code must not concatenate search fallback text in render path");
  AppendViolations(
      result, sidebar_path, text, std::regex(R"("replace>\s*"\s*\+)"),
      "render code must not concatenate replace fallback text in render path");
  return result;
}

RuleResult CheckRenderTuDoesNotCallToStringOrFormat(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "render translation units avoid std::to_string / std::format / fmt::format";
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

  const std::regex to_string_pattern(R"(\bstd::to_string\s*\()");
  const std::regex std_format_pattern(R"(\bstd::format\s*\()");
  const std::regex fmt_format_pattern(R"(\bfmt::format\s*\()");
  for (const auto& path : render_files) {
    if (!std::filesystem::exists(path)) {
      continue;
    }
    const std::string text = ReadText(path);
    AppendCodeMaskRegexViolations(
        result, path, text, to_string_pattern,
        "render TU must not call std::to_string; compute strings in RenderViewModelBuilder");
    AppendCodeMaskRegexViolations(
        result, path, text, std_format_pattern,
        "render TU must not call std::format; compute strings in RenderViewModelBuilder");
    AppendCodeMaskRegexViolations(
        result, path, text, fmt_format_pattern,
        "render TU must not call fmt::format; compute strings in RenderViewModelBuilder");
  }
  return result;
}

RuleResult CheckTextViewportNoFullDocCopy(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "TextViewport ReplaceAll full document copy";
  result.hard_fail = true;
  const std::filesystem::path path = repo_root / "src/editor/TextViewport.cpp";
  const std::string text = ReadText(path);
  const std::size_t replace_all_pos = text.find("std::size_t TextViewport::ReplaceAll(");
  if (replace_all_pos == std::string::npos) {
    result.violations.push_back(Violation{
        .path = path,
        .line = 1,
        .message = "could not locate TextViewport::ReplaceAll body for invariant scan",
    });
    return result;
  }
  const std::size_t body_start = text.find('{', replace_all_pos);
  if (body_start == std::string::npos) {
    result.violations.push_back(Violation{
        .path = path,
        .line = LineNumberAt(text, replace_all_pos),
        .message = "could not locate ReplaceAll body start",
    });
    return result;
  }
  std::size_t depth = 1;
  std::size_t body_end = body_start + 1;
  for (; body_end < text.size() && depth > 0; ++body_end) {
    if (text[body_end] == '{') {
      ++depth;
    } else if (text[body_end] == '}') {
      --depth;
    }
  }
  const std::string body = text.substr(body_start, body_end - body_start);
  const auto is_code = BuildCodeMask(body);
  const std::array<std::regex, 2> patterns = {
      std::regex(R"(std::vector<std::string>\s+\w+\s*=\s*document_->lines\s*;)"),
      std::regex(R"(auto\s+\w+\s*=\s*document_->lines\s*;)"),
  };
  for (const auto& pattern : patterns) {
    for (std::sregex_iterator it(body.begin(), body.end(), pattern), end; it != end; ++it) {
      const std::size_t local_start = static_cast<std::size_t>(it->position());
      const std::size_t local_len = static_cast<std::size_t>(it->length());
      bool in_code = true;
      for (std::size_t i = 0; i < local_len; ++i) {
        if (local_start + i >= is_code.size() || !is_code[local_start + i]) {
          in_code = false;
          break;
        }
      }
      if (!in_code) {
        continue;
      }
      const std::size_t absolute = body_start + local_start;
      result.violations.push_back(Violation{
          .path = path,
          .line = LineNumberAt(text, absolute),
          .message = "ReplaceAll must not copy document_->lines into a full vector",
      });
    }
  }
  return result;
}

std::optional<std::string> ExtractBraceDelimitedBody(const std::string& text,
                                                     std::size_t open_brace_index) {
  if (open_brace_index >= text.size() || text[open_brace_index] != '{') {
    return std::nullopt;
  }
  const auto is_code = BuildCodeMask(text);
  std::size_t depth = 0;
  for (std::size_t i = open_brace_index; i < text.size(); ++i) {
    if (i < is_code.size() && !is_code[i]) {
      continue;
    }
    if (text[i] == '{') {
      ++depth;
    } else if (text[i] == '}') {
      --depth;
      if (depth == 0) {
        return text.substr(open_brace_index + 1, i - open_brace_index - 1);
      }
    }
  }
  return std::nullopt;
}

std::optional<std::pair<std::string, std::size_t>> ExtractMemberFunctionBodyWithOffset(
    const std::string& text, std::string_view signature_needle) {
  const std::size_t sig = text.find(signature_needle);
  if (sig == std::string::npos) {
    return std::nullopt;
  }
  const std::size_t open = text.find('{', sig);
  if (open == std::string::npos || open + 1 >= text.size()) {
    return std::nullopt;
  }
  const auto body = ExtractBraceDelimitedBody(text, open);
  if (!body.has_value()) {
    return std::nullopt;
  }
  return std::pair<std::string, std::size_t>{*body, open + 1};
}

RuleResult CheckEssentialEditorCppModulesDoNotTouchLuaState(
    const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "Lua VM pointers stay out of language/fold/shape helpers";
  result.hard_fail = true;
  const std::array<std::string_view, 4> paths = {
      "src/workspace/WorkspaceLanguageContract.cpp",
      "src/editor/FoldingModel.cpp",
      "src/editor/IndentGuides.cpp",
      "src/editor/SnippetEngine.cpp",
  };
  const std::regex lua_pointer(R"(\blua_State\s*\*)");
  for (const std::string_view relative : paths) {
    const std::filesystem::path path = repo_root / relative;
    if (!std::filesystem::exists(path)) {
      result.violations.push_back(Violation{
          .path = path,
          .line = 1,
          .message = "expected editor essential translation unit",
      });
      continue;
    }
    const std::string file_text = ReadText(path);
    AppendCodeMaskRegexViolations(
        result, path, file_text, lua_pointer,
        "WorkspaceLanguageContract/FoldingModel/IndentGuides/SnippetEngine must stay Lua-free "
        "at the type level (lua_State* leaks implementation coupling)");
  }
  return result;
}

RuleResult CheckTextViewportApplyPipelineNoFullDocumentLineSnapshot(
    const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "TextViewport edit pipeline avoids full document_->lines snapshots";
  result.hard_fail = true;
  const std::filesystem::path path = repo_root / "src/editor/TextViewport.cpp";
  const std::string text = ReadText(path);
  const std::array<std::string_view, 2> signatures = {
      "bool TextViewport::ApplyLineEdit(",
      "bool TextViewport::ApplyRangeEdit(",
  };
  const std::array<std::regex, 2> patterns = {
      std::regex(R"(std::vector<std::string>\s+\w+\s*=\s*document_->lines\s*;)"),
      std::regex(R"(auto\s+\w+\s*=\s*document_->lines\s*;)"),
  };
  for (const std::string_view signature : signatures) {
    const auto body_with_offset = ExtractMemberFunctionBodyWithOffset(text, signature);
    if (!body_with_offset.has_value()) {
      result.violations.push_back(Violation{
          .path = path,
          .line = 1,
          .message = std::string("could not locate body for ") + std::string(signature),
      });
      continue;
    }
    const std::string& body = body_with_offset->first;
    const std::size_t body_offset = body_with_offset->second;
    const auto is_code = BuildCodeMask(body);
    for (const auto& pattern : patterns) {
      for (std::sregex_iterator it(body.begin(), body.end(), pattern), end; it != end; ++it) {
        const std::size_t local_start = static_cast<std::size_t>(it->position());
        const std::size_t local_len = static_cast<std::size_t>(it->length());
        bool in_code = true;
        for (std::size_t i = 0; i < local_len; ++i) {
          if (local_start + i >= is_code.size() || !is_code[local_start + i]) {
            in_code = false;
            break;
          }
        }
        if (!in_code) {
          continue;
        }
        result.violations.push_back(Violation{
            .path = path,
            .line = LineNumberAt(text, body_offset + local_start),
            .message = "ApplyLineEdit/ApplyRangeEdit must not snapshot-copy document_->lines",
        });
      }
    }
  }
  return result;
}

 RuleResult CheckBuildEditorViewModelUsesIncrementalVectorWrites(
     const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "BuildEditorViewModel clears and appends view-model vectors";
  result.hard_fail = true;
  const std::filesystem::path path = repo_root / "src/workspace/RenderViewModelBuilder.cpp";
  const std::string text = ReadText(path);
  const auto body_with_offset = ExtractMemberFunctionBodyWithOffset(
      text, "editor::EditorViewModel RenderViewModelBuilder::BuildEditorViewModel");
  if (!body_with_offset.has_value()) {
    result.violations.push_back(Violation{
        .path = path,
        .line = 1,
        .message = "could not locate BuildEditorViewModel body for vector-style scan",
    });
    return result;
  }
  const std::string& body = body_with_offset->first;
  const std::size_t body_offset = body_with_offset->second;
  const auto is_code = BuildCodeMask(body);
  const std::regex bad_assign(
      R"re(vm\.(fold_gutter_marks|occurrence_ranges|sticky_lines|whitespace_glyph_runs)\s*=)re");
  for (std::sregex_iterator it(body.begin(), body.end(), bad_assign), end; it != end; ++it) {
    const std::size_t start = static_cast<std::size_t>(it->position());
    if (start < is_code.size() && !is_code[start]) {
      continue;
    }
    result.violations.push_back(Violation{
        .path = path,
        .line = LineNumberAt(text, body_offset + start),
        .message = "EditorViewModel vectors should use clear()/reserve()/push_back() or assign("
                   "iter,iter), not whole-vector assignment",
    });
  }

  const std::regex std_string_decls(R"(\bstd::string\b)");
  for (std::sregex_iterator it(body.begin(), body.end(), std_string_decls), end; it != end;
       ++it) {
    const std::size_t start = static_cast<std::size_t>(it->position());
    if (start >= is_code.size() || !is_code[start]) {
      continue;
    }
    result.violations.push_back(Violation{
        .path = path,
        .line = LineNumberAt(text, body_offset + start),
        .message = "BuildEditorViewModel must remain free of std::string temporaries "
                   "(view-model path should stay numeric / pointer only)",
    });
  }
  return result;
}

RuleResult CheckRenderTuEditorEssentialsAvoidEphemeralLabelStrings(
    const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "editor essentials render helpers avoid std::string fold/sticky labels";
  result.hard_fail = true;
  const std::filesystem::path path = repo_root / "src/editor/EditorViewRenderer.cpp";
  const std::string text = ReadText(path);
  const std::array<std::pair<std::regex, std::string_view>, 4> patterns = {
      std::pair{std::regex(R"(\bstd::string\b[^;\n]*FoldGutter)"),
                "fold gutter painting must stay glyph/decoration-only (no std::string labels)"},
      std::pair{std::regex(R"(\bstd::string\b[^;\n]*sticky_lines)"),
                "sticky scroll rows must reuse buffer slices / views (no std::string labels)"},
      std::pair{std::regex(R"(\bstd::string\b[^;\n]*OccurrenceRange)"),
                "occurrence underlay is view-model driven (no std::string synthesis here)"},
      std::pair{std::regex(R"(\bstd::string\b[^;\n]*IndentGuide)"),
                "indent guides paint from numeric runs (no std::string labels here)"},
  };
  for (const auto& [pattern, message] : patterns) {
    AppendCodeMaskRegexViolations(result, path, text, pattern, message);
  }
  return result;
}

RuleResult CheckWorkspaceShellRenderFrameAvoidsEphemeralEditorViewModelStrings(
    const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "WorkspaceShellRenderFrame editor VM path avoids std::string assembly";
  result.hard_fail = true;
  const std::filesystem::path path = repo_root / "src/workspace/WorkspaceShellRenderFrame.cpp";
  if (!std::filesystem::exists(path)) {
    return result;
  }
  const std::string text = ReadText(path);
  const std::array<std::pair<std::regex, std::string_view>, 2> patterns = {
      std::pair{std::regex(R"(\bstd::string\b[^;\n]*fold_gutter_marks)"),
                "fold gutter data should come from RenderViewModelBuilder, not local strings"},
      std::pair{std::regex(R"(\bstd::string\b[^;\n]*BuildEditorViewModel)"),
                "BuildEditorViewModel output should remain structurally typed (no string packing)"},
  };
  for (const auto& [pattern, message] : patterns) {
    AppendCodeMaskRegexViolations(result, path, text, pattern, message);
  }
  return result;
}

void ReportRule(const RuleResult& result) {
  if (result.violations.empty()) {
    return;
  }
  std::cerr << "ArchitectureInvariants warning: " << result.label << '\n';
  const std::filesystem::path repo_root = RepoRoot().lexically_normal();
  for (const Violation& violation : result.violations) {
    const std::filesystem::path relative =
        violation.path.lexically_normal().lexically_relative(repo_root);
    const std::filesystem::path display_path =
        relative.empty() ? violation.path.lexically_normal() : relative;
    std::cerr << "  " << display_path.generic_string() << ':' << violation.line << ": "
              << violation.message << '\n';
  }
}

void TestArchitectureInvariants() {
  const std::filesystem::path repo_root = RepoRoot();
  std::vector<RuleResult> results;
  const auto run_rule = [&](const char* label, auto&& fn) {
    try {
      results.push_back(fn(repo_root));
    } catch (const std::exception& error) {
      throw std::runtime_error(std::string(label) + ": " + error.what());
    }
  };
  run_rule("CheckWorkspaceFriends", CheckWorkspaceFriends);
  run_rule("CheckCoordinatorShellConstructors", CheckCoordinatorShellConstructors);
  run_rule("CheckThrowingStoParsers", CheckThrowingStoParsers);
  run_rule("CheckPluginTranslationUnitSize", CheckPluginTranslationUnitSize);
  run_rule("CheckCoordinatorTuSize", CheckCoordinatorTuSize);
  run_rule("CheckViewModelBackReferences", CheckViewModelBackReferences);
  run_rule("CheckPersistenceFileIoBoundary", CheckPersistenceFileIoBoundary);
  run_rule("CheckPluginDrainBeforeTeardown", CheckPluginDrainBeforeTeardown);
  run_rule("CheckSinglePluginReloadPerActivation", CheckSinglePluginReloadPerActivation);
  run_rule("CheckCompareRenderStructuralGate", CheckCompareRenderStructuralGate);
  run_rule("CheckPerClipRenderPathDoesNotRunFramePrep", CheckPerClipRenderPathDoesNotRunFramePrep);
  run_rule("CheckShellFileSize(WorkspaceShell.h)",
           [&](const std::filesystem::path& root) {
             return CheckShellFileSize(root, "src/workspace/WorkspaceShell.h", 400);
           });
  run_rule("CheckShellFileSize(WorkspaceShell.cpp)",
           [&](const std::filesystem::path& root) {
             return CheckShellFileSize(root, "src/workspace/WorkspaceShell.cpp", 600);
           });
  run_rule("CheckShellFileSize(WorkspaceShellTestAccess.h)",
           [&](const std::filesystem::path& root) {
             return CheckShellFileSize(root, "src/workspace/WorkspaceShellTestAccess.h", 600);
           });
  run_rule("CheckRenderSurfaceStateAccess", CheckRenderSurfaceStateAccess);
  run_rule("CheckRenderSurfaceGeometryAccess", CheckRenderSurfaceGeometryAccess);
  run_rule("CheckNoSynchronousSubprocessWaitInWorkspace", CheckNoSynchronousSubprocessWaitInWorkspace);
  run_rule("CheckLspDidOpenIsNonBlocking", CheckLspDidOpenIsNonBlocking);
  run_rule("CheckNoLegacyPersistenceSymbols", CheckNoLegacyPersistenceSymbols);
  run_rule("CheckNoSynchronousSubprocessInWorkspace", CheckNoSynchronousSubprocessInWorkspace);
  run_rule("CheckNoExecutorPostThenFutureGetInWorkspace", CheckNoExecutorPostThenFutureGetInWorkspace);
  run_rule("CheckRenderTuDoesNotMaterializeStrings", CheckRenderTuDoesNotMaterializeStrings);
  run_rule("CheckRenderTuDoesNotCallToStringOrFormat", CheckRenderTuDoesNotCallToStringOrFormat);
  run_rule("CheckTextViewportNoFullDocCopy", CheckTextViewportNoFullDocCopy);
  run_rule("CheckEssentialEditorCppModulesDoNotTouchLuaState", CheckEssentialEditorCppModulesDoNotTouchLuaState);
  run_rule("CheckTextViewportApplyPipelineNoFullDocumentLineSnapshot",
           CheckTextViewportApplyPipelineNoFullDocumentLineSnapshot);
  run_rule("CheckBuildEditorViewModelUsesIncrementalVectorWrites",
           CheckBuildEditorViewModelUsesIncrementalVectorWrites);
  run_rule("CheckRenderTuEditorEssentialsAvoidEphemeralLabelStrings",
           CheckRenderTuEditorEssentialsAvoidEphemeralLabelStrings);
  run_rule("CheckWorkspaceShellRenderFrameAvoidsEphemeralEditorViewModelStrings",
           CheckWorkspaceShellRenderFrameAvoidsEphemeralEditorViewModelStrings);

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

void TestArchitectureInvariantTargetedScannerFixtures() {
  TemporaryDirectory temp_dir;
  const auto& root = temp_dir.path();
  std::filesystem::create_directories(root / "src/workspace");
  std::filesystem::create_directories(root / "src/editor");
  std::filesystem::create_directories(root / "tests");
  std::filesystem::create_directories(root / "tools");

  WriteFile(root / "src/workspace/NeedsExecutor.cpp",
            "void F(){ platform::RunSubprocess({\"echo\"}, {}); }\n");
  WriteFile(root / "src/workspace/WorkspaceShellRenderSidebar.cpp",
            "std::string F(){ return std::string(\"search> \") + std::string(\"x\"); }\n");
  WriteFile(root / "src/workspace/WorkspaceLanguageContract.cpp", "// lang contract fixture\n");
  WriteFile(root / "src/editor/IndentGuides.cpp", "// indent guides fixture\n");
  WriteFile(root / "src/editor/SnippetEngine.cpp", "// snippet engine fixture\n");
  WriteFile(root / "src/editor/FoldingModel.cpp", "void leak(lua_State* L){ (void)L; }\n");
  WriteFile(root / "src/editor/TextViewport.cpp",
            "std::size_t TextViewport::ReplaceAll(std::string_view, std::string_view) {\n"
            "  std::vector<std::string> before = document_->lines;\n"
            "  (void)before;\n"
            "  return 0;\n"
            "}\n");
  WriteFile(root / "tests/LegacySymbolFixture.cpp", "void X(){ WorkspacePersistenceLegacyFormat x; }\n");

  Expect(!CheckEssentialEditorCppModulesDoNotTouchLuaState(root).violations.empty(),
         "lua_State pointers should not appear in FoldingModel.cpp fixture");

  Expect(!CheckNoLegacyPersistenceSymbols(root).violations.empty(),
         "legacy-persistence rule should catch legacy symbols");
  Expect(!CheckNoSynchronousSubprocessInWorkspace(root).violations.empty(),
         "workspace subprocess rule should catch synchronous subprocess calls");
  Expect(!CheckRenderTuDoesNotMaterializeStrings(root).violations.empty(),
         "render materialization rule should catch string construction in render TU");
  Expect(!CheckTextViewportNoFullDocCopy(root).violations.empty(),
         "TextViewport rule should catch full document copies");

  WriteFile(root / "src/editor/TextViewport.cpp",
            "std::size_t TextViewport::ReplaceAll(std::string_view, std::string_view) {\n"
            "  return 0;\n"
            "}\n"
            "bool TextViewport::ApplyLineEdit(std::size_t,std::size_t,const "
            "std::vector<std::string>&) {\n"
            "  auto snap = document_->lines;\n"
            "  return !snap.empty();\n"
            "}\n");
  Expect(!CheckTextViewportApplyPipelineNoFullDocumentLineSnapshot(root).violations.empty(),
         "ApplyLineEdit fixture should flag full document_->lines snapshots");
}

}  // namespace

void RegisterArchitectureInvariantsTests(std::vector<TestCase>& tests) {
  AddTest(tests, "ArchitectureInvariants/SoftChecks", TestArchitectureInvariants);
  AddTest(tests, "ArchitectureInvariants/TryCatchStoScanner", TestTryCatchStoScanner);
  AddTest(tests, "ArchitectureInvariants/TargetedScannerFixtures",
          TestArchitectureInvariantTargetedScannerFixtures);
}

}  // namespace microide::tests
