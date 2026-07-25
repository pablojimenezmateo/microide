#include "architecture/WorkspaceCoordinatorArchitectureRules.h"

#include "architecture/ArchitectureFileScanner.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <string>
#include <string_view>

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
  // Also catch the injected `notify_lsp_buffer_open` callback: it used to bypass this
  // lint (the operation is snake_case while the pattern only matched the PascalCase
  // shell method), so activation could still synchronously hydrate the server through
  // it (TD-2026-07-17A-033). The async form is `schedule_lsp_buffer_open`, which is
  // deliberately NOT matched.
  const std::regex sync_lsp_dispatch_pattern(
      R"(\b(DidOpen|DidChange|DidChangeIncremental|EnsureLspDocumentOpen|NotifyLspBufferOpen|notify_lsp_buffer_open)\s*\()");
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

RuleResult CheckReactivationDoesNotReloadPlugins(const std::filesystem::path& repo_root) {
  // AGENTS.md policy invariant, now a narrow lint (TD-2026-07-17-037): project
  // reactivation refresh uses the refresh_plugin_surfaces_for_reactivation seam
  // and must never reload plugins (a reload tears down and re-runs every plugin
  // on a plain project-tab switch). The reactivation path lives in
  // ProjectCatalogService; a missing target file or a missing refresh-seam
  // reference is a violation so a future move re-anchors the rule instead of
  // letting it pass vacuously.
  RuleResult result;
  result.label = "project reactivation must refresh, not reload, plugins";
  result.hard_fail = true;
  const std::filesystem::path path = repo_root / "src/workspace/ProjectCatalogService.cpp";
  if (!std::filesystem::exists(path)) {
    result.violations.push_back(Violation{
        .path = path,
        .line = 1,
        .message = "reactivation rule target moved — re-anchor to the TU hosting the "
                   "project reactivation flow",
    });
    return result;
  }
  const std::string text = ReadText(path);
  AppendCodeMaskRegexViolations(
      result, path, text,
      std::regex(R"(\b(?:ReloadPluginsForCurrentProject|reload_plugins_for_current_project)\b)"),
      "project reactivation must not reload plugins; use the "
      "refresh_plugin_surfaces_for_reactivation seam");
  if (text.find("refresh_plugin_surfaces_for_reactivation") == std::string::npos) {
    result.violations.push_back(Violation{
        .path = path,
        .line = 1,
        .message = "reactivation refresh seam not found — if the flow moved, re-anchor this "
                   "rule to the new TU",
    });
  }
  return result;
}

RuleResult CheckNoFallbackEditorViewportSymbols(const std::filesystem::path& repo_root) {
  // AGENTS.md policy invariant, now a narrow lint (TD-2026-07-17-037): the
  // shell/project-level fallback editor viewport was deleted in the 2026-04-29
  // cleanup — the active viewport resolves through EditorTabService::
  // ActiveViewport(). Ban the deleted member spelling the same way the legacy
  // persistence symbols are banned, so a same-name revival fails immediately.
  // (A different-name revival stays reviewer-enforced; this is the ratchet for
  // the explicit symbol the invariant names.)
  RuleResult result;
  result.label = "no fallback editor viewport symbols";
  result.hard_fail = true;
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
      for (const std::size_t pos : FindCodeLiteralOccurrences(text, "text_viewport_")) {
        result.violations.push_back(Violation{
            .path = entry.path(),
            .line = LineNumberAt(text, pos),
            .message = "the fallback editor viewport member was deleted intentionally; resolve "
                       "the active viewport through EditorTabService::ActiveViewport()",
        });
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

// TextViewport hand-writes its copy constructor, move constructor and move
// assignment, each enumerating every one of its ~40 members. Adding a member to
// the header and forgetting one of those three lists compiles cleanly and
// silently drops that member's state on any copy/move of a viewport — the shape
// of bug that surfaces much later as "my folds/undo/highlight cache reset when I
// split the editor". The lists cannot be replaced with `= default` because the
// copy deliberately nulls folding_model_ and re-invalidates the visual-column
// cache, so this lint keeps them honest instead: every member declared in the
// private section must appear in all three.
RuleResult CheckTextViewportSpecialMembersCoverEveryField(
    const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "TextViewport copy/move cover every member";
  result.hard_fail = true;
  const std::filesystem::path header = repo_root / "src/editor/TextViewport.h";
  const std::filesystem::path source = repo_root / "src/editor/TextViewport.cpp";
  if (!std::filesystem::exists(header) || !std::filesystem::exists(source)) {
    result.violations.push_back(Violation{
        .path = header,
        .line = 1,
        .message = "TextViewport.h/.cpp not found; this lint's target moved and it is now "
                   "vacuous — repoint it or delete it",
    });
    return result;
  }

  const std::string header_text = ReadText(header);
  const std::vector<bool> header_code = BuildCodeMask(header_text);
  // Members are the trailing-underscore identifiers declared in the private
  // section: `<type> name_ = init;` / `<type> name_;`. Restrict the scan to the
  // private section so trailing-underscore uses inside inline accessors above it
  // (`return cursor_line_;`) are not mistaken for declarations.
  const std::size_t private_at = header_text.find("\n private:");
  if (private_at == std::string::npos) {
    result.violations.push_back(Violation{
        .path = header,
        .line = 1,
        .message = "TextViewport.h has no ` private:` section; this lint can no longer find "
                   "the member list — repoint it",
    });
    return result;
  }

  std::set<std::string> declared;
  const std::regex member_pattern(R"(^\s{2}(?:mutable\s+)?[A-Za-z_][\w:<>,\s\*&]*?\b(\w+_)\s*(?:=[^;]*)?;\s*$)");
  std::size_t line_start = private_at + 1;
  while (line_start < header_text.size()) {
    std::size_t line_end = header_text.find('\n', line_start);
    if (line_end == std::string::npos) {
      line_end = header_text.size();
    }
    if (header_code.empty() || (line_start < header_code.size() && header_code[line_start])) {
      const std::string line = header_text.substr(line_start, line_end - line_start);
      std::smatch match;
      // Skip member-function declarations (they carry a parameter list).
      if (line.find('(') == std::string::npos && std::regex_match(line, match, member_pattern)) {
        declared.insert(match[1].str());
      }
    }
    line_start = line_end + 1;
  }
  if (declared.size() < 20) {
    result.violations.push_back(Violation{
        .path = header,
        .line = 1,
        .message = "TextViewport member scan found only " + std::to_string(declared.size()) +
                   " members; the declaration shape changed and this lint has gone vacuous — "
                   "fix the scan rather than deleting the rule",
    });
    return result;
  }

  const std::string source_text = ReadText(source);
  struct Region {
    std::string_view label;
    std::string_view signature;
  };
  const std::array<Region, 3> regions = {
      Region{"copy constructor", "TextViewport::TextViewport(const TextViewport& other)"},
      Region{"move constructor", "TextViewport::TextViewport(TextViewport&& other) noexcept"},
      Region{"move assignment",
             "TextViewport& TextViewport::operator=(TextViewport&& other) noexcept"},
  };

  for (const Region& region : regions) {
    const std::size_t begin = source_text.find(region.signature);
    if (begin == std::string::npos) {
      result.violations.push_back(Violation{
          .path = source,
          .line = 1,
          .message = std::string("TextViewport ") + std::string(region.label) +
                     " not found at its expected signature; this lint has gone vacuous — "
                     "repoint it",
      });
      continue;
    }
    // The region runs to the next blank line followed by a top-level definition;
    // searching to the next "\n}\n" is enough because none of the three bodies
    // contains a top-level-column closing brace before its own.
    const std::size_t end = source_text.find("\n}\n", begin);
    const std::string body =
        source_text.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
    for (const std::string& member : declared) {
      if (body.find(member) == std::string::npos) {
        result.violations.push_back(Violation{
            .path = source,
            .line = LineNumberAt(source_text, begin),
            .message = "TextViewport " + std::string(region.label) + " does not mention `" +
                       member +
                       "`; every member must be copied/moved explicitly or its state is "
                       "silently dropped (see the note on this rule)",
        });
      }
    }
  }
  return result;
}

// util::SerializeLinesStreaming exists precisely so a whole-document payload is
// built straight from the TextBuffer's zero-copy LineView, instead of first
// materializing Snapshot() — a vector<std::string> with one heap allocation per
// line — and then joining it. Its own comment already said "use this instead of
// SerializeLines(buffer.Snapshot(), ...)", but that was advisory and eight call
// sites (save, compare review, merge result, LSP sync) had drifted back onto the
// slow path. This makes the advice enforceable.
RuleResult CheckSerializeLinesDoesNotMaterializeSnapshot(
    const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "whole-document serialization streams instead of Snapshot()";
  result.hard_fail = true;
  const std::filesystem::path src_dir = repo_root / "src";
  if (!std::filesystem::exists(src_dir)) {
    result.violations.push_back(Violation{
        .path = src_dir, .line = 1,
        .message = "src/ not found; this lint has gone vacuous — repoint it"});
    return result;
  }

  // `SerializeLines(` ... `Snapshot()` on the same logical call. The call is
  // sometimes wrapped across two lines, so match on the whole file text with the
  // comment mask applied, bounded so it cannot run past the call.
  const std::regex pattern(R"(SerializeLines\([^;]{0,160}?Snapshot\(\))");
  std::size_t scanned = 0;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(src_dir)) {
    if (!entry.is_regular_file()) {
      continue;
    }
    const std::string ext = entry.path().extension().string();
    if (ext != ".cpp" && ext != ".h" && ext != ".inc") {
      continue;
    }
    // The helper's own documentation names the discouraged form.
    if (entry.path().filename() == "StringUtil.h") {
      continue;
    }
    ++scanned;
    const std::string text = ReadText(entry.path());
    if (text.find("Snapshot()") == std::string::npos) {
      continue;
    }
    AppendCodeMaskRegexViolations(
        result, entry.path(), text, pattern,
        "build whole-document text with util::SerializeLinesStreaming("
        "editor::LineSpan(buffer), ending) — SerializeLines(buffer.Snapshot(), ...) "
        "materializes a vector<std::string> with one allocation per line first");
  }
  if (scanned < 100) {
    result.violations.push_back(Violation{
        .path = src_dir, .line = 1,
        .message = "scanned only " + std::to_string(scanned) +
                   " source files; this lint's traversal broke and it has gone vacuous"});
  }
  return result;
}

}  // namespace microide::tests::architecture
