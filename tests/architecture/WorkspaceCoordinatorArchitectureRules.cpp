#include "architecture/WorkspaceCoordinatorArchitectureRules.h"

#include "TestSupport.h"
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

RuleResult CheckLspDidOpenIsNonBlocking(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "LSP didOpen/didChange synchronous send from ActivateTab";
  result.hard_fail = true;
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

RuleResult CheckTextViewportNoCombinedLayoutRevision(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "TextViewport tiered revisions (no combined layout_revision)";
  result.hard_fail = true;
  const std::filesystem::path header = repo_root / "src/editor/TextViewport.h";
  if (!RequireRuleTarget(result, header)) {
    return result;
  }
  const std::string text = ReadText(header);
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

RuleResult CheckNoExecutorPostThenFutureGetInWorkspace(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "workspace executor-post-then-future-get anti-pattern";
  result.hard_fail = true;
  const std::regex pattern(R"(\bget_future\s*\()");
  for (const auto& entry :
       std::filesystem::recursive_directory_iterator(repo_root / "src/workspace")) {
    if (!entry.is_regular_file() || entry.path().extension() != ".cpp") {
      continue;
    }
    const std::string text = ReadText(entry.path());
    AppendCodeMaskRegexViolations(
        result, entry.path(), text, pattern,
        "do not hand work to another thread and wait on its future from the caller — "
        "either run inline or restructure to async completion");
  }
  return result;
}

RuleResult CheckNoSynchronousSubprocessInWorkspace(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "synchronous subprocess call in workspace";
  result.hard_fail = true;
  const std::regex pattern(
      R"(\b(platform|project)::RunSubprocess\s*\(|[Ll]auncher(\(\))?\s*(\.|->)\s*Run\s*\()");
  const std::array<std::string_view, 1> allowed_files = {
      "SaveFormatterService.cpp",
  };
  bool saw_allowlisted_match = false;
  for (const auto& entry :
       std::filesystem::recursive_directory_iterator(repo_root / "src/workspace")) {
    if (!entry.is_regular_file() || entry.path().extension() != ".cpp") {
      continue;
    }
    if (std::find(allowed_files.begin(), allowed_files.end(),
                  entry.path().filename().string()) != allowed_files.end()) {
      if (std::regex_search(ReadText(entry.path()), pattern)) {
        saw_allowlisted_match = true;
      }
      continue;
    }
    const std::string text = ReadText(entry.path());
    AppendCodeMaskRegexViolations(
        result, entry.path(), text, pattern,
        "workspace code must not run synchronous subprocesses; post the work to a "
        "worker (SaveFormatterService, ProjectBackgroundExecutor)");
  }
  if (!saw_allowlisted_match) {
    result.missing_targets.push_back(Violation{
        .path = repo_root / "src/workspace/services/SaveFormatterService.cpp",
        .line = 1,
        .message = "the one allowlisted subprocess spawn is gone from this file; either it "
                   "moved (repoint the allowlist) or the formatter stopped being a "
                   "subprocess — either way this rule is no longer scanning for the call "
                   "form it exists to bound",
    });
  }

  const std::regex run_blocking(R"(\bRunBlocking\s*\()");
  bool saw_run_blocking = false;
  for (const auto& entry :
       std::filesystem::recursive_directory_iterator(repo_root / "src/workspace")) {
    if (!entry.is_regular_file() || entry.path().extension() != ".cpp") {
      continue;
    }
    const std::string filename = entry.path().filename().string();
    const std::string text = ReadText(entry.path());
    if (filename == "SaveFormatterService.cpp" ||
        filename == "WorkspaceTabCoordinatorShellBridge.cpp") {
      if (std::regex_search(text, run_blocking)) {
        saw_run_blocking = true;
      }
      continue;
    }
    AppendCodeMaskRegexViolations(
        result, entry.path(), text, run_blocking,
        "only the save pipeline may wait on a formatter from the shell thread; an "
        "interactive save defers (SaveMode::Deferred) and its completion finishes it");
  }
  if (!saw_run_blocking) {
    result.missing_targets.push_back(Violation{
        .path = repo_root / "src/workspace/coordinators/WorkspaceTabCoordinatorShellBridge.cpp",
        .line = 1,
        .message = "no RunBlocking call remains in the save pipeline; if the blocking save "
                   "path is genuinely gone this half of the rule should be deleted, not "
                   "left scanning for nothing",
    });
  }
  return result;
}


RuleResult CheckOverlayDismissalIsCentralized(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "overlay dismissal must not bare-assign overlay.visible = false";
  result.hard_fail = true;
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

RuleResult CheckEditorGroupsMutateWithTheSplitTree(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "editor group mutations must carry the split tree with them";
  result.hard_fail = true;
  const std::regex mutation_pattern(
      R"(editor_groups\s*\.\s*(?:insert|erase|clear|push_back|emplace_back|emplace|resize|pop_back|assign)\s*\()");
  const std::regex tree_pattern(
      R"(editor_split\s*\.\s*(?:InsertLeaf|RemoveLeaf|Reset|ResetToEvenSplit|Load)\s*\()");
  std::size_t scanned_mutations = 0;
  for (const auto& entry :
       std::filesystem::recursive_directory_iterator(repo_root / "src/workspace")) {
    if (!entry.is_regular_file()) {
      continue;
    }
    const std::filesystem::path extension = entry.path().extension();
    if (extension != ".cpp" && extension != ".inc") {
      continue;
    }
    const std::string text = ReadText(entry.path());
    const std::vector<bool> is_code = BuildCodeMask(text);
    const auto in_code = [&is_code](std::size_t begin, std::size_t end) {
      for (std::size_t i = begin; i < end && i < is_code.size(); ++i) {
        if (!is_code[i]) {
          return false;
        }
      }
      return true;
    };
    bool names_tree = false;
    for (std::sregex_iterator it(text.begin(), text.end(), tree_pattern), last; it != last; ++it) {
      const std::size_t begin = static_cast<std::size_t>(it->position());
      if (in_code(begin, begin + static_cast<std::size_t>(it->length()))) {
        names_tree = true;
        break;
      }
    }
    for (std::sregex_iterator it(text.begin(), text.end(), mutation_pattern), last; it != last;
         ++it) {
      const std::size_t begin = static_cast<std::size_t>(it->position());
      if (!in_code(begin, begin + static_cast<std::size_t>(it->length()))) {
        continue;
      }
      ++scanned_mutations;
      if (names_tree) {
        continue;
      }
      result.violations.push_back(Violation{
          .path = entry.path(),
          .line = LineNumberAt(text, begin),
          .message = "this changes `editor_groups` in a TU that never calls a structural "
                     "`editor_split` method (InsertLeaf/RemoveLeaf/Reset/ResetToEvenSplit/Load) "
                     "— the split tree's leaves ARE the groups, so a group added or dropped here "
                     "leaves the tree disagreeing about how many panes exist, and the extra group "
                     "is silently never drawn. The check is per-TU: pairing each individual "
                     "mutation with its tree call stays a reviewer's job",
      });
    }
  }
  if (scanned_mutations == 0) {
    result.violations.push_back(Violation{
        .path = repo_root / "src/workspace",
        .line = 1,
        .message = "found no editor_groups mutation sites to check; this lint's scan broke and it "
                   "has gone vacuous",
    });
  }
  return result;
}

RuleResult CheckNoDirectGitRepositoryInWorkspace(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "direct GitRepository construction in workspace";
  result.hard_fail = true;
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

RuleResult CheckTextViewportSpecialMembersAreDefaulted(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "TextViewport copy/move are defaulted";
  result.hard_fail = true;
  const std::filesystem::path header = repo_root / "src/editor/TextViewport.h";
  const std::filesystem::path source = repo_root / "src/editor/TextViewport.cpp";
  const std::string header_text = ReadRuleTarget(result, header);
  const std::string source_text = ReadRuleTarget(result, source);
  if (header_text.empty() || source_text.empty()) {
    return result;
  }
  const std::array<std::string_view, 4> declarations = {
      "TextViewport(const TextViewport& other) = default;",
      "TextViewport& operator=(const TextViewport& other) = default;",
      "TextViewport(TextViewport&& other) noexcept = default;",
      "TextViewport& operator=(TextViewport&& other) noexcept = default;",
  };
  for (const std::string_view declaration : declarations) {
    if (header_text.find(declaration) == std::string::npos) {
      result.violations.push_back(Violation{
          .path = header,
          .line = 1,
          .message = "TextViewport.h no longer declares `" + std::string(declaration) +
                     "`; a hand-written special member must list every member and silently "
                     "drops any it misses -- give the member a transfer-policy type in "
                     "TextViewportMemberPolicies.h instead",
      });
    }
  }
  const std::regex definition(
      R"(TextViewport(?:&\s+TextViewport::operator=|::TextViewport)\(\s*(?:const\s+)?TextViewport\s*&&?)");
  AppendCodeMaskRegexViolations(
      result, source, source_text, definition,
      "TextViewport's copy/move are defaulted; an out-of-line definition brings back the "
      "member lists this rule exists to keep out");
  return result;
}

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

RuleResult CheckCoordinatorOperationsAreCalled(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "every coordinator Operations field has a caller";
  result.hard_fail = true;

  const std::filesystem::path src_dir = repo_root / "src";

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

  const auto member_reads = [](const std::string& text, const std::vector<bool>& is_code) {
    std::set<std::string> names;
    const auto is_ident_start = [](unsigned char c) {
      return std::isalpha(c) != 0 || c == '_';
    };
    const auto is_ident = [](unsigned char c) { return std::isalnum(c) != 0 || c == '_'; };
    for (std::size_t i = 0; i < text.size(); ++i) {
      std::size_t after_op = 0;
      if (text[i] == '.') {
        after_op = i + 1;
      } else if (text[i] == '-' && i + 1 < text.size() && text[i + 1] == '>') {
        after_op = i + 2;
      } else {
        continue;
      }
      if (i < is_code.size() && !is_code[i]) {
        continue;
      }
      std::size_t start = after_op;
      while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start])) != 0) {
        ++start;
      }
      if (start >= text.size() || !is_ident_start(static_cast<unsigned char>(text[start]))) {
        continue;
      }
      std::size_t end = start;
      while (end < text.size() && is_ident(static_cast<unsigned char>(text[end]))) {
        ++end;
      }
      std::size_t after = end;
      while (after < text.size() && std::isspace(static_cast<unsigned char>(text[after])) != 0) {
        ++after;
      }
      if (after < text.size() && text[after] == '=' &&
          (after + 1 >= text.size() || text[after + 1] != '=')) {
        continue;
      }
      names.insert(text.substr(start, end - start));
      i = end - 1;
    }
    return names;
  };
  std::map<std::string, std::set<std::string>> reads_of;
  for (const auto& [key, text] : text_of) {
    reads_of.emplace(key, member_reads(text, code_of.at(key)));
  }

  const std::regex field_pattern(R"(std::function\s*<.*>\s+([a-z_][a-z0-9_]*)\s*;)");
  std::size_t structs_seen = 0;
  std::size_t fields_seen = 0;
  for (const auto& [key, text] : text_of) {
    if (key.rfind("workspace/", 0) != 0 || !key.ends_with(".h") ||
        text.find("struct Operations") == std::string::npos) {
      continue;
    }
    const std::vector<bool>& is_code = code_of.at(key);

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
      bool called = false;
      for (const std::string& reader : scope) {
        if (reads_of.at(reader).count(name) != 0) {
          called = true;
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

void RunExecutorPostThenFutureGetRuleFixtures() {
  TemporaryDirectory fixture;
  const std::filesystem::path& root = fixture.path();
  std::filesystem::create_directories(root / "src/workspace");
  WriteFile(root / "src/workspace/Bad.cpp",
            "int F(){ auto p = std::make_shared<std::promise<int>>();\n"
            "  auto f = p->get_future();\n"
            "  executor.Post([p]{ p->set_value(1); });\n"
            "  return f.get(); }\n");
  Expect(CheckNoExecutorPostThenFutureGetInWorkspace(root).violations.size() == 1,
         "the post-then-wait rule must flag a future handed to another thread, whatever the executor");
  WriteFile(root / "src/workspace/Bad.cpp",
            "void F(){ project_background_executor_.Post([]{ Work(); }); }\n"
            "void G(){ Use(pane.get()); }\n"
            "const char* kDoc = \"get_future()\";\n");
  Expect(CheckNoExecutorPostThenFutureGetInWorkspace(root).violations.empty(),
         "the post-then-wait rule must accept a fire-and-forget Post, an unrelated .get(), and string text");
}

}
