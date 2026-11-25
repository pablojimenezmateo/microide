#include "architecture/WorkspaceViewModelArchitectureRules.h"

#include "architecture/ArchitectureFileScanner.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <map>
#include <fstream>
#include <regex>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace microide::tests::architecture {

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

RuleResult CheckCompareRenderStructuralGate(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "compare/merge render structural gate";
  result.hard_fail = true;
  const std::regex active_compare_pattern(R"(\bActiveTabIsCompare\s*\()");
  const std::regex direct_state_pattern(R"(context_\.current_project_state)");

  for (const auto& entry : std::filesystem::recursive_directory_iterator(repo_root / "src/workspace")) {
    if (!entry.is_regular_file() || entry.path().extension() != ".cpp") {
      continue;
    }
    const std::string name = entry.path().filename().string();
    const bool is_compare_render = name.starts_with("WorkspaceShellRenderCompare");
    const bool is_merge_render = name.starts_with("WorkspaceShellRenderMerge");
    if (!is_compare_render && !is_merge_render) {
      continue;
    }
    const std::string text = ReadText(entry.path());
    AppendViolations(result, entry.path(), text, direct_state_pattern,
                     "compare/merge render translation units must not read context_.current_project_state");
    if (is_compare_render) {
      AppendViolations(result, entry.path(), text, active_compare_pattern,
                       "compare render translation units must consume structural view-model gates, "
                       "not ActiveTabIsCompare()");
    }
  }
  return result;
}

RuleResult CheckBuildEditorViewModelUsesIncrementalVectorWrites(
     const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "BuildEditorViewModel clears and appends view-model vectors";
  result.hard_fail = true;
  const std::filesystem::path path = repo_root / "src/workspace/render/RenderViewModelBuilder.cpp";
  const std::string text = ReadRuleTarget(result, path);
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

RuleResult CheckSidebarSurfaceFallbackUsesStringView(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "SidebarSurfaceViewModel fallback fields stay as std::string_view";
  result.hard_fail = true;
  const std::filesystem::path path = repo_root / "src/workspace/render/RenderViewModelBuilder.h";
  if (!RequireRuleTarget(result, path)) {
    return result;
  }
  const std::string text = ReadText(path);
  const std::regex query_pattern(R"(std::string_view\s+query_fallback_text\b)");
  const std::regex replace_pattern(R"(std::string_view\s+replace_fallback_text\b)");
  if (!std::regex_search(text, query_pattern)) {
    result.violations.push_back(Violation{
        .path = path,
        .line = 1,
        .message = "SidebarSurfaceViewModel::query_fallback_text must be std::string_view "
                   "to keep BuildSidebarSurface allocation-free per frame",
    });
  }
  if (!std::regex_search(text, replace_pattern)) {
    result.violations.push_back(Violation{
        .path = path,
        .line = 1,
        .message = "SidebarSurfaceViewModel::replace_fallback_text must be std::string_view "
                   "to keep BuildSidebarSurface allocation-free per frame",
    });
  }
  return result;
}

RuleResult CheckMenuItemTextResolutionIsAllocationFree(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "menu label/accelerator resolution returns borrowed views";
  result.hard_fail = true;
  const std::filesystem::path members = repo_root / "src/workspace/shell/WorkspaceShellMembers.inc";
  if (!RequireRuleTarget(result, members)) {
    return result;
  }
  const std::string text = ReadText(members);
  for (const std::string_view name : {"MenuItemLabel", "MenuItemAccelerator"}) {
    const std::regex view_pattern(R"(std::string_view\s+)" + std::string(name) +
                                  R"(\s*\(\s*const\s+MenuItemSpec)");
    if (std::regex_search(text, view_pattern)) {
      continue;
    }
    result.violations.push_back(Violation{
        .path = members,
        .line = 1,
        .message = "WorkspaceShell::" + std::string(name) +
                   " must return std::string_view: it is called once per visible menu row "
                   "per frame, in both the popup layout and the paint path",
    });
  }
  return result;
}

RuleResult CheckRenderViewModelsOwnProjectState(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "render view models own their state (no OverlayState/ProjectWorkspaceState)";
  result.hard_fail = true;

  const std::filesystem::path header = repo_root / "src/workspace/render/RenderViewModelBuilder.h";
  if (std::filesystem::exists(header)) {
    const std::string text = ReadRuleTarget(result, header);
    AppendCodeMaskRegexViolations(
        result, header, text, std::regex(R"(\bOverlayState\b)"),
        "render view models must not reference OverlayState (owned overlay model only)");
    const std::regex struct_decl(R"(\bstruct\s+([A-Za-z_][A-Za-z0-9_]*)\b[^;{]*\{)");
    std::vector<std::pair<std::size_t, std::size_t>> allowed_ranges;
    for (std::sregex_iterator it(text.begin(), text.end(), struct_decl), end; it != end; ++it) {
      const std::string name = (*it)[1].str();
      if (name != "FrameSurfaceViewModel" && name != "SidebarSurfaceViewModel") {
        continue;
      }
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
      if (depth == 0) {
        allowed_ranges.emplace_back(body_start, cursor);
      }
    }
    const std::regex state_token(R"(\bProjectWorkspaceState\b)");
    const std::vector<bool> is_code = BuildCodeMask(text);
    for (std::sregex_iterator it(text.begin(), text.end(), state_token), end; it != end; ++it) {
      const std::size_t pos = static_cast<std::size_t>(it->position());
      if (pos < is_code.size() && !is_code[pos]) {
        continue;
      }
      bool allowed = false;
      for (const auto& [range_start, range_end] : allowed_ranges) {
        if (pos >= range_start && pos < range_end) {
          allowed = true;
          break;
        }
      }
      if (!allowed) {
        result.violations.push_back(Violation{
            .path = header,
            .line = LineNumberAt(text, pos),
            .message = "ProjectWorkspaceState may appear only inside FrameSurfaceViewModel / "
                       "SidebarSurfaceViewModel (documented escape hatches); new view models "
                       "must carry owned/precomputed data or narrow typed pointers",
        });
      }
    }
  }

  const std::array<std::string_view, 6> converted_tus = {
      "src/workspace/render/WorkspaceShellRenderOverlay.cpp",
      "src/workspace/render/WorkspaceShellRenderBottomPanel.cpp",
      "src/workspace/render/WorkspaceShellRenderTextInput.cpp",
      "src/workspace/render/WorkspaceShellHoverPopup.cpp",
      "src/workspace/render/WorkspaceShellHoverTargets.cpp",
      "src/workspace/render/DebugPaneRender.cpp",
  };
  const std::regex broad_state(R"(\b(ProjectWorkspaceState|OverlayState)\b)");
  for (const std::string_view relative : converted_tus) {
    const std::filesystem::path path = repo_root / relative;
    if (!RequireRuleTarget(result, path)) {
      continue;
    }
    const std::string text = ReadText(path);
    AppendCodeMaskRegexViolations(
        result, path, text, broad_state,
        "converted render TUs consume owned view models and must not name "
        "ProjectWorkspaceState/OverlayState");
  }
  return result;
}

RuleResult CheckEditorViewModelStickyAndOccurrenceAreSpans(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "EditorViewModel sticky/occurrence fields stay as std::span";
  result.hard_fail = true;
  const std::filesystem::path path = repo_root / "src/editor/EditorViewModel.h";
  if (!RequireRuleTarget(result, path)) {
    return result;
  }
  const std::string text = ReadText(path);
  const std::regex sticky_pattern(R"(std::span<\s*const\s+std::size_t\s*>\s+sticky_lines\b)");
  const std::regex occ_pattern(R"(std::span<\s*const\s+OccurrenceRange\s*>\s+occurrence_ranges\b)");
  if (!std::regex_search(text, sticky_pattern)) {
    result.violations.push_back(Violation{
        .path = path,
        .line = 1,
        .message = "EditorViewModel::sticky_lines must be std::span<const std::size_t> "
                   "(view into RenderViewModelBuilder cache)",
    });
  }
  if (!std::regex_search(text, occ_pattern)) {
    result.violations.push_back(Violation{
        .path = path,
        .line = 1,
        .message = "EditorViewModel::occurrence_ranges must be std::span<const OccurrenceRange> "
                   "(view into RenderViewModelBuilder cache)",
    });
  }
  return result;
}

template <typename Visit>
void ForEachStringLiteral(const std::string& text, Visit&& visit) {
  enum class Lex { Code, LineComment, BlockComment, String, Char };
  Lex state = Lex::Code;
  std::size_t literal_start = 0;
  std::string literal;
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    const char n = (i + 1 < text.size()) ? text[i + 1] : '\0';
    switch (state) {
      case Lex::Code:
        if (c == '/' && n == '/') { state = Lex::LineComment; ++i; }
        else if (c == '/' && n == '*') { state = Lex::BlockComment; ++i; }
        else if (c == '"') { state = Lex::String; literal_start = i; literal.clear(); }
        else if (c == '\'') { state = Lex::Char; }
        break;
      case Lex::LineComment:
        if (c == '\n') { state = Lex::Code; }
        break;
      case Lex::BlockComment:
        if (c == '*' && n == '/') { state = Lex::Code; ++i; }
        break;
      case Lex::String:
        if (c == '\\' && i + 1 < text.size()) { literal.push_back(c); literal.push_back(n); ++i; }
        else if (c == '"') { state = Lex::Code; visit(std::string_view(literal), literal_start); }
        else if (c == '\n') { state = Lex::Code; }
        else { literal.push_back(c); }
        break;
      case Lex::Char:
        if (c == '\\' && i + 1 < text.size()) { ++i; }
        else if (c == '\'' || c == '\n') { state = Lex::Code; }
        break;
    }
  }
}

RuleResult CheckHintSegmentsUseTheSharedSeparator(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "key-hint lists use the shared separator";
  result.hard_fail = true;

  const std::filesystem::path shared = repo_root / "src/workspace/WorkspaceUiText.h";
  const std::string shared_text = ReadRuleTarget(result, shared);
  if (shared_text.find("kHintSeparator") == std::string::npos ||
      shared_text.find("void AppendHintSegment(") == std::string::npos) {
    result.violations.push_back(Violation{
        .path = shared,
        .line = 1,
        .message = "rule target moved — WorkspaceUiText.h no longer defines kHintSeparator and "
                   "AppendHintSegment; re-anchor CheckHintSegmentsUseTheSharedSeparator",
    });
    return result;
  }

  constexpr std::string_view kPipeSeparator = "  |  ";
  const auto scan_composed_pipe_literals = [&](const std::filesystem::path& path,
                                               const std::string& text) {
    ForEachStringLiteral(text, [&](std::string_view literal, std::size_t literal_start) {
      if (literal != kPipeSeparator && literal.find(kPipeSeparator) != std::string_view::npos) {
        result.violations.push_back(Violation{
            .path = path,
            .line = LineNumberAt(text, literal_start),
            .message = "this literal joins fields on \"  |  \" inline — a composed "
                       "key-hint line uses kHintSeparator (\" · \"); write the separator "
                       "as a bare literal if these really are unrelated fields",
        });
      }
    });
  };

  const std::regex redefinition(R"(\bvoid\s+AppendHintSegment\s*\()");
  for (const auto& entry : std::filesystem::recursive_directory_iterator(repo_root / "src")) {
    if (!entry.is_regular_file()) {
      continue;
    }
    const std::filesystem::path extension = entry.path().extension();
    if (extension != ".cpp" && extension != ".h" && extension != ".inc") {
      continue;
    }
    if (entry.path().filename() == "WorkspaceUiText.h") {
      continue;
    }
    const std::string text = ReadText(entry.path());
    const std::vector<bool> is_code = BuildCodeMask(text);
    for (std::sregex_iterator it(text.begin(), text.end(), redefinition), last; it != last; ++it) {
      const auto offset = static_cast<std::size_t>(it->position());
      if (offset >= is_code.size() || !is_code[offset]) {
        continue;
      }
      result.violations.push_back(Violation{
          .path = entry.path(),
          .line = LineNumberAt(text, offset),
          .message = "AppendHintSegment is redefined here — use the shared one in "
                     "WorkspaceUiText.h so every key-hint list joins on the same separator",
      });
    }
    scan_composed_pipe_literals(entry.path(), text);
  }
  return result;
}

RuleResult CheckSideNamesAreLocalAndRemote(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "conflict, merge and compare sides are named Local and Remote";
  result.hard_fail = true;

  const std::filesystem::path anchor = repo_root / "src/workspace/services/RemoteHostService.cpp";
  const std::string anchor_text = ReadRuleTarget(result, anchor);
  if (anchor_text.find("\"Keep Local\"") == std::string::npos ||
      anchor_text.find("\"Take Remote\"") == std::string::npos) {
    result.violations.push_back(Violation{
        .path = anchor,
        .line = 1,
        .message = "rule target moved — RemoteHostService.cpp no longer labels its conflict "
                   "buttons \"Keep Local\" / \"Take Remote\"; re-anchor "
                   "CheckSideNamesAreLocalAndRemote",
    });
    return result;
  }

  constexpr std::size_t kMaxLabelLiteral = 400;
  const std::regex verb_side(
      R"(\b(Keep|Take|Accept|Copy|Use)( All)? (Mine|Yours|Ours|Theirs|Current|Incoming|Host's)\b)");
  const std::regex paren_side(R"(\((mine|yours|ours|theirs|your changes|your version)\))", std::regex::icase);
  const std::regex caption_side(R"(^(Mine|Yours|Ours|Theirs|Current|Incoming)$)");
  std::size_t scanned = 0;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(repo_root / "src")) {
    if (!entry.is_regular_file()) {
      continue;
    }
    const std::filesystem::path extension = entry.path().extension();
    if (extension != ".cpp" && extension != ".h" && extension != ".inc") {
      continue;
    }
    const std::string text = ReadText(entry.path());
    ++scanned;
    ForEachStringLiteral(text, [&](std::string_view literal, std::size_t literal_start) {
      if (literal.size() > kMaxLabelLiteral) {
        return;
      }
      const std::string body(literal);
      if (std::regex_search(body, verb_side) || std::regex_search(body, paren_side) ||
          std::regex_search(body, caption_side)) {
        result.violations.push_back(Violation{
            .path = entry.path(),
            .line = LineNumberAt(text, literal_start),
            .message = "\"" + body + "\" names a side mine/yours/ours/theirs/current/"
                       "incoming/host's — the two sides are Local and Remote everywhere",
        });
      }
    });
  }
  if (scanned == 0) {
    result.missing_targets.push_back(
        Violation{.path = repo_root / "src", .line = 1, .message = "no source under src/ was scanned"});
  }
  return result;
}

RuleResult CheckEveryPerfCounterHasAProducer(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "every perf counter is incremented somewhere in src/";
  result.hard_fail = true;

  const std::filesystem::path header = repo_root / "src/util/PerformanceCounters.h";
  const std::string header_text = ReadRuleTarget(result, header);
  if (header_text.find("#define MICROIDE_PERF_COUNTERS(X)") == std::string::npos) {
    result.violations.push_back(Violation{
        .path = header,
        .line = 1,
        .message = "rule target moved -- PerformanceCounters.h no longer declares its ids through "
                   "the MICROIDE_PERF_COUNTERS X-macro; re-anchor "
                   "CheckEveryPerfCounterHasAProducer",
    });
    return result;
  }

  std::vector<std::pair<std::string, std::size_t>> declared;
  const std::regex declaration(R"(\bX\(\s*(\w+)\s*,)");
  for (std::sregex_iterator it(header_text.begin(), header_text.end(), declaration), last;
       it != last; ++it) {
    declared.emplace_back((*it)[1].str(), LineNumberAt(header_text, static_cast<std::size_t>(it->position())));
  }
  if (declared.empty()) {
    result.violations.push_back(Violation{
        .path = header,
        .line = 1,
        .message = "rule found no counter declarations to check -- the X-macro spelling changed "
                   "and this rule is scanning nothing",
    });
    return result;
  }

  std::set<std::string> produced;
  const std::regex use(R"(\bPerfCounterId::(\w+))");
  for (const auto& entry : std::filesystem::recursive_directory_iterator(repo_root / "src")) {
    if (!entry.is_regular_file()) {
      continue;
    }
    const std::filesystem::path extension = entry.path().extension();
    if (extension != ".cpp" && extension != ".h" && extension != ".inc") {
      continue;
    }
    if (entry.path().filename() == "PerformanceCounters.h" ||
        entry.path().filename() == "PerformanceCounters.cpp") {
      continue;
    }
    const std::string text = ReadText(entry.path());
    for (std::sregex_iterator it(text.begin(), text.end(), use), last; it != last; ++it) {
      produced.insert((*it)[1].str());
    }
  }

  for (const auto& [id, line] : declared) {
    if (id == "Count") {
      continue;
    }
    if (produced.count(id) == 0) {
      result.violations.push_back(Violation{
          .path = header,
          .line = line,
          .message = "this perf counter is never incremented in src/ -- it will read zero forever "
                     "and its absence from a dump reads as \"that code did not run\". Wire it up "
                     "or delete it",
      });
    }
  }
  return result;
}

RuleResult CheckViewportFiletypeGoesThroughTheViewportMemo(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "buffer-content filetype detection goes through TextViewport::language_id()";
  result.hard_fail = true;

  const std::filesystem::path memo = repo_root / "src/editor/TextViewport.cpp";
  if (RequireRuleTarget(result, memo)) {
    const std::string memo_text = ReadText(memo);
    if (memo_text.find("DetectFiletype") == std::string::npos) {
      result.violations.push_back(Violation{
          .path = memo,
          .line = 1,
          .message = "rule target moved -- TextViewport.cpp no longer calls DetectFiletype, so "
                     "the memo this rule funnels every caller into is gone; re-anchor "
                     "CheckViewportFiletypeGoesThroughTheViewportMemo",
      });
      return result;
    }
  }

  const std::regex call(R"(\bDetectFiletype\s*\()");
  std::size_t scanned_calls = 0;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(repo_root / "src")) {
    if (!entry.is_regular_file()) {
      continue;
    }
    const std::filesystem::path extension = entry.path().extension();
    if (extension != ".cpp" && extension != ".h" && extension != ".inc") {
      continue;
    }
    const std::filesystem::path filename = entry.path().filename();
    if (filename == "RuntimeSyntaxRegistry.h" || filename == "RuntimeSyntaxRegistry.cpp" ||
        filename == "TextViewport.cpp") {
      continue;
    }
    const std::string text = ReadText(entry.path());
    const std::vector<bool> is_code = BuildCodeMask(text);
    for (std::sregex_iterator it(text.begin(), text.end(), call), last; it != last; ++it) {
      const std::size_t open_paren = static_cast<std::size_t>(it->position() + it->length()) - 1;
      if (open_paren >= is_code.size() || !is_code[open_paren]) {
        continue;
      }
      ++scanned_calls;
      std::size_t depth = 0;
      bool has_top_level_comma = false;
      for (std::size_t i = open_paren; i < text.size(); ++i) {
        if (i < is_code.size() && !is_code[i]) {
          continue;
        }
        const char ch = text[i];
        if (ch == '(') {
          ++depth;
        } else if (ch == ')') {
          if (--depth == 0) {
            break;
          }
        } else if (ch == ',' && depth == 1) {
          has_top_level_comma = true;
        }
      }
      if (!has_top_level_comma) {
        continue;
      }
      result.violations.push_back(Violation{
          .path = entry.path(),
          .line = LineNumberAt(text, static_cast<std::size_t>(it->position())),
          .message = "content-reading DetectFiletype(path, lines) outside the viewport memo: use "
                     "TextViewport::language_id(), which memoizes on document identity, content "
                     "revision, path and registry revision. Four separate caches for this already "
                     "accreted once; one of them silently served a stale language",
      });
    }
  }
  if (scanned_calls == 0) {
    result.violations.push_back(Violation{
        .path = repo_root / "src",
        .line = 1,
        .message = "rule found no DetectFiletype call sites at all -- the spelling changed and "
                   "this rule is scanning nothing",
    });
  }
  return result;
}

}
