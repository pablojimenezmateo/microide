#include "architecture/WorkspaceShellArchitectureRules.h"

#include "architecture/ArchitectureFileScanner.h"
#include "architecture/ArchitectureRuleHelpers.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>

namespace microide::tests::architecture {

RuleResult CheckWorkspaceFriends(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "workspace friend declarations";
  result.hard_fail = true;
  const std::regex pattern(R"(\bfriend\s+(class|struct)\b)");
  for (const auto& entry :
       std::filesystem::recursive_directory_iterator(repo_root / "src/workspace")) {
    if (!entry.is_regular_file()) {
      continue;
    }
    const auto ext = entry.path().extension();
    if (ext != ".h" && ext != ".cpp" && ext != ".inc") {
      continue;
    }
    const std::string text = ReadText(entry.path());
    const auto testing_mask = BuildTestingGuardMask(text);
    for (std::sregex_iterator it(text.begin(), text.end(), pattern), end; it != end; ++it) {
      const std::size_t pos = static_cast<std::size_t>(it->position());
      if (pos < testing_mask.size() && testing_mask[pos]) {
        continue;
      }
      const std::size_t decl_end = text.find(';', pos);
      const std::string_view decl(text.data() + pos,
                                  (decl_end == std::string::npos ? text.size() : decl_end) - pos);
      if (decl.find("TestAccess") != std::string_view::npos) {
        continue;
      }
      result.violations.push_back(Violation{
          .path = entry.path(),
          .line = LineNumberAt(text, pos),
          .message = "workspace code should not declare friend class or friend struct",
      });
    }
  }
  return result;
}

RuleResult CheckCoordinatorShellConstructors(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "coordinator constructors taking WorkspaceShell";
  result.hard_fail = true;
  for (const auto& entry :
       std::filesystem::recursive_directory_iterator(repo_root / "src/workspace")) {
    if (!entry.is_regular_file() || entry.path().extension() != ".h") {
      continue;
    }
    const std::string filename = entry.path().filename().string();
    if (filename.find("Coordinator") == std::string::npos || !filename.starts_with("Workspace")) {
      continue;
    }
    const std::string stem = entry.path().stem().string();
    const std::regex pattern(stem + R"(\s*\([^;{}]*WorkspaceShell\s*[&*])");
    const std::string text = ReadText(entry.path());
    AppendViolations(result, entry.path(), text, pattern,
                     "coordinator constructors should not take WorkspaceShell");
  }
  return result;
}

namespace {
constexpr std::array<std::string_view, 5> kSdlDrawAllowlist = {
    "src/render/SurfacePrimitives.h",
    "src/render/ClipStableLine.h",
    "src/render/SdlTtfTextBackend.cpp",
    "src/render/DebugTextBackend.cpp",
    "src/app/Application.cpp",
};
}

std::span<const std::string_view> SdlDrawAllowlist() { return kSdlDrawAllowlist; }

RuleResult CheckSdlDrawCallsStayInTheAllowlist(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "direct SDL draw calls only in the paint-recorded allowlist";
  result.hard_fail = true;
  const std::regex draw_call(
      R"(\bSDL_Render(FillRects?|Rects?|Lines?|Points?|Texture|TextureRotated|TextureTiled|Texture9Grid|Geometry|GeometryRaw|DebugText|DebugTextFormat)\s*\()");
  std::set<std::string> allowed;
  for (const std::string_view entry : kSdlDrawAllowlist) {
    allowed.insert(std::string(entry));
    const std::filesystem::path target = repo_root / entry;
    if (!RequireRuleTarget(result, target)) {
      continue;
    }
    if (!CodeMaskedPatternAppears(ReadText(target), draw_call)) {
      result.violations.push_back(Violation{
          .path = target, .line = 1, .message = "allowlisted for direct SDL draw calls but makes none: remove it"});
    }
  }
  constexpr std::array<std::string_view, 3> kExtensions = {".cpp", ".h", ".inc"};
  for (const std::filesystem::path& path : SourceFilesUnder(repo_root / "src", kExtensions)) {
    const std::string relative = path.lexically_relative(repo_root).generic_string();
    if (allowed.contains(relative)) {
      continue;
    }
    AppendCodeMaskRegexViolations(result, path, ReadText(path), draw_call,
                                  "draw through the primitives (render::FillRect, TextRenderer, ...), "
                                  "which the paint-overrun recorder sees, not SDL_Render* directly");
  }
  return result;
}

RuleResult CheckThrowingStoParsers(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "try/std::sto parsing";
  result.hard_fail = true;
  const std::array<std::filesystem::path, 3> roots = {
      repo_root / "src", repo_root / "tools", repo_root / "tests/perf"};
  for (const auto& root : roots) {
    std::error_code root_ec;
    if (!std::filesystem::is_directory(root, root_ec) || root_ec) {
      continue;
    }
    for (const std::filesystem::path& path : SourceFilesUnder(root)) {
      const std::string text = ReadText(path);
      const auto offsets = FindTryCatchStoViolations(text);
      for (const std::size_t offset : offsets) {
        result.violations.push_back(Violation{
            .path = path,
            .line = LineNumberAt(text, offset),
            .message = "replace try/catch std::sto parsing with util::Parse helpers",
        });
      }
    }
  }
  return result;
}

RuleResult CheckDapTransportUsesCheckedResponseSeqNarrowing(
    const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "DAP transport range-checks inbound request_seq/seq before narrowing";
  result.hard_fail = true;
  const std::filesystem::path target =
      repo_root / "src/workspace/debug/WorkspaceDapClientInternal.h";
  if (!RequireRuleTarget(result, target)) {
    return result;
  }
  const std::string text = ReadText(target);
  const std::regex raw_narrow(
      R"(static_cast<int>\s*\(\s*msg\[\"(request_seq|seq)\"\]\.AsInt\(\)\s*\))");
  AppendViolations(result, target, text, raw_narrow,
                   "narrow inbound DAP request_seq/seq via DapResponseSeqInRange, "
                   "not a bare static_cast<int>(AsInt())");
  return result;
}

RuleResult CheckPublicScriptsUseRunChecksForCtest(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "public tools/*.sh route ctest through run-checks.sh";
  result.hard_fail = true;
  const std::filesystem::path tools_dir = repo_root / "tools";
  std::error_code dir_ec;
  if (!std::filesystem::is_directory(tools_dir, dir_ec) || dir_ec) {
    return result;
  }
  const std::regex ctest_invocation(R"((^|[;&|(]|\bdo\b|\bthen\b)\s*ctest\b)");
  for (const auto& entry : std::filesystem::directory_iterator(tools_dir)) {
    if (!entry.is_regular_file() || entry.path().extension() != ".sh") {
      continue;
    }
    if (entry.path().filename() == "run-checks.sh") {
      continue;
    }
    const std::string text = ReadText(entry.path());
    std::size_t line_no = 1;
    std::size_t start = 0;
    while (start <= text.size()) {
      const std::size_t nl = text.find('\n', start);
      const std::string line =
          text.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
      const std::size_t first = line.find_first_not_of(" \t");
      const bool is_comment = first != std::string::npos && line[first] == '#';
      if (!is_comment && std::regex_search(line, ctest_invocation)) {
        result.violations.push_back(Violation{
            .path = entry.path(),
            .line = line_no,
            .message = "public tools/*.sh must call ctest via tools/run-checks.sh, not directly",
        });
      }
      if (nl == std::string::npos) {
        break;
      }
      start = nl + 1;
      ++line_no;
    }
  }
  return result;
}

RuleResult CheckOneShotWakeProducersCheckPushResultOrHaveBackstop(
    const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "one-shot wake producers route through the checked pusher";
  result.hard_fail = true;
  static constexpr std::array<const char*, 5> kProducerFiles = {
      "src/project/GitBlameService.cpp",
      "src/platform/ControlSocketServer.cpp",
      "src/workspace/coordinators/WorkspaceProjectDialogCoordinator.cpp",
      "src/util/BackgroundTaskCounter.cpp",
      "src/workspace/coordinators/WorkspaceLifecycleCoordinator.cpp",
  };
  const std::regex bare_push(R"(\bSDL_PushEvent\s*\()");
  for (const char* rel : kProducerFiles) {
    const std::filesystem::path path = repo_root / rel;
    if (!RequireRuleTarget(result, path)) {
      continue;
    }
    const std::string text = ReadText(path);
    AppendViolations(result, path, text, bare_push,
                     "one-shot wake producers must use util::PushWake, not a bare "
                     "SDL_PushEvent (which drops the wake with no backstop on failure)");
  }
  return result;
}

RuleResult CheckPerfScenariosUseNonThrowingFilesystemProbes(
    const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "perf scenarios use non-throwing filesystem probes";
  result.hard_fail = true;
  const std::filesystem::path perf_dir = repo_root / "tests/perf";
  std::error_code dir_ec;
  if (!std::filesystem::is_directory(perf_dir, dir_ec) || dir_ec) {
    return result;
  }
  const std::regex throwing_probe(
      R"(std::filesystem::(exists|is_directory)\s*\([^,()]*\))");
  for (const std::filesystem::path& path : SourceFilesUnder(perf_dir)) {
    const std::string text = ReadText(path);
    AppendViolations(result, path, text, throwing_probe,
                     "perf fixture probes must use PathExistsNoThrow/DirectoryExistsNoThrow, "
                     "not a throwing single-arg std::filesystem::exists/is_directory");
  }
  return result;
}

RuleResult CheckPerfMeasureBodiesDoNotBuildTheirOwnInput(
    const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "perf Measure bodies do not build their own input";
  result.hard_fail = true;
  const std::filesystem::path perf_dir = repo_root / "tests/perf";
  std::error_code dir_ec;
  if (!std::filesystem::is_directory(perf_dir, dir_ec) || dir_ec) {
    return result;
  }
  const std::regex materializes(
      R"((std::to_string\s*\()|((std|fmt)::format\s*\()|("\s*\+)|(\+\s*"))");
  std::size_t measure_bodies_seen = 0;
  for (const std::filesystem::path& path : SourceFilesUnder(perf_dir)) {
    const std::string text = ReadText(path);
    for (const std::size_t call : FindCodeLiteralOccurrences(text, "Measure(")) {
      const std::size_t capture = text.find(']', call);
      if (capture == std::string::npos) {
        continue;
      }
      const std::size_t brace = text.find('{', capture);
      if (brace == std::string::npos) {
        continue;
      }
      const std::optional<std::string> body = ExtractBraceDelimitedBody(text, brace);
      if (!body.has_value()) {
        continue;
      }
      ++measure_bodies_seen;
      if (body->find("perf-measure-builds-input:") != std::string::npos) {
        continue;
      }
      std::smatch match;
      if (!std::regex_search(*body, match, materializes)) {
        continue;
      }
      result.violations.push_back(Violation{
          .path = path,
          .line = LineNumberAt(text, brace),
          .message = "a Measure(...) body must not materialize strings (std::to_string, "
                     "std::format, or concatenation with a literal): the gate then measures the "
                     "scenario's own operator+ rather than the product function it names. Build "
                     "the input before the window opens, or, if construction genuinely IS the "
                     "work, say so in the body with a `perf-measure-builds-input: <reason>` "
                     "comment (TD-2026-08-07-166)",
      });
    }
  }
  if (measure_bodies_seen == 0) {
    result.violations.push_back(Violation{
        .path = perf_dir,
        .line = 1,
        .message = "found no Measure(...) lambda body under tests/perf; this rule now scans "
                   "nothing and cannot fail (TD-2026-08-07-166)",
    });
  }
  return result;
}

RuleResult CheckShellGlueIsBoundByReference(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "Make*Service()/Make*Coordinator() results must be bound by reference";
  result.hard_fail = true;
  const std::filesystem::path workspace_dir = repo_root / "src/workspace";
  std::error_code dir_ec;
  if (!std::filesystem::is_directory(workspace_dir, dir_ec) || dir_ec) {
    return result;
  }
  const std::regex bound_by_value(
      R"((?:^|[;{}])\s*(?:const\s+)?(?:auto|[A-Z][A-Za-z_0-9]*)\s+[a-z_][A-Za-z_0-9]*\s*=\s*Make[A-Za-z_0-9]*(?:Service|Coordinator)\s*\(\s*\)\s*;)");
  std::size_t factory_calls_seen = 0;
  const std::regex any_factory_call(R"(\bMake[A-Za-z_0-9]*(?:Service|Coordinator)\s*\(\s*\))");
  for (const std::filesystem::path& path : SourceFilesUnder(workspace_dir)) {
    const std::string text = ReadText(path);
    const std::vector<bool> code_mask = BuildCodeMask(text);
    for (auto it = std::sregex_iterator(text.begin(), text.end(), any_factory_call);
         it != std::sregex_iterator(); ++it) {
      const auto offset = static_cast<std::size_t>(it->position());
      if (offset < code_mask.size() && code_mask[offset]) {
        ++factory_calls_seen;
      }
    }
    AppendCodeMaskRegexViolations(
        result, path, text, bound_by_value,
        "bind a Make*Service()/Make*Coordinator() result with `auto&` (or `Type&`): it is a "
        "reference into the shell's glue cache, and a value binding copies the whole "
        "coordinator -- seventeen std::functions -- on every call");
  }
  if (factory_calls_seen == 0) {
    result.missing_targets.push_back(Violation{
        .path = workspace_dir,
        .line = 1,
        .message = "found no Make*Service()/Make*Coordinator() call sites under src/workspace; "
                   "this rule now scans nothing and cannot fail",
    });
  }
  return result;
}

RuleResult CheckFactoryResultsAreNotCapturedByValue(
    const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "Make*Service/Coordinator results are not captured by value into a callback";
  result.hard_fail = true;
  const std::filesystem::path workspace_dir = repo_root / "src/workspace";
  std::error_code dir_ec;
  if (!std::filesystem::is_directory(workspace_dir, dir_ec) || dir_ec) {
    return result;
  }
  const std::regex factory_local(
      R"(\b(?:auto|[A-Z][A-Za-z_0-9]*)\s*&?\s*([a-z_][A-Za-z_0-9]*)\s*=\s*Make[A-Za-z_0-9]*(Service|Coordinator)\s*\(\s*\)\s*;)");
  std::size_t factory_locals_seen = 0;
  for (const std::filesystem::path& path : SourceFilesUnder(workspace_dir)) {
    const std::string text = ReadText(path);
    const std::vector<bool> code_mask = BuildCodeMask(text);
    const auto is_code = [&code_mask](std::size_t offset) {
      return offset < code_mask.size() && code_mask[offset];
    };
    for (auto it = std::sregex_iterator(text.begin(), text.end(), factory_local);
         it != std::sregex_iterator(); ++it) {
      if (!is_code(static_cast<std::size_t>(it->position()))) {
        continue;
      }
      ++factory_locals_seen;
      const std::string name = (*it)[1].str();
      const std::regex captured("[\\[,]\\s*" + name + "\\s*[,\\]]");
      for (auto cap = std::sregex_iterator(text.begin(), text.end(), captured);
           cap != std::sregex_iterator(); ++cap) {
        if (!is_code(static_cast<std::size_t>(cap->position()))) {
          continue;
        }
        result.violations.push_back(Violation{
            .path = path,
            .line = LineNumberAt(text, static_cast<std::size_t>(cap->position())),
            .message = "`" + name +
                       "` comes from a Make*Service()/Make*Coordinator() factory and is captured "
                       "BY VALUE into a lambda. Those factories return structs of std::functions "
                       "(hundreds of bytes), which overflow std::function's small-object buffer, "
                       "so each capture heap-allocates a copy of the whole object -- once per "
                       "hook, every time the enclosing factory runs, which for a mouse or frame "
                       "coordinator is per event. Capture `this` and construct it inside the "
                       "body instead; building one of these allocates nothing "
                       "(TD-2026-08-10-177)",
        });
      }
    }
  }
  if (factory_locals_seen == 0) {
    result.violations.push_back(Violation{
        .path = workspace_dir,
        .line = 1,
        .message = "found no `auto x = Make*Service();` local under src/workspace; this rule now "
                   "scans nothing and cannot fail (TD-2026-08-10-177)",
    });
  }
  return result;
}

RuleResult CheckPerfMeasureBodiesDoNotWaitOnWallClock(
    const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "perf Measure bodies do not wait on the wall clock";
  result.hard_fail = true;
  const std::filesystem::path perf_dir = repo_root / "tests/perf";
  std::error_code dir_ec;
  if (!std::filesystem::is_directory(perf_dir, dir_ec) || dir_ec) {
    return result;
  }
  const std::regex waits(
      R"((\bWaitFor[A-Za-z]*\s*\()|(\bcontext\.Wait\s*\()|(std::this_thread::sleep_for\s*\())");
  std::size_t measure_bodies_seen = 0;
  for (const std::filesystem::path& path : SourceFilesUnder(perf_dir)) {
    const std::string text = ReadText(path);
    for (const std::size_t call : FindCodeLiteralOccurrences(text, "Measure(")) {
      const std::size_t capture = text.find(']', call);
      if (capture == std::string::npos) {
        continue;
      }
      const std::size_t brace = text.find('{', capture);
      if (brace == std::string::npos) {
        continue;
      }
      const std::optional<std::string> body = ExtractBraceDelimitedBody(text, brace);
      if (!body.has_value()) {
        continue;
      }
      ++measure_bodies_seen;
      if (body->find("perf-measure-waits-on-clock:") != std::string::npos) {
        continue;
      }
      std::smatch match;
      if (!std::regex_search(*body, match, waits)) {
        continue;
      }
      result.violations.push_back(Violation{
          .path = path,
          .line = LineNumberAt(text, brace),
          .message = "a Measure(...) body must not block on the wall clock (WaitFor*, "
                     "context.Wait, sleep_for): the phase's allocation count then counts poll "
                     "iterations, so it moves with machine load and not with any code, while "
                     "being gated as a deterministic metric. Wait OUTSIDE the window and measure "
                     "the fixed work the wait was waiting for -- or, if the wait provably "
                     "allocates nothing while spinning, say so in the body with a "
                     "`perf-measure-waits-on-clock: <reason>` comment and prove it with three "
                     "runs of one unchanged binary (TD-2026-08-10-179)",
      });
    }
  }
  if (measure_bodies_seen == 0) {
    result.violations.push_back(Violation{
        .path = perf_dir,
        .line = 1,
        .message = "found no Measure(...) lambda body under tests/perf; this rule now scans "
                   "nothing and cannot fail (TD-2026-08-10-179)",
    });
  }
  return result;
}

RuleResult CheckPerfHarnessIsolatesBeforeConstructingTheShell(
    const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "perf harness isolates the app-root before the Driver";
  result.hard_fail = true;
  const std::filesystem::path harness = repo_root / "tests/perf/PerfHarness.cpp";
  const std::string text = ReadRuleTarget(result, harness);
  if (text.empty()) {
    return result;
  }
  const auto first_code_offset = [&](std::string_view needle) -> std::size_t {
    const std::vector<std::size_t> offsets = FindCodeLiteralOccurrences(text, needle);
    return offsets.empty() ? std::string::npos : offsets.front();
  };
  const std::size_t establish = first_code_offset("EstablishIsolatedAppRoot");
  const std::size_t declare = first_code_offset("Driver driver;");
  if (establish == std::string::npos || declare == std::string::npos) {
    result.violations.push_back(Violation{
        .path = harness,
        .line = 1,
        .message = "expected both `EstablishIsolatedAppRoot` and `Driver driver;` in "
                   "PerfHarness.cpp; one of them was renamed, so this rule can no longer "
                   "order them and is silently not gating (TD-2026-08-07-165)",
    });
    return result;
  }
  if (establish > declare) {
    result.violations.push_back(Violation{
        .path = harness,
        .line = LineNumberAt(text, declare),
        .message = "the isolated app-root must be established BEFORE `Driver driver;`: the "
                   "Driver holds a WorkspaceShell by value, so declaring it loads the "
                   "user-level state (recents, config) — from the developer's real home "
                   "directory if isolation has not been installed yet, which makes every "
                   "project-opening scenario's allocation gate depend on the operator's "
                   "machine (TD-2026-08-07-165)",
    });
  }
  return result;
}

RuleResult CheckRenderSurfaceStateAccess(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "render surface view-model-only access";
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

  const std::regex direct_state_pattern(R"(context_\.current_project_state)");
  const std::regex current_surface_pattern(R"(\bCurrentTextInputSurface\s*\()");
  for (const auto& path : render_files) {
    const std::string text = ReadRuleTarget(result, path);
    AppendViolations(result, path, text, direct_state_pattern,
                     "render surface files should read project state through render view models");
    AppendViolations(result, path, text, current_surface_pattern,
                     "render surface files should use view-model-provided text-input surface");
  }

  return result;
}

RuleResult CheckRenderSurfaceGeometryAccess(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "render surface geometry access";
  result.hard_fail = true;

  const std::array<std::string_view, 7> render_tus = {
      "src/workspace/render/WorkspaceShellRenderOverlay.cpp",
      "src/workspace/render/WorkspaceShellRenderTextInput.cpp",
      "src/workspace/render/WorkspaceShellRenderSidebar.cpp",
      "src/workspace/render/WorkspaceShellRenderBottomPanel.cpp",
      "src/workspace/render/WorkspaceShellHoverPopup.cpp",
      "src/workspace/render/WorkspaceShellHoverTargets.cpp",
      "src/workspace/render/DebugPaneRender.cpp",
  };
  const std::regex compute_layout_pattern(R"(\bComputeLayout\s*\()");
  const std::regex direct_window_size_pattern(R"(context_\.window_size\b)");
  for (const std::string_view relative_path : render_tus) {
    const std::filesystem::path path = repo_root / relative_path;
    if (!RequireRuleTarget(result, path)) {
      continue;
    }
    const std::string text = ReadText(path);
    AppendViolations(result, path, text, compute_layout_pattern,
                     "render geometry must come from FrameToken/prepared layout, not ComputeLayout");
    AppendViolations(result, path, text, direct_window_size_pattern,
                     "render geometry must not read context_.window_size directly");
  }
  return result;
}

RuleResult CheckPerClipRenderPathDoesNotRunFramePrep(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "per-clip render path avoids frame prep";
  result.hard_fail = true;

  const std::filesystem::path app_cpp = repo_root / "src/app/Application.cpp";
  const std::filesystem::path shell_render_cpp = repo_root / "src/workspace/render/WorkspaceShellRender.cpp";
  const std::string app_text = ReadRuleTarget(result, app_cpp);
  const std::string shell_render_text = ReadRuleTarget(result, shell_render_cpp);
  const std::vector<bool> app_is_code = BuildCodeMask(app_text);
  const std::vector<bool> shell_is_code = BuildCodeMask(shell_render_text);

  const std::regex partial_loop_pattern(
      R"(for\s*\(\s*const SDL_Rect& merged_clip\s*:\s*dirty_region_analysis\.merged_clip_rects\s*\)\s*\{)");
  std::smatch partial_loop_match;
  if (std::regex_search(app_text, partial_loop_match, partial_loop_pattern)) {
    const std::size_t loop_start =
        static_cast<std::size_t>(partial_loop_match.position()) + partial_loop_match.length() - 1;
    std::size_t depth = 0;
    std::size_t loop_end = app_text.size();
    for (std::size_t i = loop_start; i < app_text.size(); ++i) {
      if (i < app_is_code.size() && !app_is_code[i]) {
        continue;
      }
      if (app_text[i] == '{') {
        ++depth;
      } else if (app_text[i] == '}') {
        --depth;
        if (depth == 0) {
          loop_end = i;
          break;
        }
      }
    }
    const std::string loop_body = app_text.substr(loop_start, loop_end - loop_start + 1);
    if (loop_body.find("PrepareFrameOnce(") != std::string::npos ||
        loop_body.find("PrepareRenderFrame(") != std::string::npos) {
      result.violations.push_back(Violation{
          .path = app_cpp,
          .line = LineNumberAt(app_text, loop_start),
          .message = "partial-clip render loop must not invoke frame prep; prepare once before the loop",
      });
    }
  } else {
    result.violations.push_back(Violation{
        .path = app_cpp,
        .line = 1,
        .message = "partial-clip render loop not found in Application.cpp; this lint's anchor "
                   "moved and it is now vacuous — repoint it rather than leaving it green",
    });
  }

  const std::regex render_clip_pattern(
      R"(WorkspaceShell::RenderClip\s*\([^)]*\)\s*\{)");
  std::smatch render_clip_match;
  if (std::regex_search(shell_render_text, render_clip_match, render_clip_pattern)) {
    const std::size_t body_start =
        static_cast<std::size_t>(render_clip_match.position()) + render_clip_match.length() - 1;
    std::size_t depth = 0;
    std::size_t body_end = shell_render_text.size();
    for (std::size_t i = body_start; i < shell_render_text.size(); ++i) {
      if (i < shell_is_code.size() && !shell_is_code[i]) {
        continue;
      }
      if (shell_render_text[i] == '{') {
        ++depth;
      } else if (shell_render_text[i] == '}') {
        --depth;
        if (depth == 0) {
          body_end = i;
          break;
        }
      }
    }
    const std::string body =
        shell_render_text.substr(body_start, body_end - body_start + 1);
    const std::array<std::string_view, 5> forbidden{
        "PrepareFrameOnce(",
        "PrepareRenderFrame(",
        "ComputeLayout(",
        "NormalizeEditorSplitTree(",
        "RenderViewModelBuilder(",
    };
    for (const std::string_view needle : forbidden) {
      if (body.find(needle) == std::string::npos) {
        continue;
      }
      result.violations.push_back(Violation{
          .path = shell_render_cpp,
          .line = LineNumberAt(shell_render_text, body_start),
          .message = "RenderClip must not run frame prep/layout/normalization/view-model construction",
      });
      break;
    }
  } else {
    result.violations.push_back(Violation{
        .path = shell_render_cpp,
        .line = 1,
        .message = "WorkspaceShell::RenderClip not found; this lint's anchor moved and it is "
                   "now vacuous — repoint it rather than leaving it green",
    });
  }

  return result;
}

RuleResult CheckPersistenceFileIoBoundary(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "persistence file-io boundary (workspace raw-stream ratchet)";
  result.hard_fail = true;
  constexpr std::array<std::string_view, 3> kAllowedFiles = {
      "src/workspace/persistence/PersistenceService.cpp",
      "src/workspace/control/ControlChannelService.cpp",
      "src/workspace/lsp/LspService.cpp",
  };
  const std::regex io_pattern(R"(\b(?:std::)?(?:ifstream|ofstream|fstream|fopen)\b)");

  for (const auto& entry : std::filesystem::recursive_directory_iterator(repo_root / "src/workspace")) {
    if (!entry.is_regular_file()) {
      continue;
    }
    const std::string ext = entry.path().extension().string();
    if (ext != ".h" && ext != ".hpp" && ext != ".cpp" && ext != ".inc") {
      continue;
    }
    const std::string rel = entry.path().lexically_normal().lexically_relative(
                                repo_root.lexically_normal()).generic_string();
    bool allowed = false;
    for (const std::string_view allowed_file : kAllowedFiles) {
      if (rel == allowed_file) {
        allowed = true;
        break;
      }
    }
    if (allowed) {
      continue;
    }
    const std::string text = ReadText(entry.path());
    AppendCodeMaskRegexViolations(
        result, entry.path(), text, io_pattern,
        "raw file-stream I/O in workspace code: persisted state routes through "
        "PersistenceService/PersistedRecord{Reader,Writer}; other file access goes through "
        "util/TextFileIO or platform helpers (or grow this rule's documented allowlist)");
  }
  return result;
}

RuleResult CheckEveryActionIdIsReachable(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "every ActionId is reachable from a surface AND handled by a dispatch";
  result.hard_fail = true;

  const std::filesystem::path types_header = repo_root / "src/workspace/actions/WorkspaceActionTypes.h";
  const std::string types_text = ReadRuleTarget(result, types_header);
  const std::size_t enum_at = types_text.find("enum class ActionId");
  const std::size_t open_brace =
      enum_at == std::string::npos ? std::string::npos : types_text.find('{', enum_at);
  const std::size_t close_brace =
      open_brace == std::string::npos ? std::string::npos : types_text.find('}', open_brace);
  if (close_brace == std::string::npos) {
    result.violations.push_back(Violation{
        .path = types_header,
        .line = 1,
        .message = "could not locate `enum class ActionId`; this lint's anchor moved and it is "
                   "now vacuous — repoint it rather than leaving it green",
    });
    return result;
  }

  std::vector<std::string> action_names;
  {
    const std::string body = types_text.substr(open_brace + 1, close_brace - open_brace - 1);
    const std::vector<bool> body_is_code = BuildCodeMask(body);
    const std::regex name_pattern(R"(\b([A-Z][A-Za-z0-9_]*)\b\s*(?:=[^,]*)?,)");
    for (std::sregex_iterator it(body.begin(), body.end(), name_pattern), last; it != last; ++it) {
      const std::size_t start = static_cast<std::size_t>(it->position(1));
      if (start < body_is_code.size() && !body_is_code[start]) {
        continue;
      }
      action_names.push_back(it->str(1));
    }
  }
  if (action_names.size() < 20) {
    result.violations.push_back(Violation{
        .path = types_header,
        .line = 1,
        .message = "parsed only " + std::to_string(action_names.size()) +
                   " ActionId enumerators; the declaration shape changed and this lint has gone "
                   "vacuous — fix the scan rather than deleting the rule",
    });
    return result;
  }

  std::set<std::string> produced;
  std::set<std::string> handled;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(repo_root / "src")) {
    if (!entry.is_regular_file()) {
      continue;
    }
    const std::filesystem::path extension = entry.path().extension();
    if (extension != ".cpp" && extension != ".h" && extension != ".inc") {
      continue;
    }
    const std::string text = ReadText(entry.path());
    if (text.find("ActionId::") == std::string::npos) {
      continue;
    }
    const std::vector<bool> is_code = BuildCodeMask(text);
    const std::regex use_pattern(R"(\bActionId::([A-Za-z0-9_]+)\b)");
    for (std::sregex_iterator it(text.begin(), text.end(), use_pattern), last; it != last; ++it) {
      const std::size_t start = static_cast<std::size_t>(it->position());
      if (start < is_code.size() && !is_code[start]) {
        continue;
      }
      const std::size_t line_start = text.rfind('\n', start) + 1;
      const std::string prefix = text.substr(line_start, start - line_start);
      if (std::regex_search(prefix, std::regex(R"(^\s*case\s*$)"))) {
        handled.insert(it->str(1));
        continue;
      }
      produced.insert(it->str(1));
    }
  }

  if (handled.empty()) {
    result.violations.push_back(Violation{
        .path = repo_root / "src",
        .line = 1,
        .message = "no `case ActionId::` labels found under src/, so the handled half of this "
                   "rule would pass for every action — repoint the scan rather than leaving it "
                   "green",
    });
    return result;
  }

  for (const std::string& name : action_names) {
    if (produced.count(name) == 0) {
      result.violations.push_back(Violation{
          .path = types_header,
          .line = 1,
          .message = "ActionId::" + name +
                     " is only ever named in `case` labels — nothing produces it, so it cannot be "
                     "invoked. Give it a command spec, a keybinding, a menu item, or a "
                     "context-menu call site; or delete it if the behavior is not wanted",
      });
    }
    if (handled.count(name) == 0) {
      result.violations.push_back(Violation{
          .path = types_header,
          .line = 1,
          .message = "ActionId::" + name +
                     " is produced but never appears as a `case ActionId::" + name +
                     ":` — the action can be invoked from the palette, a menu or a key binding "
                     "and nothing acts on it, silently. Give it a dispatch arm, or delete the "
                     "surface that offers it",
      });
    }
  }
  return result;
}

}
