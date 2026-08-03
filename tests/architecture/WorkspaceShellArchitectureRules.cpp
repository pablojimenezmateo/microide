#include "architecture/WorkspaceShellArchitectureRules.h"

#include "architecture/ArchitectureFileScanner.h"

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
      // Sanctioned exception: the test-only TestAccess backdoor. It is now
      // declared unconditionally (not under #ifdef MICROIDE_TESTING) so the
      // shared core object library compiles an ODR-identical WorkspaceShell for
      // the production and test binaries. Exempt the friend by the befriended
      // type name rather than by guard.
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

RuleResult CheckThrowingStoParsers(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "try/std::sto parsing";
  // Hard: CLAUDE.md and openspec/specs/workspace-architecture list "numeric token
  // parsing uses util/Parse.h; no try/catch around std::sto*" as a load-bearing
  // invariant, and the tree is clean of it. Warn-only meant a reintroduced
  // throwing parser printed a line nobody reads and shipped anyway.
  result.hard_fail = true;
  // The non-throwing parse policy also covers the developer tools and the perf
  // harness: benchmark CLI parsers and /proc sample parsers are part of the speed
  // regression workflow, and divergent parsing style tempts future agents to copy the
  // old try/catch-std::sto pattern back into production code.
  const std::array<std::filesystem::path, 3> roots = {
      repo_root / "src", repo_root / "tools", repo_root / "tests/perf"};
  for (const auto& root : roots) {
    std::error_code root_ec;
    if (!std::filesystem::is_directory(root, root_ec) || root_ec) {
      continue;
    }
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
      if (!entry.is_regular_file() || entry.path().extension() != ".cpp") {
        continue;
      }
      const std::string text = ReadText(entry.path());
      const auto offsets = FindTryCatchStoViolations(text);
      for (const std::size_t offset : offsets) {
        result.violations.push_back(Violation{
            .path = entry.path(),
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
  // The DAP transport must not narrow an adapter-controlled request_seq/seq with a bare
  // static_cast<int>(...AsInt()): an out-of-int-range value from a hostile/buggy adapter
  // wraps and can collide with a live pending request or forge an initialize match. All
  // inbound seq narrowing routes through DapResponseSeqInRange. (TD-2026-07-16-44.)
  const std::filesystem::path target =
      repo_root / "src/workspace/debug/WorkspaceDapClientInternal.h";
  if (!RequireRuleTarget(result, target)) {
    return result;
  }
  const std::string text = ReadText(target);
  // Any static_cast<int>( ... request_seq ... ) or ( ... ["seq"] ... AsInt()) that is
  // not the checked helper is a violation. Match the raw narrowing shapes directly.
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
  // tools/run-checks.sh is the blessed test entry point: it scopes the build, writes
  // the deterministic /tmp log, and applies sanitizer options + TSAN suppressions. Any
  // other tools/*.sh calling ctest directly is a split-brain gate that bypasses those
  // controls. Only a `ctest` token at the start of a command word counts (a comment or
  // a substring like "run-checks" does not). (TD-2026-07-16-28.)
  const std::filesystem::path tools_dir = repo_root / "tools";
  std::error_code dir_ec;
  if (!std::filesystem::is_directory(tools_dir, dir_ec) || dir_ec) {
    return result;
  }
  // A ctest invocation: line start or `;`/`|`/`&`/`(`/`do`/`then` boundary, then ctest.
  const std::regex ctest_invocation(R"((^|[;&|(]|\bdo\b|\bthen\b)\s*ctest\b)");
  for (const auto& entry : std::filesystem::directory_iterator(tools_dir)) {
    if (!entry.is_regular_file() || entry.path().extension() != ".sh") {
      continue;
    }
    if (entry.path().filename() == "run-checks.sh") {
      continue;  // the sanctioned wrapper
    }
    const std::string text = ReadText(entry.path());
    // Scan line by line so we can skip comment lines (leading # after whitespace).
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
  result.label = "one-shot SDL wake producers route through the checked pusher";
  result.hard_fail = true;
  // These producers make a result ready in shared state and then push a neutral SDL
  // wake. A bare SDL_PushEvent() is fire-and-forget: a rejected push strands the ready
  // state until unrelated input. They must route through util::PushSdlWake, which
  // latches the shared "wake owed" bit that CurrentIdleWaitState consumes as a fallback
  // wait. (TD-2026-07-16-54.) Any bare SDL_PushEvent in these files is a violation.
  static constexpr std::array<const char*, 5> kProducerFiles = {
      "src/project/GitBlameService.cpp",
      "src/platform/ControlSocketServer.cpp",
      "src/workspace/coordinators/WorkspaceProjectDialogCoordinator.cpp",
      "src/app/BackgroundTaskCounter.cpp",
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
                     "one-shot wake producers must use util::PushSdlWake, not a bare "
                     "SDL_PushEvent (which drops the wake with no backstop on failure)");
  }
  return result;
}

RuleResult CheckPerfScenariosUseNonThrowingFilesystemProbes(
    const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "perf scenarios use non-throwing filesystem probes";
  result.hard_fail = true;
  // The perf runner is the speed-regression oracle: fixture discovery must degrade to
  // a labelled "fixture absent" message, never throw a raw std::filesystem_error. A
  // single-arg (throwing) exists()/is_directory() — no error_code — is the violation;
  // the two-arg ec overload has a comma and is exempt. Use PathExistsNoThrow /
  // DirectoryExistsNoThrow instead. (TD-2026-07-16-29.)
  const std::filesystem::path perf_dir = repo_root / "tests/perf";
  std::error_code dir_ec;
  if (!std::filesystem::is_directory(perf_dir, dir_ec) || dir_ec) {
    return result;
  }
  const std::regex throwing_probe(
      R"(std::filesystem::(exists|is_directory)\s*\([^,()]*\))");
  for (const auto& entry : std::filesystem::recursive_directory_iterator(perf_dir)) {
    if (!entry.is_regular_file() || entry.path().extension() != ".cpp") {
      continue;
    }
    const std::string text = ReadText(entry.path());
    AppendViolations(result, entry.path(), text, throwing_probe,
                     "perf fixture probes must use PathExistsNoThrow/DirectoryExistsNoThrow, "
                     "not a throwing single-arg std::filesystem::exists/is_directory");
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

// Ratchet-only cap on the number of `WorkspaceShell*.cpp` translation units.
// File decomposition has reached a plateau (see dev-docs/project/known-tech-debt.md
// item #16); new shell-adjacent behavior should land on a service, not on
// a new `WorkspaceShell*.cpp` companion. If a migration removes one of
// these files, lower the cap; do not raise it. This guardrail blocks
// regressions; it is not the desired end state.

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
      R"(for\s*\(\s*const SDL_Rect& clip_rect\s*:\s*dirty_region_analysis\.merged_clip_rects\s*\)\s*\{)");
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
    // Loud-missing-target guard: both halves of this rule are anchored on an
    // exact source shape, and a rename/restructure of either would make the rule
    // silently inspect nothing while still reporting green. Fail instead so the
    // anchor gets repointed. (Three hard rules were found passing vacuously for
    // exactly this reason in the 2026-07-26 lint sweep.)
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
  // TD-2026-07-17-032. Workspace-state persistence (project state, user config,
  // session restore) routes through PersistenceService + PersistedRecordReader/
  // Writer; general text file access goes through util/TextFileIO or
  // platform-layer helpers. The previous incarnation of this rule matched raw
  // streams only when a "workspace|session|config" literal appeared inside the
  // open() argument on the same line, and exempted four file paths that no
  // longer exist — i.e. it was vacuous. The precise, load-bearing form is an
  // allowlist ratchet: raw file-stream I/O is banned in src/workspace/* except
  // the enumerated TUs below, so any NEW direct open must either use a
  // sanctioned seam or grow this list in review.
  RuleResult result;
  result.label = "persistence file-io boundary (workspace raw-stream ratchet)";
  result.hard_fail = true;
  // Each allowlisted TU opens files for a documented non-persisted-state purpose:
  // - PersistenceService.cpp: the sanctioned home of persisted-state I/O (today it
  //   routes through persistence/PersistedRecord{Reader,Writer}, but the seam is here).
  // - ControlChannelService.cpp: control-spec snapshot read + response mirror
  //   (external control channel artifacts, not workspace/session/config state).
  // - LspService.cpp: server-requested WorkspaceEdit resource-op file creation
  //   (project source files, not state).
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

// Task 4.1: No synchronous blocking-wait primitives in workspace code.
// Blocking on a subprocess from the main thread stalls the event loop.
// All git/lint subprocesses must be dispatched through ProjectBackgroundExecutor.


// Every ActionId must be REACHABLE by a user. An action is dispatched by the
// executors and gated by the availability table, both of which are `switch`
// statements — so an action can be fully implemented, compile cleanly, appear in
// three switches, and still be impossible to invoke because nothing ever
// *produces* it: no command spec, no keybinding, no menu item, no context-menu
// call site.
//
// That is not hypothetical. `ActionId::ToggleFullscreen` reached
// Application.cpp's SDL_SetWindowFullscreen call and was unreachable; so were
// `InlineCompletion` and the two `DebugBreakpointEdit*` actions.
//
// "Produced" is deliberately broad: any mention of `ActionId::X` on a line that
// is not a `case` label counts, so context-menu-only actions (the git sidebar
// entry actions, which are documented as such and are constructed directly in
// the mouse coordinators) pass without an allowlist. The rule only catches the
// case where the ONLY mentions anywhere are `case` labels.
RuleResult CheckEveryActionIdIsReachable(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "every ActionId is reachable from some user-facing surface";
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
        continue;  // a name inside a comment is not an enumerator
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

  // Collect, per action, whether any non-`case` mention exists anywhere in src/.
  std::set<std::string> produced;
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
      // `case ActionId::X:` is a handler, not a producer. Fall-through groups of
      // case labels are handled the same way.
      if (std::regex_search(prefix, std::regex(R"(^\s*case\s*$)"))) {
        continue;
      }
      produced.insert(it->str(1));
    }
  }

  for (const std::string& name : action_names) {
    if (produced.count(name) != 0) {
      continue;
    }
    result.violations.push_back(Violation{
        .path = types_header,
        .line = 1,
        .message = "ActionId::" + name +
                   " is only ever named in `case` labels — nothing produces it, so it cannot be "
                   "invoked. Give it a command spec, a keybinding, a menu item, or a context-menu "
                   "call site; or delete it if the behavior is not wanted",
    });
  }
  return result;
}

}  // namespace microide::tests::architecture
