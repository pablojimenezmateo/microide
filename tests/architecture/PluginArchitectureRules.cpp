#include "architecture/PluginArchitectureRules.h"

#include "architecture/ArchitectureFileScanner.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <regex>

namespace microide::tests::architecture {

RuleResult CheckPluginTranslationUnitSize(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "plugin translation unit size";
  result.hard_fail = true;
  for (const auto& entry : std::filesystem::directory_iterator(repo_root / "src/plugin")) {
    if (!entry.is_regular_file() || entry.path().extension() != ".cpp") {
      continue;
    }
    const std::size_t lines = CountCodeLinesInFile(entry.path());
    if (lines > 800) {
      result.violations.push_back(Violation{
          .path = entry.path(),
          .line = 1,
          .message = "plugin translation units should stay at or below 800 code lines "
                     "(comments and blank lines excluded)",
      });
    }
  }
  return result;
}

RuleResult CheckPluginDrainBeforeTeardown(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "plugin drain-before-teardown";
  result.hard_fail = true;
  for (const auto& entry : std::filesystem::directory_iterator(repo_root / "src/plugin")) {
    if (!entry.is_regular_file()) {
      continue;
    }
    const std::string ext = entry.path().extension().string();
    if (ext != ".cpp" && ext != ".inc") {
      continue;
    }
    const std::string text = ReadText(entry.path());
    const std::vector<bool> is_code = BuildCodeMask(text);
    // Target only the public-API teardown call sites (`impl_->TearDownPlugins(...)`),
    // not the definition or the inner leaf helper. Those leaf calls are reached
    // exclusively through these call sites, so guarding here is sufficient.
    const std::regex teardown_pattern(R"(impl_->\s*TearDownPlugins\s*\()");
    for (std::sregex_iterator it(text.begin(), text.end(), teardown_pattern), end; it != end;
         ++it) {
      const auto teardown_pos = static_cast<std::size_t>(it->position());
      if (teardown_pos < is_code.size() && !is_code[teardown_pos]) {
        continue;
      }
      // Drain seam call must appear within the previous 12 lines and after any
      // earlier teardown call in the same translation unit. The window is small
      // enough to keep the check tight without parsing function boundaries.
      std::size_t scan_start = teardown_pos;
      std::size_t lines_back = 0;
      while (scan_start > 0 && lines_back < 12) {
        --scan_start;
        if (text[scan_start] == '\n') {
          ++lines_back;
        }
      }
      const std::string_view window(text.data() + scan_start, teardown_pos - scan_start);
      const bool drain_seen = window.find("DrainAsyncProcessWorkers") != std::string_view::npos ||
                              window.find("DrainAndJoinWorkers") != std::string_view::npos;
      if (!drain_seen) {
        result.violations.push_back(Violation{
            .path = entry.path(),
            .line = LineNumberAt(text, teardown_pos),
            .message = "TearDownPlugins must be preceded by a drain seam call "
                       "(DrainAsyncProcessWorkers / DrainAndJoinWorkers) within the same path",
        });
      }
    }
  }
  return result;
}

RuleResult CheckSinglePluginReloadPerActivation(const std::filesystem::path& repo_root) {
  // The reactivation branch of ProjectCatalogService::ActivateProjectState SHALL NOT
  // call reload_plugins_for_current_project / ReloadPluginsForCurrentProject. The
  // first-activation branch routes through initialize_current_project, which already
  // performs exactly one reload internally. Reintroducing a direct call here would
  // restore the back-to-back reload regression the change was created to fix.
  RuleResult result;
  result.label = "single plugin reload per ActivateProjectState";
  result.hard_fail = true;
  const std::filesystem::path service_cpp = repo_root / "src/workspace/ProjectCatalogService.cpp";
  if (!std::filesystem::exists(service_cpp)) {
    return result;
  }
  const std::string text = ReadText(service_cpp);
  const std::vector<bool> is_code = BuildCodeMask(text);
  const std::regex activate_pattern(R"(ProjectCatalogService::ActivateProjectState\s*\([^)]*\)\s*\{)");
  std::smatch match;
  if (!std::regex_search(text, match, activate_pattern)) {
    return result;
  }
  const std::size_t body_start = static_cast<std::size_t>(match.position()) + match.length() - 1;
  // Walk braces to find the matching close.
  std::size_t depth = 0;
  std::size_t body_end = text.size();
  for (std::size_t i = body_start; i < text.size(); ++i) {
    if (i < is_code.size() && !is_code[i]) {
      continue;
    }
    if (text[i] == '{') {
      ++depth;
    } else if (text[i] == '}') {
      --depth;
      if (depth == 0) {
        body_end = i;
        break;
      }
    }
  }
  const std::regex reload_pattern(
      R"((reload_plugins_for_current_project|ReloadPluginsForCurrentProject)\s*\()");
  for (std::sregex_iterator it(text.begin() + static_cast<std::ptrdiff_t>(body_start),
                                text.begin() + static_cast<std::ptrdiff_t>(body_end),
                                reload_pattern),
       end;
       it != end; ++it) {
    const std::size_t pos = body_start + static_cast<std::size_t>(it->position());
    if (pos < is_code.size() && !is_code[pos]) {
      continue;
    }
    result.violations.push_back(Violation{
        .path = service_cpp,
        .line = LineNumberAt(text, pos),
        .message = "ActivateProjectState must not call reload_plugins_for_current_project; "
                   "first init goes through initialize_current_project, reactivation through "
                   "refresh_plugin_surfaces_for_reactivation",
    });
  }
  return result;
}

RuleResult CheckEssentialEditorCppModulesDoNotTouchLuaState(
    const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "Lua VM pointers stay out of language/fold/shape helpers";
  result.hard_fail = true;
  const std::array<std::string_view, 4> paths = {
      "src/workspace/WorkspaceLanguageContract.cpp",
      "src/editor/FoldingModel.cpp",
      "src/editor/IndentGuides.cpp",
      "src/editor/SnippetEngine.cpp",
  };
  const std::regex lua_pointer(R"(\blua_State\s*\*)");
  for (const std::string_view relative : paths) {
    const std::filesystem::path path = repo_root / relative;
    if (!std::filesystem::exists(path)) {
      result.violations.push_back(Violation{
          .path = path,
          .line = 1,
          .message = "expected editor essential translation unit",
      });
      continue;
    }
    const std::string file_text = ReadText(path);
    AppendCodeMaskRegexViolations(
        result, path, file_text, lua_pointer,
        "WorkspaceLanguageContract/FoldingModel/IndentGuides/SnippetEngine must stay Lua-free "
        "at the type level (lua_State* leaks implementation coupling)");
  }
  return result;
}


RuleResult CheckNoProjectLocalPluginDiscovery(const std::filesystem::path& repo_root) {
  RuleResult result;
  result.label = "home-only plugin discovery";
  result.hard_fail = true;
  const std::array<std::string_view, 4> forbidden_literals = {
      ".microide/plugins",
      "project_local",
      "project-scope",
      "project-local plugin",
  };
  const std::regex discover_with_project_root(
      R"(DiscoverPluginRoots\s*\(\s*[^)]*(current_project_root|project_root|ProjectRoot))");

  const auto scan_file = [&](const std::filesystem::path& path) {
    const std::string text = ReadText(path);
    const std::vector<bool> is_code = BuildCodeMask(text);
    for (const std::string_view literal : forbidden_literals) {
      for (const std::size_t pos : FindCodeLiteralOccurrences(text, literal)) {
        if (pos < is_code.size() && !is_code[pos]) {
          continue;
        }
        result.violations.push_back(Violation{
            .path = path,
            .line = LineNumberAt(text, pos),
            .message = "plugin discovery must stay home-only; remove project-local seam: " +
                       std::string(literal),
        });
      }
    }
    AppendCodeMaskRegexViolations(
        result, path, text, discover_with_project_root,
        "plugin discovery must not accept a project root for install scanning");
  };

  const std::filesystem::path plugin_dir = repo_root / "src/plugin";
  if (std::filesystem::exists(plugin_dir)) {
    for (const auto& entry : std::filesystem::recursive_directory_iterator(plugin_dir)) {
      if (!entry.is_regular_file()) {
        continue;
      }
      const std::string ext = entry.path().extension().string();
      if (ext != ".h" && ext != ".hpp" && ext != ".cpp" && ext != ".inc") {
        continue;
      }
      scan_file(entry.path());
    }
  }

  const std::array<std::string_view, 2> workspace_plugin_files = {
      "src/workspace/WorkspacePluginAssetMonitor.cpp",
      "src/workspace/WorkspacePluginAssetMonitor.h",
  };
  for (const std::string_view relative : workspace_plugin_files) {
    const std::filesystem::path path = repo_root / relative;
    if (std::filesystem::exists(path)) {
      scan_file(path);
    }
  }
  return result;
}

RuleResult CheckPluginLuaErrorDoesNotLongjmpOverCppLocals(const std::filesystem::path& repo_root) {
  // Raising a Lua error is a C longjmp (the project links the C build of Lua), so
  // `luaL_error` skips the destructors of any C++ automatic objects still alive on
  // the stack — undefined behaviour and a leak whenever a std::string/std::vector
  // local exists. All Lua-error raising in src/plugin must instead copy the message
  // via lua_error_util::PushMessage and then `lua_error(state)` only after those
  // locals have gone out of scope (see src/plugin/LuaError.h). Argument-entry
  // validation that longjmps before any C++ local is constructed (luaL_checktype,
  // luaL_checkstring, luaL_argerror) stays allowed; only luaL_error is banned
  // because it is the one routinely reached after objects are built.
  RuleResult result;
  result.label = "plugin Lua errors do not longjmp over C++ locals";
  result.hard_fail = true;
  const std::filesystem::path plugin_dir = repo_root / "src/plugin";
  if (!std::filesystem::exists(plugin_dir)) {
    return result;
  }
  const std::regex lua_error_call(R"(\bluaL_error\s*\()");
  for (const auto& entry : std::filesystem::recursive_directory_iterator(plugin_dir)) {
    if (!entry.is_regular_file()) {
      continue;
    }
    const std::string ext = entry.path().extension().string();
    if (ext != ".cpp" && ext != ".inc" && ext != ".h" && ext != ".hpp") {
      continue;
    }
    const std::string text = ReadText(entry.path());
    AppendCodeMaskRegexViolations(
        result, entry.path(), text, lua_error_call,
        "raise Lua errors via lua_error_util::PushMessage + lua_error after C++ locals "
        "destruct; luaL_error longjmps over them (UB + leak). See src/plugin/LuaError.h");
  }
  return result;
}

std::vector<RuleResult> RunPluginArchitectureRules(const std::filesystem::path& repo_root) {
  std::vector<RuleResult> results;
  const auto run = [&](auto&& fn) { results.push_back(fn(repo_root)); };
  run(CheckNoProjectLocalPluginDiscovery);
  run(CheckSinglePluginReloadPerActivation);
  run(CheckEssentialEditorCppModulesDoNotTouchLuaState);
  run(CheckPluginDrainBeforeTeardown);
  run(CheckPluginTranslationUnitSize);
  run(CheckPluginLuaErrorDoesNotLongjmpOverCppLocals);
  return results;
}

}  // namespace microide::tests::architecture
