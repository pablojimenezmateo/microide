#include "plugin/PluginProviderQueryInterop.h"

#if MICROIDE_HAS_LUA_PLUGINS

#include <algorithm>

#include "plugin/PluginLuaInterop.h"

namespace microide::plugin::provider_query_interop {

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
    lua_State* state = provider.state;
    lua_rawgeti(state, LUA_REGISTRYINDEX, provider.provide_ref);
    push_buffer_context(state, path);
    lua_interop::PushPosition(state, line, column);
    lua_pushlstring(state, trigger_character.data(), trigger_character.size());
    const runtime_types::PluginInstance* plugin = find_plugin_by_state(state);
    std::string call_error;
    if (plugin == nullptr || !plugin->runtime || !plugin->runtime->PCall(3, 1, &call_error)) {
      if (error_message != nullptr) {
        *error_message = "completion provider '" + provider.id + "' failed: " + call_error;
      }
      return {};
    }
    if (lua_istable(state, -1)) {
      for (lua_Integer i = 1;; ++i) {
        lua_geti(state, -1, i);
        if (lua_isnil(state, -1)) {
          lua_pop(state, 1);
          break;
        }
        if (!lua_istable(state, -1)) {
          lua_pop(state, 1);
          continue;
        }
        PluginHost::CompletionCandidate candidate;
        auto read_string = [&](const char* field) -> std::string {
          lua_getfield(state, -1, field);
          std::string value =
              lua_isstring(state, -1) ? std::string(lua_tostring(state, -1)) : std::string{};
          lua_pop(state, 1);
          return value;
        };
        candidate.label = read_string("label");
        candidate.detail = read_string("detail");
        candidate.documentation = read_string("documentation");
        candidate.insert_text = read_string("insert_text");
        if (candidate.insert_text.empty()) {
          candidate.insert_text = read_string("insertText");
        }
        if (candidate.insert_text.empty()) {
          candidate.insert_text = candidate.label;
        }
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
    lua_State* state = provider.state;
    lua_rawgeti(state, LUA_REGISTRYINDEX, provider.provide_ref);
    push_buffer_context(state, path);
    lua_interop::PushRange(state, start_line, start_column, end_line, end_column);
    const runtime_types::PluginInstance* plugin = find_plugin_by_state(state);
    std::string call_error;
    if (plugin == nullptr || !plugin->runtime || !plugin->runtime->PCall(2, 1, &call_error)) {
      if (error_message != nullptr) {
        *error_message = "code action provider '" + provider.id + "' failed: " + call_error;
      }
      return {};
    }
    if (lua_istable(state, -1)) {
      for (lua_Integer i = 1;; ++i) {
        lua_geti(state, -1, i);
        if (lua_isnil(state, -1)) {
          lua_pop(state, 1);
          break;
        }
        if (!lua_istable(state, -1)) {
          lua_pop(state, 1);
          continue;
        }
        PluginHost::CodeActionCandidate action;
        auto read_string = [&](const char* field) -> std::string {
          lua_getfield(state, -1, field);
          std::string value =
              lua_isstring(state, -1) ? std::string(lua_tostring(state, -1)) : std::string{};
          lua_pop(state, 1);
          return value;
        };
        action.title = read_string("title");
        action.command = read_string("command");
        lua_getfield(state, -1, "arguments");
        if (lua_istable(state, -1)) {
          for (lua_Integer arg_index = 1;; ++arg_index) {
            lua_geti(state, -1, arg_index);
            if (lua_isnil(state, -1)) {
              lua_pop(state, 1);
              break;
            }
            if (lua_isstring(state, -1)) {
              action.arguments.emplace_back(lua_tostring(state, -1));
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
  const runtime_types::PluginInstance* plugin = find_plugin_by_state(state);
  lua_rawgeti(state, LUA_REGISTRYINDEX, it->discover_ref);
  push_buffer_context(state, path);
  std::string call_error;
  if (plugin == nullptr || !plugin->runtime || !plugin->runtime->PCall(1, 1, &call_error)) {
    if (error_message != nullptr) {
      *error_message = "test discovery provider '" + it->id + "' failed: " + call_error;
    }
    return false;
  }
  if (lua_istable(state, -1)) {
    for (lua_Integer i = 1;; ++i) {
      lua_geti(state, -1, i);
      if (lua_isnil(state, -1)) {
        lua_pop(state, 1);
        break;
      }
      if (!lua_istable(state, -1)) {
        lua_pop(state, 1);
        continue;
      }
      PluginHost::TestCase test;
      auto read_string = [&](const char* field) -> std::string {
        lua_getfield(state, -1, field);
        std::string value =
            lua_isstring(state, -1) ? std::string(lua_tostring(state, -1)) : std::string{};
        lua_pop(state, 1);
        return value;
      };
      test.id = read_string("id");
      test.label = read_string("label");
      const std::string file = read_string("file");
      if (!file.empty()) {
        test.file = resolve_runtime_path(current_project_root, std::filesystem::path(file));
      } else {
        test.file = path.lexically_normal();
      }
      test.parent_id = read_string("parent_id");
      lua_getfield(state, -1, "line");
      if (lua_isinteger(state, -1)) {
        test.line = static_cast<int>(lua_tointeger(state, -1));
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
      *error_message = "test provider '" + it->id + "' run failed: " + call_error;
    }
    return false;
  }
  if (lua_istable(state, -1)) {
    for (lua_Integer i = 1;; ++i) {
      lua_geti(state, -1, i);
      if (lua_isnil(state, -1)) {
        lua_pop(state, 1);
        break;
      }
      if (!lua_istable(state, -1)) {
        lua_pop(state, 1);
        continue;
      }
      PluginHost::TestRunResult result;
      auto read_string = [&](const char* field) -> std::string {
        lua_getfield(state, -1, field);
        std::string value =
            lua_isstring(state, -1) ? std::string(lua_tostring(state, -1)) : std::string{};
        lua_pop(state, 1);
        return value;
      };
      result.test_id = read_string("test_id");
      result.state = read_string("state");
      result.message = read_string("message");
      lua_getfield(state, -1, "duration_ms");
      if (lua_isinteger(state, -1)) {
        result.duration_ms = static_cast<int>(lua_tointeger(state, -1));
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

}  // namespace microide::plugin::provider_query_interop

#endif
