#include "plugin/PluginHost.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "platform/AppDirectories.h"
#include "platform/Filesystem.h"
#include "platform/Subprocess.h"
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

  Callbacks callbacks{};
  std::filesystem::path current_project_root;
  std::vector<PluginInstance> plugins;
  std::unordered_map<std::string, PluginCommand> commands;
  std::vector<std::string> command_names;
  std::unordered_map<std::string, SidebarProvider> sidebars;
  std::vector<SidebarProviderInfo> sidebar_providers;
  std::unordered_map<std::string, HoverProvider> hovers;
  std::vector<std::string> hover_provider_order;
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

  std::vector<std::pair<std::filesystem::path, bool>> DiscoverPluginRoots() const {
    std::vector<std::pair<std::filesystem::path, bool>> plugin_roots;
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
        const std::filesystem::path init_path = entry.path / "init.lua";
        if (platform::ReadPathType(init_path) == platform::PathType::RegularFile) {
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
    lua_createtable(state, 0, 7);

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

    lua_createtable(state, 0, 1);
    lua_pushlightuserdata(state, this);
    lua_pushcclosure(state, &LuaProcessRun, 1);
    lua_setfield(state, -2, "run");
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
    lua_close(plugin->state);
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
    plugin->state = luaL_newstate();
    if (plugin->state == nullptr) {
      if (error_message != nullptr) {
        *error_message = "failed to create Lua state";
      }
      return false;
    }

    luaL_requiref(plugin->state, "_G", luaopen_base, 1);
    lua_pop(plugin->state, 1);
    luaL_requiref(plugin->state, LUA_TABLIBNAME, luaopen_table, 1);
    lua_pop(plugin->state, 1);
    luaL_requiref(plugin->state, LUA_STRLIBNAME, luaopen_string, 1);
    lua_pop(plugin->state, 1);
    luaL_requiref(plugin->state, LUA_MATHLIBNAME, luaopen_math, 1);
    lua_pop(plugin->state, 1);
    luaL_requiref(plugin->state, LUA_UTF8LIBNAME, luaopen_utf8, 1);
    lua_pop(plugin->state, 1);
    luaL_requiref(plugin->state, LUA_LOADLIBNAME, luaopen_package, 1);
    lua_pop(plugin->state, 1);

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
    if (lua_pcall(plugin->state, 0, 1, 0) != LUA_OK) {
      if (error_message != nullptr) {
        *error_message = "failed to evaluate " + entry_path.string() + ": " +
                         LuaErrorString(plugin->state);
      }
      lua_pop(plugin->state, 1);
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
    if (lua_pcall(plugin->state, 1, 0, 0) != LUA_OK) {
      if (error_message != nullptr) {
        *error_message = FormatPluginPrefix(plugin) + " setup failed: " +
                         LuaErrorString(plugin->state);
      }
      active_plugin = nullptr;
      lua_pop(plugin->state, 1);
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
    if (lua_pcall(plugin->state, 2, 0, 0) != LUA_OK) {
      RecordError(FormatPluginPrefix(plugin) + " " + callback_name + " failed: " +
                  LuaErrorString(plugin->state));
      lua_pop(plugin->state, 1);
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
    if (lua_pcall(plugin->state, 2, 0, 0) != LUA_OK) {
      RecordError(FormatPluginPrefix(plugin) + " " + callback_name + " failed: " +
                  LuaErrorString(plugin->state));
      lua_pop(plugin->state, 1);
    }
  }

  void CallShutdown(PluginInstance* plugin) {
    if (plugin == nullptr || plugin->shutdown_ref == LUA_NOREF) {
      return;
    }
    lua_rawgeti(plugin->state, LUA_REGISTRYINDEX, plugin->shutdown_ref);
    PushPluginContext(plugin->state);
    if (lua_pcall(plugin->state, 1, 0, 0) != LUA_OK) {
      RecordError(FormatPluginPrefix(plugin) + " shutdown failed: " +
                  LuaErrorString(plugin->state));
      lua_pop(plugin->state, 1);
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
  impl_->TearDownPlugins();
#endif
  impl_->current_project_root.clear();
  impl_->SetReloadSummary();
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
