#include "architecture/WorkspaceServiceArchitectureRules.h"

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
  const std::filesystem::path sidebar_path = repo_root / "src/workspace/WorkspaceShellRenderSidebar.cpp";
  const std::string text = ReadRuleTarget(result, sidebar_path);
  AppendViolations(
      result, sidebar_path, text, std::regex(R"(std::string\s*\(\s*"search>\s*"\s*\)\s*\+)"),
      "render code must use RenderViewModelBuilder query_fallback_text");
  AppendViolations(
      result, sidebar_path, text, std::regex(R"(std::string\s*\(\s*"replace>\s*"\s*\)\s*\+)"),
      "render code must use RenderViewModelBuilder replace_fallback_text");
  // These two patterns anchor on a string-literal quote, so they must use the
  // trailing-anchored helper -- AppendViolations/AppendCodeMaskRegexViolations
  // would never fire on a `"..." +` match because BuildCodeMask flags the
  // opening quote as non-code.
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
  // The debug pane's bottom-panel render TU is view-model-only like the editor
  // render TUs: it draws prebuilt row strings (DebugVariableRowView /
  // DebugBreakpointRowView) and must never format on the render hot path.
  render_files.push_back(repo_root / "src/workspace/DebugPaneRender.cpp");

  const std::regex to_string_pattern(R"(\bstd::to_string\s*\()");
  const std::regex std_format_pattern(R"(\bstd::format\s*\()");
  const std::regex fmt_format_pattern(R"(\bfmt::format\s*\()");
  // Throwaway string concatenation in the render path: `std::string(view) + ...`
  // or `... + std::string(view)` builds a temporary buffer per frame. Numeric `+`
  // is unaffected — these only match an std::string construction adjacent to `+`.
  const std::regex concat_lhs_pattern(R"(std::string\s*\([^)]*\)\s*\+)");
  const std::regex concat_rhs_pattern(R"(\+\s*std::string\s*\()");
  // std::filesystem::path -> std::string conversion. Every one of these returns a
  // FRESH std::string, so it is a per-frame (often per-row) allocation, but none of
  // the patterns above name it: the ban on std::to_string / std::format / string
  // concat let `viewport.path().generic_string()` through untouched. Two render TUs
  // were doing exactly that. The normalized form is already cached per document as
  // TextViewport::path_key() (editor::NormalizedPathKey), and any other render text
  // belongs in RenderViewModelBuilder.
  const std::regex path_to_string_pattern(
      R"(\.\s*(generic_string|generic_u8string|u8string|string)\s*\(\s*\))");
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
  }
  return result;
}

namespace {

// Shared scan for the no-whole-buffer-materialization invariant: within the body
// of `signature`, flag every construct that materializes all of document_->lines.
// The assignment forms guard a hypothetical return to the old vector<string>
// document model; ToVector()/Snapshot()/begin()/end() are the live TextBuffer
// APIs that copy every line and are banned in the edit-apply funnel (bounded
// LineView/SliceLines/ReplaceLineRange are the sanctioned accessors there).
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

}  // namespace

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
  const std::filesystem::path path = repo_root / "src/workspace/WorkspaceShellRenderFrame.cpp";
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

// SdlTtfTextBackend must not regress to issuing one SDL_RenderTexture per
// character. Per-glyph dispatch was the steady-state cost that the
// 2026-05-14 render-perf pass replaced with a per-string composite cache.
// If a future refactor reintroduces a per-character DrawString loop we want
// the regression to be loud and immediate.

RuleResult CheckSdlTtfBackendNoPerGlyphLoop(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "SdlTtfTextBackend must not regress to per-glyph SDL_RenderTexture loops";
  result.hard_fail = true;
  const std::filesystem::path path = repo_root / "src/render/SdlTtfTextBackend.cpp";
  if (!RequireRuleTarget(result, path)) {
    return result;
  }
  const std::string text = ReadText(path);
  // The legacy per-glyph dispatcher was named DrawFastAsciiString. The new
  // composite path uses BuildAsciiCompositeSurface (single SDL_RenderTexture
  // per cached string). DrawFastAsciiString must stay deleted.
  AppendCodeMaskRegexViolations(
      result, path, text, std::regex(R"(\bDrawFastAsciiString\b)"),
      "per-glyph DrawFastAsciiString path was removed in the 2026-05-14 perf pass; "
      "ASCII text must render through ResolveEntry+BuildAsciiCompositeSurface so the "
      "steady-state path is one SDL_RenderTexture call per cached (text, color)");
  // Required-presence anchor. Banning the old symbol alone is satisfied by a
  // rewrite that reintroduces a per-glyph loop under any other name, and by the
  // file being renamed out from under the rule. Requiring the composite path to
  // still be here makes the rule fail loudly instead of going quiet.
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

// DecoratedTextGridRenderer::RenderRow must keep emitting SDL_RenderFillRects
// for same-color fill runs. Without this batching the editor row paint flips
// SDL3's draw-color state once per fill, breaking the internal command
// batcher and roughly doubling fill-side CPU.

RuleResult CheckDecoratedTextGridRendererBatchesFills(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "DecoratedTextGridRenderer must coalesce same-color fills via SDL_RenderFillRects";
  result.hard_fail = true;
  const std::filesystem::path path = repo_root / "src/editor/DecoratedTextGridRenderer.cpp";
  if (!RequireRuleTarget(result, path)) {
    return result;
  }
  const std::string text = ReadText(path);
  if (text.find("SDL_RenderFillRects") == std::string::npos) {
    result.violations.push_back(Violation{
        .path = path,
        .line = 1,
        .message = "DecoratedTextGridRenderer.cpp must call SDL_RenderFillRects to batch "
                   "same-color fills; do not regress to one SDL_RenderFillRect per fill",
    });
  }
  return result;
}

// EditorViewRenderer::Render must reuse scratch DecoratedTextRow members
// across rows/frames instead of constructing fresh per-row instances. The
// scratch_row_ / sticky_scratch_row_ members on the renderer keep their
// fills/runs/underlines vector capacity alive across frames; falling back to
// per-row stack instances allocates and frees ~150 vectors per frame in a
// typical editor pane.

RuleResult CheckEditorViewRendererUsesScratchRows(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "EditorViewRenderer must reuse scratch DecoratedTextRow members";
  result.hard_fail = true;
  const std::filesystem::path path = repo_root / "src/editor/EditorViewRenderer.cpp";
  if (!RequireRuleTarget(result, path)) {
    return result;
  }
  const std::string text = ReadText(path);
  // A stack-local `DecoratedTextRow name;` declaration inside the renderer is
  // the regression we want to catch. Reference-bindings of the scratch
  // members (`DecoratedTextRow& name = scratch_row_;`) are allowed and use
  // `&`.
  AppendCodeMaskRegexViolations(
      result, path, text,
      std::regex(R"(\bDecoratedTextRow\s+[A-Za-z_][A-Za-z_0-9]*\s*;)"),
      "do not declare a fresh DecoratedTextRow per row; use scratch_row_/sticky_scratch_row_");
  // Also require the scratch members to be referenced — if a refactor removes
  // them but the regex above still passes (e.g. via auto), this catches it.
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
  // The compare/merge surface render TUs build one DecoratedTextRow per visible
  // row per pane per frame. Reusing mutable scratch members (cleared by
  // BuildDecoratedRow before refill) keeps that path allocation-free; a fresh
  // stack `DecoratedTextRow name;` per row is the regression this blocks.
  RuleResult result;
  result.label = "compare/merge render TUs must reuse scratch DecoratedTextRow members";
  result.hard_fail = true;
  struct Target {
    std::filesystem::path path;
    std::array<std::string_view, 2> scratch_members;
  };
  const std::array<Target, 2> targets = {
      Target{repo_root / "src/workspace/WorkspaceShellRenderCompare.cpp",
             {"compare_left_scratch_row_", "compare_right_scratch_row_"}},
      Target{repo_root / "src/workspace/WorkspaceShellRenderMerge.cpp",
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

// SceneTexturePresenter::Ensure must coalesce reallocation across resize
// bursts. Without the resize-time check, dragging the window destroyed and
// recreated the full-window render target on every WINDOW_RESIZED event.

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

// HandleMouseWheel must accumulate fractional wheel.y deltas. Without the
// accumulator, smooth-scroll trackpad input (event.wheel.integer_y == 0,
// event.wheel.y ~ 0.3) produces no scroll until enough deltas land in a
// single discrete tick.

RuleResult CheckMouseWheelUsesFractionalAccumulator(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "HandleMouseWheel must use the fractional wheel accumulator";
  result.hard_fail = true;
  const std::filesystem::path path = repo_root / "src/workspace/WorkspaceShellMouseMotion.cpp";
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
  // The legacy form that rounds fractional deltas to zero must not be
  // reintroduced as the only source of the wheel tick.
  AppendCodeMaskRegexViolations(
      result, path, text,
      std::regex(R"(integer_y\s*!=\s*0\s*\?\s*event\.wheel\.integer_y\s*:\s*static_cast<int>\(std::lround)"),
      "do not derive wheel ticks directly from event.wheel.integer_y / lround(y); "
      "feed event.wheel.{y,x} into AccumulateWheelEvent and consume the returned ticks");
  return result;
}

// PrepareFrameOnce must short-circuit ResizeTerminalToPanel when the bottom
// panel rect is unchanged from the previous frame. Without this the resize
// call ran every frame (its internal short-circuit avoided the actual
// terminal resize but the rect math and trace marker still ran).

RuleResult CheckBottomPanelTerminalRectCache(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "PrepareFrameOnce must cache the last terminal panel rect";
  result.hard_fail = true;
  const std::filesystem::path path = repo_root / "src/workspace/WorkspaceShellRenderFrame.cpp";
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

// 2026-05-15 P0 lint additions (perf deep-dive round 2 Finding 19). Each guards a regression that
// the same-day fixes closed; see dev-docs/performance/investigations/performance-bottleneck-deep-dive-2.md.

RuleResult CheckNoStdStoInRenderOrBuilderTus(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "no std::sto* parsing in render/view-model translation units";
  result.hard_fail = true;
  const std::regex pattern(R"(\bstd::(stol|stoi|stoul|stoll|stod|stof|stold)\s*\()");

  std::vector<std::filesystem::path> targets;
  for (const auto& entry : std::filesystem::directory_iterator(repo_root / "src/workspace")) {
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
  // RefreshStatusBar runs in PrepareFrameOnce. It MUST NOT spawn subprocesses or
  // synchronously consult `git symbolic-ref`/`rev-parse` -- the branch label is
  // populated by the async sidebar git coordinator. The previous synchronous
  // `git symbolic-ref --short HEAD` fallback was removed in
  // dev-docs/performance/investigations/performance-bottleneck-deep-dive-2.md Finding 3 to eliminate per-frame
  // UI stalls.
  RuleResult result;
  result.label = "RefreshStatusBar must not run synchronous git from the frame path";
  result.hard_fail = true;
  // The frame-path status-bar refresh spans two TUs: WorkspaceShell::RefreshStatusBar
  // (WorkspaceShellPresentation.cpp, called from PrepareFrameOnce) and the model build
  // it invokes (StatusBarModelService.cpp). This rule previously scanned the retired
  // WorkspaceShellChrome.cpp and silently passed once that file was split up — a
  // missing target is now a violation so a future rename fails loudly instead of
  // going vacuous (the 022 fixture-path-drift lesson).
  const std::array<std::string_view, 2> targets = {
      "src/workspace/WorkspaceShellPresentation.cpp",
      "src/workspace/StatusBarModelService.cpp",
  };
  // Block any `repo.Execute(` or `git symbolic-ref` mention in the files. The IsValid() probe is
  // still allowed because it is a filesystem-only check cached by the model service.
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
  // 2026-05-15 perf deep-dive round 2 Finding 7: render TUs used to construct
  // `std::string(1, ch)` for single-char git markers and `std::string("prefix ")` /
  // `"prefix " + …` for prompt scaffolds, allocating once per row/frame. The
  // fixes route through std::string_view (the marker case) or a thread_local
  // scratch (the prompt case). This rule blocks the regression.
  RuleResult result;
  result.label = "render TUs do not materialize std::string(1, ch) / prefix-concat strings";
  result.hard_fail = true;

  std::vector<std::filesystem::path> render_files;
  for (const auto& entry : std::filesystem::directory_iterator(repo_root / "src/workspace")) {
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

  // `std::string(1, x)` materializes a 1-char std::string — use a string_view over the char
  // storage instead. `std::string(<view>)` is also a per-call allocation.
  const std::regex single_char_pattern(R"(std::string\s*\(\s*1\s*,)");
  // `"literal" + <expr>` concatenation in a render TU.
  const std::regex literal_plus_pattern(R"("[^"\n]*"\s*\+\s*[A-Za-z_])");
  // Plain `std::string("literal")` constructions in render TUs (occasional helpful in tests but
  // not in hot paint paths).
  const std::regex string_from_view_pattern(R"(std::string\s*\(\s*[A-Za-z_][A-Za-z0-9_]*\s*\.)");

  // single_char_pattern is enforced across every render TU. literal+ident
  // concatenation is enforced only on the hot per-row render TUs
  // (EditorViewRenderer/DecoratedTextGridRenderer plus the compare/merge
  // surface render TUs), where it would allocate once per visible row at frame
  // rate. The overlay/sidebar/hover paint surfaces also build prefix strings,
  // but only once per visible-surface repaint (not per row at 60fps); routing
  // those through the view model is a separate, larger refactor and is
  // intentionally out of scope for this rule.
  const std::array<std::filesystem::path, 4> hot_editor_render_files = {
      repo_root / "src/editor/EditorViewRenderer.cpp",
      repo_root / "src/editor/DecoratedTextGridRenderer.cpp",
      repo_root / "src/workspace/WorkspaceShellRenderCompare.cpp",
      repo_root / "src/workspace/WorkspaceShellRenderMerge.cpp",
  };

  for (const auto& path : render_files) {
    if (!RequireRuleTarget(result, path)) {
      continue;
    }
    const std::string text = ReadText(path);
    AppendCodeMaskRegexViolations(
        result, path, text, single_char_pattern,
        "render TU must not build std::string(1, ch); use std::string_view over the char storage");
  }

  for (const auto& path : hot_editor_render_files) {
    if (!RequireRuleTarget(result, path)) {
      continue;
    }
    const std::string text = ReadText(path);
    // Trailing-anchored: the pattern starts on a string-literal quote, which
    // BuildCodeMask flags as non-code, so AppendCodeMaskRegexViolations would
    // never fire here.
    AppendTrailingCodeRegexViolations(
        result, path, text, literal_plus_pattern,
        "hot editor render TU must not concatenate literal + identifier per row; use a "
        "thread_local scratch or compose the string in RenderViewModelBuilder");
  }
  (void)string_from_view_pattern;  // advisory; some derived-type constructors still match.
  return result;
}

RuleResult CheckDebugSubsystemThreadingBehindDapClient(const std::filesystem::path& repo_root) {
  // The debug subsystem's entire threading model lives behind WorkspaceDapClient:
  // it owns the adapter I/O thread (plus the init/shutdown threads) and marshals
  // every response/event back to the main thread via a wake event +
  // DrainCallbacks. DebugSession, DebugService, DapManager, the value tree, and
  // the pane are therefore plain single-threaded main-thread code. Spawning a
  // thread (or std::async) anywhere else in the subsystem would bypass that
  // single-owner model and reintroduce exactly the data races TSAN guards (two
  // adapter I/O threads, callbacks firing off-thread). Keep all concurrency
  // behind the DAP client; this lint blocks the regression structurally rather
  // than relying on TSAN catching it after the fact. WorkspaceDapClient.cpp is
  // the sole exempt TU (it is the owner).
  RuleResult result;
  result.label = "debug subsystem threading stays behind WorkspaceDapClient";
  result.hard_fail = true;
  const std::regex pattern(R"(\bstd::(thread|jthread|async)\b)");
  for (const auto& entry : std::filesystem::directory_iterator(repo_root / "src/workspace")) {
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


}  // namespace microide::tests::architecture
