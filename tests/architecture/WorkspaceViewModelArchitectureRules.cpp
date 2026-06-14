#include "architecture/WorkspaceViewModelArchitectureRules.h"

#include "architecture/ArchitectureFileScanner.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <regex>

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

  for (const auto& entry : std::filesystem::directory_iterator(repo_root / "src/workspace")) {
    if (!entry.is_regular_file() || entry.path().extension() != ".cpp") {
      continue;
    }
    const std::string name = entry.path().filename().string();
    const bool is_compare_render = name.starts_with("WorkspaceShellCompareRender");
    const bool is_merge_render = name.starts_with("WorkspaceShellMergeRender");
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

RuleResult CheckSidebarSurfaceFallbackUsesStringView(const std::filesystem::path& repo_root) {
  // BuildSidebarSurface() is called multiple times per frame. The fallback text fields used to be
  // std::string and allocated on every call. They are now std::string_view so the typical render
  // frame is allocation-free. The header is the load-bearing surface for this rule.
  RuleResult result;
  result.label = "SidebarSurfaceViewModel fallback fields stay as std::string_view";
  result.hard_fail = true;
  const std::filesystem::path path = repo_root / "src/workspace/RenderViewModelBuilder.h";
  if (!std::filesystem::exists(path)) {
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

RuleResult CheckEditorViewModelStickyAndOccurrenceAreSpans(const std::filesystem::path& repo_root) {
  // sticky_lines and occurrence_ranges are spans into thread_local builder caches. Reverting them
  // to owning std::vectors reintroduces the per-frame element copy that Finding 11 closed.
  RuleResult result;
  result.label = "EditorViewModel sticky/occurrence fields stay as std::span";
  result.hard_fail = true;
  const std::filesystem::path path = repo_root / "src/editor/EditorViewModel.h";
  if (!std::filesystem::exists(path)) {
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

}  // namespace microide::tests::architecture
