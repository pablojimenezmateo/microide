#include "architecture/WorkspaceViewModelArchitectureRules.h"

#include "architecture/ArchitectureFileScanner.h"

#include <array>
#include <filesystem>
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
    // Both compare and merge render TUs must receive project state as injected POD
    // parameters from the frame caller, never reach into context_.current_project_state.
    AppendViolations(result, entry.path(), text, direct_state_pattern,
                     "compare/merge render translation units must not read context_.current_project_state");
    // ActiveTabIsCompare() is the compare-specific active-tab predicate; merge render
    // legitimately resolves its tab through ActiveMergeTab(), so only gate compare here.
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
  // BuildSidebarSurface() is called multiple times per frame. The fallback text fields used to be
  // std::string and allocated on every call. They are now std::string_view so the typical render
  // frame is allocation-free. The header is the load-bearing surface for this rule.
  RuleResult result;
  result.label = "SidebarSurfaceViewModel fallback fields stay as std::string_view";
  result.hard_fail = true;
  const std::filesystem::path path = repo_root / "src/workspace/render/RenderViewModelBuilder.h";
  if (!RequireRuleTarget(result, path)) {
    return result;
  }
  const std::string text = ReadText(path);
  // Require that the two fields are declared as `std::string_view`. A naive grep is enough because
  // the field appears exactly once and never inside a comment.
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
  // A popup menu re-resolves every visible row's label and accelerator on each frame it
  // is open — both while laying the popup out and while painting it. Both resolvers used
  // to return std::string, so an open File menu cost two allocations per row per frame
  // for as long as the pointer hovered it. They return borrowed views now (static
  // registry literals, plus one reused buffer for the composed "(LSP: Starting…)" form).
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
  // TD-2026-07-17-084/26: render view models must be owned/precomputed data, not
  // live pointers into project state, and the converted render TUs must not name
  // the broad state types at all. Two documented escape hatches remain until the
  // editor-surface and sidebar view-model passes land: FrameSurfaceViewModel and
  // SidebarSurfaceViewModel may still carry a ProjectWorkspaceState pointer (and
  // their render TUs, WorkspaceShellRenderFrame.cpp / WorkspaceShellRenderSidebar
  // .cpp, may dereference it). Nothing else may — this rule is the ratchet.
  RuleResult result;
  result.label = "render view models own their state (no OverlayState/ProjectWorkspaceState)";
  result.hard_fail = true;

  const std::filesystem::path header = repo_root / "src/workspace/render/RenderViewModelBuilder.h";
  if (std::filesystem::exists(header)) {
    const std::string text = ReadRuleTarget(result, header);
    // OverlayState must not appear anywhere in the view-model header: the overlay
    // model is fully owned (TD-2026-07-17-084).
    AppendCodeMaskRegexViolations(
        result, header, text, std::regex(R"(\bOverlayState\b)"),
        "render view models must not reference OverlayState (owned overlay model only)");
    // ProjectWorkspaceState may appear only inside the two allowlisted structs.
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
        continue;  // comment or string literal
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

  // Converted render TUs: no broad state type names at all.
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
  // sticky_lines and occurrence_ranges are spans into thread_local builder caches. Reverting them
  // to owning std::vectors reintroduces the per-frame element copy that Finding 11 closed.
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

// Key-hint lists join on " · " (WorkspaceUiText.h's kHintSeparator / JoinHintSegments
// / AppendHintSegment). The rule exists because the shell drifted off it twice, in
// the same way both times: GitSidebarCommandCenter.cpp and CompareTabReview.cpp each
// grew a private byte-identical AppendHintSegment that joined on "  |  ", so the two
// longest and most-read hint lines in the app -- the git sidebar's action line and
// the compare review header, both also mirrored into Help/About -- were the two that
// disagreed with the documented convention.
//
// Scoped to a re-declared AppendHintSegment rather than to the "  |  " literal:
// that separator is legitimate between unrelated fields (the breadcrumb's
// "path  |  left -> right", the merge status line), and banning the literal outright
// would fail those honest uses.
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
      continue;  // the definition itself
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

  // Ids as declared, in order. The X-macro guarantees each has a *name*; nothing
  // guarantees anything ever increments it, and a counter with no producer reads
  // zero forever. That is worse than an absent counter: a reader sees the row
  // missing from the dump and concludes the code path did not run.
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
      continue;  // the declaration and the name table are not producers
    }
    const std::string text = ReadText(entry.path());
    for (std::sregex_iterator it(text.begin(), text.end(), use), last; it != last; ++it) {
      produced.insert((*it)[1].str());
    }
  }

  for (const auto& [id, line] : declared) {
    if (id == "Count") {
      continue;  // the enum terminator, not a counter
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

  // Filetype detection is a bounded head scan plus regexes plus an owned string,
  // and callers ask constantly about buffers that did not change: every prepared
  // frame (fold refresh, status bar), every LSP/assist request, every save, and
  // once per open tab on every settings change.
  //
  // Four independent memos had grown around that -- `runtime_syntax::FiletypeMemo`
  // with two instances, plus `LspUiState::language_cache_*`, whose path-only key
  // silently pinned a stale language for content-detected buffers (a shebang with
  // no extension). Detection now lives behind `TextViewport::language_id()`, keyed
  // on the inputs detection actually reads. This rule is the ratchet that stops a
  // fifth from accreting: the two-argument `DetectFiletype(path, lines)` form --
  // the one that reads buffer content -- is reserved for the memo itself.
  //
  // The one-argument path-only form stays legal everywhere: callers with no buffer
  // in hand (session restore's language hint, a diagnostics bucket keyed by path)
  // have nothing to memoize against.
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

  // `DetectFiletype(` followed by a top-level comma before its closing paren, i.e.
  // the two-argument content-reading overload.
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
    // The declaration/definition of the overload, and the memo that owns it.
    if (filename == "RuntimeSyntaxRegistry.h" || filename == "RuntimeSyntaxRegistry.cpp" ||
        filename == "TextViewport.cpp") {
      continue;
    }
    const std::string text = ReadText(entry.path());
    const std::vector<bool> is_code = BuildCodeMask(text);
    for (std::sregex_iterator it(text.begin(), text.end(), call), last; it != last; ++it) {
      const std::size_t open_paren = static_cast<std::size_t>(it->position() + it->length()) - 1;
      if (open_paren >= is_code.size() || !is_code[open_paren]) {
        continue;  // a mention in a comment or a string literal is not a call
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
        continue;  // path-only overload
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

}  // namespace microide::tests::architecture
