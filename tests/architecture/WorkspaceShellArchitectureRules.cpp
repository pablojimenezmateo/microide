#include "architecture/WorkspaceShellArchitectureRules.h"

#include "architecture/ArchitectureFileScanner.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <regex>

namespace microide::tests::architecture {

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
  render_files.push_back(repo_root / "src/workspace/DebugPaneRender.cpp");

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

  const std::array<std::string_view, 7> render_tus = {
      "src/workspace/WorkspaceShellRenderOverlay.cpp",
      "src/workspace/WorkspaceShellRenderTextInput.cpp",
      "src/workspace/WorkspaceShellRenderSidebar.cpp",
      "src/workspace/WorkspaceShellRenderBottomPanel.cpp",
      "src/workspace/WorkspaceShellHoverPopup.cpp",
      "src/workspace/WorkspaceShellHoverTargets.cpp",
      "src/workspace/DebugPaneRender.cpp",
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

// Ratchet-only cap on the number of `WorkspaceShell*.cpp` translation units.
// File decomposition has reached a plateau (see dev-docs/project/known-tech-debt.md
// item #16); new shell-adjacent behavior should land on a service, not on
// a new `WorkspaceShell*.cpp` companion. If a migration removes one of
// these files, lower the cap; do not raise it. This guardrail blocks
// regressions; it is not the desired end state.

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


}  // namespace microide::tests::architecture
