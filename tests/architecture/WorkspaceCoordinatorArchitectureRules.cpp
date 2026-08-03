#include "architecture/WorkspaceCoordinatorArchitectureRules.h"

#include "architecture/ArchitectureFileScanner.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <map>
#include <utility>
#include <vector>
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
  const std::filesystem::path target_path = repo_root / "src/workspace/coordinators/WorkspaceTabCoordinator.cpp";
  if (!RequireRuleTarget(result, target_path)) {
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
  if (!RequireRuleTarget(result, header)) {
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
    if (!RequireRuleTarget(result, root_dir)) {
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
  const std::filesystem::path path = repo_root / "src/workspace/services/ProjectCatalogService.cpp";
  if (!std::filesystem::exists(path)) {
    result.violations.push_back(Violation{
        .path = path,
        .line = 1,
        .message = "reactivation rule target moved — re-anchor to the TU hosting the "
                   "project reactivation flow",
    });
    return result;
  }
  const std::string text = ReadRuleTarget(result, path);
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
    if (!RequireRuleTarget(result, root_dir)) {
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
  // Both construction spellings. The original pattern was only
  // `GitRepository\s*\(`, which matches a *temporary* (`GitRepository(root)`) —
  // a form nobody writes. Every real construction in the tree is the named
  // declaration `GitRepository repo(root);` / `const project::GitRepository
  // repo(root);`, so the rule could not fire on the very shape it exists to ban.
  // `\b` after the type name keeps `GitRepositoryService` / `GitRepositoryState`
  // / `GitRepository::Foo` out, and requiring an identifier before the `(`/`{`
  // keeps reference and pointer parameters (`GitRepository& repo`) out.
  const std::regex pattern(
      R"(\b(?:project::)?GitRepository\b\s*(?:[A-Za-z_]\w*\s*[({]|\())");
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

  const std::string header_text = ReadRuleTarget(result, header);
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

  const std::string source_text = ReadRuleTarget(result, source);
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

// Every std::function field of a coordinator's `Operations` struct must have a
// caller.
//
// These structs are how a coordinator declares what it needs the shell to do,
// and the shell fills every field in with a working lambda. That makes an unused
// field invisible: it compiles, it is wired, it reads like part of the contract,
// and it does nothing. One sweep found 35 of them — PanelMouseCoordinator kept
// ten debug callbacks after the debug pane moved to its own coordinator;
// KeyInputCoordinator kept nineteen that lost to `dispatch_git_sidebar_action`,
// to AssistService, or to a live copy on another struct. Two functions existed
// only to be bound into one (`DestroyLifecycleCursors`, 58 lines of cursor
// teardown that never ran; `DapManager::HasRegisteredAdapters`).
//
// Reads are scoped by include graph, not by name. Six of those 35 were invisible
// to a name-only search because a *different* struct has a field spelled the
// same and that one is called — `activate_tab`, `reset_caret_blink`,
// `read_primary_selection_text` and friends are each live somewhere and dead
// here. So a field counts as called only if the read appears in a file that
// transitively includes the header declaring it.
//
// A "call" is any `.field` / `->field` that is not the `.field =` of a
// designated initializer. That is deliberately loose: taking the field's
// address, testing it for null, or forwarding it all count, because all three
// mean a human still cares about it.
RuleResult CheckCoordinatorOperationsAreCalled(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "every coordinator Operations field has a caller";
  result.hard_fail = true;

  const std::filesystem::path src_dir = repo_root / "src";

  // Read the tree once, keyed by the "src/"-relative path that #include uses.
  std::map<std::string, std::filesystem::path> by_include;
  std::map<std::string, std::string> text_of;
  std::map<std::string, std::vector<bool>> code_of;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(src_dir)) {
    if (!entry.is_regular_file()) {
      continue;
    }
    const std::string ext = entry.path().extension().string();
    if (ext != ".cpp" && ext != ".h" && ext != ".inc") {
      continue;
    }
    const std::string key = std::filesystem::relative(entry.path(), src_dir).generic_string();
    by_include[key] = entry.path();
    text_of[key] = ReadText(entry.path());
    code_of[key] = BuildCodeMask(text_of[key]);
  }
  if (text_of.size() < 100) {
    result.violations.push_back(Violation{
        .path = src_dir,
        .line = 1,
        .message = "scanned only " + std::to_string(text_of.size()) +
                   " source files; this lint's traversal broke and it has gone vacuous",
    });
    return result;
  }

  // Direct include edges, then a transitive closure of "who can see this header".
  std::map<std::string, std::set<std::string>> includes;
  const std::regex include_pattern(R"RX(#\s*include\s+"([^"]+)")RX");
  for (const auto& [key, text] : text_of) {
    for (std::sregex_iterator it(text.begin(), text.end(), include_pattern), last; it != last;
         ++it) {
      const std::string target = it->str(1);
      if (by_include.count(target) != 0) {
        includes[key].insert(target);
      }
    }
  }
  const auto includers_of = [&](const std::string& header) {
    std::set<std::string> seen{header};
    bool grew = true;
    while (grew) {
      grew = false;
      for (const auto& [key, deps] : includes) {
        if (seen.count(key) != 0) {
          continue;
        }
        for (const std::string& dep : deps) {
          if (seen.count(dep) != 0) {
            seen.insert(key);
            grew = true;
            break;
          }
        }
      }
    }
    return seen;
  };

  const std::regex field_pattern(R"(std::function\s*<.*>\s+([a-z_][a-z0-9_]*)\s*;)");
  std::size_t structs_seen = 0;
  std::size_t fields_seen = 0;
  for (const auto& [key, text] : text_of) {
    if (key.rfind("workspace/", 0) != 0 || !key.ends_with(".h") ||
        text.find("struct Operations") == std::string::npos) {
      continue;
    }
    const std::vector<bool>& is_code = code_of.at(key);

    // Fields declared inside the `struct Operations { ... }` block.
    std::vector<std::pair<std::string, std::size_t>> fields;
    {
      std::size_t offset = 0;
      std::size_t line = 1;
      bool inside = false;
      int depth = 0;
      std::size_t cursor = 0;
      while (cursor <= text.size()) {
        const std::size_t newline = text.find('\n', cursor);
        const std::string raw = text.substr(
            cursor, newline == std::string::npos ? std::string::npos : newline - cursor);
        const bool code_line = offset < is_code.size() && is_code[offset];
        if (!inside && code_line && raw.find("struct Operations") != std::string::npos) {
          inside = true;
          depth = 0;
          ++structs_seen;
        }
        if (inside) {
          for (const char ch : raw) {
            if (ch == '{') {
              ++depth;
            } else if (ch == '}') {
              --depth;
            }
          }
          std::smatch match;
          if (code_line && std::regex_search(raw, match, field_pattern)) {
            fields.emplace_back(match.str(1), line);
            ++fields_seen;
          }
          if (depth <= 0 && raw.find('}') != std::string::npos) {
            inside = false;
          }
        }
        offset += raw.size() + 1;
        ++line;
        cursor = newline == std::string::npos ? text.size() + 1 : newline + 1;
      }
    }
    if (fields.empty()) {
      continue;
    }

    const std::set<std::string> scope = includers_of(key);
    for (const auto& [name, line] : fields) {
      const std::regex use_pattern(R"((?:\.|->)\s*)" + name + R"(\b)");
      bool called = false;
      for (const std::string& reader : scope) {
        const std::string& body = text_of.at(reader);
        const std::vector<bool>& reader_code = code_of.at(reader);
        for (std::sregex_iterator it(body.begin(), body.end(), use_pattern), last;
             it != last && !called; ++it) {
          const std::size_t at = static_cast<std::size_t>(it->position());
          if (at < reader_code.size() && !reader_code[at]) {
            continue;
          }
          std::size_t after = static_cast<std::size_t>(it->position() + it->length());
          while (after < body.size() && std::isspace(static_cast<unsigned char>(body[after])) != 0) {
            ++after;
          }
          // `.field =` is the designated initializer that wires it up, not a call.
          if (after < body.size() && body[after] == '=' &&
              (after + 1 >= body.size() || body[after + 1] != '=')) {
            continue;
          }
          called = true;
        }
        if (called) {
          break;
        }
      }
      if (called) {
        continue;
      }
      result.violations.push_back(Violation{
          .path = by_include.at(key),
          .line = line,
          .message = "Operations field `" + name +
                     "` is wired up but never called by anything that includes this header. "
                     "Either call it, or delete the field and its initializer — a hook nobody "
                     "invokes reads as part of the contract while doing nothing",
      });
    }
  }
  if (structs_seen < 20 || fields_seen < 200) {
    result.violations.push_back(Violation{
        .path = repo_root / "src/workspace",
        .line = 1,
        .message = "found only " + std::to_string(structs_seen) + " Operations structs and " +
                   std::to_string(fields_seen) +
                   " std::function fields; the declaration shape changed and this lint has gone "
                   "vacuous — fix the scan rather than deleting the rule",
    });
  }
  return result;
}

}  // namespace microide::tests::architecture
