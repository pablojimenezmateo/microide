#include "plugin/PluginHost.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#if MICROIDE_HAS_LUA_PLUGINS
#include <lua.hpp>
#endif

namespace microide::plugin {

namespace {

std::filesystem::path GlobalPluginDirectory() {
  if (const char* xdg_config_home = std::getenv("XDG_CONFIG_HOME");
      xdg_config_home != nullptr && *xdg_config_home != '\0') {
    return std::filesystem::path(xdg_config_home) / "microide" / "plugins";
  }
  if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
    return std::filesystem::path(home) / ".config" / "microide" / "plugins";
  }
  return {};
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

  Callbacks callbacks{};
  std::filesystem::path current_project_root;
  std::vector<PluginInstance> plugins;
  std::unordered_map<std::string, PluginCommand> commands;
  std::vector<std::string> command_names;
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
      std::error_code error;
      if (!std::filesystem::exists(plugins_dir, error) || error ||
          !std::filesystem::is_directory(plugins_dir, error)) {
        return;
      }

      std::vector<std::filesystem::path> entries;
      for (const auto& entry : std::filesystem::directory_iterator(plugins_dir, error)) {
        if (error || !entry.is_directory()) {
          continue;
        }
        const std::filesystem::path init_path = entry.path() / "init.lua";
        if (std::filesystem::exists(init_path, error) && !error) {
          entries.push_back(entry.path().lexically_normal());
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

    std::filesystem::path path(raw_path);
    if (path.is_relative()) {
      if (host->current_project_root.empty()) {
        lua_pushboolean(state, 0);
        return 1;
      }
      path = host->current_project_root / path;
    }

    lua_pushboolean(state, host->callbacks.open_file(path.lexically_normal()) ? 1 : 0);
    return 1;
  }

  void PushPluginContext(lua_State* state) {
    lua_createtable(state, 0, 3);

    lua_pushlightuserdata(state, this);
    lua_pushcclosure(state, &LuaLog, 1);
    lua_setfield(state, -2, "log");

    lua_createtable(state, 0, 1);
    lua_pushlightuserdata(state, this);
    lua_pushcclosure(state, &LuaCommandsAdd, 1);
    lua_setfield(state, -2, "add");
    lua_setfield(state, -2, "commands");

    lua_createtable(state, 0, 2);
    lua_pushlightuserdata(state, this);
    lua_pushcclosure(state, &LuaWorkspaceProjectRoot, 1);
    lua_setfield(state, -2, "project_root");
    lua_pushlightuserdata(state, this);
    lua_pushcclosure(state, &LuaWorkspaceOpenFile, 1);
    lua_setfield(state, -2, "open_file");
    lua_setfield(state, -2, "workspace");
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

  void UnregisterCommandsForState(lua_State* state) {
    for (auto it = commands.begin(); it != commands.end();) {
      if (it->second.state != state) {
        ++it;
        continue;
      }
      luaL_unref(state, LUA_REGISTRYINDEX, it->second.function_ref);
      it = commands.erase(it);
    }
    RebuildCommandNames();
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
    for (auto& command : commands) {
      if (command.second.state != nullptr &&
          command.second.function_ref != LUA_NOREF &&
          command.second.function_ref != LUA_REFNIL) {
        luaL_unref(command.second.state, LUA_REGISTRYINDEX, command.second.function_ref);
      }
    }
    commands.clear();
    command_names.clear();
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
      UnregisterCommandsForState(plugin.state);
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
#endif

  if (error_message != nullptr) {
    error_message->clear();
  }
  return true;
}

const std::vector<std::string>& PluginHost::CommandNames() const {
  return impl_->command_names;
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
