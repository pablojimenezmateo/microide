#include "plugin/PluginHost.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <SDL3/SDL.h>

#include "platform/AppDirectories.h"
#include "platform/Filesystem.h"
#include "platform/Subprocess.h"
#include "plugin/LuaRuntime.h"
#include "util/TextFileIO.h"

#if MICROIDE_HAS_LUA_PLUGINS
#include <lua.hpp>
#endif

namespace microide::plugin {

namespace {

std::filesystem::path GlobalPluginDirectory() {
  const std::filesystem::path config_root =
      platform::ResolveAppDirectory(platform::UserDirectoryKind::Config, "microide");
  return config_root.empty() ? std::filesystem::path{} : config_root / "plugins";
}

bool ShouldSkipPluginDirectoryName(std::string_view name) {
  return name.ends_with(".bak") || name.find(".bak-") != std::string_view::npos;
}

#ifndef MICROIDE_TESTING
std::filesystem::path RepoPluginDirectory() {
  const auto repo_plugins_from_root = [](const std::filesystem::path& start) {
    if (start.empty()) {
      return std::filesystem::path{};
    }
    std::error_code error;
    std::filesystem::path current = std::filesystem::weakly_canonical(start, error);
    if (error) {
      current = start.lexically_normal();
    }
    while (!current.empty()) {
      const std::filesystem::path plugins_dir = current / "plugins";
      if (platform::ReadPathType(plugins_dir) == platform::PathType::Directory &&
          platform::ReadPathType(plugins_dir / "README.md") == platform::PathType::RegularFile) {
        return plugins_dir.lexically_normal();
      }
      const std::filesystem::path parent = current.parent_path();
      if (parent == current) {
        break;
      }
      current = parent;
    }
    return std::filesystem::path{};
  };

  if (const char* raw_base_path = SDL_GetBasePath();
      raw_base_path != nullptr && raw_base_path[0] != '\0') {
    if (const std::filesystem::path plugins_dir =
            repo_plugins_from_root(std::filesystem::path(raw_base_path));
        !plugins_dir.empty()) {
      return plugins_dir;
    }
  }

  std::error_code error;
  const std::filesystem::path cwd = std::filesystem::current_path(error);
  return error ? std::filesystem::path{} : repo_plugins_from_root(cwd);
}
#endif

bool IsValidIdentifier(std::string_view value) {
  if (value.empty()) {
    return false;
  }
  return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
    return std::isalnum(ch) || ch == '.' || ch == '-' || ch == '_';
  });
}

std::string Basename(const std::filesystem::path& path) {
  return path.filename().empty() ? path.lexically_normal().string() : path.filename().string();
}

std::filesystem::path ResolveRuntimePath(const std::filesystem::path& project_root,
                                         const std::filesystem::path& path) {
  if (path.empty()) {
    return {};
  }
  if (path.is_absolute() || project_root.empty()) {
    return path.lexically_normal();
  }
  return (project_root / path).lexically_normal();
}

std::string ToLowerAscii(std::string_view text) {
  std::string lowered(text);
  std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return lowered;
}

bool ParseDiagnosticSeverity(std::string_view raw_value, editor::DiagnosticSeverity* severity) {
  if (severity == nullptr) {
    return false;
  }
  const std::string value = ToLowerAscii(raw_value);
  if (value == "error") {
    *severity = editor::DiagnosticSeverity::Error;
    return true;
  }
  if (value == "warning" || value == "warn") {
    *severity = editor::DiagnosticSeverity::Warning;
    return true;
  }
  if (value == "info" || value == "information") {
    *severity = editor::DiagnosticSeverity::Info;
    return true;
  }
  if (value == "hint") {
    *severity = editor::DiagnosticSeverity::Hint;
    return true;
  }
  return false;
}

}  // namespace

struct PluginHost::Impl {
  struct PluginInstance {
    std::string id;
    std::filesystem::path root;
    bool project_local = false;
#if MICROIDE_HAS_LUA_PLUGINS
    std::unique_ptr<LuaRuntime> runtime;
    lua_State* state = nullptr;
    int setup_ref = LUA_NOREF;
    int on_project_open_ref = LUA_NOREF;
    int on_project_close_ref = LUA_NOREF;
    int on_buffer_open_ref = LUA_NOREF;
    int on_buffer_save_ref = LUA_NOREF;
    int shutdown_ref = LUA_NOREF;
#endif
  };

  struct PluginCommand {
    std::string plugin_id;
#if MICROIDE_HAS_LUA_PLUGINS
    lua_State* state = nullptr;
    int function_ref = LUA_NOREF;
#endif
  };

  struct SidebarProvider {
    SidebarProviderInfo info;
#if MICROIDE_HAS_LUA_PLUGINS
    lua_State* state = nullptr;
    int snapshot_ref = LUA_NOREF;
    int confirm_ref = LUA_NOREF;
#endif
  };

  struct HoverProvider {
    std::string id;
    std::string plugin_id;
#if MICROIDE_HAS_LUA_PLUGINS
    lua_State* state = nullptr;
    int provide_ref = LUA_NOREF;
#endif
  };

  struct SaveParticipantRuntime {
    std::string id;
    std::string plugin_id;
#if MICROIDE_HAS_LUA_PLUGINS
    lua_State* state = nullptr;
    int function_ref = LUA_NOREF;
#endif
  };

  struct CompletionRuntime {
    std::string id;
    std::string language_id;
    std::string trigger_characters;
    std::string plugin_id;
#if MICROIDE_HAS_LUA_PLUGINS
    lua_State* state = nullptr;
    int provide_ref = LUA_NOREF;
#endif
  };

  struct CodeActionRuntime {
    std::string id;
    std::string language_id;
    std::string plugin_id;
#if MICROIDE_HAS_LUA_PLUGINS
    lua_State* state = nullptr;
    int provide_ref = LUA_NOREF;
#endif
  };

  struct TestProviderRuntime {
    std::string id;
    std::string language_id;
    std::string plugin_id;
#if MICROIDE_HAS_LUA_PLUGINS
    lua_State* state = nullptr;
    int discover_ref = LUA_NOREF;
    int run_ref = LUA_NOREF;
#endif
  };

  struct ScmProviderRuntime {
    std::string id;
    std::string plugin_id;
#if MICROIDE_HAS_LUA_PLUGINS
    lua_State* state = nullptr;
    int snapshot_ref = LUA_NOREF;
#endif
  };

  struct AnnotationProviderRuntime {
    std::string id;
    std::string language_id;
    std::string type;
    std::string plugin_id;
#if MICROIDE_HAS_LUA_PLUGINS
    lua_State* state = nullptr;
    int provide_ref = LUA_NOREF;
#endif
  };

  struct AuthProviderRuntime {
    std::string id;
    std::string plugin_id;
#if MICROIDE_HAS_LUA_PLUGINS
    lua_State* state = nullptr;
    int login_ref = LUA_NOREF;
    int refresh_ref = LUA_NOREF;
    int logout_ref = LUA_NOREF;
#endif
  };

  struct McpToolRuntime {
    std::string id;
    std::string plugin_id;
#if MICROIDE_HAS_LUA_PLUGINS
    lua_State* state = nullptr;
    int run_ref = LUA_NOREF;
#endif
  };

  Callbacks callbacks{};
  std::filesystem::path current_project_root;

  struct AsyncProcessCallback {
#if MICROIDE_HAS_LUA_PLUGINS
    lua_State* lua_state = nullptr;
    int callback_ref = LUA_NOREF;
#endif
    platform::SubprocessResult result;
  };
  struct AsyncProcessRequest {
#if MICROIDE_HAS_LUA_PLUGINS
    lua_State* lua_state = nullptr;
    int callback_ref = LUA_NOREF;
#endif
    bool cancelled = false;
  };
  struct AsyncProcessState {
    Uint32 event_type = 0;
    std::mutex mutex;
    std::atomic<int> in_flight{0};
    std::vector<std::shared_ptr<AsyncProcessRequest>> active_requests;
    std::vector<AsyncProcessCallback> pending_callbacks;
  };
  std::shared_ptr<AsyncProcessState> async_process_state = std::make_shared<AsyncProcessState>();
  std::vector<PluginInstance> plugins;
  std::unordered_map<std::string, PluginCommand> commands;
  std::vector<std::string> command_names;
  std::unordered_map<std::string, SidebarProvider> sidebars;
  std::vector<SidebarProviderInfo> sidebar_providers;
  std::unordered_map<std::string, HoverProvider> hovers;
  std::vector<std::string> hover_provider_order;
  std::vector<PluginHost::ContributedMenuEntry> menu_entries;
  std::vector<PluginHost::ContributedKeybinding> keybindings;
  std::vector<PluginHost::ContributedSettingSpec> settings;
  std::unordered_map<std::string, PluginHost::ContributedStatusItem> status_items;
  std::vector<PluginHost::ContributedStatusItem> status_item_order;
  std::vector<PluginHost::ContributedFormatter> formatters;
  std::vector<PluginHost::ContributedSaveParticipant> save_participants;
  std::vector<SaveParticipantRuntime> save_participant_runtimes;
  std::vector<PluginHost::ContributedCompletion> completions;
  std::vector<CompletionRuntime> completion_runtimes;
  std::vector<PluginHost::ContributedCodeAction> code_actions;
  std::vector<CodeActionRuntime> code_action_runtimes;
  std::vector<PluginHost::ContributedLanguageServer> language_servers;
  std::vector<PluginHost::ContributedTask> tasks;
  std::vector<PluginHost::ContributedTool> tools;
  std::vector<PluginHost::ContributedDebugger> debuggers;
  std::vector<PluginHost::ContributedTestProvider> test_providers;
  std::vector<TestProviderRuntime> test_provider_runtimes;
  std::vector<PluginHost::ContributedScmProvider> scm_providers;
  std::vector<ScmProviderRuntime> scm_provider_runtimes;
  std::vector<PluginHost::ContributedAnnotationProvider> annotation_providers;
  std::vector<AnnotationProviderRuntime> annotation_provider_runtimes;
  std::vector<PluginHost::ContributedAuthProvider> auth_providers;
  std::vector<AuthProviderRuntime> auth_provider_runtimes;
  std::vector<PluginHost::ContributedAiProvider> ai_providers;
  std::vector<PluginHost::ContributedExternalAgent> external_agents;
  std::vector<PluginHost::ContributedMcpTool> mcp_tools;
  std::vector<McpToolRuntime> mcp_tool_runtimes;
  std::vector<std::string> messages;
  std::vector<std::string> errors;
  std::string reload_summary = "Lua plugin runtime unavailable";
#if MICROIDE_HAS_LUA_PLUGINS
  PluginInstance* active_plugin = nullptr;
#endif

  [[nodiscard]] bool enabled() const {
#if MICROIDE_HAS_LUA_PLUGINS
    return true;
#else
    return false;
#endif
  }

  void SetReloadSummary() {
    if (!enabled()) {
      reload_summary = "Lua plugin runtime unavailable";
      return;
    }
    reload_summary = "Loaded " + std::to_string(plugins.size()) + " plugin";
    if (plugins.size() != 1) {
      reload_summary += "s";
    }
    reload_summary += " and " + std::to_string(command_names.size()) + " command";
    if (command_names.size() != 1) {
      reload_summary += "s";
    }
    reload_summary += " and " + std::to_string(sidebar_providers.size()) + " sidebar";
    if (sidebar_providers.size() != 1) {
      reload_summary += "s";
    }
    reload_summary += " and " + std::to_string(hover_provider_order.size()) + " hover provider";
    if (hover_provider_order.size() != 1) {
      reload_summary += "s";
    }
    if (!errors.empty()) {
      reload_summary += " with " + std::to_string(errors.size()) + " error";
      if (errors.size() != 1) {
        reload_summary += "s";
      }
    }
  }

  void RecordMessage(std::string message) {
    messages.push_back(message);
    if (callbacks.log_sink) {
      callbacks.log_sink(messages.back());
    }
  }

  void RecordError(std::string error) {
    errors.push_back(error);
    if (callbacks.error_sink) {
      callbacks.error_sink(errors.back());
    }
  }

  std::optional<std::string> RelativePathString(const std::filesystem::path& path) const {
    if (current_project_root.empty() || path.empty()) {
      return std::nullopt;
    }
    std::error_code error;
    const std::filesystem::path relative =
        std::filesystem::relative(path.lexically_normal(), current_project_root, error);
    if (error || relative.empty()) {
      return std::nullopt;
    }
    return relative.generic_string();
  }

#if MICROIDE_HAS_LUA_PLUGINS
  void PushBufferContext(lua_State* state,
                         const std::filesystem::path& path,
                         std::optional<std::string_view> text = std::nullopt) const {
    lua_createtable(state, 0, 3);
    const std::filesystem::path normalized_path = path.lexically_normal();
    const std::string path_string = normalized_path.string();
    lua_pushlstring(state, path_string.c_str(), path_string.size());
    lua_setfield(state, -2, "path");

    const std::string relative_path = RelativePathString(normalized_path).value_or(
        normalized_path.filename().empty() ? normalized_path.string()
                                           : normalized_path.filename().string());
    lua_pushlstring(state, relative_path.c_str(), relative_path.size());
    lua_setfield(state, -2, "relative_path");

    if (text.has_value()) {
      lua_pushlstring(state, text->data(), text->size());
      lua_setfield(state, -2, "text");
    }
  }

  static void PushPosition(lua_State* state, std::size_t line, std::size_t column) {
    lua_createtable(state, 0, 2);
    lua_pushinteger(state, static_cast<lua_Integer>(line));
    lua_setfield(state, -2, "line");
    lua_pushinteger(state, static_cast<lua_Integer>(column));
    lua_setfield(state, -2, "column");
  }

  static void PushRange(lua_State* state,
                        std::size_t start_line,
                        std::size_t start_column,
                        std::size_t end_line,
                        std::size_t end_column) {
    lua_createtable(state, 0, 2);
    PushPosition(state, start_line, start_column);
    lua_setfield(state, -2, "start");
    PushPosition(state, end_line, end_column);
    lua_setfield(state, -2, "end");
  }
#endif

  std::vector<std::pair<std::filesystem::path, bool>> DiscoverPluginRoots() const {
    std::vector<std::pair<std::filesystem::path, bool>> plugin_roots;
    std::set<std::string> seen_directory_names;
    const auto append = [&](const std::filesystem::path& plugins_dir, bool project_local) {
      if (plugins_dir.empty()) {
        return;
      }
      if (platform::ReadPathType(plugins_dir) != platform::PathType::Directory) {
        return;
      }

      std::vector<std::filesystem::path> entries;
      for (const auto& entry : platform::ListDirectory(plugins_dir)) {
        if (entry.type != platform::PathType::Directory) {
          continue;
        }
        const std::string directory_name = entry.path.filename().string();
        if (ShouldSkipPluginDirectoryName(directory_name) ||
            seen_directory_names.contains(directory_name)) {
          continue;
        }
        const std::filesystem::path init_path = entry.path / "init.lua";
        if (platform::ReadPathType(init_path) == platform::PathType::RegularFile) {
          seen_directory_names.insert(directory_name);
          entries.push_back(entry.path.lexically_normal());
        }
      }
      std::sort(entries.begin(), entries.end(),
                [](const auto& lhs, const auto& rhs) { return lhs.filename() < rhs.filename(); });
      for (const auto& path : entries) {
        plugin_roots.emplace_back(path, project_local);
      }
    };

    append(GlobalPluginDirectory(), false);
#ifndef MICROIDE_TESTING
    append(RepoPluginDirectory(), false);
#endif
    if (!current_project_root.empty()) {
      append(current_project_root / ".microide" / "plugins", true);
    }
    return plugin_roots;
  }

  void RebuildCommandNames() {
    command_names.clear();
    command_names.reserve(commands.size());
    for (const auto& entry : commands) {
      command_names.push_back(entry.first);
    }
    std::sort(command_names.begin(), command_names.end());
  }

  void RebuildSidebarProviders() {
    sidebar_providers.clear();
    sidebar_providers.reserve(sidebars.size());
    for (const auto& entry : sidebars) {
      sidebar_providers.push_back(entry.second.info);
    }
    std::sort(sidebar_providers.begin(), sidebar_providers.end(),
              [](const SidebarProviderInfo& lhs, const SidebarProviderInfo& rhs) {
                return lhs.id < rhs.id;
              });
  }

#if MICROIDE_HAS_LUA_PLUGINS
  static int LuaMicroidePlugin(lua_State* state) {
    luaL_checktype(state, 1, LUA_TTABLE);
    return 1;
  }

  static int LuaOpenMicroide(lua_State* state) {
    lua_createtable(state, 0, 2);
    lua_pushcfunction(state, &LuaMicroidePlugin);
    lua_setfield(state, -2, "plugin");
    lua_pushinteger(state, 1);
    lua_setfield(state, -2, "api_version");
    return 1;
  }

  static Impl* HostFromUpvalue(lua_State* state) {
    return static_cast<Impl*>(lua_touserdata(state, lua_upvalueindex(1)));
  }

  PluginInstance* FindPluginByState(lua_State* state) {
    if (active_plugin != nullptr && active_plugin->state == state) {
      return active_plugin;
    }
    const auto it =
        std::find_if(plugins.begin(), plugins.end(), [state](const PluginInstance& plugin) {
          return plugin.state == state;
        });
    return it == plugins.end() ? nullptr : &(*it);
  }

  std::string FormatPluginPrefix(const PluginInstance* plugin) const {
    return plugin == nullptr ? std::string("plugin")
                             : std::string("plugin ") + plugin->id;
  }

  static std::string LuaErrorString(lua_State* state) {
    const char* message = lua_tostring(state, -1);
    return message != nullptr ? std::string(message) : std::string("unknown Lua error");
  }

  static int LuaLog(lua_State* state) {
    Impl* host = HostFromUpvalue(state);
    const char* message = luaL_checkstring(state, 1);
    const PluginInstance* plugin = host != nullptr ? host->FindPluginByState(state) : nullptr;
    if (host != nullptr) {
      std::string formatted = plugin != nullptr ? plugin->id + ": " + std::string(message)
                                                : std::string(message);
      host->RecordMessage(std::move(formatted));
    }
    return 0;
  }

  bool RegisterCommand(lua_State* state,
                       std::string_view command_name,
                       int function_index,
                       std::string* error_message) {
    PluginInstance* plugin = FindPluginByState(state);
    if (plugin == nullptr) {
      if (error_message != nullptr) {
        *error_message = "plugin command registration requires an active plugin state";
      }
      return false;
    }
    if (!IsValidIdentifier(command_name)) {
      if (error_message != nullptr) {
        *error_message = "invalid command name: " + std::string(command_name);
      }
      return false;
    }
    if (callbacks.is_command_name_available &&
        !callbacks.is_command_name_available(command_name)) {
      if (error_message != nullptr) {
        *error_message = "command name already used by the host: " + std::string(command_name);
      }
      return false;
    }
    if (commands.contains(std::string(command_name))) {
      if (error_message != nullptr) {
        *error_message = "duplicate plugin command: " + std::string(command_name);
      }
      return false;
    }

    lua_pushvalue(state, function_index);
    const int function_ref = luaL_ref(state, LUA_REGISTRYINDEX);
    commands.emplace(std::string(command_name),
                     PluginCommand{
                         .plugin_id = plugin->id,
                         .state = state,
                         .function_ref = function_ref,
                     });
    RebuildCommandNames();
    return true;
  }

  static int LuaCommandsAdd(lua_State* state) {
    Impl* host = HostFromUpvalue(state);
    const char* name = luaL_checkstring(state, 1);
    luaL_checktype(state, 2, LUA_TFUNCTION);
    std::string error_message;
    if (host == nullptr || !host->RegisterCommand(state, name, 2, &error_message)) {
      return luaL_error(state, "%s",
                        error_message.empty() ? "failed to register plugin command"
                                              : error_message.c_str());
    }
    return 0;
  }

  bool RegisterSidebar(lua_State* state, int table_index, std::string* error_message) {
    PluginInstance* plugin = FindPluginByState(state);
    if (plugin == nullptr) {
      if (error_message != nullptr) {
        *error_message = "plugin sidebar registration requires an active plugin state";
      }
      return false;
    }

    lua_getfield(state, table_index, "id");
    if (!lua_isstring(state, -1)) {
      if (error_message != nullptr) {
        *error_message = "sidebar id must be a string";
      }
      lua_pop(state, 1);
      return false;
    }
    const std::string id = lua_tostring(state, -1);
    lua_pop(state, 1);
    if (!IsValidIdentifier(id)) {
      if (error_message != nullptr) {
        *error_message = "invalid sidebar id: " + id;
      }
      return false;
    }
    if (sidebars.contains(id)) {
      if (error_message != nullptr) {
        *error_message = "duplicate plugin sidebar: " + id;
      }
      return false;
    }

    lua_getfield(state, table_index, "label");
    if (!lua_isstring(state, -1)) {
      if (error_message != nullptr) {
        *error_message = "sidebar label must be a string";
      }
      lua_pop(state, 1);
      return false;
    }
    const std::string label = lua_tostring(state, -1);
    lua_pop(state, 1);

    lua_getfield(state, table_index, "snapshot");
    if (!lua_isfunction(state, -1)) {
      if (error_message != nullptr) {
        *error_message = "sidebar snapshot must be a function";
      }
      lua_pop(state, 1);
      return false;
    }
    const int snapshot_ref = luaL_ref(state, LUA_REGISTRYINDEX);

    int confirm_ref = LUA_NOREF;
    lua_getfield(state, table_index, "on_confirm");
    if (lua_isnil(state, -1)) {
      lua_pop(state, 1);
    } else if (lua_isfunction(state, -1)) {
      confirm_ref = luaL_ref(state, LUA_REGISTRYINDEX);
    } else {
      if (error_message != nullptr) {
        *error_message = "sidebar on_confirm must be a function";
      }
      lua_pop(state, 1);
      luaL_unref(state, LUA_REGISTRYINDEX, snapshot_ref);
      return false;
    }

    sidebars.emplace(id, SidebarProvider{
                            .info =
                                SidebarProviderInfo{
                                    .id = id,
                                    .label = label,
                                    .plugin_id = plugin->id,
                                },
                            .state = state,
                            .snapshot_ref = snapshot_ref,
                            .confirm_ref = confirm_ref,
                        });
    RebuildSidebarProviders();
    return true;
  }

  static int LuaSidebarAdd(lua_State* state) {
    Impl* host = HostFromUpvalue(state);
    luaL_checktype(state, 1, LUA_TTABLE);
    std::string error_message;
    if (host == nullptr || !host->RegisterSidebar(state, 1, &error_message)) {
      return luaL_error(state, "%s",
                        error_message.empty() ? "failed to register plugin sidebar"
                                              : error_message.c_str());
    }
    return 0;
  }

  bool RegisterHoverProvider(lua_State* state, int table_index, std::string* error_message) {
    PluginInstance* plugin = FindPluginByState(state);
    if (plugin == nullptr) {
      if (error_message != nullptr) {
        *error_message = "hover provider registration requires an active plugin";
      }
      return false;
    }

    const int absolute_index = lua_absindex(state, table_index);
    lua_getfield(state, absolute_index, "id");
    if (!lua_isstring(state, -1)) {
      if (error_message != nullptr) {
        *error_message = "hover provider id must be a string";
      }
      lua_pop(state, 1);
      return false;
    }
    const std::string id = lua_tostring(state, -1);
    lua_pop(state, 1);
    if (!IsValidIdentifier(id)) {
      if (error_message != nullptr) {
        *error_message = "invalid hover provider id: " + id;
      }
      return false;
    }
    if (hovers.contains(id)) {
      if (error_message != nullptr) {
        *error_message = "duplicate hover provider: " + id;
      }
      return false;
    }

    lua_getfield(state, absolute_index, "provide");
    if (!lua_isfunction(state, -1)) {
      if (error_message != nullptr) {
        *error_message = "hover provider provide must be a function";
      }
      lua_pop(state, 1);
      return false;
    }
    const int provide_ref = luaL_ref(state, LUA_REGISTRYINDEX);

    hovers.emplace(id, HoverProvider{
                           .id = id,
                           .plugin_id = plugin->id,
                           .state = state,
                           .provide_ref = provide_ref,
                       });
    hover_provider_order.push_back(id);
    return true;
  }

  static int LuaHoverAdd(lua_State* state) {
    Impl* host = HostFromUpvalue(state);
    luaL_checktype(state, 1, LUA_TTABLE);
    std::string error_message;
    if (host == nullptr || !host->RegisterHoverProvider(state, 1, &error_message)) {
      return luaL_error(state, "%s",
                        error_message.empty() ? "failed to register hover provider"
                                              : error_message.c_str());
    }
    return 0;
  }

  bool RegisterMenuEntry(lua_State* state, int table_index, std::string* error_message) {
    PluginInstance* plugin = FindPluginByState(state);
    if (plugin == nullptr) {
      if (error_message != nullptr) {
        *error_message = "menu contribution requires an active plugin state";
      }
      return false;
    }

    const int abs = lua_absindex(state, table_index);

    auto read_string_field = [&](const char* field, bool required) -> std::optional<std::string> {
      lua_getfield(state, abs, field);
      if (lua_isstring(state, -1)) {
        std::string val = lua_tostring(state, -1);
        lua_pop(state, 1);
        return val;
      }
      lua_pop(state, 1);
      if (required) {
        if (error_message != nullptr) {
          *error_message = std::string("menu entry '") + field + "' must be a string";
        }
        return std::nullopt;
      }
      return std::string{};
    };

    auto id_opt = read_string_field("id", true);
    if (!id_opt.has_value()) {
      return false;
    }
    auto menu_opt = read_string_field("menu", true);
    if (!menu_opt.has_value()) {
      return false;
    }
    auto action_opt = read_string_field("action", true);
    if (!action_opt.has_value()) {
      return false;
    }
    auto label_opt = read_string_field("label", true);
    if (!label_opt.has_value()) {
      return false;
    }
    auto accel_opt = read_string_field("accelerator", false);
    bool sep_before = false;
    lua_getfield(state, abs, "separator_before");
    if (lua_isboolean(state, -1)) {
      sep_before = lua_toboolean(state, -1) != 0;
    }
    lua_pop(state, 1);

    if (!IsValidIdentifier(*id_opt)) {
      if (error_message != nullptr) {
        *error_message = "invalid menu entry id: " + *id_opt;
      }
      return false;
    }

    const std::string full_id = plugin->id + "." + *id_opt;
    for (const auto& existing : menu_entries) {
      if (existing.id == full_id) {
        if (error_message != nullptr) {
          *error_message = "duplicate menu entry: " + full_id;
        }
        return false;
      }
    }

    menu_entries.push_back(PluginHost::ContributedMenuEntry{
        .id = full_id,
        .menu = std::move(*menu_opt),
        .action = std::move(*action_opt),
        .label = std::move(*label_opt),
        .accelerator = std::move(*accel_opt),
        .separator_before = sep_before,
        .plugin_id = plugin->id,
    });
    return true;
  }

  static int LuaMenusAdd(lua_State* state) {
    Impl* host = HostFromUpvalue(state);
    luaL_checktype(state, 1, LUA_TTABLE);
    std::string error_message;
    if (host == nullptr || !host->RegisterMenuEntry(state, 1, &error_message)) {
      return luaL_error(state, "%s",
                        error_message.empty() ? "failed to register menu entry"
                                              : error_message.c_str());
    }
    return 0;
  }

  bool RegisterKeybinding(lua_State* state, int table_index, std::string* error_message) {
    PluginInstance* plugin = FindPluginByState(state);
    if (plugin == nullptr) {
      if (error_message != nullptr) {
        *error_message = "keybinding registration requires an active plugin state";
      }
      return false;
    }

    const int abs = lua_absindex(state, table_index);

    auto read_string = [&](const char* field) -> std::optional<std::string> {
      lua_getfield(state, abs, field);
      if (!lua_isstring(state, -1)) {
        lua_pop(state, 1);
        if (error_message != nullptr) {
          *error_message = std::string("keybinding '") + field + "' must be a string";
        }
        return std::nullopt;
      }
      std::string val = lua_tostring(state, -1);
      lua_pop(state, 1);
      return val;
    };

    auto id_opt = read_string("id");
    if (!id_opt.has_value()) {
      return false;
    }
    auto action_opt = read_string("action");
    if (!action_opt.has_value()) {
      return false;
    }
    auto key_opt = read_string("key");
    if (!key_opt.has_value()) {
      return false;
    }
    std::string context;
    lua_getfield(state, abs, "context");
    if (lua_isstring(state, -1)) {
      context = lua_tostring(state, -1);
    }
    lua_pop(state, 1);

    if (!IsValidIdentifier(*id_opt)) {
      if (error_message != nullptr) {
        *error_message = "invalid keybinding id: " + *id_opt;
      }
      return false;
    }

    const std::string full_id = plugin->id + "." + *id_opt;
    for (const auto& existing : keybindings) {
      if (existing.id == full_id) {
        if (error_message != nullptr) {
          *error_message = "duplicate keybinding: " + full_id;
        }
        return false;
      }
    }

    keybindings.push_back(PluginHost::ContributedKeybinding{
        .id = full_id,
        .action = std::move(*action_opt),
        .key_chord = std::move(*key_opt),
        .context = std::move(context),
        .plugin_id = plugin->id,
    });
    return true;
  }

  static int LuaKeybindingsAdd(lua_State* state) {
    Impl* host = HostFromUpvalue(state);
    luaL_checktype(state, 1, LUA_TTABLE);
    std::string error_message;
    if (host == nullptr || !host->RegisterKeybinding(state, 1, &error_message)) {
      return luaL_error(state, "%s",
                        error_message.empty() ? "failed to register keybinding"
                                              : error_message.c_str());
    }
    return 0;
  }

  bool RegisterSetting(lua_State* state, int table_index, std::string* error_message) {
    PluginInstance* plugin = FindPluginByState(state);
    if (plugin == nullptr) {
      if (error_message != nullptr) {
        *error_message = "setting declaration requires an active plugin state";
      }
      return false;
    }

    const int abs = lua_absindex(state, table_index);

    auto read_string = [&](const char* field, bool required) -> std::optional<std::string> {
      lua_getfield(state, abs, field);
      if (lua_isstring(state, -1)) {
        std::string val = lua_tostring(state, -1);
        lua_pop(state, 1);
        return val;
      }
      lua_pop(state, 1);
      if (required) {
        if (error_message != nullptr) {
          *error_message = std::string("setting '") + field + "' must be a string";
        }
        return std::nullopt;
      }
      return std::string{};
    };

    auto id_opt = read_string("id", true);
    if (!id_opt.has_value()) {
      return false;
    }
    auto type_opt = read_string("type", true);
    if (!type_opt.has_value()) {
      return false;
    }

    const std::string& type = *type_opt;
    static const char* const kValidTypes[] = {"bool", "int", "float", "string", "enum"};
    bool type_valid = false;
    for (const char* t : kValidTypes) {
      if (type == t) {
        type_valid = true;
        break;
      }
    }
    if (!type_valid) {
      if (error_message != nullptr) {
        *error_message = "setting type must be one of: bool, int, float, string, enum";
      }
      return false;
    }

    auto label_opt = read_string("label", false);
    auto desc_opt = read_string("description", false);
    auto scope_opt = read_string("scope", false);
    auto default_opt = read_string("default", false);

    if (!IsValidIdentifier(*id_opt)) {
      if (error_message != nullptr) {
        *error_message = "invalid setting id: " + *id_opt;
      }
      return false;
    }

    const std::string full_id = plugin->id + "." + *id_opt;
    for (const auto& existing : settings) {
      if (existing.id == full_id) {
        if (error_message != nullptr) {
          *error_message = "duplicate setting: " + full_id;
        }
        return false;
      }
    }

    std::vector<std::string> enum_values;
    if (type == "enum") {
      lua_getfield(state, abs, "enum_values");
      if (lua_istable(state, -1)) {
        const lua_Integer n = static_cast<lua_Integer>(lua_rawlen(state, -1));
        for (lua_Integer i = 1; i <= n; ++i) {
          lua_rawgeti(state, -1, i);
          if (lua_isstring(state, -1)) {
            enum_values.emplace_back(lua_tostring(state, -1));
          }
          lua_pop(state, 1);
        }
      }
      lua_pop(state, 1);
    }

    settings.push_back(PluginHost::ContributedSettingSpec{
        .id = full_id,
        .label = std::move(*label_opt),
        .description = std::move(*desc_opt),
        .type = type,
        .scope = std::move(*scope_opt),
        .default_value = std::move(*default_opt),
        .enum_values = std::move(enum_values),
        .plugin_id = plugin->id,
    });
    return true;
  }

  static int LuaSettingsDeclare(lua_State* state) {
    Impl* host = HostFromUpvalue(state);
    luaL_checktype(state, 1, LUA_TTABLE);
    std::string error_message;
    if (host == nullptr || !host->RegisterSetting(state, 1, &error_message)) {
      return luaL_error(state, "%s",
                        error_message.empty() ? "failed to declare setting"
                                              : error_message.c_str());
    }
    return 0;
  }

  static int LuaSettingsGet(lua_State* state) {
    Impl* host = HostFromUpvalue(state);
    const char* id = luaL_checkstring(state, 1);
    if (host == nullptr || !host->callbacks.get_setting) {
      lua_pushnil(state);
      return 1;
    }
    const std::optional<std::string> value = host->callbacks.get_setting(id);
    if (!value.has_value()) {
      lua_pushnil(state);
      return 1;
    }
    lua_pushlstring(state, value->c_str(), value->size());
    return 1;
  }

  bool RegisterStatusItem(lua_State* state, int table_index, std::string* error_message) {
    PluginInstance* plugin = FindPluginByState(state);
    if (plugin == nullptr) {
      if (error_message != nullptr) {
        *error_message = "status item registration requires an active plugin state";
      }
      return false;
    }

    const int abs = lua_absindex(state, table_index);

    auto read_string = [&](const char* field, bool required) -> std::optional<std::string> {
      lua_getfield(state, abs, field);
      if (lua_isstring(state, -1)) {
        std::string val = lua_tostring(state, -1);
        lua_pop(state, 1);
        return val;
      }
      lua_pop(state, 1);
      if (required) {
        if (error_message != nullptr) {
          *error_message = std::string("status item '") + field + "' must be a string";
        }
        return std::nullopt;
      }
      return std::string{};
    };

    auto id_opt = read_string("id", true);
    if (!id_opt.has_value()) {
      return false;
    }
    auto text_opt = read_string("text", false);
    auto tooltip_opt = read_string("tooltip", false);
    auto align_opt = read_string("alignment", false);

    int priority = 0;
    lua_getfield(state, abs, "priority");
    if (lua_isinteger(state, -1)) {
      priority = static_cast<int>(lua_tointeger(state, -1));
    }
    lua_pop(state, 1);

    if (!IsValidIdentifier(*id_opt)) {
      if (error_message != nullptr) {
        *error_message = "invalid status item id: " + *id_opt;
      }
      return false;
    }

    const std::string full_id = plugin->id + "." + *id_opt;
    if (status_items.contains(full_id)) {
      if (error_message != nullptr) {
        *error_message = "duplicate status item: " + full_id;
      }
      return false;
    }

    PluginHost::ContributedStatusItem item{
        .id = full_id,
        .text = std::move(*text_opt),
        .tooltip = std::move(*tooltip_opt),
        .alignment = std::move(*align_opt),
        .priority = priority,
        .plugin_id = plugin->id,
    };
    status_item_order.push_back(item);
    status_items.emplace(full_id, std::move(item));
    return true;
  }

  static int LuaStatusAdd(lua_State* state) {
    Impl* host = HostFromUpvalue(state);
    luaL_checktype(state, 1, LUA_TTABLE);
    std::string error_message;
    if (host == nullptr || !host->RegisterStatusItem(state, 1, &error_message)) {
      return luaL_error(state, "%s",
                        error_message.empty() ? "failed to register status item"
                                              : error_message.c_str());
    }
    return 0;
  }

  static int LuaStatusUpdate(lua_State* state) {
    Impl* host = HostFromUpvalue(state);
    const char* id = luaL_checkstring(state, 1);
    luaL_checktype(state, 2, LUA_TTABLE);
    if (host == nullptr) {
      return 0;
    }
    const PluginInstance* plugin = host->FindPluginByState(state);
    if (plugin == nullptr) {
      return 0;
    }
    const std::string full_id = plugin->id + "." + std::string(id);
    auto it = host->status_items.find(full_id);
    if (it == host->status_items.end()) {
      return 0;
    }
    lua_getfield(state, 2, "text");
    if (lua_isstring(state, -1)) {
      it->second.text = lua_tostring(state, -1);
    }
    lua_pop(state, 1);
    lua_getfield(state, 2, "tooltip");
    if (lua_isstring(state, -1)) {
      it->second.tooltip = lua_tostring(state, -1);
    }
    lua_pop(state, 1);
    // Sync order vector.
    for (auto& order_item : host->status_item_order) {
      if (order_item.id == full_id) {
        order_item.text = it->second.text;
        order_item.tooltip = it->second.tooltip;
        break;
      }
    }
    if (host->callbacks.request_status_redraw) {
      host->callbacks.request_status_redraw();
    }
    return 0;
  }

  static int LuaFormattersAdd(lua_State* state) {
    Impl* host = HostFromUpvalue(state);
    luaL_checktype(state, 1, LUA_TTABLE);
    const PluginInstance* plugin = host->FindPluginByState(state);
    if (plugin == nullptr) {
      return luaL_error(state, "formatter registration requires an active plugin state");
    }

    auto read_string = [&](const char* field) -> std::optional<std::string> {
      lua_getfield(state, 1, field);
      if (!lua_isstring(state, -1)) {
        lua_pop(state, 1);
        return std::nullopt;
      }
      std::string val = lua_tostring(state, -1);
      lua_pop(state, 1);
      return val;
    };

    auto id_opt = read_string("id");
    auto language_id_opt = read_string("language_id");
    auto label_opt = read_string("label");
    if (!id_opt || !language_id_opt || !label_opt) {
      return luaL_error(state, "formatter requires id, language_id, and label");
    }

    lua_getfield(state, 1, "command");
    if (!lua_istable(state, -1)) {
      lua_pop(state, 1);
      return luaL_error(state, "formatter command must be an array");
    }
    std::vector<std::string> command;
    for (lua_Integer i = 1; ; ++i) {
      lua_geti(state, -1, i);
      if (lua_isnil(state, -1)) {
        lua_pop(state, 1);
        break;
      }
      if (!lua_isstring(state, -1)) {
        lua_pop(state, 2);
        return luaL_error(state, "formatter command must be a string array");
      }
      command.push_back(lua_tostring(state, -1));
      lua_pop(state, 1);
    }
    lua_pop(state, 1);

    if (command.empty()) {
      return luaL_error(state, "formatter command cannot be empty");
    }

    host->formatters.push_back(PluginHost::ContributedFormatter{
        .id = plugin->id + "." + *id_opt,
        .language_id = std::move(*language_id_opt),
        .label = std::move(*label_opt),
        .command = std::move(command),
        .plugin_id = plugin->id,
    });
    return 0;
  }

  static int LuaSaveParticipantsAdd(lua_State* state) {
    Impl* host = HostFromUpvalue(state);
    const char* id = luaL_checkstring(state, 1);
    luaL_checktype(state, 2, LUA_TFUNCTION);
    const PluginInstance* plugin = host->FindPluginByState(state);
    if (plugin == nullptr) {
      return luaL_error(state, "save participant registration requires an active plugin state");
    }
    lua_pushvalue(state, 2);
    const int function_ref = luaL_ref(state, LUA_REGISTRYINDEX);
    host->save_participants.push_back(PluginHost::ContributedSaveParticipant{
        .id = plugin->id + "." + std::string(id),
        .plugin_id = plugin->id,
    });
    host->save_participant_runtimes.push_back(SaveParticipantRuntime{
        .id = plugin->id + "." + std::string(id),
        .plugin_id = plugin->id,
        .state = state,
        .function_ref = function_ref,
    });
    return 0;
  }

  static int LuaCompletionAdd(lua_State* state) {
    Impl* host = HostFromUpvalue(state);
    luaL_checktype(state, 1, LUA_TTABLE);
    const PluginInstance* plugin = host->FindPluginByState(state);
    if (plugin == nullptr) {
      return luaL_error(state, "completion registration requires an active plugin state");
    }

    auto read_string = [&](const char* field) -> std::optional<std::string> {
      lua_getfield(state, 1, field);
      if (!lua_isstring(state, -1)) {
        lua_pop(state, 1);
        return std::nullopt;
      }
      std::string val = lua_tostring(state, -1);
      lua_pop(state, 1);
      return val;
    };

    auto id_opt = read_string("id");
    auto language_id_opt = read_string("language_id");
    if (!id_opt || !language_id_opt) {
      return luaL_error(state, "completion requires id and language_id");
    }

    std::string trigger_characters;
    if (auto trigger_opt = read_string("trigger_characters")) {
      trigger_characters = std::move(*trigger_opt);
    }

    lua_getfield(state, 1, "provide");
    const bool has_provider = lua_isfunction(state, -1);
    int provide_ref = LUA_NOREF;
    if (has_provider) {
      provide_ref = luaL_ref(state, LUA_REGISTRYINDEX);
    } else {
      lua_pop(state, 1);
    }

    host->completions.push_back(PluginHost::ContributedCompletion{
        .id = plugin->id + "." + *id_opt,
        .language_id = std::move(*language_id_opt),
        .trigger_characters = std::move(trigger_characters),
        .plugin_id = plugin->id,
    });
    if (has_provider) {
      const auto& contributed = host->completions.back();
      host->completion_runtimes.push_back(CompletionRuntime{
          .id = contributed.id,
          .language_id = contributed.language_id,
          .trigger_characters = contributed.trigger_characters,
          .plugin_id = contributed.plugin_id,
          .state = state,
          .provide_ref = provide_ref,
      });
    }
    return 0;
  }

  static int LuaCodeActionAdd(lua_State* state) {
    Impl* host = HostFromUpvalue(state);
    luaL_checktype(state, 1, LUA_TTABLE);
    const PluginInstance* plugin = host->FindPluginByState(state);
    if (plugin == nullptr) {
      return luaL_error(state, "code action registration requires an active plugin state");
    }

    auto read_string = [&](const char* field) -> std::optional<std::string> {
      lua_getfield(state, 1, field);
      if (!lua_isstring(state, -1)) {
        lua_pop(state, 1);
        return std::nullopt;
      }
      std::string val = lua_tostring(state, -1);
      lua_pop(state, 1);
      return val;
    };

    auto id_opt = read_string("id");
    auto language_id_opt = read_string("language_id");
    if (!id_opt || !language_id_opt) {
      return luaL_error(state, "code action requires id and language_id");
    }

    lua_getfield(state, 1, "provide");
    const bool has_provider = lua_isfunction(state, -1);
    int provide_ref = LUA_NOREF;
    if (has_provider) {
      provide_ref = luaL_ref(state, LUA_REGISTRYINDEX);
    } else {
      lua_pop(state, 1);
    }

    host->code_actions.push_back(PluginHost::ContributedCodeAction{
        .id = plugin->id + "." + *id_opt,
        .language_id = std::move(*language_id_opt),
        .plugin_id = plugin->id,
    });
    if (has_provider) {
      const auto& contributed = host->code_actions.back();
      host->code_action_runtimes.push_back(CodeActionRuntime{
          .id = contributed.id,
          .language_id = contributed.language_id,
          .plugin_id = contributed.plugin_id,
          .state = state,
          .provide_ref = provide_ref,
      });
    }
    return 0;
  }

  static int LuaTaskAdd(lua_State* state) {
    Impl* host = HostFromUpvalue(state);
    luaL_checktype(state, 1, LUA_TTABLE);
    const PluginInstance* plugin = host->FindPluginByState(state);
    if (plugin == nullptr) {
      return luaL_error(state, "task registration requires an active plugin state");
    }

    auto read_string = [&](const char* field) -> std::optional<std::string> {
      lua_getfield(state, 1, field);
      if (!lua_isstring(state, -1)) {
        lua_pop(state, 1);
        return std::nullopt;
      }
      std::string val = lua_tostring(state, -1);
      lua_pop(state, 1);
      return val;
    };

    auto id_opt = read_string("id");
    auto label_opt = read_string("label");
    if (!id_opt || !label_opt) {
      return luaL_error(state, "task requires id and label");
    }

    lua_getfield(state, 1, "command");
    if (!lua_istable(state, -1)) {
      lua_pop(state, 1);
      return luaL_error(state, "task command must be an array");
    }
    std::vector<std::string> command;
    for (lua_Integer i = 1; ; ++i) {
      lua_geti(state, -1, i);
      if (lua_isnil(state, -1)) {
        lua_pop(state, 1);
        break;
      }
      if (!lua_isstring(state, -1)) {
        lua_pop(state, 2);
        return luaL_error(state, "task command must be a string array");
      }
      command.push_back(lua_tostring(state, -1));
      lua_pop(state, 1);
    }
    lua_pop(state, 1);

    if (command.empty()) {
      return luaL_error(state, "task command cannot be empty");
    }

    std::string group;
    if (auto group_opt = read_string("group")) {
      group = std::move(*group_opt);
    }

    std::string cwd;
    if (auto cwd_opt = read_string("cwd")) {
      cwd = std::move(*cwd_opt);
    }

    bool run_in_shell = false;
    lua_getfield(state, 1, "run_in_shell");
    if (lua_isboolean(state, -1)) {
      run_in_shell = lua_toboolean(state, -1) != 0;
    }
    lua_pop(state, 1);

    host->tasks.push_back(PluginHost::ContributedTask{
        .id = plugin->id + "." + *id_opt,
        .label = std::move(*label_opt),
        .group = std::move(group),
        .command = std::move(command),
        .cwd = std::move(cwd),
        .run_in_shell = run_in_shell,
        .plugin_id = plugin->id,
    });
    return 0;
  }

  static int LuaLspAdd(lua_State* state) {
    Impl* host = HostFromUpvalue(state);
    luaL_checktype(state, 1, LUA_TTABLE);
    const PluginInstance* plugin = host->FindPluginByState(state);
    if (plugin == nullptr) {
      return luaL_error(state, "language server registration requires an active plugin state");
    }

    auto read_string = [&](const char* field) -> std::optional<std::string> {
      lua_getfield(state, 1, field);
      if (!lua_isstring(state, -1)) {
        lua_pop(state, 1);
        return std::nullopt;
      }
      std::string val = lua_tostring(state, -1);
      lua_pop(state, 1);
      return val;
    };

    auto id_opt = read_string("id");
    auto language_id_opt = read_string("language_id");
    if (!id_opt || !language_id_opt) {
      return luaL_error(state, "language server requires id and language_id");
    }

    lua_getfield(state, 1, "command");
    if (!lua_istable(state, -1)) {
      lua_pop(state, 1);
      return luaL_error(state, "language server command must be an array");
    }
    std::vector<std::string> command;
    for (lua_Integer i = 1; ; ++i) {
      lua_geti(state, -1, i);
      if (lua_isnil(state, -1)) {
        lua_pop(state, 1);
        break;
      }
      if (!lua_isstring(state, -1)) {
        lua_pop(state, 2);
        return luaL_error(state, "language server command must be a string array");
      }
      command.push_back(lua_tostring(state, -1));
      lua_pop(state, 1);
    }
    lua_pop(state, 1);

    if (command.empty()) {
      return luaL_error(state, "language server command cannot be empty");
    }

    host->language_servers.push_back(PluginHost::ContributedLanguageServer{
        .id = plugin->id + "." + *id_opt,
        .language_id = std::move(*language_id_opt),
        .command = std::move(command),
        .plugin_id = plugin->id,
    });
    return 0;
  }

  static int LuaToolAdd(lua_State* state) {
    Impl* host = HostFromUpvalue(state);
    luaL_checktype(state, 1, LUA_TTABLE);
    const PluginInstance* plugin = host->FindPluginByState(state);
    if (plugin == nullptr) {
      return luaL_error(state, "tool registration requires an active plugin state");
    }

    auto read_string = [&](const char* field) -> std::optional<std::string> {
      lua_getfield(state, 1, field);
      if (!lua_isstring(state, -1)) {
        lua_pop(state, 1);
        return std::nullopt;
      }
      std::string val = lua_tostring(state, -1);
      lua_pop(state, 1);
      return val;
    };

    auto id_opt = read_string("id");
    auto platform_opt = read_string("platform");
    auto url_opt = read_string("url");
    auto sha256_opt = read_string("sha256");
    if (!id_opt || !platform_opt || !url_opt || !sha256_opt) {
      return luaL_error(state, "tool requires id, platform, url, and sha256");
    }

    std::string label;
    if (auto label_opt = read_string("label")) {
      label = std::move(*label_opt);
    }

    std::string install_dir;
    if (auto dir_opt = read_string("install_dir")) {
      install_dir = std::move(*dir_opt);
    }

    host->tools.push_back(PluginHost::ContributedTool{
        .id = plugin->id + "." + *id_opt,
        .label = std::move(label),
        .platform = std::move(*platform_opt),
        .download_url = std::move(*url_opt),
        .sha256 = std::move(*sha256_opt),
        .install_dir = std::move(install_dir),
        .plugin_id = plugin->id,
    });
    return 0;
  }

  static int LuaDebuggerAdd(lua_State* state) {
    Impl* host = HostFromUpvalue(state);
    luaL_checktype(state, 1, LUA_TTABLE);
    const PluginInstance* plugin = host->FindPluginByState(state);
    if (plugin == nullptr) {
      return luaL_error(state, "debugger registration requires an active plugin state");
    }

    auto read_string = [&](const char* field) -> std::optional<std::string> {
      lua_getfield(state, 1, field);
      if (!lua_isstring(state, -1)) {
        lua_pop(state, 1);
        return std::nullopt;
      }
      std::string val = lua_tostring(state, -1);
      lua_pop(state, 1);
      return val;
    };

    auto id_opt = read_string("id");
    auto type_opt = read_string("type");
    if (!id_opt || !type_opt) {
      return luaL_error(state, "debugger requires id and type");
    }

    lua_getfield(state, 1, "command");
    if (!lua_istable(state, -1)) {
      lua_pop(state, 1);
      return luaL_error(state, "debugger command must be an array");
    }
    std::vector<std::string> command;
    for (lua_Integer i = 1; ; ++i) {
      lua_geti(state, -1, i);
      if (lua_isnil(state, -1)) {
        lua_pop(state, 1);
        break;
      }
      if (!lua_isstring(state, -1)) {
        lua_pop(state, 2);
        return luaL_error(state, "debugger command must be a string array");
      }
      command.push_back(lua_tostring(state, -1));
      lua_pop(state, 1);
    }
    lua_pop(state, 1);

    if (command.empty()) {
      return luaL_error(state, "debugger command cannot be empty");
    }

    host->debuggers.push_back(PluginHost::ContributedDebugger{
        .id = plugin->id + "." + *id_opt,
        .type = std::move(*type_opt),
        .command = std::move(command),
        .plugin_id = plugin->id,
    });
    return 0;
  }

  static int LuaTestProviderAdd(lua_State* state) {
    Impl* host = HostFromUpvalue(state);
    luaL_checktype(state, 1, LUA_TTABLE);
    const PluginInstance* plugin = host->FindPluginByState(state);
    if (plugin == nullptr) {
      return luaL_error(state, "test provider registration requires an active plugin state");
    }

    auto read_string = [&](const char* field) -> std::optional<std::string> {
      lua_getfield(state, 1, field);
      if (!lua_isstring(state, -1)) {
        lua_pop(state, 1);
        return std::nullopt;
      }
      std::string val = lua_tostring(state, -1);
      lua_pop(state, 1);
      return val;
    };

    auto id_opt = read_string("id");
    auto language_id_opt = read_string("language_id");
    if (!id_opt || !language_id_opt) {
      return luaL_error(state, "test provider requires id and language_id");
    }

    lua_getfield(state, 1, "discover");
    const bool has_discover = lua_isfunction(state, -1);
    int discover_ref = LUA_NOREF;
    if (has_discover) {
      discover_ref = luaL_ref(state, LUA_REGISTRYINDEX);
    } else {
      lua_pop(state, 1);
    }

    lua_getfield(state, 1, "run");
    const bool has_run = lua_isfunction(state, -1);
    int run_ref = LUA_NOREF;
    if (has_run) {
      run_ref = luaL_ref(state, LUA_REGISTRYINDEX);
    } else {
      lua_pop(state, 1);
    }

    host->test_providers.push_back(PluginHost::ContributedTestProvider{
        .id = plugin->id + "." + *id_opt,
        .language_id = std::move(*language_id_opt),
        .plugin_id = plugin->id,
    });
    if (has_discover || has_run) {
      const auto& contributed = host->test_providers.back();
      host->test_provider_runtimes.push_back(TestProviderRuntime{
          .id = contributed.id,
          .language_id = contributed.language_id,
          .plugin_id = contributed.plugin_id,
          .state = state,
          .discover_ref = discover_ref,
          .run_ref = run_ref,
      });
    }
    return 0;
  }

  static int LuaScmProviderAdd(lua_State* state) {
    Impl* host = HostFromUpvalue(state);
    const PluginInstance* plugin = host->FindPluginByState(state);
    if (plugin == nullptr) {
      return luaL_error(state, "scm provider registration requires an active plugin state");
    }

    std::string id;
    std::string label;
    int snapshot_ref = LUA_NOREF;
    if (lua_istable(state, 1)) {
      auto read_string = [&](const char* field) -> std::optional<std::string> {
        lua_getfield(state, 1, field);
        if (!lua_isstring(state, -1)) {
          lua_pop(state, 1);
          return std::nullopt;
        }
        std::string value = lua_tostring(state, -1);
        lua_pop(state, 1);
        return value;
      };
      auto id_opt = read_string("id");
      auto label_opt = read_string("label");
      if (!id_opt || !label_opt) {
        return luaL_error(state, "scm provider requires id and label");
      }
      id = std::move(*id_opt);
      label = std::move(*label_opt);

      lua_getfield(state, 1, "snapshot");
      if (lua_isfunction(state, -1)) {
        snapshot_ref = luaL_ref(state, LUA_REGISTRYINDEX);
      } else {
        lua_pop(state, 1);
      }
    } else {
      id = luaL_checkstring(state, 1);
      label = luaL_checkstring(state, 2);
    }

    host->scm_providers.push_back(PluginHost::ContributedScmProvider{
        .id = plugin->id + "." + id,
        .label = std::move(label),
        .plugin_id = plugin->id,
    });
    if (snapshot_ref != LUA_NOREF && snapshot_ref != LUA_REFNIL) {
      host->scm_provider_runtimes.push_back(ScmProviderRuntime{
          .id = host->scm_providers.back().id,
          .plugin_id = plugin->id,
          .state = state,
          .snapshot_ref = snapshot_ref,
      });
    }
    return 0;
  }

  static int LuaAnnotationProviderAdd(lua_State* state) {
    Impl* host = HostFromUpvalue(state);
    luaL_checktype(state, 1, LUA_TTABLE);
    const PluginInstance* plugin = host->FindPluginByState(state);
    if (plugin == nullptr) {
      return luaL_error(state, "annotation provider registration requires an active plugin state");
    }

    auto read_string = [&](const char* field) -> std::optional<std::string> {
      lua_getfield(state, 1, field);
      if (!lua_isstring(state, -1)) {
        lua_pop(state, 1);
        return std::nullopt;
      }
      std::string val = lua_tostring(state, -1);
      lua_pop(state, 1);
      return val;
    };

    auto id_opt = read_string("id");
    auto label_opt = read_string("label");
    auto type_opt = read_string("type");
    auto language_id_opt = read_string("language_id");
    if (!id_opt || !label_opt || !type_opt || !language_id_opt) {
      return luaL_error(state, "annotation provider requires id, label, type, and language_id");
    }

    host->annotation_providers.push_back(PluginHost::ContributedAnnotationProvider{
        .id = plugin->id + "." + *id_opt,
        .label = std::move(*label_opt),
        .type = std::move(*type_opt),
        .language_id = std::move(*language_id_opt),
        .plugin_id = plugin->id,
    });
    lua_getfield(state, 1, "provide");
    const bool has_provide = lua_isfunction(state, -1);
    int provide_ref = LUA_NOREF;
    if (has_provide) {
      provide_ref = luaL_ref(state, LUA_REGISTRYINDEX);
    } else {
      lua_pop(state, 1);
    }
    if (has_provide) {
      const auto& contributed = host->annotation_providers.back();
      host->annotation_provider_runtimes.push_back(AnnotationProviderRuntime{
          .id = contributed.id,
          .language_id = contributed.language_id,
          .type = contributed.type,
          .plugin_id = contributed.plugin_id,
          .state = state,
          .provide_ref = provide_ref,
      });
    }
    return 0;
  }

  static int LuaAuthProviderAdd(lua_State* state) {
    Impl* host = HostFromUpvalue(state);
    const PluginInstance* plugin = host->FindPluginByState(state);
    if (plugin == nullptr) {
      return luaL_error(state, "auth provider registration requires an active plugin state");
    }

    std::string id;
    std::string label;
    int login_ref = LUA_NOREF;
    int refresh_ref = LUA_NOREF;
    int logout_ref = LUA_NOREF;
    if (lua_istable(state, 1)) {
      auto read_string = [&](const char* field) -> std::optional<std::string> {
        lua_getfield(state, 1, field);
        if (!lua_isstring(state, -1)) {
          lua_pop(state, 1);
          return std::nullopt;
        }
        std::string value = lua_tostring(state, -1);
        lua_pop(state, 1);
        return value;
      };
      auto id_opt = read_string("id");
      auto label_opt = read_string("label");
      if (!id_opt || !label_opt) {
        return luaL_error(state, "auth provider requires id and label");
      }
      id = std::move(*id_opt);
      label = std::move(*label_opt);

      lua_getfield(state, 1, "login");
      if (lua_isfunction(state, -1)) {
        login_ref = luaL_ref(state, LUA_REGISTRYINDEX);
      } else {
        lua_pop(state, 1);
      }
      lua_getfield(state, 1, "refresh");
      if (lua_isfunction(state, -1)) {
        refresh_ref = luaL_ref(state, LUA_REGISTRYINDEX);
      } else {
        lua_pop(state, 1);
      }
      lua_getfield(state, 1, "logout");
      if (lua_isfunction(state, -1)) {
        logout_ref = luaL_ref(state, LUA_REGISTRYINDEX);
      } else {
        lua_pop(state, 1);
      }
    } else {
      id = luaL_checkstring(state, 1);
      label = luaL_checkstring(state, 2);
    }

    host->auth_providers.push_back(PluginHost::ContributedAuthProvider{
        .id = plugin->id + "." + id,
        .label = std::move(label),
        .plugin_id = plugin->id,
    });
    if (login_ref != LUA_NOREF || refresh_ref != LUA_NOREF || logout_ref != LUA_NOREF) {
      host->auth_provider_runtimes.push_back(AuthProviderRuntime{
          .id = host->auth_providers.back().id,
          .plugin_id = plugin->id,
          .state = state,
          .login_ref = login_ref,
          .refresh_ref = refresh_ref,
          .logout_ref = logout_ref,
      });
    }
    return 0;
  }

  static int LuaAiProviderAdd(lua_State* state) {
    Impl* host = HostFromUpvalue(state);
    luaL_checktype(state, 1, LUA_TTABLE);
    const PluginInstance* plugin = host->FindPluginByState(state);
    if (plugin == nullptr) {
      return luaL_error(state, "AI provider registration requires an active plugin state");
    }

    auto read_string = [&](const char* field) -> std::optional<std::string> {
      lua_getfield(state, 1, field);
      if (!lua_isstring(state, -1)) {
        lua_pop(state, 1);
        return std::nullopt;
      }
      std::string val = lua_tostring(state, -1);
      lua_pop(state, 1);
      return val;
    };

    auto id_opt = read_string("id");
    auto label_opt = read_string("label");
    auto type_opt = read_string("type");
    if (!id_opt || !label_opt || !type_opt) {
      return luaL_error(state, "AI provider requires id, label, and type");
    }

    lua_getfield(state, 1, "models");
    std::vector<std::string> models;
    if (lua_istable(state, -1)) {
      for (lua_Integer i = 1; ; ++i) {
        lua_geti(state, -1, i);
        if (lua_isnil(state, -1)) {
          lua_pop(state, 1);
          break;
        }
        if (lua_isstring(state, -1)) {
          models.push_back(lua_tostring(state, -1));
        }
        lua_pop(state, 1);
      }
    }
    lua_pop(state, 1);

    host->ai_providers.push_back(PluginHost::ContributedAiProvider{
        .id = plugin->id + "." + *id_opt,
        .label = std::move(*label_opt),
        .type = std::move(*type_opt),
        .models = std::move(models),
        .plugin_id = plugin->id,
    });
    return 0;
  }

  static int LuaExternalAgentAdd(lua_State* state) {
    Impl* host = HostFromUpvalue(state);
    luaL_checktype(state, 1, LUA_TTABLE);
    const PluginInstance* plugin = host->FindPluginByState(state);
    if (plugin == nullptr) {
      return luaL_error(state, "external agent registration requires an active plugin state");
    }

    auto read_string = [&](const char* field) -> std::optional<std::string> {
      lua_getfield(state, 1, field);
      if (!lua_isstring(state, -1)) {
        lua_pop(state, 1);
        return std::nullopt;
      }
      std::string val = lua_tostring(state, -1);
      lua_pop(state, 1);
      return val;
    };

    auto id_opt = read_string("id");
    auto label_opt = read_string("label");
    auto protocol_opt = read_string("protocol");
    auto read_string_array = [&](const char* field) -> std::optional<std::vector<std::string>> {
      lua_getfield(state, 1, field);
      if (!lua_istable(state, -1)) {
        lua_pop(state, 1);
        return std::nullopt;
      }
      std::vector<std::string> values;
      for (lua_Integer i = 1;; ++i) {
        lua_geti(state, -1, i);
        if (lua_isnil(state, -1)) {
          lua_pop(state, 1);
          break;
        }
        if (!lua_isstring(state, -1)) {
          lua_pop(state, 2);
          return std::nullopt;
        }
        values.emplace_back(lua_tostring(state, -1));
        lua_pop(state, 1);
      }
      lua_pop(state, 1);
      return values;
    };

    auto command_opt = read_string_array("command");
    if (!id_opt || !label_opt || !protocol_opt || !command_opt || command_opt->empty()) {
      return luaL_error(state,
                        "external agent requires id, label, protocol, and non-empty command");
    }

    lua_getfield(state, 1, "capabilities");
    std::vector<std::string> capabilities;
    if (lua_istable(state, -1)) {
      for (lua_Integer i = 1; ; ++i) {
        lua_geti(state, -1, i);
        if (lua_isnil(state, -1)) {
          lua_pop(state, 1);
          break;
        }
        if (lua_isstring(state, -1)) {
          capabilities.push_back(lua_tostring(state, -1));
        }
        lua_pop(state, 1);
      }
    }
    lua_pop(state, 1);

    PluginHost::ContributedExternalAgent contributed;
    contributed.id = plugin->id + "." + *id_opt;
    contributed.label = std::move(*label_opt);
    contributed.protocol = std::move(*protocol_opt);
    contributed.command = std::move(*command_opt);
    contributed.capabilities = std::move(capabilities);
    contributed.plugin_id = plugin->id;
    host->external_agents.push_back(std::move(contributed));
    return 0;
  }

  static int LuaMcpToolAdd(lua_State* state) {
    Impl* host = HostFromUpvalue(state);
    luaL_checktype(state, 1, LUA_TTABLE);
    const PluginInstance* plugin = host->FindPluginByState(state);
    if (plugin == nullptr) {
      return luaL_error(state, "MCP tool registration requires an active plugin state");
    }

    auto read_string = [&](const char* field) -> std::optional<std::string> {
      lua_getfield(state, 1, field);
      if (!lua_isstring(state, -1)) {
        lua_pop(state, 1);
        return std::nullopt;
      }
      std::string val = lua_tostring(state, -1);
      lua_pop(state, 1);
      return val;
    };

    auto id_opt = read_string("id");
    auto name_opt = read_string("name");
    auto description_opt = read_string("description");
    auto schema_opt = read_string("input_schema");
    if (!id_opt || !name_opt || !description_opt || !schema_opt) {
      return luaL_error(state,
                        "MCP tool requires id, name, description, and input_schema");
    }

    host->mcp_tools.push_back(PluginHost::ContributedMcpTool{
        .id = plugin->id + "." + *id_opt,
        .name = std::move(*name_opt),
        .description = std::move(*description_opt),
        .input_schema = std::move(*schema_opt),
        .plugin_id = plugin->id,
    });
    lua_getfield(state, 1, "run");
    const bool has_run = lua_isfunction(state, -1);
    int run_ref = LUA_NOREF;
    if (has_run) {
      run_ref = luaL_ref(state, LUA_REGISTRYINDEX);
    } else {
      lua_pop(state, 1);
    }
    if (has_run) {
      host->mcp_tool_runtimes.push_back(McpToolRuntime{
          .id = host->mcp_tools.back().id,
          .plugin_id = plugin->id,
          .state = state,
          .run_ref = run_ref,
      });
    }
    return 0;
  }

  static int LuaWorkspaceProjectRoot(lua_State* state) {
    Impl* host = HostFromUpvalue(state);
    if (host == nullptr || host->current_project_root.empty()) {
      lua_pushnil(state);
      return 1;
    }
    lua_pushstring(state, host->current_project_root.generic_string().c_str());
    return 1;
  }

  static int LuaWorkspaceOpenFile(lua_State* state) {
    Impl* host = HostFromUpvalue(state);
    const char* raw_path = luaL_checkstring(state, 1);
    if (host == nullptr || !host->callbacks.open_file) {
      lua_pushboolean(state, 0);
      return 1;
    }

    const std::filesystem::path path =
        ResolveRuntimePath(host->current_project_root, std::filesystem::path(raw_path));
    if (path.empty()) {
      lua_pushboolean(state, 0);
      return 1;
    }

    const lua_Integer line = luaL_optinteger(state, 2, 0);
    const lua_Integer column = luaL_optinteger(state, 3, 0);
    lua_pushboolean(
        state,
        host->callbacks.open_file(OpenFileRequest{
                                      .path = path,
                                      .line = line > 0 ? static_cast<std::size_t>(line) : 0,
                                      .column =
                                          column > 0 ? static_cast<std::size_t>(column) : 0,
                                  })
            ? 1
            : 0);
    return 1;
  }

  static int LuaWorkspaceActiveBuffer(lua_State* state) {
    Impl* host = HostFromUpvalue(state);
    if (host == nullptr || !host->callbacks.active_buffer) {
      lua_pushnil(state);
      return 1;
    }

    const std::optional<PluginHost::ActiveBuffer> active_buffer = host->callbacks.active_buffer();
    if (!active_buffer.has_value() || active_buffer->path.empty()) {
      lua_pushnil(state);
      return 1;
    }

    host->PushBufferTable(state, active_buffer->path);
    lua_pushinteger(state, static_cast<lua_Integer>(active_buffer->line));
    lua_setfield(state, -2, "line");
    lua_pushinteger(state, static_cast<lua_Integer>(active_buffer->column));
    lua_setfield(state, -2, "column");
    return 1;
  }

  static int LuaFilesReadText(lua_State* state) {
    Impl* host = HostFromUpvalue(state);
    const char* raw_path = luaL_checkstring(state, 1);
    if (host == nullptr) {
      lua_pushnil(state);
      return 1;
    }

    const std::filesystem::path path =
        ResolveRuntimePath(host->current_project_root, std::filesystem::path(raw_path));
    const std::optional<std::string> text = util::ReadTextFile(path);
    if (!text.has_value()) {
      lua_pushnil(state);
      return 1;
    }
    lua_pushlstring(state, text->c_str(), text->size());
    return 1;
  }

  static int LuaFilesWriteText(lua_State* state) {
    Impl* host = HostFromUpvalue(state);
    const char* raw_path = luaL_checkstring(state, 1);
    size_t text_length = 0;
    const char* text = luaL_checklstring(state, 2, &text_length);
    if (host == nullptr) {
      lua_pushboolean(state, 0);
      return 1;
    }

    const std::filesystem::path path =
        ResolveRuntimePath(host->current_project_root, std::filesystem::path(raw_path));
    lua_pushboolean(
        state, util::WriteTextFileAtomically(path, std::string_view(text, text_length)) ? 1 : 0);
    return 1;
  }

  static int LuaFilesExists(lua_State* state) {
    Impl* host = HostFromUpvalue(state);
    const char* raw_path = luaL_checkstring(state, 1);
    if (host == nullptr) {
      lua_pushboolean(state, 0);
      return 1;
    }

    std::error_code error;
    const std::filesystem::path path =
        ResolveRuntimePath(host->current_project_root, std::filesystem::path(raw_path));
    lua_pushboolean(state, std::filesystem::exists(path, error) && !error ? 1 : 0);
    return 1;
  }

  static int LuaProcessRun(lua_State* state) {
    Impl* host = HostFromUpvalue(state);
    luaL_checktype(state, 1, LUA_TTABLE);

    std::vector<std::string> argv;
    const lua_Integer argc = static_cast<lua_Integer>(lua_rawlen(state, 1));
    argv.reserve(static_cast<std::size_t>(argc));
    for (lua_Integer i = 1; i <= argc; ++i) {
      lua_rawgeti(state, 1, i);
      if (!lua_isstring(state, -1)) {
        return luaL_error(state, "process argv entries must be strings");
      }
      argv.emplace_back(lua_tostring(state, -1));
      lua_pop(state, 1);
    }

    std::filesystem::path cwd = host != nullptr ? host->current_project_root : std::filesystem::path{};
    std::string stdin_text;
    std::vector<platform::SubprocessEnvironmentOverride> environment_overrides;
    if (lua_gettop(state) >= 2 && !lua_isnil(state, 2)) {
      luaL_checktype(state, 2, LUA_TTABLE);
      lua_getfield(state, 2, "cwd");
      if (lua_isstring(state, -1)) {
        cwd = ResolveRuntimePath(host != nullptr ? host->current_project_root : std::filesystem::path{},
                                 std::filesystem::path(lua_tostring(state, -1)));
      }
      lua_pop(state, 1);

      lua_getfield(state, 2, "stdin");
      if (lua_isstring(state, -1)) {
        size_t length = 0;
        const char* text = lua_tolstring(state, -1, &length);
        stdin_text.assign(text, length);
      }
      lua_pop(state, 1);

      lua_getfield(state, 2, "env");
      if (!lua_isnil(state, -1)) {
        luaL_checktype(state, -1, LUA_TTABLE);
        lua_pushnil(state);
        while (lua_next(state, -2) != 0) {
          if (!lua_isstring(state, -2)) {
            return luaL_error(state, "process env keys must be strings");
          }

          platform::SubprocessEnvironmentOverride override_entry;
          override_entry.name = lua_tostring(state, -2);
          if (lua_isstring(state, -1)) {
            size_t length = 0;
            const char* text = lua_tolstring(state, -1, &length);
            override_entry.value = std::string(text, length);
          } else if (lua_isboolean(state, -1) && lua_toboolean(state, -1) == 0) {
            override_entry.value = std::nullopt;
          } else {
            return luaL_error(state, "process env values must be strings or false");
          }

          environment_overrides.push_back(std::move(override_entry));
          lua_pop(state, 1);
        }
      }
      lua_pop(state, 1);
    }

    const platform::SubprocessResult result = platform::RunSubprocess(
        argv, platform::SubprocessOptions{
                  .cwd = cwd,
                  .stdin_text = stdin_text,
                  .environment_overrides = std::move(environment_overrides),
                  .capture_stdout = true,
                  .capture_stderr = true,
              });
    lua_createtable(state, 0, 4);
    lua_pushinteger(state, result.exit_code);
    lua_setfield(state, -2, "exit_code");
    lua_pushboolean(state, result.exit_code == 0 ? 1 : 0);
    lua_setfield(state, -2, "ok");
    lua_pushlstring(state, result.stdout_text.c_str(), result.stdout_text.size());
    lua_setfield(state, -2, "stdout");
    lua_pushlstring(state, result.stderr_text.c_str(), result.stderr_text.size());
    lua_setfield(state, -2, "stderr");
    return 1;
  }

  static int LuaProcessRunAsync(lua_State* state) {
    Impl* host = HostFromUpvalue(state);
    luaL_checktype(state, 1, LUA_TTABLE);
    luaL_checktype(state, 3, LUA_TFUNCTION);

    std::vector<std::string> argv;
    const lua_Integer argc = static_cast<lua_Integer>(lua_rawlen(state, 1));
    argv.reserve(static_cast<std::size_t>(argc));
    for (lua_Integer i = 1; i <= argc; ++i) {
      lua_rawgeti(state, 1, i);
      if (!lua_isstring(state, -1)) {
        return luaL_error(state, "process argv entries must be strings");
      }
      argv.emplace_back(lua_tostring(state, -1));
      lua_pop(state, 1);
    }

    std::filesystem::path cwd = host != nullptr ? host->current_project_root : std::filesystem::path{};
    std::string stdin_text;
    std::vector<platform::SubprocessEnvironmentOverride> environment_overrides;
    if (!lua_isnil(state, 2)) {
      luaL_checktype(state, 2, LUA_TTABLE);
      lua_getfield(state, 2, "cwd");
      if (lua_isstring(state, -1)) {
        cwd = ResolveRuntimePath(host != nullptr ? host->current_project_root : std::filesystem::path{},
                                 std::filesystem::path(lua_tostring(state, -1)));
      }
      lua_pop(state, 1);

      lua_getfield(state, 2, "stdin");
      if (lua_isstring(state, -1)) {
        size_t length = 0;
        const char* text = lua_tolstring(state, -1, &length);
        stdin_text.assign(text, length);
      }
      lua_pop(state, 1);

      lua_getfield(state, 2, "env");
      if (!lua_isnil(state, -1)) {
        luaL_checktype(state, -1, LUA_TTABLE);
        lua_pushnil(state);
        while (lua_next(state, -2) != 0) {
          if (!lua_isstring(state, -2)) {
            return luaL_error(state, "process env keys must be strings");
          }
          platform::SubprocessEnvironmentOverride override_entry;
          override_entry.name = lua_tostring(state, -2);
          if (lua_isstring(state, -1)) {
            size_t length = 0;
            const char* text = lua_tolstring(state, -1, &length);
            override_entry.value = std::string(text, length);
          } else if (lua_isboolean(state, -1) && lua_toboolean(state, -1) == 0) {
            override_entry.value = std::nullopt;
          } else {
            return luaL_error(state, "process env values must be strings or false");
          }
          environment_overrides.push_back(std::move(override_entry));
          lua_pop(state, 1);
        }
      }
      lua_pop(state, 1);
    }

    if (host == nullptr) {
      return 0;
    }

    // Store the Lua callback ref — must be done on the main thread before launching the thread.
    lua_pushvalue(state, 3);
    const int callback_ref = luaL_ref(state, LUA_REGISTRYINDEX);
    const auto request = std::make_shared<AsyncProcessRequest>();
    request->lua_state = state;
    request->callback_ref = callback_ref;
    const std::shared_ptr<AsyncProcessState> async_state =
        host != nullptr ? host->async_process_state : nullptr;
    if (async_state == nullptr) {
      luaL_unref(state, LUA_REGISTRYINDEX, callback_ref);
      return 0;
    }
    {
      std::lock_guard lock(async_state->mutex);
      async_state->active_requests.push_back(request);
    }

    platform::SubprocessOptions opts{
        .cwd = std::move(cwd),
        .stdin_text = std::move(stdin_text),
        .environment_overrides = std::move(environment_overrides),
        .capture_stdout = true,
        .capture_stderr = true,
    };

    async_state->in_flight.fetch_add(1, std::memory_order_relaxed);
    std::thread([async_state,
                 request,
                 argv = std::move(argv),
                 opts = std::move(opts)]() mutable {
      platform::SubprocessResult result = platform::RunSubprocess(argv, opts);
      Uint32 event_type = 0;
      bool should_push_event = false;
      {
        std::lock_guard lock(async_state->mutex);
        auto it = std::find(async_state->active_requests.begin(), async_state->active_requests.end(),
                            request);
        if (it != async_state->active_requests.end()) {
          async_state->active_requests.erase(it);
        }
        if (!request->cancelled && request->lua_state != nullptr &&
            request->callback_ref != LUA_NOREF) {
          async_state->pending_callbacks.push_back(
              {request->lua_state, request->callback_ref, std::move(result)});
          request->lua_state = nullptr;
          request->callback_ref = LUA_NOREF;
          event_type = async_state->event_type;
          should_push_event = true;
        }
      }
      async_state->in_flight.fetch_sub(1, std::memory_order_release);
      if (should_push_event && event_type != 0) {
        SDL_Event event{};
        event.type = event_type;
        SDL_PushEvent(&event);
      }
    }).detach();

    return 0;
  }

  bool ReadDiagnosticTable(lua_State* state,
                           int table_index,
                           editor::Diagnostic* diagnostic,
                           std::string* error_message) {
    if (diagnostic == nullptr) {
      if (error_message != nullptr) {
        *error_message = "diagnostic output pointer is required";
      }
      return false;
    }

    const int absolute_index = lua_absindex(state, table_index);

    lua_getfield(state, absolute_index, "message");
    if (!lua_isstring(state, -1)) {
      if (error_message != nullptr) {
        *error_message = "diagnostic message must be a string";
      }
      lua_pop(state, 1);
      return false;
    }
    diagnostic->message = lua_tostring(state, -1);
    lua_pop(state, 1);

    lua_getfield(state, absolute_index, "line");
    if (!lua_isinteger(state, -1) || lua_tointeger(state, -1) <= 0) {
      if (error_message != nullptr) {
        *error_message = "diagnostic line must be a positive integer";
      }
      lua_pop(state, 1);
      return false;
    }
    const lua_Integer line = lua_tointeger(state, -1);
    lua_pop(state, 1);

    lua_getfield(state, absolute_index, "column");
    if (!lua_isinteger(state, -1) || lua_tointeger(state, -1) <= 0) {
      if (error_message != nullptr) {
        *error_message = "diagnostic column must be a positive integer";
      }
      lua_pop(state, 1);
      return false;
    }
    const lua_Integer column = lua_tointeger(state, -1);
    lua_pop(state, 1);

    lua_Integer end_line = line;
    lua_getfield(state, absolute_index, "end_line");
    if (lua_isinteger(state, -1)) {
      end_line = lua_tointeger(state, -1);
    }
    lua_pop(state, 1);

    lua_Integer end_column = column + 1;
    lua_getfield(state, absolute_index, "end_column");
    if (lua_isinteger(state, -1)) {
      end_column = lua_tointeger(state, -1);
    }
    lua_pop(state, 1);

    if (end_line <= 0 || end_column <= 0) {
      if (error_message != nullptr) {
        *error_message = "diagnostic end positions must be positive integers";
      }
      return false;
    }
    if (end_line < line || (end_line == line && end_column < column)) {
      if (error_message != nullptr) {
        *error_message = "diagnostic end position must not precede the start position";
      }
      return false;
    }

    diagnostic->range =
        editor::SelectionRange{
            .start = editor::TextPosition{
                .line = static_cast<std::size_t>(line - 1),
                .column = static_cast<std::size_t>(column - 1),
            },
            .end = editor::TextPosition{
                .line = static_cast<std::size_t>(end_line - 1),
                .column = static_cast<std::size_t>(end_column - 1),
            },
        };
    diagnostic->severity = editor::DiagnosticSeverity::Error;
    lua_getfield(state, absolute_index, "severity");
    if (lua_isnil(state, -1)) {
      lua_pop(state, 1);
      return true;
    }
    if (!lua_isstring(state, -1) ||
        !ParseDiagnosticSeverity(lua_tostring(state, -1), &diagnostic->severity)) {
      if (error_message != nullptr) {
        *error_message = "diagnostic severity must be one of: error, warning, info, hint";
      }
      lua_pop(state, 1);
      return false;
    }
    lua_pop(state, 1);
    return true;
  }

  bool PublishDiagnostics(lua_State* state,
                          std::string_view raw_path,
                          int diagnostics_index,
                          std::string* error_message) {
    PluginInstance* plugin = FindPluginByState(state);
    if (plugin == nullptr) {
      if (error_message != nullptr) {
        *error_message = "diagnostic publication requires an active plugin state";
      }
      return false;
    }
    if (!callbacks.publish_diagnostics) {
      if (error_message != nullptr) {
        *error_message = "diagnostics API unavailable";
      }
      return false;
    }

    const std::filesystem::path path =
        ResolveRuntimePath(current_project_root, std::filesystem::path(raw_path));
    if (path.empty()) {
      if (error_message != nullptr) {
        *error_message = "diagnostic path must not be empty";
      }
      return false;
    }

    const int absolute_index = lua_absindex(state, diagnostics_index);
    const lua_Integer count = static_cast<lua_Integer>(lua_rawlen(state, absolute_index));
    std::vector<editor::Diagnostic> diagnostics;
    diagnostics.reserve(static_cast<std::size_t>(count));
    for (lua_Integer i = 1; i <= count; ++i) {
      lua_rawgeti(state, absolute_index, i);
      if (!lua_istable(state, -1)) {
        if (error_message != nullptr) {
          *error_message = "diagnostic entries must be tables";
        }
        lua_pop(state, 1);
        return false;
      }
      editor::Diagnostic diagnostic;
      if (!ReadDiagnosticTable(state, -1, &diagnostic, error_message)) {
        lua_pop(state, 1);
        return false;
      }
      diagnostics.push_back(std::move(diagnostic));
      lua_pop(state, 1);
    }

    callbacks.publish_diagnostics(plugin->id, path, std::move(diagnostics));
    if (error_message != nullptr) {
      error_message->clear();
    }
    return true;
  }

  bool ClearDiagnostics(lua_State* state,
                        const std::optional<std::filesystem::path>& path,
                        std::string* error_message) {
    PluginInstance* plugin = FindPluginByState(state);
    if (plugin == nullptr) {
      if (error_message != nullptr) {
        *error_message = "diagnostic clearing requires an active plugin state";
      }
      return false;
    }

    if (path.has_value()) {
      if (!callbacks.clear_file_diagnostics) {
        if (error_message != nullptr) {
          *error_message = "diagnostics API unavailable";
        }
        return false;
      }
      callbacks.clear_file_diagnostics(plugin->id, path->lexically_normal());
    } else {
      if (!callbacks.clear_owner_diagnostics) {
        if (error_message != nullptr) {
          *error_message = "diagnostics API unavailable";
        }
        return false;
      }
      callbacks.clear_owner_diagnostics(plugin->id);
    }
    if (error_message != nullptr) {
      error_message->clear();
    }
    return true;
  }

  static int LuaDiagnosticsPublish(lua_State* state) {
    Impl* host = HostFromUpvalue(state);
    const char* raw_path = luaL_checkstring(state, 1);
    luaL_checktype(state, 2, LUA_TTABLE);
    std::string error_message;
    if (host == nullptr || !host->PublishDiagnostics(state, raw_path, 2, &error_message)) {
      return luaL_error(state, "%s",
                        error_message.empty() ? "failed to publish diagnostics"
                                              : error_message.c_str());
    }
    return 0;
  }

  static int LuaDiagnosticsClear(lua_State* state) {
    Impl* host = HostFromUpvalue(state);
    std::optional<std::filesystem::path> path;
    if (lua_gettop(state) >= 1 && !lua_isnil(state, 1)) {
      path = ResolveRuntimePath(host != nullptr ? host->current_project_root : std::filesystem::path{},
                                std::filesystem::path(luaL_checkstring(state, 1)));
    }

    std::string error_message;
    if (host == nullptr || !host->ClearDiagnostics(state, path, &error_message)) {
      return luaL_error(state, "%s",
                        error_message.empty() ? "failed to clear diagnostics"
                                              : error_message.c_str());
    }
    return 0;
  }

  static int LuaSidebarShow(lua_State* state) {
    Impl* host = HostFromUpvalue(state);
    const char* id = luaL_checkstring(state, 1);
    if (host == nullptr || !host->callbacks.show_sidebar) {
      lua_pushboolean(state, 0);
      return 1;
    }
    lua_pushboolean(state, host->callbacks.show_sidebar(id) ? 1 : 0);
    return 1;
  }

  void PushPluginContext(lua_State* state) {
    lua_createtable(state, 0, 11);

    lua_pushlightuserdata(state, this);
    lua_pushcclosure(state, &LuaLog, 1);
    lua_setfield(state, -2, "log");

    lua_createtable(state, 0, 1);
    lua_pushlightuserdata(state, this);
    lua_pushcclosure(state, &LuaCommandsAdd, 1);
    lua_setfield(state, -2, "add");
    lua_setfield(state, -2, "commands");

    lua_createtable(state, 0, 3);
    lua_pushlightuserdata(state, this);
    lua_pushcclosure(state, &LuaWorkspaceProjectRoot, 1);
    lua_setfield(state, -2, "project_root");
    lua_pushlightuserdata(state, this);
    lua_pushcclosure(state, &LuaWorkspaceOpenFile, 1);
    lua_setfield(state, -2, "open_file");
    lua_pushlightuserdata(state, this);
    lua_pushcclosure(state, &LuaWorkspaceActiveBuffer, 1);
    lua_setfield(state, -2, "active_buffer");
    lua_setfield(state, -2, "workspace");

    lua_createtable(state, 0, 3);
    lua_pushlightuserdata(state, this);
    lua_pushcclosure(state, &LuaFilesReadText, 1);
    lua_setfield(state, -2, "read_text");
    lua_pushlightuserdata(state, this);
    lua_pushcclosure(state, &LuaFilesWriteText, 1);
    lua_setfield(state, -2, "write_text");
    lua_pushlightuserdata(state, this);
    lua_pushcclosure(state, &LuaFilesExists, 1);
    lua_setfield(state, -2, "exists");
    lua_setfield(state, -2, "files");

    lua_createtable(state, 0, 2);
    lua_pushlightuserdata(state, this);
    lua_pushcclosure(state, &LuaProcessRun, 1);
    lua_setfield(state, -2, "run");
    lua_pushlightuserdata(state, this);
    lua_pushcclosure(state, &LuaProcessRunAsync, 1);
    lua_setfield(state, -2, "run_async");
    lua_setfield(state, -2, "process");

    lua_createtable(state, 0, 2);
    lua_pushlightuserdata(state, this);
    lua_pushcclosure(state, &LuaDiagnosticsPublish, 1);
    lua_setfield(state, -2, "publish");
    lua_pushlightuserdata(state, this);
    lua_pushcclosure(state, &LuaDiagnosticsClear, 1);
    lua_setfield(state, -2, "clear");
    lua_setfield(state, -2, "diagnostics");

    lua_createtable(state, 0, 2);
    lua_pushlightuserdata(state, this);
    lua_pushcclosure(state, &LuaSidebarAdd, 1);
    lua_setfield(state, -2, "add");
    lua_pushlightuserdata(state, this);
    lua_pushcclosure(state, &LuaSidebarShow, 1);
    lua_setfield(state, -2, "show");
    lua_setfield(state, -2, "sidebar");

    lua_createtable(state, 0, 1);
    lua_pushlightuserdata(state, this);
    lua_pushcclosure(state, &LuaHoverAdd, 1);
    lua_setfield(state, -2, "add");
    lua_setfield(state, -2, "hover");

    lua_createtable(state, 0, 1);
    lua_pushlightuserdata(state, this);
    lua_pushcclosure(state, &LuaMenusAdd, 1);
    lua_setfield(state, -2, "add");
    lua_setfield(state, -2, "menus");

    lua_createtable(state, 0, 1);
    lua_pushlightuserdata(state, this);
    lua_pushcclosure(state, &LuaKeybindingsAdd, 1);
    lua_setfield(state, -2, "add");
    lua_setfield(state, -2, "keybindings");

    lua_createtable(state, 0, 2);
    lua_pushlightuserdata(state, this);
    lua_pushcclosure(state, &LuaSettingsDeclare, 1);
    lua_setfield(state, -2, "declare");
    lua_pushlightuserdata(state, this);
    lua_pushcclosure(state, &LuaSettingsGet, 1);
    lua_setfield(state, -2, "get");
    lua_setfield(state, -2, "settings");

    lua_createtable(state, 0, 2);
    lua_pushlightuserdata(state, this);
    lua_pushcclosure(state, &LuaStatusAdd, 1);
    lua_setfield(state, -2, "add");
    lua_pushlightuserdata(state, this);
    lua_pushcclosure(state, &LuaStatusUpdate, 1);
    lua_setfield(state, -2, "update");
    lua_setfield(state, -2, "status");

    lua_createtable(state, 0, 1);
    lua_pushlightuserdata(state, this);
    lua_pushcclosure(state, &LuaFormattersAdd, 1);
    lua_setfield(state, -2, "add");
    lua_setfield(state, -2, "formatters");

    lua_createtable(state, 0, 1);
    lua_pushlightuserdata(state, this);
    lua_pushcclosure(state, &LuaSaveParticipantsAdd, 1);
    lua_setfield(state, -2, "add");
    lua_setfield(state, -2, "save_participants");

    lua_createtable(state, 0, 1);
    lua_pushlightuserdata(state, this);
    lua_pushcclosure(state, &LuaCompletionAdd, 1);
    lua_setfield(state, -2, "add");
    lua_setfield(state, -2, "completion");

    lua_createtable(state, 0, 1);
    lua_pushlightuserdata(state, this);
    lua_pushcclosure(state, &LuaCodeActionAdd, 1);
    lua_setfield(state, -2, "add");
    lua_setfield(state, -2, "code_actions");

    lua_createtable(state, 0, 1);
    lua_pushlightuserdata(state, this);
    lua_pushcclosure(state, &LuaLspAdd, 1);
    lua_setfield(state, -2, "add");
    lua_setfield(state, -2, "lsp");

    lua_createtable(state, 0, 1);
    lua_pushlightuserdata(state, this);
    lua_pushcclosure(state, &LuaTaskAdd, 1);
    lua_setfield(state, -2, "add");
    lua_setfield(state, -2, "tasks");

    lua_createtable(state, 0, 1);
    lua_pushlightuserdata(state, this);
    lua_pushcclosure(state, &LuaToolAdd, 1);
    lua_setfield(state, -2, "add");
    lua_setfield(state, -2, "tools");

    lua_createtable(state, 0, 1);
    lua_pushlightuserdata(state, this);
    lua_pushcclosure(state, &LuaDebuggerAdd, 1);
    lua_setfield(state, -2, "add");
    lua_setfield(state, -2, "debuggers");

    lua_createtable(state, 0, 1);
    lua_pushlightuserdata(state, this);
    lua_pushcclosure(state, &LuaTestProviderAdd, 1);
    lua_setfield(state, -2, "add");
    lua_setfield(state, -2, "tests");

    lua_createtable(state, 0, 1);
    lua_pushlightuserdata(state, this);
    lua_pushcclosure(state, &LuaScmProviderAdd, 1);
    lua_setfield(state, -2, "add");
    lua_setfield(state, -2, "scm");

    lua_createtable(state, 0, 1);
    lua_pushlightuserdata(state, this);
    lua_pushcclosure(state, &LuaAnnotationProviderAdd, 1);
    lua_setfield(state, -2, "add");
    lua_setfield(state, -2, "annotations");

    lua_createtable(state, 0, 1);
    lua_pushlightuserdata(state, this);
    lua_pushcclosure(state, &LuaAuthProviderAdd, 1);
    lua_setfield(state, -2, "add");
    lua_setfield(state, -2, "auth");

    lua_createtable(state, 0, 1);
    lua_pushlightuserdata(state, this);
    lua_pushcclosure(state, &LuaAiProviderAdd, 1);
    lua_setfield(state, -2, "add");
    lua_setfield(state, -2, "ai_providers");

    lua_createtable(state, 0, 1);
    lua_pushlightuserdata(state, this);
    lua_pushcclosure(state, &LuaExternalAgentAdd, 1);
    lua_setfield(state, -2, "add");
    lua_setfield(state, -2, "external_agents");

    lua_createtable(state, 0, 1);
    lua_pushlightuserdata(state, this);
    lua_pushcclosure(state, &LuaMcpToolAdd, 1);
    lua_setfield(state, -2, "add");
    lua_setfield(state, -2, "mcp_tools");
  }

  void PushProjectTable(lua_State* state, const std::filesystem::path& project_root) {
    lua_createtable(state, 0, 2);
    lua_pushstring(state, project_root.generic_string().c_str());
    lua_setfield(state, -2, "root");
    lua_pushstring(state, Basename(project_root).c_str());
    lua_setfield(state, -2, "name");
  }

  void PushBufferTable(lua_State* state, const std::filesystem::path& path) {
    lua_createtable(state, 0, 3);
    const std::filesystem::path normalized_path = path.lexically_normal();
    lua_pushstring(state, normalized_path.generic_string().c_str());
    lua_setfield(state, -2, "path");
    lua_pushstring(state, normalized_path.filename().string().c_str());
    lua_setfield(state, -2, "name");
    if (const std::optional<std::string> relative = RelativePathString(normalized_path);
        relative.has_value()) {
      lua_pushstring(state, relative->c_str());
      lua_setfield(state, -2, "relative_path");
    }
  }

  static void PushHoverPositionTable(lua_State* state, std::size_t line, std::size_t column) {
    lua_createtable(state, 0, 2);
    lua_pushinteger(state, static_cast<lua_Integer>(line));
    lua_setfield(state, -2, "line");
    lua_pushinteger(state, static_cast<lua_Integer>(column));
    lua_setfield(state, -2, "column");
  }

  bool ReadHoverResultTable(lua_State* state,
                            int table_index,
                            PluginHost::HoverResult* result,
                            std::string_view provider_id,
                            std::string* error_message) const {
    if (result == nullptr) {
      if (error_message != nullptr) {
        *error_message = "hover result output pointer is required";
      }
      return false;
    }

    const int absolute_index = lua_absindex(state, table_index);
    result->title.clear();
    result->content.clear();

    lua_getfield(state, absolute_index, "title");
    if (lua_isstring(state, -1)) {
      result->title = lua_tostring(state, -1);
    } else if (!lua_isnil(state, -1)) {
      if (error_message != nullptr) {
        *error_message = "plugin hover '" + std::string(provider_id) +
                         "' title must be a string when present";
      }
      lua_pop(state, 1);
      return false;
    }
    lua_pop(state, 1);

    lua_getfield(state, absolute_index, "content");
    if (lua_isstring(state, -1)) {
      result->content = lua_tostring(state, -1);
    } else if (!lua_isnil(state, -1)) {
      if (error_message != nullptr) {
        *error_message = "plugin hover '" + std::string(provider_id) +
                         "' content must be a string when present";
      }
      lua_pop(state, 1);
      return false;
    }
    lua_pop(state, 1);

    if (result->title.empty() && result->content.empty()) {
      if (error_message != nullptr) {
        *error_message = "plugin hover '" + std::string(provider_id) +
                         "' must return a title or content";
      }
      return false;
    }

    if (error_message != nullptr) {
      error_message->clear();
    }
    return true;
  }

  static SidebarItem ReadSidebarItem(lua_State* state, int table_index) {
    SidebarItem item;
    const int absolute_index = lua_absindex(state, table_index);

    lua_getfield(state, absolute_index, "label");
    if (lua_isstring(state, -1)) {
      item.label = lua_tostring(state, -1);
    }
    lua_pop(state, 1);

    lua_getfield(state, absolute_index, "detail");
    if (lua_isstring(state, -1)) {
      item.detail = lua_tostring(state, -1);
    }
    lua_pop(state, 1);

    lua_getfield(state, absolute_index, "path");
    if (lua_isstring(state, -1)) {
      item.path = lua_tostring(state, -1);
    }
    lua_pop(state, 1);

    lua_getfield(state, absolute_index, "line");
    if (lua_isinteger(state, -1)) {
      const lua_Integer line = lua_tointeger(state, -1);
      item.line = line > 0 ? static_cast<std::size_t>(line) : 0;
    }
    lua_pop(state, 1);

    lua_getfield(state, absolute_index, "column");
    if (lua_isinteger(state, -1)) {
      const lua_Integer column = lua_tointeger(state, -1);
      item.column = column > 0 ? static_cast<std::size_t>(column) : 0;
    }
    lua_pop(state, 1);

    return item;
  }

  void PushSidebarItemTable(lua_State* state, const SidebarItem& item) const {
    lua_createtable(state, 0, 5);
    lua_pushstring(state, item.label.c_str());
    lua_setfield(state, -2, "label");
    if (!item.detail.empty()) {
      lua_pushstring(state, item.detail.c_str());
      lua_setfield(state, -2, "detail");
    }
    if (!item.path.empty()) {
      const std::filesystem::path resolved_path =
          ResolveRuntimePath(current_project_root, item.path);
      lua_pushstring(state, resolved_path.generic_string().c_str());
      lua_setfield(state, -2, "path");
    }
    if (item.line > 0) {
      lua_pushinteger(state, static_cast<lua_Integer>(item.line));
      lua_setfield(state, -2, "line");
    }
    if (item.column > 0) {
      lua_pushinteger(state, static_cast<lua_Integer>(item.column));
      lua_setfield(state, -2, "column");
    }
  }

  bool SnapshotSidebarProvider(const SidebarProvider& provider,
                               std::vector<SidebarItem>* items,
                               std::string* error_message) {
    if (provider.state == nullptr || provider.snapshot_ref == LUA_NOREF || items == nullptr) {
      if (error_message != nullptr) {
        *error_message = "plugin sidebar is unavailable";
      }
      return false;
    }

    items->clear();
    lua_rawgeti(provider.state, LUA_REGISTRYINDEX, provider.snapshot_ref);
    if (lua_pcall(provider.state, 0, 1, 0) != LUA_OK) {
      if (error_message != nullptr) {
        *error_message =
            "plugin sidebar '" + provider.info.id + "' snapshot failed: " +
            LuaErrorString(provider.state);
      }
      lua_pop(provider.state, 1);
      return false;
    }
    if (!lua_istable(provider.state, -1)) {
      if (error_message != nullptr) {
        *error_message = "plugin sidebar '" + provider.info.id +
                         "' snapshot must return an array table";
      }
      lua_pop(provider.state, 1);
      return false;
    }

    const lua_Integer count = static_cast<lua_Integer>(lua_rawlen(provider.state, -1));
    items->reserve(static_cast<std::size_t>(count));
    for (lua_Integer i = 1; i <= count; ++i) {
      lua_rawgeti(provider.state, -1, i);
      if (!lua_istable(provider.state, -1)) {
        if (error_message != nullptr) {
          *error_message = "plugin sidebar '" + provider.info.id +
                           "' snapshot items must be tables";
        }
        lua_pop(provider.state, 2);
        items->clear();
        return false;
      }
      SidebarItem item = ReadSidebarItem(provider.state, -1);
      if (item.label.empty()) {
        if (error_message != nullptr) {
          *error_message = "plugin sidebar '" + provider.info.id +
                           "' items require a label";
        }
        lua_pop(provider.state, 2);
        items->clear();
        return false;
      }
      if (!item.path.empty()) {
        item.path = ResolveRuntimePath(current_project_root, item.path);
      }
      items->push_back(std::move(item));
      lua_pop(provider.state, 1);
    }

    lua_pop(provider.state, 1);
    if (error_message != nullptr) {
      error_message->clear();
    }
    return true;
  }

  bool ConfirmSidebarProviderItem(const SidebarProvider& provider,
                                  const SidebarItem& item,
                                  std::string* error_message) {
    if (provider.confirm_ref == LUA_NOREF || provider.state == nullptr) {
      if (!item.path.empty() && callbacks.open_file) {
        const bool opened = callbacks.open_file(OpenFileRequest{
            .path = item.path,
            .line = item.line,
            .column = item.column,
        });
        if (error_message != nullptr) {
          error_message->clear();
        }
        return opened;
      }
      if (error_message != nullptr) {
        *error_message = "plugin sidebar '" + provider.info.id + "' has no confirm action";
      }
      return false;
    }

    lua_rawgeti(provider.state, LUA_REGISTRYINDEX, provider.confirm_ref);
    PushSidebarItemTable(provider.state, item);
    if (lua_pcall(provider.state, 1, 0, 0) != LUA_OK) {
      if (error_message != nullptr) {
        *error_message =
            "plugin sidebar '" + provider.info.id + "' confirm failed: " +
            LuaErrorString(provider.state);
      }
      lua_pop(provider.state, 1);
      return false;
    }
    if (error_message != nullptr) {
      error_message->clear();
    }
    return true;
  }

  bool QueryHoverProvider(const HoverProvider& provider,
                          const std::filesystem::path& path,
                          std::size_t line,
                          std::size_t column,
                          PluginHost::HoverResult* result,
                          std::string* error_message) {
    if (provider.state == nullptr || provider.provide_ref == LUA_NOREF || result == nullptr) {
      if (error_message != nullptr) {
        *error_message = "plugin hover provider is unavailable";
      }
      return false;
    }

    lua_rawgeti(provider.state, LUA_REGISTRYINDEX, provider.provide_ref);
    PushBufferTable(provider.state, path);
    PushHoverPositionTable(provider.state, line, column);
    if (lua_pcall(provider.state, 2, 1, 0) != LUA_OK) {
      if (error_message != nullptr) {
        *error_message = "plugin hover '" + provider.id + "' failed: " +
                         LuaErrorString(provider.state);
      }
      lua_pop(provider.state, 1);
      return false;
    }

    if (lua_isnil(provider.state, -1)) {
      lua_pop(provider.state, 1);
      result->title.clear();
      result->content.clear();
      if (error_message != nullptr) {
        error_message->clear();
      }
      return true;
    }

    if (!lua_istable(provider.state, -1)) {
      if (error_message != nullptr) {
        *error_message = "plugin hover '" + provider.id + "' must return a table or nil";
      }
      lua_pop(provider.state, 1);
      return false;
    }

    const bool ok = ReadHoverResultTable(provider.state, -1, result, provider.id, error_message);
    lua_pop(provider.state, 1);
    return ok;
  }

  void ClearPluginDiagnostics(const PluginInstance* plugin) {
    if (plugin == nullptr || plugin->id.empty() || !callbacks.clear_owner_diagnostics) {
      return;
    }
    callbacks.clear_owner_diagnostics(plugin->id);
  }

  void DestroyPluginState(PluginInstance* plugin) {
    if (plugin == nullptr || plugin->state == nullptr) {
      return;
    }

    auto unref = [&](int* ref) {
      if (*ref != LUA_NOREF && *ref != LUA_REFNIL) {
        luaL_unref(plugin->state, LUA_REGISTRYINDEX, *ref);
        *ref = LUA_NOREF;
      }
    };
    unref(&plugin->setup_ref);
    unref(&plugin->on_project_open_ref);
    unref(&plugin->on_project_close_ref);
    unref(&plugin->on_buffer_open_ref);
    unref(&plugin->on_buffer_save_ref);
    unref(&plugin->shutdown_ref);
    plugin->runtime.reset();
    plugin->state = nullptr;
  }

  void UnregisterContributionsForState(lua_State* state) {
    for (auto it = commands.begin(); it != commands.end();) {
      if (it->second.state != state) {
        ++it;
        continue;
      }
      luaL_unref(state, LUA_REGISTRYINDEX, it->second.function_ref);
      it = commands.erase(it);
    }
    RebuildCommandNames();

    for (auto it = sidebars.begin(); it != sidebars.end();) {
      if (it->second.state != state) {
        ++it;
        continue;
      }
      luaL_unref(state, LUA_REGISTRYINDEX, it->second.snapshot_ref);
      if (it->second.confirm_ref != LUA_NOREF && it->second.confirm_ref != LUA_REFNIL) {
        luaL_unref(state, LUA_REGISTRYINDEX, it->second.confirm_ref);
      }
      it = sidebars.erase(it);
    }
    RebuildSidebarProviders();

    for (auto it = hovers.begin(); it != hovers.end();) {
      if (it->second.state != state) {
        ++it;
        continue;
      }
      luaL_unref(state, LUA_REGISTRYINDEX, it->second.provide_ref);
      it = hovers.erase(it);
    }
    hover_provider_order.erase(
        std::remove_if(hover_provider_order.begin(), hover_provider_order.end(),
                       [&](std::string_view id) { return !hovers.contains(std::string(id)); }),
        hover_provider_order.end());

    const PluginInstance* plugin = FindPluginByState(state);
    if (plugin != nullptr) {
      const std::string plugin_id = plugin->id;
      menu_entries.erase(
          std::remove_if(menu_entries.begin(), menu_entries.end(),
                         [&](const PluginHost::ContributedMenuEntry& e) {
                           return e.plugin_id == plugin_id;
                         }),
          menu_entries.end());
      keybindings.erase(
          std::remove_if(keybindings.begin(), keybindings.end(),
                         [&](const PluginHost::ContributedKeybinding& e) {
                           return e.plugin_id == plugin_id;
                         }),
          keybindings.end());
      settings.erase(
          std::remove_if(settings.begin(), settings.end(),
                         [&](const PluginHost::ContributedSettingSpec& e) {
                           return e.plugin_id == plugin_id;
                         }),
          settings.end());
      for (auto it = status_items.begin(); it != status_items.end();) {
        if (it->second.plugin_id == plugin_id) {
          it = status_items.erase(it);
        } else {
          ++it;
        }
      }
      status_item_order.erase(
          std::remove_if(status_item_order.begin(), status_item_order.end(),
                         [&](const PluginHost::ContributedStatusItem& e) {
                           return e.plugin_id == plugin_id;
                         }),
          status_item_order.end());
      formatters.erase(
          std::remove_if(formatters.begin(), formatters.end(),
                         [&](const PluginHost::ContributedFormatter& e) {
                           return e.plugin_id == plugin_id;
                         }),
          formatters.end());
      save_participants.erase(
          std::remove_if(save_participants.begin(), save_participants.end(),
                         [&](const PluginHost::ContributedSaveParticipant& e) {
                           return e.plugin_id == plugin_id;
                         }),
          save_participants.end());
      for (auto it = save_participant_runtimes.begin(); it != save_participant_runtimes.end();) {
        if (it->plugin_id != plugin_id) {
          ++it;
          continue;
        }
        luaL_unref(state, LUA_REGISTRYINDEX, it->function_ref);
        it = save_participant_runtimes.erase(it);
      }
      completions.erase(
          std::remove_if(completions.begin(), completions.end(),
                         [&](const PluginHost::ContributedCompletion& e) {
                           return e.plugin_id == plugin_id;
                         }),
          completions.end());
      for (auto it = completion_runtimes.begin(); it != completion_runtimes.end();) {
        if (it->plugin_id != plugin_id) {
          ++it;
          continue;
        }
        luaL_unref(state, LUA_REGISTRYINDEX, it->provide_ref);
        it = completion_runtimes.erase(it);
      }
      code_actions.erase(
          std::remove_if(code_actions.begin(), code_actions.end(),
                         [&](const PluginHost::ContributedCodeAction& e) {
                           return e.plugin_id == plugin_id;
                         }),
          code_actions.end());
      for (auto it = code_action_runtimes.begin(); it != code_action_runtimes.end();) {
        if (it->plugin_id != plugin_id) {
          ++it;
          continue;
        }
        luaL_unref(state, LUA_REGISTRYINDEX, it->provide_ref);
        it = code_action_runtimes.erase(it);
      }
      language_servers.erase(
          std::remove_if(language_servers.begin(), language_servers.end(),
                         [&](const PluginHost::ContributedLanguageServer& e) {
                           return e.plugin_id == plugin_id;
                         }),
          language_servers.end());
      tasks.erase(
          std::remove_if(tasks.begin(), tasks.end(),
                         [&](const PluginHost::ContributedTask& e) {
                           return e.plugin_id == plugin_id;
                         }),
          tasks.end());
      tools.erase(
          std::remove_if(tools.begin(), tools.end(),
                         [&](const PluginHost::ContributedTool& e) {
                           return e.plugin_id == plugin_id;
                         }),
          tools.end());
      debuggers.erase(
          std::remove_if(debuggers.begin(), debuggers.end(),
                         [&](const PluginHost::ContributedDebugger& e) {
                           return e.plugin_id == plugin_id;
                         }),
          debuggers.end());
      test_providers.erase(
          std::remove_if(test_providers.begin(), test_providers.end(),
                         [&](const PluginHost::ContributedTestProvider& e) {
                           return e.plugin_id == plugin_id;
                         }),
          test_providers.end());
      for (auto it = test_provider_runtimes.begin(); it != test_provider_runtimes.end();) {
        if (it->plugin_id != plugin_id) {
          ++it;
          continue;
        }
        if (it->discover_ref != LUA_NOREF && it->discover_ref != LUA_REFNIL) {
          luaL_unref(state, LUA_REGISTRYINDEX, it->discover_ref);
        }
        if (it->run_ref != LUA_NOREF && it->run_ref != LUA_REFNIL) {
          luaL_unref(state, LUA_REGISTRYINDEX, it->run_ref);
        }
        it = test_provider_runtimes.erase(it);
      }
      scm_providers.erase(
          std::remove_if(scm_providers.begin(), scm_providers.end(),
                         [&](const PluginHost::ContributedScmProvider& e) {
                           return e.plugin_id == plugin_id;
                         }),
          scm_providers.end());
      for (auto it = scm_provider_runtimes.begin(); it != scm_provider_runtimes.end();) {
        if (it->plugin_id != plugin_id) {
          ++it;
          continue;
        }
        if (it->snapshot_ref != LUA_NOREF && it->snapshot_ref != LUA_REFNIL) {
          luaL_unref(state, LUA_REGISTRYINDEX, it->snapshot_ref);
        }
        it = scm_provider_runtimes.erase(it);
      }
      annotation_providers.erase(
          std::remove_if(annotation_providers.begin(), annotation_providers.end(),
                         [&](const PluginHost::ContributedAnnotationProvider& e) {
                           return e.plugin_id == plugin_id;
                         }),
          annotation_providers.end());
      for (auto it = annotation_provider_runtimes.begin();
           it != annotation_provider_runtimes.end();) {
        if (it->plugin_id != plugin_id) {
          ++it;
          continue;
        }
        if (it->provide_ref != LUA_NOREF && it->provide_ref != LUA_REFNIL) {
          luaL_unref(state, LUA_REGISTRYINDEX, it->provide_ref);
        }
        it = annotation_provider_runtimes.erase(it);
      }
      auth_providers.erase(
          std::remove_if(auth_providers.begin(), auth_providers.end(),
                         [&](const PluginHost::ContributedAuthProvider& e) {
                           return e.plugin_id == plugin_id;
                         }),
          auth_providers.end());
      for (auto it = auth_provider_runtimes.begin(); it != auth_provider_runtimes.end();) {
        if (it->plugin_id != plugin_id) {
          ++it;
          continue;
        }
        if (it->login_ref != LUA_NOREF && it->login_ref != LUA_REFNIL) {
          luaL_unref(state, LUA_REGISTRYINDEX, it->login_ref);
        }
        if (it->refresh_ref != LUA_NOREF && it->refresh_ref != LUA_REFNIL) {
          luaL_unref(state, LUA_REGISTRYINDEX, it->refresh_ref);
        }
        if (it->logout_ref != LUA_NOREF && it->logout_ref != LUA_REFNIL) {
          luaL_unref(state, LUA_REGISTRYINDEX, it->logout_ref);
        }
        it = auth_provider_runtimes.erase(it);
      }
      ai_providers.erase(
          std::remove_if(ai_providers.begin(), ai_providers.end(),
                         [&](const PluginHost::ContributedAiProvider& e) {
                           return e.plugin_id == plugin_id;
                         }),
          ai_providers.end());
      external_agents.erase(
          std::remove_if(external_agents.begin(), external_agents.end(),
                         [&](const PluginHost::ContributedExternalAgent& e) {
                           return e.plugin_id == plugin_id;
                         }),
          external_agents.end());
      mcp_tools.erase(
          std::remove_if(mcp_tools.begin(), mcp_tools.end(),
                         [&](const PluginHost::ContributedMcpTool& e) {
                           return e.plugin_id == plugin_id;
                         }),
          mcp_tools.end());
      for (auto it = mcp_tool_runtimes.begin(); it != mcp_tool_runtimes.end();) {
        if (it->plugin_id != plugin_id) {
          ++it;
          continue;
        }
        if (it->run_ref != LUA_NOREF && it->run_ref != LUA_REFNIL) {
          luaL_unref(state, LUA_REGISTRYINDEX, it->run_ref);
        }
        it = mcp_tool_runtimes.erase(it);
      }
    }
  }

  void ConfigurePackage(lua_State* state, const std::filesystem::path& plugin_root) {
    lua_getglobal(state, "package");
    if (!lua_istable(state, -1)) {
      lua_pop(state, 1);
      return;
    }

    const std::string prefix =
        (plugin_root / "?.lua").generic_string() + ";" +
        (plugin_root / "?" / "init.lua").generic_string();
    lua_pushlstring(state, prefix.c_str(), prefix.size());
    lua_setfield(state, -2, "path");
    lua_pushliteral(state, "");
    lua_setfield(state, -2, "cpath");
    lua_pushnil(state);
    lua_setfield(state, -2, "loadlib");

    lua_getfield(state, -1, "preload");
    if (lua_istable(state, -1)) {
      lua_pushcfunction(state, &LuaOpenMicroide);
      lua_setfield(state, -2, "microide");
    }
    lua_pop(state, 2);
  }

  bool InitializeState(PluginInstance* plugin, std::string* error_message) {
    plugin->runtime = LuaRuntime::Create(error_message);
    if (!plugin->runtime) {
      return false;
    }
    plugin->state = plugin->runtime->state();
    ConfigurePackage(plugin->state, plugin->root);
    return true;
  }

  int ExtractFunctionRef(lua_State* state,
                         int table_index,
                         const char* field_name,
                         const std::filesystem::path& plugin_root,
                         std::string* error_message) {
    lua_getfield(state, table_index, field_name);
    if (lua_isnil(state, -1)) {
      lua_pop(state, 1);
      return LUA_NOREF;
    }
    if (!lua_isfunction(state, -1)) {
      if (error_message != nullptr) {
        *error_message = std::string("expected ") + field_name + " to be a function in " +
                         plugin_root.string();
      }
      lua_pop(state, 1);
      return LUA_NOREF;
    }
    return luaL_ref(state, LUA_REGISTRYINDEX);
  }

  bool LoadPluginDescriptor(PluginInstance* plugin, std::string* error_message) {
    const std::filesystem::path entry_path = plugin->root / "init.lua";
    if (luaL_loadfile(plugin->state, entry_path.string().c_str()) != LUA_OK) {
      if (error_message != nullptr) {
        *error_message = "failed to load " + entry_path.string() + ": " +
                         LuaErrorString(plugin->state);
      }
      lua_pop(plugin->state, 1);
      return false;
    }
    std::string call_error;
    if (!plugin->runtime->PCall(0, 1, &call_error)) {
      if (error_message != nullptr) {
        *error_message = "failed to evaluate " + entry_path.string() + ": " + call_error;
      }
      return false;
    }
    if (!lua_istable(plugin->state, -1)) {
      if (error_message != nullptr) {
        *error_message = "plugin entry point must return a table: " + entry_path.string();
      }
      lua_pop(plugin->state, 1);
      return false;
    }

    const int table_index = lua_absindex(plugin->state, -1);
    lua_getfield(plugin->state, table_index, "id");
    if (!lua_isstring(plugin->state, -1)) {
      if (error_message != nullptr) {
        *error_message = "plugin id must be a string: " + entry_path.string();
      }
      lua_pop(plugin->state, 2);
      return false;
    }
    plugin->id = lua_tostring(plugin->state, -1);
    lua_pop(plugin->state, 1);
    if (!IsValidIdentifier(plugin->id)) {
      if (error_message != nullptr) {
        *error_message = "invalid plugin id: " + plugin->id;
      }
      lua_pop(plugin->state, 1);
      return false;
    }

    plugin->setup_ref =
        ExtractFunctionRef(plugin->state, table_index, "setup", plugin->root, error_message);
    if (error_message != nullptr && !error_message->empty()) {
      lua_pop(plugin->state, 1);
      return false;
    }
    plugin->on_project_open_ref =
        ExtractFunctionRef(plugin->state, table_index, "on_project_open", plugin->root,
                           error_message);
    if (error_message != nullptr && !error_message->empty()) {
      lua_pop(plugin->state, 1);
      return false;
    }
    plugin->on_project_close_ref =
        ExtractFunctionRef(plugin->state, table_index, "on_project_close", plugin->root,
                           error_message);
    if (error_message != nullptr && !error_message->empty()) {
      lua_pop(plugin->state, 1);
      return false;
    }
    plugin->on_buffer_open_ref =
        ExtractFunctionRef(plugin->state, table_index, "on_buffer_open", plugin->root,
                           error_message);
    if (error_message != nullptr && !error_message->empty()) {
      lua_pop(plugin->state, 1);
      return false;
    }
    plugin->on_buffer_save_ref =
        ExtractFunctionRef(plugin->state, table_index, "on_buffer_save", plugin->root,
                           error_message);
    if (error_message != nullptr && !error_message->empty()) {
      lua_pop(plugin->state, 1);
      return false;
    }
    plugin->shutdown_ref =
        ExtractFunctionRef(plugin->state, table_index, "shutdown", plugin->root, error_message);
    if (error_message != nullptr && !error_message->empty()) {
      lua_pop(plugin->state, 1);
      return false;
    }

    lua_pop(plugin->state, 1);
    return true;
  }

  bool CallSetup(PluginInstance* plugin, std::string* error_message) {
    if (plugin->setup_ref == LUA_NOREF) {
      return true;
    }
    active_plugin = plugin;
    lua_rawgeti(plugin->state, LUA_REGISTRYINDEX, plugin->setup_ref);
    PushPluginContext(plugin->state);
    std::string call_error;
    if (!plugin->runtime->PCall(1, 0, &call_error)) {
      if (error_message != nullptr) {
        *error_message = FormatPluginPrefix(plugin) + " setup failed: " + call_error;
      }
      active_plugin = nullptr;
      return false;
    }
    active_plugin = nullptr;
    return true;
  }

  void CallProjectCallback(PluginInstance* plugin, int ref, const char* callback_name) {
    if (plugin == nullptr || ref == LUA_NOREF || current_project_root.empty()) {
      return;
    }
    lua_rawgeti(plugin->state, LUA_REGISTRYINDEX, ref);
    PushPluginContext(plugin->state);
    PushProjectTable(plugin->state, current_project_root);
    std::string call_error;
    if (!plugin->runtime->PCall(2, 0, &call_error)) {
      RecordError(FormatPluginPrefix(plugin) + " " + callback_name + " failed: " + call_error);
    }
  }

  void CallBufferCallback(PluginInstance* plugin,
                          int ref,
                          const char* callback_name,
                          const std::filesystem::path& path) {
    if (plugin == nullptr || ref == LUA_NOREF || path.empty()) {
      return;
    }
    lua_rawgeti(plugin->state, LUA_REGISTRYINDEX, ref);
    PushPluginContext(plugin->state);
    PushBufferTable(plugin->state, path);
    std::string call_error;
    if (!plugin->runtime->PCall(2, 0, &call_error)) {
      RecordError(FormatPluginPrefix(plugin) + " " + callback_name + " failed: " + call_error);
    }
  }

  void CallShutdown(PluginInstance* plugin) {
    if (plugin == nullptr || plugin->shutdown_ref == LUA_NOREF) {
      return;
    }
    lua_rawgeti(plugin->state, LUA_REGISTRYINDEX, plugin->shutdown_ref);
    PushPluginContext(plugin->state);
    std::string call_error;
    if (!plugin->runtime->PCall(1, 0, &call_error)) {
      RecordError(FormatPluginPrefix(plugin) + " shutdown failed: " + call_error);
    }
  }

  void TearDownPlugins() {
    if (!current_project_root.empty()) {
      for (auto& plugin : plugins) {
        CallProjectCallback(&plugin, plugin.on_project_close_ref, "on_project_close");
      }
    }
    for (auto& plugin : plugins) {
      CallShutdown(&plugin);
    }
    for (auto& plugin : plugins) {
      if (plugin.state != nullptr) {
        UnregisterContributionsForState(plugin.state);
      }
    }
    for (const auto& plugin : plugins) {
      ClearPluginDiagnostics(&plugin);
    }
    for (auto& plugin : plugins) {
      DestroyPluginState(&plugin);
    }
    plugins.clear();
  }

  bool LoadPluginRoot(const std::filesystem::path& plugin_root,
                      bool project_local,
                      std::string* error_message) {
    PluginInstance plugin{
        .id = {},
        .root = plugin_root.lexically_normal(),
        .project_local = project_local,
        .runtime = nullptr,
        .state = nullptr,
        .setup_ref = LUA_NOREF,
        .on_project_open_ref = LUA_NOREF,
        .on_project_close_ref = LUA_NOREF,
        .on_buffer_open_ref = LUA_NOREF,
        .on_buffer_save_ref = LUA_NOREF,
        .shutdown_ref = LUA_NOREF,
    };

    if (!InitializeState(&plugin, error_message)) {
      DestroyPluginState(&plugin);
      return false;
    }
    if (!LoadPluginDescriptor(&plugin, error_message)) {
      DestroyPluginState(&plugin);
      return false;
    }

    const auto duplicate =
        std::find_if(plugins.begin(), plugins.end(), [&](const PluginInstance& loaded) {
          return loaded.id == plugin.id;
        });
    if (duplicate != plugins.end()) {
      if (error_message != nullptr) {
        *error_message = "duplicate plugin id '" + plugin.id + "' in " +
                         plugin.root.string() + " and " + duplicate->root.string();
      }
      DestroyPluginState(&plugin);
      return false;
    }

    if (!CallSetup(&plugin, error_message)) {
      UnregisterContributionsForState(plugin.state);
      ClearPluginDiagnostics(&plugin);
      DestroyPluginState(&plugin);
      return false;
    }

    plugins.push_back(std::move(plugin));
    return true;
  }
#endif

  void ClearMessages() { messages.clear(); }

#if MICROIDE_HAS_LUA_PLUGINS
  void CancelAsyncProcessCallbacks() {
    std::lock_guard lock(async_process_state->mutex);
    for (auto& request : async_process_state->active_requests) {
      if (!request) {
        continue;
      }
      if (request->callback_ref != LUA_NOREF && request->lua_state != nullptr) {
        luaL_unref(request->lua_state, LUA_REGISTRYINDEX, request->callback_ref);
      }
      request->lua_state = nullptr;
      request->callback_ref = LUA_NOREF;
      request->cancelled = true;
    }
    for (auto& callback : async_process_state->pending_callbacks) {
      if (callback.callback_ref != LUA_NOREF && callback.lua_state != nullptr) {
        luaL_unref(callback.lua_state, LUA_REGISTRYINDEX, callback.callback_ref);
      }
      callback.lua_state = nullptr;
      callback.callback_ref = LUA_NOREF;
    }
    async_process_state->pending_callbacks.clear();
  }
#endif

};

PluginHost::PluginHost() : impl_(std::make_unique<Impl>()) {}

PluginHost::~PluginHost() {
  Shutdown();
}

PluginHost::PluginHost(PluginHost&& other) noexcept = default;

PluginHost& PluginHost::operator=(PluginHost&& other) noexcept = default;

void PluginHost::SetCallbacks(Callbacks callbacks) {
  impl_->callbacks = std::move(callbacks);
}

bool PluginHost::enabled() const {
  return impl_->enabled();
}

bool PluginHost::Reload(const std::filesystem::path& project_root) {
  impl_->errors.clear();
  if (!impl_->enabled()) {
    impl_->current_project_root = project_root.empty() ? std::filesystem::path{}
                                                       : project_root.lexically_normal();
    impl_->commands.clear();
    impl_->command_names.clear();
    impl_->sidebars.clear();
    impl_->sidebar_providers.clear();
    impl_->hovers.clear();
    impl_->hover_provider_order.clear();
    impl_->plugins.clear();
    impl_->SetReloadSummary();
    return false;
  }

#if MICROIDE_HAS_LUA_PLUGINS
  const std::filesystem::path next_project_root =
      project_root.empty() ? std::filesystem::path{} : project_root.lexically_normal();
  impl_->CancelAsyncProcessCallbacks();
  impl_->TearDownPlugins();
  impl_->current_project_root = next_project_root;

  for (const auto& entry : impl_->DiscoverPluginRoots()) {
    std::string error_message;
    if (!impl_->LoadPluginRoot(entry.first, entry.second, &error_message) &&
        !error_message.empty()) {
      impl_->RecordError(std::move(error_message));
    }
  }
  if (!impl_->current_project_root.empty()) {
    for (auto& plugin : impl_->plugins) {
      impl_->CallProjectCallback(&plugin, plugin.on_project_open_ref, "on_project_open");
    }
  }
#endif

  impl_->SetReloadSummary();
  return impl_->errors.empty();
}

void PluginHost::Shutdown() {
  if (!impl_->enabled()) {
    impl_->plugins.clear();
    impl_->commands.clear();
    impl_->command_names.clear();
    impl_->sidebars.clear();
    impl_->sidebar_providers.clear();
    impl_->hovers.clear();
    impl_->hover_provider_order.clear();
    impl_->current_project_root.clear();
    impl_->SetReloadSummary();
    return;
  }

#if MICROIDE_HAS_LUA_PLUGINS
  impl_->CancelAsyncProcessCallbacks();
  impl_->TearDownPlugins();
#endif
  impl_->current_project_root.clear();
  impl_->SetReloadSummary();
}

void PluginHost::SetAsyncProcessEventType(std::uint32_t type) {
  std::lock_guard lock(impl_->async_process_state->mutex);
  impl_->async_process_state->event_type = static_cast<Uint32>(type);
}

int PluginHost::ConsumeAsyncProcessCallbacks() {
#if MICROIDE_HAS_LUA_PLUGINS
  std::vector<Impl::AsyncProcessCallback> callbacks;
  {
    std::lock_guard lock(impl_->async_process_state->mutex);
    callbacks.swap(impl_->async_process_state->pending_callbacks);
  }
  for (auto& cb : callbacks) {
    lua_State* state = cb.lua_state;
    if (state == nullptr || cb.callback_ref == LUA_NOREF) {
      continue;
    }
    lua_rawgeti(state, LUA_REGISTRYINDEX, cb.callback_ref);
    luaL_unref(state, LUA_REGISTRYINDEX, cb.callback_ref);
    cb.callback_ref = LUA_NOREF;
    lua_createtable(state, 0, 3);
    lua_pushinteger(state, cb.result.exit_code);
    lua_setfield(state, -2, "exit_code");
    lua_pushlstring(state, cb.result.stdout_text.c_str(), cb.result.stdout_text.size());
    lua_setfield(state, -2, "stdout");
    lua_pushlstring(state, cb.result.stderr_text.c_str(), cb.result.stderr_text.size());
    lua_setfield(state, -2, "stderr");
    if (lua_pcall(state, 1, 0, 0) != LUA_OK) {
      const char* msg = lua_tostring(state, -1);
      if (impl_->callbacks.error_sink && msg != nullptr) {
        impl_->callbacks.error_sink(std::string("plugin async callback: ") + msg);
      }
      lua_pop(state, 1);
    }
  }
  return static_cast<int>(callbacks.size());
#endif
  return 0;
}

int PluginHost::PendingAsyncProcessCount() const {
  std::lock_guard lock(impl_->async_process_state->mutex);
  int active_count = 0;
  for (const auto& request : impl_->async_process_state->active_requests) {
    if (request != nullptr && !request->cancelled) {
      ++active_count;
    }
  }
  return active_count + static_cast<int>(impl_->async_process_state->pending_callbacks.size());
}

void PluginHost::OnBufferOpen(const std::filesystem::path& path) {
  if (!impl_->enabled() || path.empty()) {
    return;
  }

#if MICROIDE_HAS_LUA_PLUGINS
  for (auto& plugin : impl_->plugins) {
    impl_->CallBufferCallback(&plugin, plugin.on_buffer_open_ref, "on_buffer_open", path);
  }
#endif
}

void PluginHost::OnBufferSave(const std::filesystem::path& path) {
  if (!impl_->enabled() || path.empty()) {
    return;
  }

#if MICROIDE_HAS_LUA_PLUGINS
  for (auto& plugin : impl_->plugins) {
    impl_->CallBufferCallback(&plugin, plugin.on_buffer_save_ref, "on_buffer_save", path);
  }
#endif
}

bool PluginHost::ExecuteCommand(std::string_view name,
                                const std::vector<std::string>& args,
                                std::string* error_message) {
  const auto it = impl_->commands.find(std::string(name));
  if (it == impl_->commands.end()) {
    if (error_message != nullptr) {
      error_message->clear();
    }
    return false;
  }

  if (!impl_->enabled()) {
    if (error_message != nullptr) {
      *error_message = "Lua plugin runtime unavailable";
    }
    return false;
  }

#if MICROIDE_HAS_LUA_PLUGINS
  lua_State* state = it->second.state;
  lua_rawgeti(state, LUA_REGISTRYINDEX, it->second.function_ref);
  impl_->PushPluginContext(state);
  lua_createtable(state, static_cast<int>(args.size()), 0);
  for (std::size_t i = 0; i < args.size(); ++i) {
    lua_pushstring(state, args[i].c_str());
    lua_rawseti(state, -2, static_cast<lua_Integer>(i + 1));
  }

  if (lua_pcall(state, 2, 0, 0) != LUA_OK) {
    if (error_message != nullptr) {
      *error_message =
          "plugin command '" + std::string(name) + "' failed: " + Impl::LuaErrorString(state);
    }
    lua_pop(state, 1);
    return false;
  }
#else
  (void)args;
#endif

  if (error_message != nullptr) {
    error_message->clear();
  }
  return true;
}

const std::vector<std::string>& PluginHost::CommandNames() const {
  return impl_->command_names;
}

const std::vector<PluginHost::SidebarProviderInfo>& PluginHost::SidebarProviders() const {
  return impl_->sidebar_providers;
}

const PluginHost::SidebarProviderInfo* PluginHost::FindSidebarProvider(std::string_view id) const {
  const auto it = impl_->sidebars.find(std::string(id));
  return it == impl_->sidebars.end() ? nullptr : &it->second.info;
}

bool PluginHost::SnapshotSidebar(std::string_view id,
                                 std::vector<SidebarItem>* items,
                                 std::string* error_message) {
  const auto it = impl_->sidebars.find(std::string(id));
  if (it == impl_->sidebars.end()) {
    if (error_message != nullptr) {
      *error_message = "unknown plugin sidebar: " + std::string(id);
    }
    if (items != nullptr) {
      items->clear();
    }
    return false;
  }
#if MICROIDE_HAS_LUA_PLUGINS
  return impl_->SnapshotSidebarProvider(it->second, items, error_message);
#else
  if (items != nullptr) {
    items->clear();
  }
  if (error_message != nullptr) {
    *error_message = "Lua plugin runtime unavailable";
  }
  return false;
#endif
}

bool PluginHost::ConfirmSidebarItem(std::string_view id,
                                    const SidebarItem& item,
                                    std::string* error_message) {
  const auto it = impl_->sidebars.find(std::string(id));
  if (it == impl_->sidebars.end()) {
    if (error_message != nullptr) {
      *error_message = "unknown plugin sidebar: " + std::string(id);
    }
    return false;
  }
#if MICROIDE_HAS_LUA_PLUGINS
  return impl_->ConfirmSidebarProviderItem(it->second, item, error_message);
#else
  (void)item;
  if (error_message != nullptr) {
    *error_message = "Lua plugin runtime unavailable";
  }
  return false;
#endif
}

bool PluginHost::QueryHover(const std::filesystem::path& path,
                            std::size_t line,
                            std::size_t column,
                            HoverResult* result,
                            std::string* error_message) const {
  if (result == nullptr) {
    if (error_message != nullptr) {
      *error_message = "hover result output pointer is required";
    }
    return false;
  }
  result->title.clear();
  result->content.clear();

  if (!impl_->enabled() || path.empty() || line == 0 || column == 0) {
    if (error_message != nullptr) {
      error_message->clear();
    }
    return false;
  }

  const std::filesystem::path resolved_path =
      ResolveRuntimePath(impl_->current_project_root, path).lexically_normal();
  Impl* impl = impl_.get();
#if MICROIDE_HAS_LUA_PLUGINS
  for (const std::string& provider_id : impl->hover_provider_order) {
    const auto it = impl->hovers.find(provider_id);
    if (it == impl->hovers.end()) {
      continue;
    }
    if (!impl->QueryHoverProvider(it->second, resolved_path, line, column, result, error_message)) {
      return false;
    }
    if (!result->title.empty() || !result->content.empty()) {
      if (error_message != nullptr) {
        error_message->clear();
      }
      return true;
    }
  }
#else
  (void)resolved_path;
  (void)impl;
#endif

  if (error_message != nullptr) {
    error_message->clear();
  }
  return false;
}

std::vector<std::filesystem::path> PluginHost::DataDirectories(std::string_view subdirectory) const {
  if (subdirectory.empty()) {
    return {};
  }

  std::vector<std::filesystem::path> directories;
  directories.reserve(impl_->plugins.size());

  const auto append_matching_directories = [&](bool project_local) {
    for (const auto& plugin : impl_->plugins) {
      if (plugin.project_local != project_local) {
        continue;
      }
      const std::filesystem::path candidate = (plugin.root / subdirectory).lexically_normal();
      if (platform::ReadPathType(candidate) != platform::PathType::Directory) {
        continue;
      }
      directories.push_back(candidate);
    }
  };

  append_matching_directories(true);
  append_matching_directories(false);
  return directories;
}

const std::vector<PluginHost::ContributedMenuEntry>& PluginHost::ContributedMenuEntries() const {
  return impl_->menu_entries;
}

const std::vector<PluginHost::ContributedKeybinding>& PluginHost::ContributedKeybindings() const {
  return impl_->keybindings;
}

const std::vector<PluginHost::ContributedSettingSpec>& PluginHost::ContributedSettings() const {
  return impl_->settings;
}

const std::vector<PluginHost::ContributedStatusItem>& PluginHost::ContributedStatusItems() const {
  return impl_->status_item_order;
}

bool PluginHost::UpdateStatusItem(std::string_view id, std::string text, std::string tooltip) {
  auto it = impl_->status_items.find(std::string(id));
  if (it == impl_->status_items.end()) {
    return false;
  }
  it->second.text = std::move(text);
  if (!tooltip.empty()) {
    it->second.tooltip = std::move(tooltip);
  }
  for (auto& order_item : impl_->status_item_order) {
    if (order_item.id == it->first) {
      order_item.text = it->second.text;
      order_item.tooltip = it->second.tooltip;
      break;
    }
  }
  if (impl_->callbacks.request_status_redraw) {
    impl_->callbacks.request_status_redraw();
  }
  return true;
}

bool PluginHost::RunSaveParticipants(const std::filesystem::path& path,
                                     std::string* text,
                                     std::string* error_message) const {
  if (error_message != nullptr) {
    error_message->clear();
  }
  if (text == nullptr) {
    if (error_message != nullptr) {
      *error_message = "save participants require mutable text";
    }
    return false;
  }
  if (!impl_->enabled()) {
    return false;
  }

#if MICROIDE_HAS_LUA_PLUGINS
  for (const auto& participant : impl_->save_participant_runtimes) {
    lua_State* state = participant.state;
    lua_rawgeti(state, LUA_REGISTRYINDEX, participant.function_ref);
    impl_->PushBufferContext(state, path, *text);
    if (lua_pcall(state, 1, 1, 0) != LUA_OK) {
      if (error_message != nullptr) {
        *error_message = "save participant '" + participant.id +
                         "' failed: " + Impl::LuaErrorString(state);
      }
      lua_pop(state, 1);
      return false;
    }
    if (lua_isstring(state, -1)) {
      std::size_t size = 0;
      const char* updated = lua_tolstring(state, -1, &size);
      if (updated != nullptr) {
        *text = std::string(updated, size);
      }
    } else if (lua_istable(state, -1)) {
      lua_getfield(state, -1, "text");
      if (lua_isstring(state, -1)) {
        std::size_t size = 0;
        const char* updated = lua_tolstring(state, -1, &size);
        if (updated != nullptr) {
          *text = std::string(updated, size);
        }
      }
      lua_pop(state, 1);
    }
    lua_pop(state, 1);
  }
#endif
  return true;
}

std::vector<PluginHost::CompletionCandidate> PluginHost::QueryCompletions(
    std::string_view language_id,
    const std::filesystem::path& path,
    std::size_t line,
    std::size_t column,
    std::string_view trigger_character,
    std::string* error_message) const {
  if (error_message != nullptr) {
    error_message->clear();
  }

  std::vector<CompletionCandidate> results;
  if (!impl_->enabled()) {
    return results;
  }

#if MICROIDE_HAS_LUA_PLUGINS
  for (const auto& provider : impl_->completion_runtimes) {
    if (provider.language_id != language_id) {
      continue;
    }
    lua_State* state = provider.state;
    lua_rawgeti(state, LUA_REGISTRYINDEX, provider.provide_ref);
    impl_->PushBufferContext(state, path);
    Impl::PushPosition(state, line, column);
    lua_pushlstring(state, trigger_character.data(), trigger_character.size());
    if (lua_pcall(state, 3, 1, 0) != LUA_OK) {
      if (error_message != nullptr) {
        *error_message = "completion provider '" + provider.id +
                         "' failed: " + Impl::LuaErrorString(state);
      }
      lua_pop(state, 1);
      return {};
    }
    if (lua_istable(state, -1)) {
      for (lua_Integer i = 1; ; ++i) {
        lua_geti(state, -1, i);
        if (lua_isnil(state, -1)) {
          lua_pop(state, 1);
          break;
        }
        if (!lua_istable(state, -1)) {
          lua_pop(state, 1);
          continue;
        }
        CompletionCandidate candidate;
        auto read_string = [&](const char* field) -> std::string {
          lua_getfield(state, -1, field);
          std::string value = lua_isstring(state, -1) ? std::string(lua_tostring(state, -1))
                                                      : std::string{};
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
#else
  (void)language_id;
  (void)path;
  (void)line;
  (void)column;
  (void)trigger_character;
#endif
  return results;
}

std::vector<PluginHost::CodeActionCandidate> PluginHost::QueryCodeActions(
    std::string_view language_id,
    const std::filesystem::path& path,
    std::size_t start_line,
    std::size_t start_column,
    std::size_t end_line,
    std::size_t end_column,
    std::string* error_message) const {
  if (error_message != nullptr) {
    error_message->clear();
  }

  std::vector<CodeActionCandidate> results;
  if (!impl_->enabled()) {
    return results;
  }

#if MICROIDE_HAS_LUA_PLUGINS
  for (const auto& provider : impl_->code_action_runtimes) {
    if (provider.language_id != language_id) {
      continue;
    }
    lua_State* state = provider.state;
    lua_rawgeti(state, LUA_REGISTRYINDEX, provider.provide_ref);
    impl_->PushBufferContext(state, path);
    Impl::PushRange(state, start_line, start_column, end_line, end_column);
    if (lua_pcall(state, 2, 1, 0) != LUA_OK) {
      if (error_message != nullptr) {
        *error_message = "code action provider '" + provider.id +
                         "' failed: " + Impl::LuaErrorString(state);
      }
      lua_pop(state, 1);
      return {};
    }
    if (lua_istable(state, -1)) {
      for (lua_Integer i = 1; ; ++i) {
        lua_geti(state, -1, i);
        if (lua_isnil(state, -1)) {
          lua_pop(state, 1);
          break;
        }
        if (!lua_istable(state, -1)) {
          lua_pop(state, 1);
          continue;
        }
        CodeActionCandidate action;
        auto read_string = [&](const char* field) -> std::string {
          lua_getfield(state, -1, field);
          std::string value = lua_isstring(state, -1) ? std::string(lua_tostring(state, -1))
                                                      : std::string{};
          lua_pop(state, 1);
          return value;
        };
        action.title = read_string("title");
        action.command = read_string("command");
        lua_getfield(state, -1, "arguments");
        if (lua_istable(state, -1)) {
          for (lua_Integer arg_index = 1; ; ++arg_index) {
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
#else
  (void)language_id;
  (void)path;
  (void)start_line;
  (void)start_column;
  (void)end_line;
  (void)end_column;
#endif
  return results;
}

bool PluginHost::DiscoverTests(std::string_view provider_id,
                               const std::filesystem::path& path,
                               std::vector<TestCase>* tests,
                               std::string* error_message) const {
  if (error_message != nullptr) {
    error_message->clear();
  }
  if (tests == nullptr) {
    if (error_message != nullptr) {
      *error_message = "test discovery requires an output vector";
    }
    return false;
  }
  tests->clear();
  if (!impl_->enabled()) {
    return false;
  }

#if MICROIDE_HAS_LUA_PLUGINS
  const auto it =
      std::find_if(impl_->test_provider_runtimes.begin(), impl_->test_provider_runtimes.end(),
                   [provider_id](const auto& provider) { return provider.id == provider_id; });
  if (it == impl_->test_provider_runtimes.end()) {
    if (error_message != nullptr) {
      *error_message = "unknown test provider: " + std::string(provider_id);
    }
    return false;
  }
  if (it->discover_ref == LUA_NOREF || it->discover_ref == LUA_REFNIL) {
    return true;
  }

  lua_State* state = it->state;
  lua_rawgeti(state, LUA_REGISTRYINDEX, it->discover_ref);
  impl_->PushBufferContext(state, path);
  if (lua_pcall(state, 1, 1, 0) != LUA_OK) {
    if (error_message != nullptr) {
      *error_message = "test discovery provider '" + it->id +
                       "' failed: " + Impl::LuaErrorString(state);
    }
    lua_pop(state, 1);
    return false;
  }
  if (lua_istable(state, -1)) {
    for (lua_Integer i = 1; ; ++i) {
      lua_geti(state, -1, i);
      if (lua_isnil(state, -1)) {
        lua_pop(state, 1);
        break;
      }
      if (!lua_istable(state, -1)) {
        lua_pop(state, 1);
        continue;
      }
      TestCase test;
      auto read_string = [&](const char* field) -> std::string {
        lua_getfield(state, -1, field);
        std::string value = lua_isstring(state, -1) ? std::string(lua_tostring(state, -1))
                                                    : std::string{};
        lua_pop(state, 1);
        return value;
      };
      test.id = read_string("id");
      test.label = read_string("label");
      const std::string file = read_string("file");
      if (!file.empty()) {
        test.file = ResolveRuntimePath(impl_->current_project_root, std::filesystem::path(file));
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
#else
  (void)provider_id;
  (void)path;
  return false;
#endif
}

bool PluginHost::RunTests(std::string_view provider_id,
                          const std::vector<std::string>& test_ids,
                          std::vector<TestRunResult>* results,
                          std::string* error_message) const {
  if (error_message != nullptr) {
    error_message->clear();
  }
  if (results == nullptr) {
    if (error_message != nullptr) {
      *error_message = "test execution requires an output vector";
    }
    return false;
  }
  results->clear();
  if (!impl_->enabled()) {
    return false;
  }

#if MICROIDE_HAS_LUA_PLUGINS
  const auto it =
      std::find_if(impl_->test_provider_runtimes.begin(), impl_->test_provider_runtimes.end(),
                   [provider_id](const auto& provider) { return provider.id == provider_id; });
  if (it == impl_->test_provider_runtimes.end()) {
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
  lua_rawgeti(state, LUA_REGISTRYINDEX, it->run_ref);
  lua_createtable(state, static_cast<int>(test_ids.size()), 0);
  for (std::size_t i = 0; i < test_ids.size(); ++i) {
    lua_pushlstring(state, test_ids[i].c_str(), test_ids[i].size());
    lua_rawseti(state, -2, static_cast<lua_Integer>(i + 1));
  }
  if (lua_pcall(state, 1, 1, 0) != LUA_OK) {
    if (error_message != nullptr) {
      *error_message = "test provider '" + it->id +
                       "' run failed: " + Impl::LuaErrorString(state);
    }
    lua_pop(state, 1);
    return false;
  }
  if (lua_istable(state, -1)) {
    for (lua_Integer i = 1; ; ++i) {
      lua_geti(state, -1, i);
      if (lua_isnil(state, -1)) {
        lua_pop(state, 1);
        break;
      }
      if (!lua_istable(state, -1)) {
        lua_pop(state, 1);
        continue;
      }
      TestRunResult result;
      auto read_string = [&](const char* field) -> std::string {
        lua_getfield(state, -1, field);
        std::string value = lua_isstring(state, -1) ? std::string(lua_tostring(state, -1))
                                                    : std::string{};
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
#else
  (void)provider_id;
  (void)test_ids;
  return false;
#endif
}

bool PluginHost::SnapshotScm(std::string_view provider_id,
                             ScmSnapshot* snapshot,
                             std::string* error_message) const {
  if (error_message != nullptr) {
    error_message->clear();
  }
  if (snapshot == nullptr) {
    if (error_message != nullptr) {
      *error_message = "scm snapshot requires an output value";
    }
    return false;
  }
  *snapshot = ScmSnapshot{};
  if (!impl_->enabled()) {
    return false;
  }

#if MICROIDE_HAS_LUA_PLUGINS
  const auto it =
      std::find_if(impl_->scm_provider_runtimes.begin(), impl_->scm_provider_runtimes.end(),
                   [provider_id](const auto& runtime) { return runtime.id == provider_id; });
  if (it == impl_->scm_provider_runtimes.end()) {
    if (error_message != nullptr) {
      *error_message = "unknown scm provider: " + std::string(provider_id);
    }
    return false;
  }

  lua_State* state = it->state;
  lua_rawgeti(state, LUA_REGISTRYINDEX, it->snapshot_ref);
  if (lua_pcall(state, 0, 1, 0) != LUA_OK) {
    if (error_message != nullptr) {
      *error_message = "scm provider '" + it->id +
                       "' failed: " + Impl::LuaErrorString(state);
    }
    lua_pop(state, 1);
    return false;
  }

  if (lua_istable(state, -1)) {
    auto read_string = [&](const char* field) -> std::string {
      lua_getfield(state, -1, field);
      std::string value = lua_isstring(state, -1) ? std::string(lua_tostring(state, -1))
                                                  : std::string{};
      lua_pop(state, 1);
      return value;
    };
    snapshot->base_ref = read_string("base_ref");
    snapshot->base_label = read_string("base_label");
    lua_getfield(state, -1, "supports_mutations");
    snapshot->supports_mutations = lua_toboolean(state, -1) != 0;
    lua_pop(state, 1);

    lua_getfield(state, -1, "entries");
    if (lua_istable(state, -1)) {
      for (lua_Integer index = 1; ; ++index) {
        lua_geti(state, -1, index);
        if (lua_isnil(state, -1)) {
          lua_pop(state, 1);
          break;
        }
        if (!lua_istable(state, -1)) {
          lua_pop(state, 1);
          continue;
        }
        ScmEntry entry;
        auto read_entry_string = [&](const char* field) -> std::string {
          lua_getfield(state, -1, field);
          std::string value = lua_isstring(state, -1) ? std::string(lua_tostring(state, -1))
                                                      : std::string{};
          lua_pop(state, 1);
          return value;
        };
        const std::string path = read_entry_string("path");
        const std::string relative_path = read_entry_string("relative_path");
        entry.path = ResolveRuntimePath(impl_->current_project_root, std::filesystem::path(path));
        entry.relative_path =
            relative_path.empty() ? std::filesystem::path{} : std::filesystem::path(relative_path);
        entry.status = read_entry_string("status");
        lua_getfield(state, -1, "conflicted");
        entry.conflicted = lua_toboolean(state, -1) != 0;
        lua_pop(state, 1);
        lua_getfield(state, -1, "staged");
        entry.staged = lua_toboolean(state, -1) != 0;
        lua_pop(state, 1);
        lua_getfield(state, -1, "supports_stage");
        entry.supports_stage = lua_toboolean(state, -1) != 0;
        lua_pop(state, 1);
        lua_getfield(state, -1, "supports_discard");
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
#else
  (void)provider_id;
  return false;
#endif
}

std::vector<PluginHost::AnnotationLine> PluginHost::QueryAnnotations(
    std::string_view provider_id,
    const std::filesystem::path& path,
    std::string_view language_id,
    std::size_t visible_start_line,
    std::size_t visible_end_line,
    std::string* error_message) const {
  if (error_message != nullptr) {
    error_message->clear();
  }

  std::vector<AnnotationLine> lines;
  if (!impl_->enabled()) {
    return lines;
  }

#if MICROIDE_HAS_LUA_PLUGINS
  for (const auto& provider : impl_->annotation_provider_runtimes) {
    if ((!provider_id.empty() && provider.id != provider_id) ||
        (!language_id.empty() && provider.language_id != language_id)) {
      continue;
    }

    lua_State* state = provider.state;
    lua_rawgeti(state, LUA_REGISTRYINDEX, provider.provide_ref);
    impl_->PushBufferContext(state, path);
    lua_pushinteger(state, static_cast<lua_Integer>(visible_start_line));
    lua_pushinteger(state, static_cast<lua_Integer>(visible_end_line));
    if (lua_pcall(state, 3, 1, 0) != LUA_OK) {
      if (error_message != nullptr) {
        *error_message = "annotation provider '" + provider.id +
                         "' failed: " + Impl::LuaErrorString(state);
      }
      lua_pop(state, 1);
      return {};
    }
    if (lua_istable(state, -1)) {
      for (lua_Integer index = 1; ; ++index) {
        lua_geti(state, -1, index);
        if (lua_isnil(state, -1)) {
          lua_pop(state, 1);
          break;
        }
        if (!lua_istable(state, -1)) {
          lua_pop(state, 1);
          continue;
        }
        AnnotationLine line;
        auto read_string = [&](const char* field) -> std::string {
          lua_getfield(state, -1, field);
          std::string value = lua_isstring(state, -1) ? std::string(lua_tostring(state, -1))
                                                      : std::string{};
          lua_pop(state, 1);
          return value;
        };
        lua_getfield(state, -1, "line");
        if (lua_isinteger(state, -1)) {
          line.line = static_cast<std::size_t>(std::max<lua_Integer>(0, lua_tointeger(state, -1)));
        }
        lua_pop(state, 1);
        line.text = read_string("text");
        line.author = read_string("author");
        line.summary = read_string("summary");
        line.date = read_string("date");
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
#else
  (void)provider_id;
  (void)path;
  (void)language_id;
  (void)visible_start_line;
  (void)visible_end_line;
#endif
  return lines;
}

bool PluginHost::LoginAuthProvider(std::string_view provider_id,
                                   const std::vector<std::string>& scopes,
                                   AuthSessionData* session,
                                   std::string* error_message) const {
  if (error_message != nullptr) {
    error_message->clear();
  }
  if (session == nullptr) {
    if (error_message != nullptr) {
      *error_message = "auth login requires an output session";
    }
    return false;
  }
  *session = AuthSessionData{};
  if (!impl_->enabled()) {
    return false;
  }

#if MICROIDE_HAS_LUA_PLUGINS
  const auto it =
      std::find_if(impl_->auth_provider_runtimes.begin(), impl_->auth_provider_runtimes.end(),
                   [provider_id](const auto& runtime) { return runtime.id == provider_id; });
  if (it == impl_->auth_provider_runtimes.end()) {
    if (error_message != nullptr) {
      *error_message = "unknown auth provider: " + std::string(provider_id);
    }
    return false;
  }
  if (it->login_ref == LUA_NOREF || it->login_ref == LUA_REFNIL) {
    if (error_message != nullptr) {
      *error_message = "auth provider '" + std::string(provider_id) +
                       "' does not support login";
    }
    return false;
  }

  lua_State* state = it->state;
  lua_rawgeti(state, LUA_REGISTRYINDEX, it->login_ref);
  lua_createtable(state, static_cast<int>(scopes.size()), 0);
  for (std::size_t i = 0; i < scopes.size(); ++i) {
    lua_pushlstring(state, scopes[i].c_str(), scopes[i].size());
    lua_rawseti(state, -2, static_cast<lua_Integer>(i + 1));
  }
  if (lua_pcall(state, 1, 1, 0) != LUA_OK) {
    if (error_message != nullptr) {
      *error_message = "auth provider '" + it->id +
                       "' login failed: " + Impl::LuaErrorString(state);
    }
    lua_pop(state, 1);
    return false;
  }
  if (lua_istable(state, -1)) {
    auto read_string = [&](const char* field) -> std::string {
      lua_getfield(state, -1, field);
      std::string value = lua_isstring(state, -1) ? std::string(lua_tostring(state, -1))
                                                  : std::string{};
      lua_pop(state, 1);
      return value;
    };
    session->id = read_string("id");
    session->account = read_string("account");
    session->access_token = read_string("access_token");
    lua_getfield(state, -1, "scopes");
    if (lua_istable(state, -1)) {
      for (lua_Integer i = 1; ; ++i) {
        lua_geti(state, -1, i);
        if (lua_isnil(state, -1)) {
          lua_pop(state, 1);
          break;
        }
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
#else
  (void)provider_id;
  (void)scopes;
  return false;
#endif
}

bool PluginHost::RefreshAuthSession(std::string_view provider_id,
                                    std::string_view session_id,
                                    AuthSessionData* session,
                                    std::string* error_message) const {
  if (error_message != nullptr) {
    error_message->clear();
  }
  if (session == nullptr) {
    if (error_message != nullptr) {
      *error_message = "auth refresh requires an output session";
    }
    return false;
  }
  *session = AuthSessionData{};
  if (!impl_->enabled()) {
    return false;
  }

#if MICROIDE_HAS_LUA_PLUGINS
  const auto it =
      std::find_if(impl_->auth_provider_runtimes.begin(), impl_->auth_provider_runtimes.end(),
                   [provider_id](const auto& runtime) { return runtime.id == provider_id; });
  if (it == impl_->auth_provider_runtimes.end()) {
    if (error_message != nullptr) {
      *error_message = "unknown auth provider: " + std::string(provider_id);
    }
    return false;
  }
  if (it->refresh_ref == LUA_NOREF || it->refresh_ref == LUA_REFNIL) {
    if (error_message != nullptr) {
      *error_message = "auth provider '" + std::string(provider_id) +
                       "' does not support refresh";
    }
    return false;
  }

  lua_State* state = it->state;
  lua_rawgeti(state, LUA_REGISTRYINDEX, it->refresh_ref);
  lua_pushlstring(state, session_id.data(), session_id.size());
  if (lua_pcall(state, 1, 1, 0) != LUA_OK) {
    if (error_message != nullptr) {
      *error_message = "auth provider '" + it->id +
                       "' refresh failed: " + Impl::LuaErrorString(state);
    }
    lua_pop(state, 1);
    return false;
  }
  if (lua_istable(state, -1)) {
    auto read_string = [&](const char* field) -> std::string {
      lua_getfield(state, -1, field);
      std::string value = lua_isstring(state, -1) ? std::string(lua_tostring(state, -1))
                                                  : std::string{};
      lua_pop(state, 1);
      return value;
    };
    session->id = read_string("id");
    if (session->id.empty()) {
      session->id = std::string(session_id);
    }
    session->account = read_string("account");
    session->access_token = read_string("access_token");
    lua_getfield(state, -1, "scopes");
    if (lua_istable(state, -1)) {
      for (lua_Integer i = 1; ; ++i) {
        lua_geti(state, -1, i);
        if (lua_isnil(state, -1)) {
          lua_pop(state, 1);
          break;
        }
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
#else
  (void)provider_id;
  (void)session_id;
  return false;
#endif
}

bool PluginHost::LogoutAuthSession(std::string_view provider_id,
                                   std::string_view session_id,
                                   std::string* error_message) const {
  if (error_message != nullptr) {
    error_message->clear();
  }
  if (!impl_->enabled()) {
    return false;
  }

#if MICROIDE_HAS_LUA_PLUGINS
  const auto it =
      std::find_if(impl_->auth_provider_runtimes.begin(), impl_->auth_provider_runtimes.end(),
                   [provider_id](const auto& runtime) { return runtime.id == provider_id; });
  if (it == impl_->auth_provider_runtimes.end()) {
    if (error_message != nullptr) {
      *error_message = "unknown auth provider: " + std::string(provider_id);
    }
    return false;
  }
  if (it->logout_ref == LUA_NOREF || it->logout_ref == LUA_REFNIL) {
    if (error_message != nullptr) {
      *error_message = "auth provider '" + std::string(provider_id) +
                       "' does not support logout";
    }
    return false;
  }

  lua_State* state = it->state;
  lua_rawgeti(state, LUA_REGISTRYINDEX, it->logout_ref);
  lua_pushlstring(state, session_id.data(), session_id.size());
  if (lua_pcall(state, 1, 0, 0) != LUA_OK) {
    if (error_message != nullptr) {
      *error_message = "auth provider '" + it->id +
                       "' logout failed: " + Impl::LuaErrorString(state);
    }
    lua_pop(state, 1);
    return false;
  }
  return true;
#else
  (void)provider_id;
  (void)session_id;
  return false;
#endif
}

bool PluginHost::InvokeMcpTool(std::string_view tool_id,
                               std::string_view input_json,
                               std::string* output_json,
                               std::string* error_message) const {
  if (error_message != nullptr) {
    error_message->clear();
  }
  if (output_json == nullptr) {
    if (error_message != nullptr) {
      *error_message = "mcp invocation requires an output string";
    }
    return false;
  }
  output_json->clear();
  if (!impl_->enabled()) {
    return false;
  }

#if MICROIDE_HAS_LUA_PLUGINS
  const auto it =
      std::find_if(impl_->mcp_tool_runtimes.begin(), impl_->mcp_tool_runtimes.end(),
                   [tool_id](const auto& runtime) { return runtime.id == tool_id; });
  if (it == impl_->mcp_tool_runtimes.end()) {
    if (error_message != nullptr) {
      *error_message = "unknown mcp tool: " + std::string(tool_id);
    }
    return false;
  }

  lua_State* state = it->state;
  lua_rawgeti(state, LUA_REGISTRYINDEX, it->run_ref);
  lua_pushlstring(state, input_json.data(), input_json.size());
  if (lua_pcall(state, 1, 1, 0) != LUA_OK) {
    if (error_message != nullptr) {
      *error_message = "mcp tool '" + it->id +
                       "' failed: " + Impl::LuaErrorString(state);
    }
    lua_pop(state, 1);
    return false;
  }
  if (lua_isstring(state, -1)) {
    *output_json = lua_tostring(state, -1);
  } else if (lua_istable(state, -1)) {
    lua_getfield(state, -1, "output");
    if (lua_isstring(state, -1)) {
      *output_json = lua_tostring(state, -1);
    }
    lua_pop(state, 1);
  }
  lua_pop(state, 1);
  return true;
#else
  (void)tool_id;
  (void)input_json;
  return false;
#endif
}

const std::vector<PluginHost::ContributedFormatter>& PluginHost::ContributedFormatters() const {
  return impl_->formatters;
}

const std::vector<PluginHost::ContributedSaveParticipant>& PluginHost::ContributedSaveParticipants()
    const {
  return impl_->save_participants;
}

const std::vector<PluginHost::ContributedCompletion>& PluginHost::ContributedCompletions() const {
  return impl_->completions;
}

const std::vector<PluginHost::ContributedCodeAction>& PluginHost::ContributedCodeActions() const {
  return impl_->code_actions;
}

const std::vector<PluginHost::ContributedLanguageServer>&
PluginHost::ContributedLanguageServers() const {
  return impl_->language_servers;
}

const std::vector<PluginHost::ContributedTask>& PluginHost::ContributedTasks() const {
  return impl_->tasks;
}

const std::vector<PluginHost::ContributedTool>& PluginHost::ContributedTools() const {
  return impl_->tools;
}

const std::vector<PluginHost::ContributedDebugger>& PluginHost::ContributedDebuggers() const {
  return impl_->debuggers;
}

const std::vector<PluginHost::ContributedTestProvider>& PluginHost::ContributedTestProviders()
    const {
  return impl_->test_providers;
}

const std::vector<PluginHost::ContributedScmProvider>& PluginHost::ContributedScmProviders()
    const {
  return impl_->scm_providers;
}

const std::vector<PluginHost::ContributedAnnotationProvider>&
PluginHost::ContributedAnnotationProviders() const {
  return impl_->annotation_providers;
}

const std::vector<PluginHost::ContributedAuthProvider>& PluginHost::ContributedAuthProviders()
    const {
  return impl_->auth_providers;
}

const std::vector<PluginHost::ContributedAiProvider>& PluginHost::ContributedAiProviders()
    const {
  return impl_->ai_providers;
}

const std::vector<PluginHost::ContributedExternalAgent>& PluginHost::ContributedExternalAgents()
    const {
  return impl_->external_agents;
}

const std::vector<PluginHost::ContributedMcpTool>& PluginHost::ContributedMcpTools() const {
  return impl_->mcp_tools;
}

const std::vector<std::string>& PluginHost::Messages() const {
  return impl_->messages;
}

const std::vector<std::string>& PluginHost::Errors() const {
  return impl_->errors;
}

void PluginHost::ClearMessages() {
  impl_->ClearMessages();
}

std::string PluginHost::ReloadSummary() const {
  return impl_->reload_summary;
}

std::size_t PluginHost::LoadedPluginCount() const {
  return impl_->plugins.size();
}

}  // namespace microide::plugin
