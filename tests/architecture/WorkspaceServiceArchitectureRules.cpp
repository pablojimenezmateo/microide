#include "architecture/WorkspaceServiceArchitectureRules.h"

#include "TestSupport.h"
#include "architecture/ArchitectureFileScanner.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <regex>
#include <string>
#include <string_view>

namespace microide::tests::architecture {

RuleResult CheckRenderTuDoesNotMaterializeStrings(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "render translation units do not materialize search fallback strings";
  result.hard_fail = true;
  const std::filesystem::path sidebar_path = repo_root / "src/workspace/render/WorkspaceShellRenderSidebar.cpp";
  const std::string text = ReadRuleTarget(result, sidebar_path);
  AppendViolations(
      result, sidebar_path, text, std::regex(R"(std::string\s*\(\s*"search>\s*"\s*\)\s*\+)"),
      "render code must use RenderViewModelBuilder query_fallback_text");
  AppendViolations(
      result, sidebar_path, text, std::regex(R"(std::string\s*\(\s*"replace>\s*"\s*\)\s*\+)"),
      "render code must use RenderViewModelBuilder replace_fallback_text");
  AppendTrailingCodeRegexViolations(
      result, sidebar_path, text, std::regex(R"("search>\s*"\s*\+\s*[A-Za-z_])"),
      "render code must not concatenate search fallback text in render path");
  AppendTrailingCodeRegexViolations(
      result, sidebar_path, text, std::regex(R"("replace>\s*"\s*\+\s*[A-Za-z_])"),
      "render code must not concatenate replace fallback text in render path");
  return result;
}

RuleResult CheckRenderTuDoesNotCallToStringOrFormat(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label =
      "render translation units avoid std::to_string / std::format / fmt::format / string concat";
  result.hard_fail = true;

  std::vector<std::filesystem::path> render_files;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(repo_root / "src/workspace")) {
    if (!entry.is_regular_file() || entry.path().extension() != ".cpp") {
      continue;
    }
    const std::string name = entry.path().filename().string();
    if (name.starts_with("WorkspaceShellRender")) {
      render_files.push_back(entry.path());
    }
  }
  render_files.push_back(repo_root / "src/workspace/render/WorkspaceShellHoverPopup.cpp");
  render_files.push_back(repo_root / "src/workspace/render/WorkspaceShellHoverTargets.cpp");
  render_files.push_back(repo_root / "src/workspace/render/DebugPaneRender.cpp");

  const std::regex to_string_pattern(R"(\bstd::to_string\s*\()");
  const std::regex std_format_pattern(R"(\bstd::format\s*\()");
  const std::regex fmt_format_pattern(R"(\bfmt::format\s*\()");
  const std::regex concat_lhs_pattern(R"(std::string\s*\([^)]*\)\s*\+)");
  const std::regex concat_rhs_pattern(R"(\+\s*std::string\s*\()");
  const std::regex path_to_string_pattern(
      R"(\.\s*(generic_string|generic_u8string|u8string|string)\s*\(\s*\))");
  const std::regex ui_text_composer_pattern(
      R"(\b(JoinHintSegments|FormatEmptyState|BuildCountStatus|BuildSelectionSummary|BuildShownOfTotalStatus|BuildSearchProgressSuffix)\s*\()");
  for (const auto& path : render_files) {
    if (!RequireRuleTarget(result, path)) {
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
    AppendCodeMaskRegexViolations(
        result, path, text, concat_lhs_pattern,
        "render TU must not build concatenated strings in the hot path; measure across "
        "segments or compute the string in RenderViewModelBuilder");
    AppendCodeMaskRegexViolations(
        result, path, text, path_to_string_pattern,
        "render TU must not convert a path to a string (each call allocates); use the "
        "cached TextViewport::path_key() or precompute the text in RenderViewModelBuilder");
    AppendCodeMaskRegexViolations(
        result, path, text, concat_rhs_pattern,
        "render TU must not build concatenated strings in the hot path; measure across "
        "segments or compute the string in RenderViewModelBuilder");
    AppendCodeMaskRegexViolations(
        result, path, text, ui_text_composer_pattern,
        "render TU must not call a WorkspaceUiText composer; every one returns a FRESH "
        "std::string, so it allocates per frame — compose in RenderViewModelBuilder and "
        "hand the render TU a string_view");
  }
  return result;
}

namespace {

void AppendFullDocumentMaterializationViolations(RuleResult& result,
                                                 const std::filesystem::path& path,
                                                 const std::string& text,
                                                 std::string_view signature) {
  const auto body_with_offset = ExtractMemberFunctionBodyWithOffset(text, signature);
  if (!body_with_offset.has_value()) {
    result.violations.push_back(Violation{
        .path = path,
        .line = 1,
        .message = std::string("could not locate body for ") + std::string(signature),
    });
    return;
  }
  struct ForbiddenPattern {
    std::regex pattern;
    std::string_view message;
  };
  static const std::array<ForbiddenPattern, 5> kPatterns = {{
      {std::regex(R"(std::vector<std::string>\s+\w+\s*=\s*document_->lines\s*;)"),
       "edit path must not copy document_->lines into a full vector"},
      {std::regex(R"(auto\s+\w+\s*=\s*document_->lines\s*;)"),
       "edit path must not copy document_->lines into a full vector"},
      {std::regex(R"(document_->lines\.ToVector\s*\()"),
       "edit path must not materialize the whole document via ToVector()"},
      {std::regex(R"(document_->lines\.Snapshot\s*\()"),
       "edit path must not materialize the whole document via Snapshot()"},
      {std::regex(R"(document_->lines\.(?:begin|end)\s*\()"),
       "edit path must not iterate document_->lines via begin()/end() (snapshot-backed)"},
  }};
  const std::string& body = body_with_offset->first;
  const std::size_t body_offset = body_with_offset->second;
  const auto is_code = BuildCodeMask(body);
  for (const ForbiddenPattern& forbidden : kPatterns) {
    for (std::sregex_iterator it(body.begin(), body.end(), forbidden.pattern), end; it != end;
         ++it) {
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
          .message = std::string(signature) + "...: " + std::string(forbidden.message),
      });
    }
  }
}

}

RuleResult CheckTextViewportNoFullDocCopy(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "TextViewport batch replace paths avoid full document materialization";
  result.hard_fail = true;
  const std::filesystem::path path = repo_root / "src/editor/TextViewportEditEngine.cpp";
  const std::string text = ReadRuleTarget(result, path);
  AppendFullDocumentMaterializationViolations(result, path, text,
                                              "std::size_t TextViewport::ReplaceAll(");
  AppendFullDocumentMaterializationViolations(
      result, path, text, "std::optional<std::size_t> TextViewport::ReplaceAllRanges(");
  return result;
}

RuleResult CheckTextViewportApplyPipelineNoFullDocumentLineSnapshot(
    const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "TextViewport edit pipeline avoids full document_->lines snapshots";
  result.hard_fail = true;
  const std::filesystem::path path = repo_root / "src/editor/TextViewportEditEngine.cpp";
  const std::string text = ReadRuleTarget(result, path);
  const std::array<std::string_view, 3> signatures = {
      "bool TextViewport::ApplyLineEdit(",
      "bool TextViewport::ApplyRangeEdit(",
      "void TextViewport::ApplyHistoryEntry(",
  };
  for (const std::string_view signature : signatures) {
    AppendFullDocumentMaterializationViolations(result, path, text, signature);
  }
  return result;
}

RuleResult CheckRenderTuEditorEssentialsAvoidEphemeralLabelStrings(
    const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "editor essentials render helpers avoid std::string fold/sticky labels";
  result.hard_fail = true;
  const std::filesystem::path path = repo_root / "src/editor/EditorViewRenderer.cpp";
  const std::string text = ReadRuleTarget(result, path);
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
  const std::filesystem::path path = repo_root / "src/workspace/render/WorkspaceShellRenderFrame.cpp";
  if (!RequireRuleTarget(result, path)) {
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

RuleResult CheckSdlTtfBackendNoPerGlyphLoop(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "SdlTtfTextBackend must not regress to per-glyph SDL_RenderTexture loops";
  result.hard_fail = true;
  const std::filesystem::path path = repo_root / "src/render/SdlTtfTextBackend.cpp";
  if (!RequireRuleTarget(result, path)) {
    return result;
  }
  const std::string text = ReadText(path);
  AppendCodeMaskRegexViolations(
      result, path, text, std::regex(R"(\bDrawFastAsciiString\b)"),
      "per-glyph DrawFastAsciiString path was removed in the 2026-05-14 perf pass; "
      "ASCII text must render through ResolveEntry+BuildAsciiCompositeSurface so the "
      "steady-state path is one SDL_RenderTexture call per cached (text, color)");
  if (text.find("BuildAsciiCompositeSurface") == std::string::npos) {
    result.violations.push_back(Violation{
        .path = path,
        .line = 1,
        .message = "SdlTtfTextBackend.cpp no longer builds an ASCII composite surface; the "
                   "one-draw-per-cached-string path this lint protects is gone (or the lint's "
                   "anchor moved) — repoint it rather than leaving it green",
    });
  }
  return result;
}

RuleResult CheckDecoratedTextGridRendererBatchesFills(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "DecoratedTextGridRenderer must coalesce same-color fills via SDL_RenderFillRects";
  result.hard_fail = true;
  const std::filesystem::path path = repo_root / "src/editor/DecoratedTextGridRenderer.cpp";
  if (!RequireRuleTarget(result, path)) {
    return result;
  }
  const std::string text = ReadText(path);
  const std::vector<bool> is_code = BuildCodeMask(text);
  const std::regex batched(R"(\bFillRectsInDrawColor\s*\()");
  bool batches = false;
  for (std::sregex_iterator it(text.begin(), text.end(), batched), last; it != last && !batches; ++it) {
    const auto at = static_cast<std::size_t>(it->position());
    batches = at < is_code.size() && is_code[at];
  }
  if (!batches) {
    result.violations.push_back(Violation{
        .path = path,
        .line = 1,
        .message = "DecoratedTextGridRenderer.cpp must batch same-color fills through "
                   "render::FillRectsInDrawColor (SDL_RenderFillRects); do not regress to one "
                   "fill call per rect",
    });
  }
  return result;
}

RuleResult CheckEditorViewRendererUsesScratchRows(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "EditorViewRenderer must reuse scratch DecoratedTextRow members";
  result.hard_fail = true;
  const std::filesystem::path path = repo_root / "src/editor/EditorViewRenderer.cpp";
  if (!RequireRuleTarget(result, path)) {
    return result;
  }
  const std::string text = ReadText(path);
  AppendCodeMaskRegexViolations(
      result, path, text,
      std::regex(R"(\bDecoratedTextRow\s+[A-Za-z_][A-Za-z_0-9]*\s*;)"),
      "do not declare a fresh DecoratedTextRow per row; use scratch_row_/sticky_scratch_row_");
  if (text.find("scratch_row_") == std::string::npos ||
      text.find("sticky_scratch_row_") == std::string::npos) {
    result.violations.push_back(Violation{
        .path = path,
        .line = 1,
        .message = "EditorViewRenderer.cpp must consume scratch_row_ and sticky_scratch_row_",
    });
  }
  return result;
}

RuleResult CheckCompareMergeRenderUsesScratchRows(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "compare/merge render TUs must reuse scratch DecoratedTextRow members";
  result.hard_fail = true;
  struct Target {
    std::filesystem::path path;
    std::array<std::string_view, 2> scratch_members;
  };
  const std::array<Target, 2> targets = {
      Target{repo_root / "src/workspace/render/WorkspaceShellRenderCompare.cpp",
             {"compare_left_scratch_row_", "compare_right_scratch_row_"}},
      Target{repo_root / "src/workspace/render/WorkspaceShellRenderMerge.cpp",
             {"merge_incoming_scratch_row_", "merge_current_scratch_row_"}},
  };
  for (const auto& target : targets) {
    if (!RequireRuleTarget(result, target.path)) {
      continue;
    }
    const std::string text = ReadText(target.path);
    AppendCodeMaskRegexViolations(
        result, target.path, text,
        std::regex(R"(\bDecoratedTextRow\s+[A-Za-z_][A-Za-z_0-9]*\s*;)"),
        "do not declare a fresh DecoratedTextRow per row; bind the scratch member by reference");
    for (const std::string_view member : target.scratch_members) {
      if (text.find(member) == std::string::npos) {
        result.violations.push_back(Violation{
            .path = target.path,
            .line = 1,
            .message = "compare/merge render TU must consume its scratch DecoratedTextRow member",
        });
      }
    }
  }
  return result;
}

RuleResult CheckApplicationCoalescesResize(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "Scene texture must coalesce realloc during resize bursts";
  result.hard_fail = true;
  const std::filesystem::path path = repo_root / "src/app/SceneTexturePresenter.cpp";
  if (!RequireRuleTarget(result, path)) {
    return result;
  }
  const std::string text = ReadText(path);
  if (text.find("last_resize_event_ns_") == std::string::npos ||
      text.find("kResizeSettleNs") == std::string::npos) {
    result.violations.push_back(Violation{
        .path = path,
        .line = 1,
        .message = "SceneTexturePresenter::Ensure must guard reallocation behind "
                   "last_resize_event_ns_ + kResizeSettleNs to coalesce resize bursts",
    });
  }
  return result;
}

RuleResult CheckMouseWheelUsesFractionalAccumulator(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "HandleMouseWheel must use the fractional wheel accumulator";
  result.hard_fail = true;
  const std::filesystem::path path = repo_root / "src/workspace/shell/WorkspaceShellMouseMotion.cpp";
  if (!RequireRuleTarget(result, path)) {
    return result;
  }
  const std::string text = ReadText(path);
  if (text.find("AccumulateWheelEvent") == std::string::npos) {
    result.violations.push_back(Violation{
        .path = path,
        .line = 1,
        .message = "HandleMouseWheel must funnel event.wheel.y/x through "
                   "AccumulateWheelEvent so fractional deltas accumulate across events",
    });
  }
  AppendCodeMaskRegexViolations(
      result, path, text,
      std::regex(R"(integer_y\s*!=\s*0\s*\?\s*event\.wheel\.integer_y\s*:\s*static_cast<int>\(std::lround)"),
      "do not derive wheel ticks directly from event.wheel.integer_y / lround(y); "
      "feed event.wheel.{y,x} into AccumulateWheelEvent and consume the returned ticks");
  return result;
}

RuleResult CheckBottomPanelTerminalRectCache(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "PrepareFrameOnce must cache the last terminal panel rect";
  result.hard_fail = true;
  const std::filesystem::path path = repo_root / "src/workspace/render/WorkspaceShellRenderFrame.cpp";
  if (!RequireRuleTarget(result, path)) {
    return result;
  }
  const std::string text = ReadText(path);
  if (text.find("last_terminal_panel_rect_") == std::string::npos) {
    result.violations.push_back(Violation{
        .path = path,
        .line = 1,
        .message = "PrepareFrameOnce must consult last_terminal_panel_rect_ before "
                   "calling ResizeTerminalToPanel; the call is otherwise made every render frame",
    });
  }
  return result;
}

RuleResult CheckNoStdStoInRenderOrBuilderTus(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "no std::sto* parsing in render/view-model translation units";
  result.hard_fail = true;
  const std::regex pattern(R"(\bstd::(stol|stoi|stoul|stoll|stod|stof|stold)\s*\()");

  std::vector<std::filesystem::path> targets;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(repo_root / "src/workspace")) {
    if (!entry.is_regular_file() || entry.path().extension() != ".cpp") {
      continue;
    }
    const std::string name = entry.path().filename().string();
    if (name.starts_with("WorkspaceShellRender") || name == "RenderViewModelBuilder.cpp" ||
        name == "WorkspaceShellChrome.cpp" || name == "WorkspaceShellHoverPopup.cpp" ||
        name == "WorkspaceShellHoverTargets.cpp" || name == "DebugPaneRender.cpp") {
      targets.push_back(entry.path());
    }
  }
  targets.push_back(repo_root / "src/editor/EditorViewRenderer.cpp");
  targets.push_back(repo_root / "src/editor/DecoratedTextGridRenderer.cpp");

  for (const auto& path : targets) {
    if (!RequireRuleTarget(result, path)) {
      continue;
    }
    const std::string text = ReadText(path);
    AppendCodeMaskRegexViolations(
        result, path, text, pattern,
        "render/view-model TU must use util::ParseInt/ParseFloat instead of std::sto*");
  }
  return result;
}

RuleResult CheckStatusBarRefreshIsAsyncOnly(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "RefreshStatusBar must not run synchronous git from the frame path";
  result.hard_fail = true;
  const std::array<std::string_view, 2> targets = {
      "src/workspace/shell/WorkspaceShellPresentation.cpp",
      "src/workspace/services/StatusBarModelService.cpp",
  };
  const std::array<std::regex, 3> patterns = {
      std::regex(R"(\brepo\.Execute\s*\()"),
      std::regex(R"(\.Execute\s*\(\s*\{\s*\"symbolic-ref\")"),
      std::regex(R"(\bResolveBranchLabel\s*\()"),
  };
  for (const std::string_view relative : targets) {
    const std::filesystem::path path = repo_root / relative;
    if (!std::filesystem::exists(path)) {
      result.violations.push_back(Violation{
          .path = path,
          .line = 1,
          .message = "status-bar async rule target moved — re-anchor the rule to the TU that "
                     "now hosts the frame-path status-bar refresh",
      });
      continue;
    }
    const std::string text = ReadRuleTarget(result, path);
    const auto is_code = BuildCodeMask(text);
    for (const auto& pattern : patterns) {
      for (std::sregex_iterator it(text.begin(), text.end(), pattern), end; it != end; ++it) {
        const auto pos = static_cast<std::size_t>(it->position());
        if (pos < is_code.size() && !is_code[pos]) {
          continue;
        }
        result.violations.push_back(Violation{
            .path = path,
            .line = LineNumberAt(text, pos),
            .message = "RefreshStatusBar must not synchronously run git; the branch label is "
                       "populated by the async sidebar git refresh path",
        });
      }
    }
  }
  return result;
}

RuleResult CheckRenderTuDoesNotMaterializeSingleCharOrPrefixStrings(
    const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "render TUs do not materialize std::string(1, ch) / prefix-concat strings";
  result.hard_fail = true;

  std::vector<std::filesystem::path> render_files;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(repo_root / "src/workspace")) {
    if (!entry.is_regular_file() || entry.path().extension() != ".cpp") {
      continue;
    }
    const std::string name = entry.path().filename().string();
    if (name.starts_with("WorkspaceShellRender") || name == "WorkspaceShellHoverPopup.cpp" ||
        name == "WorkspaceShellHoverTargets.cpp" || name == "DebugPaneRender.cpp") {
      render_files.push_back(entry.path());
    }
  }
  render_files.push_back(repo_root / "src/editor/EditorViewRenderer.cpp");
  render_files.push_back(repo_root / "src/editor/DecoratedTextGridRenderer.cpp");

  const std::regex single_char_pattern(R"(std::string\s*\(\s*1\s*,)");
  const std::regex literal_plus_pattern(R"("[^"\n]*"\s*\+\s*[A-Za-z_])");
  const std::regex string_from_view_pattern(R"(std::string\s*\(\s*[A-Za-z_][A-Za-z0-9_]*\s*\.)");
  const std::regex owning_truncate_pattern(R"(\bTruncateToWidth\s*\()");

  for (const auto& path : render_files) {
    if (!RequireRuleTarget(result, path)) {
      continue;
    }
    const std::string text = ReadText(path);
    AppendCodeMaskRegexViolations(
        result, path, text, single_char_pattern,
        "render TU must not build std::string(1, ch); use std::string_view over the char storage");
    AppendTrailingCodeRegexViolations(
        result, path, text, literal_plus_pattern,
        "render TU must not concatenate literal + identifier in a paint path; use a "
        "thread_local scratch or compose the string in RenderViewModelBuilder");
    AppendCodeMaskRegexViolations(
        result, path, text, owning_truncate_pattern,
        "render TU must not call TruncateToWidth (it allocates an owning std::string per "
        "call); draw TruncateToWidthEphemeralView / TruncateLabelView instead, and consume "
        "the view before the next truncation on the same thread");
  }
  (void)string_from_view_pattern;
  return result;
}

RuleResult CheckPaintedScrollbarsAreGrabbable(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "painted scrollbars are grabbable (no hardcoded inactive flag)";
  result.hard_fail = true;
  const std::regex pattern(R"(DrawScrollbar\s*\((?:[^();]|\([^()]*\))*\bfalse\s*\))");

  std::vector<std::filesystem::path> targets;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(repo_root / "src/workspace")) {
    if (!entry.is_regular_file() || entry.path().extension() != ".cpp") {
      continue;
    }
    targets.push_back(entry.path());
  }
  if (targets.empty()) {
    result.violations.push_back(Violation{
        .path = repo_root / "src/workspace",
        .line = 1,
        .message = "scrollbar rule found no workspace translation units to scan",
    });
    return result;
  }
  for (const auto& path : targets) {
    const std::string text = ReadRuleTarget(result, path);
    AppendCodeMaskRegexViolations(
        result, path, text, pattern,
        "a scrollbar painted with a literal `false` active flag is decoration: pass the "
        "drag-target comparison for its DragTarget so the bar can be grabbed and lights up "
        "while it is");
  }
  return result;
}

RuleResult CheckPointerHandlersReadQueuedModifiers(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "handlers read the modifiers in queue order, not SDL's live state";
  result.hard_fail = true;
  const std::regex live_read(R"(\bSDL_GetModState\s*\()");
  const std::filesystem::path dispatcher = repo_root / "src/workspace/WorkspaceEventOrchestrator.cpp";
  const std::string dispatcher_text = ReadRuleTarget(result, dispatcher);
  if (!dispatcher_text.empty() && !CodeMaskedPatternAppears(dispatcher_text, live_read)) {
    result.violations.push_back(Violation{
        .path = dispatcher,
        .line = 1,
        .message = "rule target moved: the dispatcher no longer resyncs the held modifiers on "
                   "focus gain; re-anchor CheckPointerHandlersReadQueuedModifiers",
    });
  }
  for (const auto& entry : std::filesystem::recursive_directory_iterator(repo_root / "src/workspace")) {
    const std::filesystem::path extension = entry.path().extension();
    if (!entry.is_regular_file() || entry.path() == dispatcher ||
        (extension != ".cpp" && extension != ".h" && extension != ".inc")) {
      continue;
    }
    AppendCodeMaskRegexViolations(
        result, entry.path(), ReadText(entry.path()), live_read,
        "SDL_GetModState() runs ahead of the event being handled: read a key event's own "
        "`mod`, or InteractionState::key_modifiers for a pointer event");
  }
  return result;
}

RuleResult CheckWheelHandlersDoNotMoveFocus(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "wheel handlers scroll without taking keyboard focus";
  result.hard_fail = true;
  const std::regex focus_write(R"(surface\.focus\s*=[^=])");

  std::vector<std::filesystem::path> scanned;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(repo_root / "src/workspace")) {
    if (!entry.is_regular_file() || entry.path().extension() != ".cpp") {
      continue;
    }
    const std::string text = ReadText(entry.path());
    const auto body_with_offset = ExtractMemberFunctionBodyWithOffset(text, "::HandleWheel(");
    if (!body_with_offset.has_value()) {
      continue;
    }
    scanned.push_back(entry.path());
    const std::string& body = body_with_offset->first;
    const std::size_t body_offset = body_with_offset->second;
    const auto is_code = BuildCodeMask(body);
    for (std::sregex_iterator it(body.begin(), body.end(), focus_write), end; it != end; ++it) {
      const auto local_start = static_cast<std::size_t>(it->position());
      const auto local_len = static_cast<std::size_t>(it->length());
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
          .path = entry.path(),
          .line = LineNumberAt(text, body_offset + local_start),
          .message = "HandleWheel must not assign surface.focus: scrolling a surface must not "
                     "pull the keyboard away from whatever the user is typing into",
      });
    }
  }
  if (scanned.empty()) {
    result.violations.push_back(Violation{
        .path = repo_root / "src/workspace",
        .line = 1,
        .message = "wheel-focus rule found no HandleWheel definitions to scan",
    });
  }
  return result;
}

RuleResult CheckDebugSubsystemThreadingBehindDapClient(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "debug subsystem threading stays behind WorkspaceDapClient";
  result.hard_fail = true;
  const std::regex pattern(R"(\bstd::(thread|jthread|async)\b)");
  for (const auto& entry : std::filesystem::recursive_directory_iterator(repo_root / "src/workspace")) {
    if (!entry.is_regular_file() || entry.path().extension() != ".cpp") {
      continue;
    }
    const std::string name = entry.path().filename().string();
    const bool in_subsystem = name.starts_with("Debug") || name.starts_with("WorkspaceDap");
    if (!in_subsystem || name == "WorkspaceDapClient.cpp") {
      continue;
    }
    const std::string text = ReadText(entry.path());
    AppendCodeMaskRegexViolations(
        result, entry.path(), text, pattern,
        "debug/DAP code outside WorkspaceDapClient must not spawn threads or std::async; "
        "all adapter concurrency lives behind the DAP client and marshals to the main "
        "thread via DrainCallbacks");
  }
  return result;
}


void RunDecoratedGridBatchesFillsRuleFixtures() {
  TemporaryDirectory fixture;
  const std::filesystem::path& root = fixture.path();
  std::filesystem::create_directories(root / "src/editor");
  const std::filesystem::path file = root / "src/editor/DecoratedTextGridRenderer.cpp";
  WriteFile(file,
            "// render::FillRectsInDrawColor(r, rects, n) used to batch here\n"
            "void F(){ for (auto& r : rects) render::FillRect(renderer, r); }\n");
  Expect(CheckDecoratedTextGridRendererBatchesFills(root).violations.size() == 1,
         "the fill-batching rule must not count a call named only in a comment");
  WriteFile(file, "void F(){ render::FillRectsInDrawColor(renderer, batch.data(), n); }\n");
  Expect(CheckDecoratedTextGridRendererBatchesFills(root).violations.empty(),
         "the fill-batching rule must accept the batched call");
  std::filesystem::remove(file);
  Expect(!CheckDecoratedTextGridRendererBatchesFills(root).missing_targets.empty(),
         "the fill-batching rule must fail loudly when its file moves");
}

}
