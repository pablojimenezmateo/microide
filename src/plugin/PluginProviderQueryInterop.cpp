#include "plugin/PluginProviderQueryInterop.h"

#if MICROIDE_HAS_LUA_PLUGINS

#include <algorithm>
#include <limits>

#include "plugin/PluginLuaInterop.h"
#include "util/TextFileIO.h"

namespace microide::plugin::provider_query_interop {
namespace {

// Cap host-side harvests from plugin result tables. lua_rawlen already bounds the
// loop against a metamethod-driven __len, but a provider can still return a
// genuinely huge (or sparse-border-overstated) array within the 256 MB Lua budget;
// without a cap that stalls the worker thread and grows an unbounded host vector.
// Mirrors the clamps in the sidebar/diagnostics/language-provider harvesters.
constexpr lua_Integer kMaxCompletionCandidates = 20000;
constexpr lua_Integer kMaxCodeActions = 4096;
constexpr lua_Integer kMaxCodeActionArguments = 256;
constexpr lua_Integer kMaxDiscoveredTests = 20000;
constexpr lua_Integer kMaxTestRunResults = 20000;
constexpr lua_Integer kMaxScmEntries = 200000;
constexpr lua_Integer kMaxAnnotationLines = 200000;
constexpr lua_Integer kMaxAuthScopes = 4096;

// Per-QUERY aggregate ceilings across ALL matching providers. The per-provider
// caps above are an INPUT guard, not the final product size: a language can
// register many providers of a kind, each staying under its own table cap yet
// summing to millions of rows fed to the assist/annotation UI. Bound the harvested
// total per query so provider count does not multiply into unbounded host memory
// (TD-2026-07-17A-046).
constexpr lua_Integer kMaxAggregateCompletionCandidates = 20000;
constexpr lua_Integer kMaxAggregateCodeActions = 4096;
constexpr lua_Integer kMaxAggregateAnnotationLines = 200000;

// A discovered test's 1-based `line`, validated: a non-positive or out-of-int value is
// treated as ABSENT (0), never a wrapped positive that would navigate the Tests sidebar
// row to a wrong/huge line. (TD-2026-07-16-67.)
constexpr int ReadTestLineOrAbsent(lua_Integer raw) {
  if (raw <= 0 || raw > static_cast<lua_Integer>(std::numeric_limits<int>::max())) {
    return 0;
  }
  return static_cast<int>(raw);
}

// A test-run `duration_ms`, clamped to [0, INT_MAX] so a huge/negative Lua integer can
// never wrap into a misleading small/negative duration. (TD-2026-07-16-67.)
constexpr int ClampDurationMs(lua_Integer raw) {
  if (raw < 0) {
    return 0;
  }
  if (raw > static_cast<lua_Integer>(std::numeric_limits<int>::max())) {
    return std::numeric_limits<int>::max();
  }
  return static_cast<int>(raw);
}

}  // namespace

std::vector<PluginHost::CompletionCandidate> QueryCompletions(
    std::string_view language_id,
    const std::filesystem::path& path,
    std::size_t line,
    std::size_t column,
    std::string_view trigger_character,
    const std::vector<runtime_types::CompletionRuntime>& completion_runtimes,
    const std::function<const runtime_types::PluginInstance*(lua_State*)>& find_plugin_by_state,
    const std::function<void(lua_State*, const std::filesystem::path&)>& push_buffer_context,
    std::string* error_message) {
  std::vector<PluginHost::CompletionCandidate> results;
  for (const auto& provider : completion_runtimes) {
    if (provider.language_id != language_id) {
      continue;
    }
    if (static_cast<lua_Integer>(results.size()) >= kMaxAggregateCompletionCandidates) {
      break;  // aggregate budget reached across providers
    }
    lua_State* state = provider.state;
    const lua_interop::StackResetGuard stack_guard(state);
    lua_rawgeti(state, LUA_REGISTRYINDEX, provider.provide_ref);
    push_buffer_context(state, path);
    lua_interop::PushPosition(state, line, column);
    lua_pushlstring(state, trigger_character.data(), trigger_character.size());
    const runtime_types::PluginInstance* plugin = find_plugin_by_state(state);
    std::string call_error;
    if (plugin == nullptr || !plugin->runtime || !plugin->runtime->PCall(3, 1, &call_error)) {
      // Continue to later providers instead of discarding every earlier provider's
      // healthy candidates (TD-2026-07-17A-047). Queries run with
      // allow_registration=false, so `completion_runtimes` cannot reallocate across the
      // PCall and `provider` stays valid for the error record.
      lua_interop::AppendProviderFailure(error_message, "completion", provider.id, call_error);
      continue;
    }
    if (lua_istable(state, -1)) {
      // Bound the harvest by lua_rawlen and read entries with lua_rawgeti: the
      // result table arrives after PCall has disarmed the count-hook watchdog, so
      // an unbounded for(;;) + metamethod-invoking lua_geti over an adversarial
      // __index/__len would hang the worker thread or longjmp past native frames.
      const int array_index = lua_absindex(state, -1);
      const lua_Integer remaining =
          kMaxAggregateCompletionCandidates - static_cast<lua_Integer>(results.size());
      const lua_Integer count = std::min<lua_Integer>(
          std::min<lua_Integer>(static_cast<lua_Integer>(lua_rawlen(state, array_index)),
                                 kMaxCompletionCandidates),
          remaining);
      for (lua_Integer i = 1; i <= count; ++i) {
        lua_rawgeti(state, array_index, i);
        if (!lua_istable(state, -1)) {
          lua_pop(state, 1);
          continue;
        }
        PluginHost::CompletionCandidate candidate;
        candidate.label = lua_interop::ReadStringField(state, -1, "label");
        candidate.detail = lua_interop::ReadStringField(state, -1, "detail");
        candidate.documentation = lua_interop::ReadStringField(state, -1, "documentation");
        candidate.insert_text = lua_interop::ReadStringField(state, -1, "insert_text");
        if (candidate.insert_text.empty()) {
          candidate.insert_text = lua_interop::ReadStringField(state, -1, "insertText");
        }
        if (candidate.insert_text.empty()) {
          candidate.insert_text = candidate.label;
        }
        lua_interop::GetFieldProtected(state, -1, "is_snippet");
        if (lua_isboolean(state, -1)) {
          candidate.is_snippet = lua_toboolean(state, -1) != 0;
        }
        lua_pop(state, 1);
        lua_interop::GetFieldProtected(state, -1, "snippet");
        if (lua_isboolean(state, -1) && lua_toboolean(state, -1) != 0) {
          candidate.is_snippet = true;
        }
        lua_pop(state, 1);
        if (!candidate.label.empty()) {
          results.push_back(std::move(candidate));
        }
        lua_pop(state, 1);
      }
    }
    lua_pop(state, 1);
  }
  return results;
}

std::vector<PluginHost::CodeActionCandidate> QueryCodeActions(
    std::string_view language_id,
    const std::filesystem::path& path,
    std::size_t start_line,
    std::size_t start_column,
    std::size_t end_line,
    std::size_t end_column,
    const std::vector<runtime_types::CodeActionRuntime>& code_action_runtimes,
    const std::function<const runtime_types::PluginInstance*(lua_State*)>& find_plugin_by_state,
    const std::function<void(lua_State*, const std::filesystem::path&)>& push_buffer_context,
    std::string* error_message) {
  std::vector<PluginHost::CodeActionCandidate> results;
  for (const auto& provider : code_action_runtimes) {
    if (provider.language_id != language_id) {
      continue;
    }
    if (static_cast<lua_Integer>(results.size()) >= kMaxAggregateCodeActions) {
      break;  // aggregate budget reached across providers
    }
    lua_State* state = provider.state;
    const lua_interop::StackResetGuard stack_guard(state);
    lua_rawgeti(state, LUA_REGISTRYINDEX, provider.provide_ref);
    push_buffer_context(state, path);
    lua_interop::PushRange(state, start_line, start_column, end_line, end_column);
    const runtime_types::PluginInstance* plugin = find_plugin_by_state(state);
    std::string call_error;
    if (plugin == nullptr || !plugin->runtime || !plugin->runtime->PCall(2, 1, &call_error)) {
      // Keep earlier providers' actions and keep scanning later providers
      // (TD-2026-07-17A-047).
      lua_interop::AppendProviderFailure(error_message, "code action", provider.id, call_error);
      continue;
    }
    if (lua_istable(state, -1)) {
      const int array_index = lua_absindex(state, -1);
      const lua_Integer remaining =
          kMaxAggregateCodeActions - static_cast<lua_Integer>(results.size());
      const lua_Integer count = std::min<lua_Integer>(
          std::min<lua_Integer>(static_cast<lua_Integer>(lua_rawlen(state, array_index)),
                                 kMaxCodeActions),
          remaining);
      for (lua_Integer i = 1; i <= count; ++i) {
        lua_rawgeti(state, array_index, i);
        if (!lua_istable(state, -1)) {
          lua_pop(state, 1);
          continue;
        }
        PluginHost::CodeActionCandidate action;
        action.title = lua_interop::ReadStringField(state, -1, "title");
        action.command = lua_interop::ReadStringField(state, -1, "command");
        lua_interop::GetFieldProtected(state, -1, "arguments");
        if (lua_istable(state, -1)) {
          const int args_index = lua_absindex(state, -1);
          const lua_Integer args_count = std::min<lua_Integer>(
              static_cast<lua_Integer>(lua_rawlen(state, args_index)), kMaxCodeActionArguments);
          for (lua_Integer arg_index = 1; arg_index <= args_count; ++arg_index) {
            lua_rawgeti(state, args_index, arg_index);
            // These arguments feed command execution; NUL-reject rather than truncate
            // (TD-2026-07-17A-080).
            if (auto arg = lua_interop::ToHostString(state, -1)) {
              action.arguments.push_back(std::move(*arg));
            }
            lua_pop(state, 1);
          }
        }
        lua_pop(state, 1);
        if (!action.title.empty()) {
          results.push_back(std::move(action));
        }
        lua_pop(state, 1);
      }
    }
    lua_pop(state, 1);
  }
  return results;
}

bool DiscoverTests(
    std::string_view provider_id,
    const std::filesystem::path& path,
    const std::filesystem::path& current_project_root,
    const std::vector<runtime_types::TestProviderRuntime>& test_provider_runtimes,
    const std::function<const runtime_types::PluginInstance*(lua_State*)>& find_plugin_by_state,
    const std::function<void(lua_State*, const std::filesystem::path&)>& push_buffer_context,
    const std::function<std::filesystem::path(const std::filesystem::path&,
                                              const std::filesystem::path&)>&
        resolve_runtime_path,
    std::vector<PluginHost::TestCase>* tests,
    std::string* error_message) {
  const auto it =
      std::find_if(test_provider_runtimes.begin(), test_provider_runtimes.end(),
                   [provider_id](const auto& provider) { return provider.id == provider_id; });
  if (it == test_provider_runtimes.end()) {
    if (error_message != nullptr) {
      *error_message = "unknown test provider: " + std::string(provider_id);
    }
    return false;
  }
  if (it->discover_ref == LUA_NOREF || it->discover_ref == LUA_REFNIL) {
    return true;
  }

  lua_State* state = it->state;
  const lua_interop::StackResetGuard stack_guard(state);
  const runtime_types::PluginInstance* plugin = find_plugin_by_state(state);
  lua_rawgeti(state, LUA_REGISTRYINDEX, it->discover_ref);
  push_buffer_context(state, path);
  std::string call_error;
  if (plugin == nullptr || !plugin->runtime || !plugin->runtime->PCall(1, 1, &call_error)) {
    if (error_message != nullptr) {
      // NB: use the caller-owned `provider_id` (== it->id for the matched item), not
      // `it->id`: the PCall above runs with allow_registration=true, so a provider
      // callback that registers another provider can reallocate the runtimes vector and
      // dangle `it` before this error path reads it (use-after-free).
      *error_message =
          "test discovery provider '" + std::string(provider_id) + "' failed: " + call_error;
    }
    return false;
  }
  if (lua_istable(state, -1)) {
    const int array_index = lua_absindex(state, -1);
    const lua_Integer count = std::min<lua_Integer>(
        static_cast<lua_Integer>(lua_rawlen(state, array_index)), kMaxDiscoveredTests);
    for (lua_Integer i = 1; i <= count; ++i) {
      lua_rawgeti(state, array_index, i);
      if (!lua_istable(state, -1)) {
        lua_pop(state, 1);
        continue;
      }
      PluginHost::TestCase test;
      test.id = lua_interop::ReadStringField(state, -1, "id");
      test.label = lua_interop::ReadStringField(state, -1, "label");
      const std::string file = lua_interop::ReadStringField(state, -1, "file");
      if (!file.empty()) {
        test.file = resolve_runtime_path(current_project_root, std::filesystem::path(file));
      } else {
        test.file = path.lexically_normal();
      }
      test.parent_id = lua_interop::ReadStringField(state, -1, "parent_id");
      lua_interop::GetFieldProtected(state, -1, "line");
      if (lua_isinteger(state, -1)) {
        test.line = ReadTestLineOrAbsent(lua_tointeger(state, -1));
      }
      lua_pop(state, 1);
      if (!test.id.empty()) {
        tests->push_back(std::move(test));
      }
      lua_pop(state, 1);
    }
  }
  lua_pop(state, 1);
  return true;
}

bool RunTests(
    std::string_view provider_id,
    const std::vector<std::string>& test_ids,
    const std::vector<runtime_types::TestProviderRuntime>& test_provider_runtimes,
    const std::function<const runtime_types::PluginInstance*(lua_State*)>& find_plugin_by_state,
    std::vector<PluginHost::TestRunResult>* results,
    std::string* error_message) {
  const auto it =
      std::find_if(test_provider_runtimes.begin(), test_provider_runtimes.end(),
                   [provider_id](const auto& provider) { return provider.id == provider_id; });
  if (it == test_provider_runtimes.end()) {
    if (error_message != nullptr) {
      *error_message = "unknown test provider: " + std::string(provider_id);
    }
    return false;
  }
  if (it->run_ref == LUA_NOREF || it->run_ref == LUA_REFNIL) {
    if (error_message != nullptr) {
      *error_message = "test provider '" + std::string(provider_id) + "' does not support run";
    }
    return false;
  }

  lua_State* state = it->state;
  const lua_interop::StackResetGuard stack_guard(state);
  const runtime_types::PluginInstance* plugin = find_plugin_by_state(state);
  lua_rawgeti(state, LUA_REGISTRYINDEX, it->run_ref);
  lua_createtable(state, static_cast<int>(test_ids.size()), 0);
  for (std::size_t i = 0; i < test_ids.size(); ++i) {
    lua_pushlstring(state, test_ids[i].c_str(), test_ids[i].size());
    lua_rawseti(state, -2, static_cast<lua_Integer>(i + 1));
  }
  std::string call_error;
  if (plugin == nullptr || !plugin->runtime || !plugin->runtime->PCall(1, 1, &call_error)) {
    if (error_message != nullptr) {
      // provider_id (== it->id) is caller-owned; `it` may dangle after PCall (see above).
      *error_message = "test provider '" + std::string(provider_id) + "' run failed: " + call_error;
    }
    return false;
  }
  if (lua_istable(state, -1)) {
    const int array_index = lua_absindex(state, -1);
    const lua_Integer count = std::min<lua_Integer>(
        static_cast<lua_Integer>(lua_rawlen(state, array_index)), kMaxTestRunResults);
    for (lua_Integer i = 1; i <= count; ++i) {
      lua_rawgeti(state, array_index, i);
      if (!lua_istable(state, -1)) {
        lua_pop(state, 1);
        continue;
      }
      PluginHost::TestRunResult result;
      result.test_id = lua_interop::ReadStringField(state, -1, "test_id");
      result.state = lua_interop::ReadStringField(state, -1, "state");
      result.message = lua_interop::ReadStringField(state, -1, "message");
      lua_interop::GetFieldProtected(state, -1, "duration_ms");
      if (lua_isinteger(state, -1)) {
        result.duration_ms = ClampDurationMs(lua_tointeger(state, -1));
      }
      lua_pop(state, 1);
      if (!result.test_id.empty()) {
        results->push_back(std::move(result));
      }
      lua_pop(state, 1);
    }
  }
  lua_pop(state, 1);
  return true;
}

bool SnapshotScm(
    std::string_view provider_id,
    const std::filesystem::path& current_project_root,
    const std::vector<runtime_types::ScmProviderRuntime>& scm_provider_runtimes,
    const std::function<const runtime_types::PluginInstance*(lua_State*)>& find_plugin_by_state,
    const std::function<std::filesystem::path(const std::filesystem::path&,
                                              const std::filesystem::path&)>&
        resolve_runtime_path,
    PluginHost::ScmSnapshot* snapshot,
    std::string* error_message) {
  const auto it =
      std::find_if(scm_provider_runtimes.begin(), scm_provider_runtimes.end(),
                   [provider_id](const auto& runtime) { return runtime.id == provider_id; });
  if (it == scm_provider_runtimes.end()) {
    if (error_message != nullptr) {
      *error_message = "unknown scm provider: " + std::string(provider_id);
    }
    return false;
  }

  lua_State* state = it->state;
  const lua_interop::StackResetGuard stack_guard(state);
  const runtime_types::PluginInstance* plugin = find_plugin_by_state(state);
  lua_rawgeti(state, LUA_REGISTRYINDEX, it->snapshot_ref);
  std::string call_error;
  if (plugin == nullptr || !plugin->runtime || !plugin->runtime->PCall(0, 1, &call_error)) {
    if (error_message != nullptr) {
      // provider_id (== it->id) is caller-owned; `it` may dangle after PCall (see above).
      *error_message = "scm provider '" + std::string(provider_id) + "' failed: " + call_error;
    }
    return false;
  }

  if (lua_istable(state, -1)) {
    snapshot->base_ref = lua_interop::ReadStringField(state, -1, "base_ref");
    snapshot->base_label = lua_interop::ReadStringField(state, -1, "base_label");
    lua_interop::GetFieldProtected(state, -1, "supports_mutations");
    snapshot->supports_mutations = lua_toboolean(state, -1) != 0;
    lua_pop(state, 1);

    lua_interop::GetFieldProtected(state, -1, "entries");
    if (lua_istable(state, -1)) {
      const int entries_index = lua_absindex(state, -1);
      const lua_Integer entries_count = std::min<lua_Integer>(
          static_cast<lua_Integer>(lua_rawlen(state, entries_index)), kMaxScmEntries);
      for (lua_Integer index = 1; index <= entries_count; ++index) {
        lua_rawgeti(state, entries_index, index);
        if (!lua_istable(state, -1)) {
          lua_pop(state, 1);
          continue;
        }
        PluginHost::ScmEntry entry;
        const std::string file_path = lua_interop::ReadStringField(state, -1, "path");
        const std::string relative_path = lua_interop::ReadStringField(state, -1, "relative_path");
        entry.path = resolve_runtime_path(current_project_root, std::filesystem::path(file_path));
        entry.relative_path = relative_path.empty() ? std::filesystem::path{}
                                                    : std::filesystem::path(relative_path);
        entry.status = lua_interop::ReadStringField(state, -1, "status");
        lua_interop::GetFieldProtected(state, -1, "conflicted");
        entry.conflicted = lua_toboolean(state, -1) != 0;
        lua_pop(state, 1);
        lua_interop::GetFieldProtected(state, -1, "staged");
        entry.staged = lua_toboolean(state, -1) != 0;
        lua_pop(state, 1);
        lua_interop::GetFieldProtected(state, -1, "supports_stage");
        entry.supports_stage = lua_toboolean(state, -1) != 0;
        lua_pop(state, 1);
        lua_interop::GetFieldProtected(state, -1, "supports_discard");
        entry.supports_discard = lua_toboolean(state, -1) != 0;
        lua_pop(state, 1);
        if (!entry.path.empty()) {
          snapshot->entries.push_back(std::move(entry));
        }
        lua_pop(state, 1);
      }
    }
    lua_pop(state, 1);
  }
  lua_pop(state, 1);
  return true;
}

std::vector<PluginHost::AnnotationLine> QueryAnnotations(
    std::string_view provider_id,
    const std::filesystem::path& path,
    std::string_view language_id,
    std::size_t visible_start_line,
    std::size_t visible_end_line,
    const std::vector<runtime_types::AnnotationProviderRuntime>& annotation_provider_runtimes,
    const std::function<const runtime_types::PluginInstance*(lua_State*)>& find_plugin_by_state,
    const std::function<void(lua_State*, const std::filesystem::path&)>& push_buffer_context,
    std::string* error_message) {
  std::vector<PluginHost::AnnotationLine> lines;
  for (const auto& provider : annotation_provider_runtimes) {
    if ((!provider_id.empty() && provider.id != provider_id) ||
        (!language_id.empty() && provider.language_id != language_id)) {
      continue;
    }
    if (static_cast<lua_Integer>(lines.size()) >= kMaxAggregateAnnotationLines) {
      break;  // aggregate budget reached across providers (provider_id-empty fan-out)
    }

    lua_State* state = provider.state;
    const lua_interop::StackResetGuard stack_guard(state);
    lua_rawgeti(state, LUA_REGISTRYINDEX, provider.provide_ref);
    push_buffer_context(state, path);
    lua_pushinteger(state, static_cast<lua_Integer>(visible_start_line));
    lua_pushinteger(state, static_cast<lua_Integer>(visible_end_line));
    const runtime_types::PluginInstance* plugin = find_plugin_by_state(state);
    std::string call_error;
    if (plugin == nullptr || !plugin->runtime || !plugin->runtime->PCall(3, 1, &call_error)) {
      if (error_message != nullptr) {
        *error_message = "annotation provider '" + provider.id + "' failed: " + call_error;
      }
      return {};
    }
    if (lua_istable(state, -1)) {
      const int array_index = lua_absindex(state, -1);
      const lua_Integer remaining =
          kMaxAggregateAnnotationLines - static_cast<lua_Integer>(lines.size());
      const lua_Integer count = std::min<lua_Integer>(
          std::min<lua_Integer>(static_cast<lua_Integer>(lua_rawlen(state, array_index)),
                                 kMaxAnnotationLines),
          remaining);
      for (lua_Integer index = 1; index <= count; ++index) {
        lua_rawgeti(state, array_index, index);
        if (!lua_istable(state, -1)) {
          lua_pop(state, 1);
          continue;
        }
        PluginHost::AnnotationLine line;
        lua_interop::GetFieldProtected(state, -1, "line");
        if (lua_isinteger(state, -1)) {
          line.line = static_cast<std::size_t>(std::max<lua_Integer>(0, lua_tointeger(state, -1)));
        }
        lua_pop(state, 1);
        line.text = lua_interop::ReadStringField(state, -1, "text");
        line.author = lua_interop::ReadStringField(state, -1, "author");
        line.summary = lua_interop::ReadStringField(state, -1, "summary");
        line.date = lua_interop::ReadStringField(state, -1, "date");
        if (!line.text.empty()) {
          lines.push_back(std::move(line));
        }
        lua_pop(state, 1);
      }
    }
    lua_pop(state, 1);
    if (!provider_id.empty()) {
      break;
    }
  }
  return lines;
}

bool LoginAuthProvider(
    std::string_view provider_id,
    const std::vector<std::string>& scopes,
    const std::vector<runtime_types::AuthProviderRuntime>& auth_provider_runtimes,
    const std::function<const runtime_types::PluginInstance*(lua_State*)>& find_plugin_by_state,
    PluginHost::AuthSessionData* session,
    std::string* error_message) {
  const auto it =
      std::find_if(auth_provider_runtimes.begin(), auth_provider_runtimes.end(),
                   [provider_id](const auto& runtime) { return runtime.id == provider_id; });
  if (it == auth_provider_runtimes.end()) {
    if (error_message != nullptr) {
      *error_message = "unknown auth provider: " + std::string(provider_id);
    }
    return false;
  }
  if (it->login_ref == LUA_NOREF || it->login_ref == LUA_REFNIL) {
    if (error_message != nullptr) {
      *error_message = "auth provider '" + std::string(provider_id) + "' does not support login";
    }
    return false;
  }

  lua_State* state = it->state;
  const lua_interop::StackResetGuard stack_guard(state);
  const runtime_types::PluginInstance* plugin = find_plugin_by_state(state);
  lua_rawgeti(state, LUA_REGISTRYINDEX, it->login_ref);
  lua_createtable(state, static_cast<int>(scopes.size()), 0);
  for (std::size_t i = 0; i < scopes.size(); ++i) {
    lua_pushlstring(state, scopes[i].c_str(), scopes[i].size());
    lua_rawseti(state, -2, static_cast<lua_Integer>(i + 1));
  }
  std::string call_error;
  if (plugin == nullptr || !plugin->runtime || !plugin->runtime->PCall(1, 1, &call_error)) {
    if (error_message != nullptr) {
      // provider_id (== it->id) is caller-owned; `it` may dangle after PCall (see above).
      *error_message = "auth provider '" + std::string(provider_id) + "' login failed: " + call_error;
    }
    return false;
  }
  if (lua_istable(state, -1)) {
    session->id = lua_interop::ReadStringField(state, -1, "id");
    session->account = lua_interop::ReadStringField(state, -1, "account");
    session->access_token = lua_interop::ReadStringField(state, -1, "access_token");
    lua_interop::GetFieldProtected(state, -1, "scopes");
    if (lua_istable(state, -1)) {
      const int scopes_index = lua_absindex(state, -1);
      const lua_Integer scopes_count = std::min<lua_Integer>(
          static_cast<lua_Integer>(lua_rawlen(state, scopes_index)), kMaxAuthScopes);
      for (lua_Integer i = 1; i <= scopes_count; ++i) {
        lua_rawgeti(state, scopes_index, i);
        if (lua_isstring(state, -1)) {
          session->scopes.emplace_back(lua_tostring(state, -1));
        }
        lua_pop(state, 1);
      }
    }
    lua_pop(state, 1);
  }
  lua_pop(state, 1);
  return !session->id.empty();
}

bool RefreshAuthSession(
    std::string_view provider_id,
    std::string_view session_id,
    const std::vector<runtime_types::AuthProviderRuntime>& auth_provider_runtimes,
    const std::function<const runtime_types::PluginInstance*(lua_State*)>& find_plugin_by_state,
    PluginHost::AuthSessionData* session,
    std::string* error_message) {
  const auto it =
      std::find_if(auth_provider_runtimes.begin(), auth_provider_runtimes.end(),
                   [provider_id](const auto& runtime) { return runtime.id == provider_id; });
  if (it == auth_provider_runtimes.end()) {
    if (error_message != nullptr) {
      *error_message = "unknown auth provider: " + std::string(provider_id);
    }
    return false;
  }
  if (it->refresh_ref == LUA_NOREF || it->refresh_ref == LUA_REFNIL) {
    if (error_message != nullptr) {
      *error_message =
          "auth provider '" + std::string(provider_id) + "' does not support refresh";
    }
    return false;
  }

  lua_State* state = it->state;
  const lua_interop::StackResetGuard stack_guard(state);
  const runtime_types::PluginInstance* plugin = find_plugin_by_state(state);
  lua_rawgeti(state, LUA_REGISTRYINDEX, it->refresh_ref);
  lua_pushlstring(state, session_id.data(), session_id.size());
  std::string call_error;
  if (plugin == nullptr || !plugin->runtime || !plugin->runtime->PCall(1, 1, &call_error)) {
    if (error_message != nullptr) {
      // provider_id (== it->id) is caller-owned; `it` may dangle after PCall (see above).
      *error_message =
          "auth provider '" + std::string(provider_id) + "' refresh failed: " + call_error;
    }
    return false;
  }
  if (lua_istable(state, -1)) {
    session->id = lua_interop::ReadStringField(state, -1, "id");
    if (session->id.empty()) {
      session->id = std::string(session_id);
    }
    session->account = lua_interop::ReadStringField(state, -1, "account");
    session->access_token = lua_interop::ReadStringField(state, -1, "access_token");
    lua_interop::GetFieldProtected(state, -1, "scopes");
    if (lua_istable(state, -1)) {
      const int scopes_index = lua_absindex(state, -1);
      const lua_Integer scopes_count = std::min<lua_Integer>(
          static_cast<lua_Integer>(lua_rawlen(state, scopes_index)), kMaxAuthScopes);
      for (lua_Integer i = 1; i <= scopes_count; ++i) {
        lua_rawgeti(state, scopes_index, i);
        if (lua_isstring(state, -1)) {
          session->scopes.emplace_back(lua_tostring(state, -1));
        }
        lua_pop(state, 1);
      }
    }
    lua_pop(state, 1);
  }
  lua_pop(state, 1);
  return !session->id.empty();
}

bool LogoutAuthSession(
    std::string_view provider_id,
    std::string_view session_id,
    const std::vector<runtime_types::AuthProviderRuntime>& auth_provider_runtimes,
    const std::function<const runtime_types::PluginInstance*(lua_State*)>& find_plugin_by_state,
    std::string* error_message) {
  const auto it =
      std::find_if(auth_provider_runtimes.begin(), auth_provider_runtimes.end(),
                   [provider_id](const auto& runtime) { return runtime.id == provider_id; });
  if (it == auth_provider_runtimes.end()) {
    if (error_message != nullptr) {
      *error_message = "unknown auth provider: " + std::string(provider_id);
    }
    return false;
  }
  if (it->logout_ref == LUA_NOREF || it->logout_ref == LUA_REFNIL) {
    if (error_message != nullptr) {
      *error_message = "auth provider '" + std::string(provider_id) + "' does not support logout";
    }
    return false;
  }

  lua_State* state = it->state;
  const lua_interop::StackResetGuard stack_guard(state);
  const runtime_types::PluginInstance* plugin = find_plugin_by_state(state);
  lua_rawgeti(state, LUA_REGISTRYINDEX, it->logout_ref);
  lua_pushlstring(state, session_id.data(), session_id.size());
  std::string call_error;
  if (plugin == nullptr || !plugin->runtime || !plugin->runtime->PCall(1, 0, &call_error)) {
    if (error_message != nullptr) {
      // provider_id (== it->id) is caller-owned; `it` may dangle after PCall (see above).
      *error_message =
          "auth provider '" + std::string(provider_id) + "' logout failed: " + call_error;
    }
    return false;
  }
  return true;
}

bool ExecuteCommand(
    std::string_view name,
    const std::vector<std::string>& args,
    const std::unordered_map<std::string, runtime_types::PluginCommand>& commands,
    const std::function<const runtime_types::PluginInstance*(lua_State*)>& find_plugin_by_state,
    const std::function<void(lua_State*)>& push_plugin_context,
    std::string* error_message,
    std::string* feedback) {
  if (feedback != nullptr) {
    feedback->clear();
  }
  const auto it = commands.find(std::string(name));
  if (it == commands.end()) {
    if (error_message != nullptr) {
      error_message->clear();
    }
    return false;
  }

  lua_State* state = it->second.state;
  const lua_interop::StackResetGuard stack_guard(state);
  const runtime_types::PluginInstance* plugin = find_plugin_by_state(state);
  lua_rawgeti(state, LUA_REGISTRYINDEX, it->second.function_ref);
  push_plugin_context(state);
  lua_createtable(state, static_cast<int>(args.size()), 0);
  for (std::size_t i = 0; i < args.size(); ++i) {
    lua_pushstring(state, args[i].c_str());
    lua_rawseti(state, -2, static_cast<lua_Integer>(i + 1));
  }

  std::string call_error;
  if (plugin == nullptr || !plugin->runtime || !plugin->runtime->PCall(2, 1, &call_error)) {
    if (error_message != nullptr) {
      *error_message = "plugin command '" + std::string(name) + "' failed: " + call_error;
    }
    return false;
  }
  // A command may return a string (or {message = "..."}) to surface as host feedback.
  if (feedback != nullptr) {
    if (lua_isstring(state, -1)) {
      *feedback = lua_tostring(state, -1);
    } else if (lua_istable(state, -1)) {
      *feedback = lua_interop::ReadStringField(state, -1, "message");
    }
  }
  if (error_message != nullptr) {
    error_message->clear();
  }
  return true;
}

bool RunSaveParticipants(
    const std::filesystem::path& path,
    std::string* text,
    const std::vector<runtime_types::SaveParticipantRuntime>& save_participant_runtimes,
    const std::function<const runtime_types::PluginInstance*(lua_State*)>& find_plugin_by_state,
    const std::function<void(lua_State*, const std::filesystem::path&, std::string_view)>&
        push_buffer_context_with_text,
    std::string* error_message) {
  for (const auto& participant : save_participant_runtimes) {
    lua_State* state = participant.state;
    const lua_interop::StackResetGuard stack_guard(state);
    const runtime_types::PluginInstance* plugin = find_plugin_by_state(state);
    lua_rawgeti(state, LUA_REGISTRYINDEX, participant.function_ref);
    push_buffer_context_with_text(state, path, *text);
    std::string call_error;
    if (plugin == nullptr || !plugin->runtime || !plugin->runtime->PCall(1, 1, &call_error)) {
      if (error_message != nullptr) {
        *error_message = "save participant '" + participant.id + "' failed: " + call_error;
      }
      return false;
    }
    // Cap the participant's replacement text at the same ceiling the editor uses to
    // read/write files (kMaxTextFileBytes). Without it, a participant could hand back
    // an arbitrarily large string that the host would assign straight into the
    // document, blowing past the size the buffer could ever have been loaded from and
    // ballooning host memory. An over-limit result rejects that participant's
    // transform (the text is left unchanged) and is surfaced through Lua's warning
    // channel rather than failing the whole save.
    const auto assign_if_within_limit = [&](const char* updated, std::size_t size) {
      if (updated == nullptr) {
        return;
      }
      if (static_cast<std::uintmax_t>(size) > util::kMaxTextFileBytes) {
        lua_warning(state,
                    "microide: save participant output exceeds the maximum text file "
                    "size; discarding this transform",
                    0);
        return;
      }
      *text = std::string(updated, size);
    };
    if (lua_isstring(state, -1)) {
      std::size_t size = 0;
      const char* updated = lua_tolstring(state, -1, &size);
      assign_if_within_limit(updated, size);
    } else if (lua_istable(state, -1)) {
      lua_interop::GetFieldProtected(state, -1, "text");
      if (lua_isstring(state, -1)) {
        std::size_t size = 0;
        const char* updated = lua_tolstring(state, -1, &size);
        assign_if_within_limit(updated, size);
      }
      lua_pop(state, 1);
    }
    lua_pop(state, 1);
  }
  return true;
}

}  // namespace microide::plugin::provider_query_interop

#endif
