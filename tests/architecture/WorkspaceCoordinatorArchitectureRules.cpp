#include "architecture/WorkspaceCoordinatorArchitectureRules.h"

#include "architecture/ArchitectureFileScanner.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <regex>

namespace microide::tests::architecture {

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

// Forbids reintroducing a single combined `layout_revision` member or
// accessor on `TextViewport::DocumentState`. The four-tier revision model
// (content/syntax/layout_shape/presentation) replaced it intentionally; a
// regression here would re-merge invalidations across tiers and silently
// undo the openspec change `split-layout-revision-tiers`.

RuleResult CheckTextViewportNoCombinedLayoutRevision(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "TextViewport tiered revisions (no combined layout_revision)";
  result.hard_fail = true;
  const std::filesystem::path header = repo_root / "src/editor/TextViewport.h";
  if (!std::filesystem::exists(header)) {
    return result;
  }
  const std::string text = ReadText(header);
  // Look for a `layout_revision` member or accessor on the DocumentState.
  // FoldingModel::Snapshot::layout_revision is in a different header and
  // intentionally retains its name, so scoping to TextViewport.h is enough.
  const std::regex pattern(R"(\blayout_revision\b)");
  AppendCodeMaskRegexViolations(
      result, header, text, pattern,
      "do not reintroduce a combined layout_revision on TextViewport::DocumentState; "
      "use content_revision / syntax_revision / layout_shape_revision / "
      "presentation_revision per openspec change split-layout-revision-tiers");
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
  // Catch BOTH the platform:: entry point and the transparent project:: alias — the
  // old lint only saw platform::, letting project::RunSubprocess slip the "dispatch
  // through ProjectBackgroundExecutor" policy. (TD-2026-07-16-15.)
  const std::regex pattern(R"(\b(platform|project)::RunSubprocess\s*\()");
  // Deliberate, documented exception: format-on-save runs the contributed formatter
  // synchronously because an EXPLICIT save is a user-initiated blocking action that must
  // complete before returning (bounded by a 5 s timeout; autosave — the frequent path —
  // suppresses formatters so background writes never block the UI). This one site is
  // allowlisted; making it async would change the save contract (visible in-progress /
  // cancellation UX) and is tracked separately. Do NOT add new entries here.
  const std::array<std::string_view, 2> allowed_files = {
      "WorkspaceTabCoordinatorShellBridge.cpp",
      // ToolDownloader runs its sha256 hash subprocess inside a lambda posted to its own
      // background_executor_ (ComputeSha256Blocking off the shell thread), so it does not
      // block the UI. Allowlisted as a deliberate off-thread use.
      "WorkspaceToolDownloader.cpp",
  };
  for (const auto& entry :
       std::filesystem::recursive_directory_iterator(repo_root / "src/workspace")) {
    if (!entry.is_regular_file() || entry.path().extension() != ".cpp") {
      continue;
    }
    if (std::find(allowed_files.begin(), allowed_files.end(),
                  entry.path().filename().string()) != allowed_files.end()) {
      continue;
    }
    const std::string text = ReadText(entry.path());
    AppendCodeMaskRegexViolations(
        result, entry.path(), text, pattern,
        "workspace code must not run synchronous subprocesses; use ProjectBackgroundExecutor");
  }
  return result;
}


RuleResult CheckOverlayDismissalIsCentralized(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "overlay dismissal must not bare-assign overlay.visible = false";
  result.hard_fail = true;
  // Hiding an overlay without resetting keyboard focus strands input on the dead
  // surface ("dead input" bugs). Dismissal must go through WorkspaceShell::DismissOverlay
  // or the focus-safe HideOverlay() helper. WorkspacePersistenceCoordinatorSession.cpp is
  // exempt: it reinitializes the entire project state on restore (no focus to strand).
  const std::regex pattern(R"(overlay\.visible\s*=\s*false)");
  const std::array<std::string_view, 2> allowed_files = {
      "WorkspaceShellOverlay.cpp",
      "WorkspacePersistenceCoordinatorSession.cpp",
  };
  for (const auto& entry :
       std::filesystem::recursive_directory_iterator(repo_root / "src/workspace")) {
    if (!entry.is_regular_file() || entry.path().extension() != ".cpp") {
      continue;
    }
    const std::string filename = entry.path().filename().string();
    if (std::find(allowed_files.begin(), allowed_files.end(), filename) != allowed_files.end()) {
      continue;
    }
    const std::string text = ReadText(entry.path());
    AppendCodeMaskRegexViolations(
        result, entry.path(), text, pattern,
        "hide overlays via WorkspaceShell::DismissOverlay or HideOverlay(state), not a bare "
        "overlay.visible = false (which strands keyboard focus)");
  }
  return result;
}

RuleResult CheckNoDirectGitRepositoryInWorkspace(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "direct GitRepository construction in workspace";
  result.hard_fail = true;
  const std::regex pattern(R"(\bproject::GitRepository\s*\(|\bGitRepository\s*\()");
  const std::array<std::string_view, 1> allowed_files = {
      "GitRepositoryService.cpp",
  };
  for (const auto& entry :
       std::filesystem::recursive_directory_iterator(repo_root / "src/workspace")) {
    if (!entry.is_regular_file() || entry.path().extension() != ".cpp") {
      continue;
    }
    const std::string filename = entry.path().filename().string();
    if (std::find(allowed_files.begin(), allowed_files.end(), filename) != allowed_files.end()) {
      continue;
    }
    const std::string text = ReadText(entry.path());
    AppendCodeMaskRegexViolations(
        result, entry.path(), text, pattern,
        "workspace code must use GitRepositoryService instead of constructing GitRepository");
  }
  return result;
}

}  // namespace microide::tests::architecture
