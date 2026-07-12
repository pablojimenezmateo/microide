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

RuleResult CheckPluginFieldReadsAreMetamethodProtected(const std::filesystem::path& repo_root) {
  // Companion to the luaL_error rule, from the read-in direction. A plugin controls
  // the tables the host harvests and can install a raising __index metamethod
  // (setmetatable+error are in the exposed base lib). The metamethod-capable field
  // reads lua_getfield / lua_gettable / lua_geti would then longjmp (Lua links as C)
  // to the enclosing lua_pcall, skipping the destructors of any C++ locals alive in
  // the harvest loop (std::vector / std::string / std::filesystem::path) — the same
  // UB + leak the luaL_error rule guards against. All field reads in src/plugin must
  // instead go through lua_interop::GetFieldProtected, which runs the lookup inside a
  // nested lua_pcall. Indexed sequence reads already use the raw, metamethod-free
  // lua_rawgeti (allowed). Only PluginLuaInterop.cpp is exempt: it defines
  // GetFieldProtected + its GetFieldTrampoline, the one sanctioned site.
  RuleResult result;
  result.label = "plugin field reads are metamethod-protected (no raw lua_getfield/gettable/geti)";
  result.hard_fail = true;
  const std::filesystem::path plugin_dir = repo_root / "src/plugin";
  if (!std::filesystem::exists(plugin_dir)) {
    return result;
  }
  const std::regex raw_field_read(R"(\blua_get(field|table|i)\s*\()");
  for (const auto& entry : std::filesystem::recursive_directory_iterator(plugin_dir)) {
    if (!entry.is_regular_file()) {
      continue;
    }
    const std::string ext = entry.path().extension().string();
    if (ext != ".cpp" && ext != ".inc" && ext != ".h" && ext != ".hpp") {
      continue;
    }
    if (entry.path().filename() == "PluginLuaInterop.cpp") {
      continue;  // sanctioned home of GetFieldProtected / GetFieldTrampoline
    }
    const std::string text = ReadText(entry.path());
    AppendCodeMaskRegexViolations(
        result, entry.path(), text, raw_field_read,
        "harvest plugin table fields via lua_interop::GetFieldProtected; raw "
        "lua_getfield/lua_gettable/lua_geti invoke __index and can longjmp over live "
        "C++ locals (UB + leak). Indexed reads use lua_rawgeti. See PluginLuaInterop.h");
  }
  return result;
}

RuleResult CheckCoreIsNetworkFree(const std::filesystem::path& repo_root) {
  // Hard product guarantee: the microide core binary makes no network calls and
  // links no networking client libraries. Plugins may reach the network in their
  // own sandboxed subprocess (declared `network` capability); the host only ever
  // decodes/caches/blits the plugin's local output (e.g. the Phase E raster path
  // accepts plugin-supplied bytes, never a URL). This bans the unambiguous
  // HTTP/TLS/DNS client tokens. It deliberately does NOT ban socket()/connect()/
  // AF_INET, which have legitimate non-network uses here: the control channel
  // speaks AF_UNIX (local IPC) and the subprocess sandbox names AF_INET only to
  // BLOCK it in its seccomp filter.
  RuleResult result;
  result.label = "core binary is network-free";
  result.hard_fail = true;
  const std::filesystem::path src_dir = repo_root / "src";
  if (!std::filesystem::exists(src_dir)) {
    return result;
  }
  const std::regex network_client(
      R"((curl_easy|curl_global|curl/curl\.h|libcurl|gethostbyname|getaddrinfo|openssl/|SSL_CTX|SSL_connect|<arpa/inet|httplib|cpr::|boost/asio|<asio))");
  for (const auto& entry : std::filesystem::recursive_directory_iterator(src_dir)) {
    if (!entry.is_regular_file()) {
      continue;
    }
    const std::string ext = entry.path().extension().string();
    if (ext != ".cpp" && ext != ".inc" && ext != ".h" && ext != ".hpp") {
      continue;
    }
    const std::string text = ReadText(entry.path());
    AppendCodeMaskRegexViolations(
        result, entry.path(), text, network_client,
        "the core binary must not link or call a network client (HTTP/TLS/DNS); plugins "
        "reach the network in their own sandboxed subprocess and the host consumes only "
        "their local output. See the network-isolation guarantee in CLAUDE.md / the plan.");
  }
  return result;
}

RuleResult CheckNoUnwiredMcpScaffolding(const std::filesystem::path& repo_root) {
  // The MCP-tool contribution scaffolding (ContributedMcpTool, McpToolRuntime,
  // McpToolRegistration, ParseMcpToolRegistration, RegisterMcpTool, InvokeMcpTool)
  // was a complete but never-dispatched stub: it duplicated the shape of the live
  // contribution/query/teardown machinery without being wired into the Lua API
  // registry or host storage, so it could never run. It was deleted as dead code.
  // This guards against silently re-adding an unwired MCP stub; a real MCP feature
  // is new work (registry verb + host storage + teardown), not a resurrection.
  RuleResult result;
  result.label = "no unwired MCP-tool scaffolding in src/plugin";
  result.hard_fail = true;
  const std::filesystem::path plugin_dir = repo_root / "src/plugin";
  if (!std::filesystem::exists(plugin_dir)) {
    return result;
  }
  const std::regex mcp_token(R"(\b(Mcp|McpTool|RegisterMcpTool|InvokeMcpTool)\b)");
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
        result, entry.path(), text, mcp_token,
        "the never-dispatched MCP-tool contribution stub was deleted as dead code; do not "
        "re-add unwired MCP scaffolding. A real MCP feature needs a Lua-API registry verb, "
        "host runtime storage, and teardown wiring.");
  }
  return result;
}

std::vector<RuleResult> RunPluginArchitectureRules(const std::filesystem::path& repo_root) {
  std::vector<RuleResult> results;
  const auto run = [&](auto&& fn) { results.push_back(fn(repo_root)); };
  run(CheckNoProjectLocalPluginDiscovery);
  run(CheckSinglePluginReloadPerActivation);
  run(CheckEssentialEditorCppModulesDoNotTouchLuaState);
  run(CheckPluginTranslationUnitSize);
  run(CheckPluginLuaErrorDoesNotLongjmpOverCppLocals);
  run(CheckPluginFieldReadsAreMetamethodProtected);
  run(CheckCoreIsNetworkFree);
  run(CheckNoUnwiredMcpScaffolding);
  return results;
}

}  // namespace microide::tests::architecture
