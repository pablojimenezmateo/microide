#include "architecture/WorkspaceServiceArchitectureRules.h"

#include "architecture/ArchitectureFileScanner.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <regex>

namespace microide::tests::architecture {

RuleResult CheckRenderTuDoesNotMaterializeStrings(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "render translation units do not materialize search fallback strings";
  result.hard_fail = true;
  const std::filesystem::path sidebar_path = repo_root / "src/workspace/WorkspaceShellRenderSidebar.cpp";
  const std::string text = ReadText(sidebar_path);
  AppendViolations(
      result, sidebar_path, text, std::regex(R"(std::string\s*\(\s*"search>\s*"\s*\)\s*\+)"),
      "render code must use RenderViewModelBuilder query_fallback_text");
  AppendViolations(
      result, sidebar_path, text, std::regex(R"(std::string\s*\(\s*"replace>\s*"\s*\)\s*\+)"),
      "render code must use RenderViewModelBuilder replace_fallback_text");
  AppendViolations(
      result, sidebar_path, text, std::regex(R"("search>\s*"\s*\+)"),
      "render code must not concatenate search fallback text in render path");
  AppendViolations(
      result, sidebar_path, text, std::regex(R"("replace>\s*"\s*\+)"),
      "render code must not concatenate replace fallback text in render path");
  return result;
}

RuleResult CheckRenderTuDoesNotCallToStringOrFormat(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "render translation units avoid std::to_string / std::format / fmt::format";
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

  const std::regex to_string_pattern(R"(\bstd::to_string\s*\()");
  const std::regex std_format_pattern(R"(\bstd::format\s*\()");
  const std::regex fmt_format_pattern(R"(\bfmt::format\s*\()");
  for (const auto& path : render_files) {
    if (!std::filesystem::exists(path)) {
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
  }
  return result;
}

RuleResult CheckTextViewportNoFullDocCopy(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "TextViewport ReplaceAll full document copy";
  result.hard_fail = true;
  const std::filesystem::path path = repo_root / "src/editor/TextViewportEditEngine.cpp";
  const std::string text = ReadText(path);
  const std::size_t replace_all_pos = text.find("std::size_t TextViewport::ReplaceAll(");
  if (replace_all_pos == std::string::npos) {
    result.violations.push_back(Violation{
        .path = path,
        .line = 1,
        .message = "could not locate TextViewport::ReplaceAll body for invariant scan",
    });
    return result;
  }
  const std::size_t body_start = text.find('{', replace_all_pos);
  if (body_start == std::string::npos) {
    result.violations.push_back(Violation{
        .path = path,
        .line = LineNumberAt(text, replace_all_pos),
        .message = "could not locate ReplaceAll body start",
    });
    return result;
  }
  std::size_t depth = 1;
  std::size_t body_end = body_start + 1;
  for (; body_end < text.size() && depth > 0; ++body_end) {
    if (text[body_end] == '{') {
      ++depth;
    } else if (text[body_end] == '}') {
      --depth;
    }
  }
  const std::string body = text.substr(body_start, body_end - body_start);
  const auto is_code = BuildCodeMask(body);
  const std::array<std::regex, 2> patterns = {
      std::regex(R"(std::vector<std::string>\s+\w+\s*=\s*document_->lines\s*;)"),
      std::regex(R"(auto\s+\w+\s*=\s*document_->lines\s*;)"),
  };
  for (const auto& pattern : patterns) {
    for (std::sregex_iterator it(body.begin(), body.end(), pattern), end; it != end; ++it) {
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
      const std::size_t absolute = body_start + local_start;
      result.violations.push_back(Violation{
          .path = path,
          .line = LineNumberAt(text, absolute),
          .message = "ReplaceAll must not copy document_->lines into a full vector",
      });
    }
  }
  return result;
}

RuleResult CheckTextViewportApplyPipelineNoFullDocumentLineSnapshot(
    const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "TextViewport edit pipeline avoids full document_->lines snapshots";
  result.hard_fail = true;
  const std::filesystem::path path = repo_root / "src/editor/TextViewportEditEngine.cpp";
  const std::string text = ReadText(path);
  const std::array<std::string_view, 2> signatures = {
      "bool TextViewport::ApplyLineEdit(",
      "bool TextViewport::ApplyRangeEdit(",
  };
  const std::array<std::regex, 2> patterns = {
      std::regex(R"(std::vector<std::string>\s+\w+\s*=\s*document_->lines\s*;)"),
      std::regex(R"(auto\s+\w+\s*=\s*document_->lines\s*;)"),
  };
  for (const std::string_view signature : signatures) {
    const auto body_with_offset = ExtractMemberFunctionBodyWithOffset(text, signature);
    if (!body_with_offset.has_value()) {
      result.violations.push_back(Violation{
          .path = path,
          .line = 1,
          .message = std::string("could not locate body for ") + std::string(signature),
      });
      continue;
    }
    const std::string& body = body_with_offset->first;
    const std::size_t body_offset = body_with_offset->second;
    const auto is_code = BuildCodeMask(body);
    for (const auto& pattern : patterns) {
      for (std::sregex_iterator it(body.begin(), body.end(), pattern), end; it != end; ++it) {
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
            .message = "ApplyLineEdit/ApplyRangeEdit must not snapshot-copy document_->lines",
        });
      }
    }
  }
  return result;
}

RuleResult CheckRenderTuEditorEssentialsAvoidEphemeralLabelStrings(
    const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "editor essentials render helpers avoid std::string fold/sticky labels";
  result.hard_fail = true;
  const std::filesystem::path path = repo_root / "src/editor/EditorViewRenderer.cpp";
  const std::string text = ReadText(path);
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
  if (!std::filesystem::exists(path)) {
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
  if (!std::filesystem::exists(path)) {
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
  if (!std::filesystem::exists(path)) {
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
  if (!std::filesystem::exists(path)) {
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

// Application::EnsureSceneTexture must coalesce reallocation across resize
// bursts. Without the resize-time check, dragging the window destroyed and
// recreated the full-window render target on every WINDOW_RESIZED event.

RuleResult CheckApplicationCoalescesResize(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "Application must coalesce scene-texture realloc during resize bursts";
  result.hard_fail = true;
  const std::filesystem::path path = repo_root / "src/app/Application.cpp";
  if (!std::filesystem::exists(path)) {
    return result;
  }
  const std::string text = ReadText(path);
  if (text.find("last_resize_event_ns_") == std::string::npos ||
      text.find("kSceneTextureResizeSettleNs") == std::string::npos) {
    result.violations.push_back(Violation{
        .path = path,
        .line = 1,
        .message = "Application::EnsureSceneTexture must guard reallocation behind "
                   "last_resize_event_ns_ + kSceneTextureResizeSettleNs to coalesce resize bursts",
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
  if (!std::filesystem::exists(path)) {
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
  if (!std::filesystem::exists(path)) {
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
// the same-day fixes closed; see dev-docs/performance/performance-bottleneck-deep-dive-2.md.

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
        name == "WorkspaceShellHoverTargets.cpp") {
      targets.push_back(entry.path());
    }
  }
  targets.push_back(repo_root / "src/editor/EditorViewRenderer.cpp");
  targets.push_back(repo_root / "src/editor/DecoratedTextGridRenderer.cpp");

  for (const auto& path : targets) {
    if (!std::filesystem::exists(path)) {
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
  // dev-docs/performance/performance-bottleneck-deep-dive-2.md Finding 3 to eliminate per-frame
  // UI stalls.
  RuleResult result;
  result.label = "RefreshStatusBar must not run synchronous git from the frame path";
  result.hard_fail = true;
  const std::filesystem::path path = repo_root / "src/workspace/WorkspaceShellChrome.cpp";
  if (!std::filesystem::exists(path)) {
    return result;
  }
  const std::string text = ReadText(path);
  const auto is_code = BuildCodeMask(text);
  // Block any `repo.Execute(` or `git symbolic-ref` mention in the file. The IsValid() probe is
  // still allowed because it is a filesystem-only check cached by status_bar_repo_cache_.
  const std::array<std::regex, 3> patterns = {
      std::regex(R"(\brepo\.Execute\s*\()"),
      std::regex(R"(\.Execute\s*\(\s*\{\s*\"symbolic-ref\")"),
      std::regex(R"(\bResolveBranchLabel\s*\()"),
  };
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
        name == "WorkspaceShellHoverTargets.cpp") {
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

  for (const auto& path : render_files) {
    if (!std::filesystem::exists(path)) {
      continue;
    }
    const std::string text = ReadText(path);
    AppendCodeMaskRegexViolations(
        result, path, text, single_char_pattern,
        "render TU must not build std::string(1, ch); use std::string_view over the char storage");
    AppendCodeMaskRegexViolations(
        result, path, text, literal_plus_pattern,
        "render TU must not concatenate literal + identifier; use a thread_local scratch or "
        "compose the string in RenderViewModelBuilder");
    (void)string_from_view_pattern;  // currently advisory; some constructors of derived types
                                      // still match; left in code for future tightening.
  }
  return result;
}


}  // namespace microide::tests::architecture
