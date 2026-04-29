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
#include "plugin/PluginAsyncStateInterop.h"
#include "plugin/PluginLuaInterop.h"
#include "plugin/PluginLuaContextInterop.h"
#include "plugin/PluginDiscoveryInterop.h"
#include "plugin/PluginDiagnosticsInterop.h"
#include "plugin/PluginLifecycleCallbackInterop.h"
#include "plugin/PluginLifecycleLoadInterop.h"
#include "plugin/PluginPathInterop.h"
#include "plugin/PluginProcessInterop.h"
#include "plugin/PluginLuaProviderRegistrationInterop.h"
#include "plugin/PluginProviderQueryInterop.h"
#include "plugin/PluginSidebarHoverInterop.h"
#include "plugin/PluginStateTeardownInterop.h"
#include "plugin/PluginWorkspaceInterop.h"
#include "plugin/PluginHostRuntimeTypes.h"
#include "plugin/LuaRuntime.h"
#include "util/TextFileIO.h"

#if MICROIDE_HAS_LUA_PLUGINS
#include <lua.hpp>
#endif

namespace microide::plugin {

namespace {

}  // namespace

struct PluginHost::Impl {
  using PluginInstance = runtime_types::PluginInstance;
  using PluginCommand = runtime_types::PluginCommand;
  using SidebarProvider = runtime_types::SidebarProvider;
  using HoverProvider = runtime_types::HoverProvider;
  using SaveParticipantRuntime = runtime_types::SaveParticipantRuntime;
  using CompletionRuntime = runtime_types::CompletionRuntime;
  using CodeActionRuntime = runtime_types::CodeActionRuntime;
  using TestProviderRuntime = runtime_types::TestProviderRuntime;
  using ScmProviderRuntime = runtime_types::ScmProviderRuntime;
  using AnnotationProviderRuntime = runtime_types::AnnotationProviderRuntime;
  using AuthProviderRuntime = runtime_types::AuthProviderRuntime;
  using McpToolRuntime = runtime_types::McpToolRuntime;

  Callbacks callbacks{};
  std::filesystem::path current_project_root;

  using AsyncProcessCallback = runtime_types::AsyncProcessCallback;
  using AsyncProcessRequest = runtime_types::AsyncProcessRequest;
  using AsyncProcessState = runtime_types::AsyncProcessState;
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

#endif

  std::vector<std::pair<std::filesystem::path, bool>> DiscoverPluginRoots() const {
    return discovery_interop::DiscoverPluginRoots(current_project_root);
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

  const PluginInstance* FindPluginByState(lua_State* state) const {
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

  static int LuaCommandsAdd(lua_State* state) {
    Impl* host = HostFromUpvalue(state);
    const char* name = luaL_checkstring(state, 1);
    luaL_checktype(state, 2, LUA_TFUNCTION);
    std::string error_message;
    if (host == nullptr || !lua_provider_registration_interop::RegisterCommand(
                               state, host->FindPluginByState(state), host->callbacks, name, 2,
                               &host->commands, &host->command_names, &error_message)) {
      return luaL_error(state, "%s",
                        error_message.empty() ? "failed to register plugin command"
                                              : error_message.c_str());
    }
    return 0;
  }

  static int LuaSidebarAdd(lua_State* state) {
    Impl* host = HostFromUpvalue(state);
    luaL_checktype(state, 1, LUA_TTABLE);
    std::string error_message;
    if (host == nullptr || !lua_provider_registration_interop::RegisterSidebar(
                               state, host->FindPluginByState(state), 1, &host->sidebars,
                               &host->sidebar_providers, &error_message)) {
      return luaL_error(state, "%s",
                        error_message.empty() ? "failed to register plugin sidebar"
                                              : error_message.c_str());
    }
    return 0;
  }

  static int LuaHoverAdd(lua_State* state) {
    Impl* host = HostFromUpvalue(state);
    luaL_checktype(state, 1, LUA_TTABLE);
    std::string error_message;
    if (host == nullptr || !lua_provider_registration_interop::RegisterHoverProvider(
                               state, host->FindPluginByState(state), 1, &host->hovers,
                               &host->hover_provider_order, &error_message)) {
      return luaL_error(state, "%s",
                        error_message.empty() ? "failed to register hover provider"
                                              : error_message.c_str());
    }
    return 0;
  }

  static int LuaMenusAdd(lua_State* state) {
    Impl* host = HostFromUpvalue(state);
    luaL_checktype(state, 1, LUA_TTABLE);
    const PluginInstance* plugin = host != nullptr ? host->FindPluginByState(state) : nullptr;
    std::string error_message;
    if (host == nullptr || !lua_provider_registration_interop::RegisterMenuEntry(
                               state, plugin, &host->menu_entries, &error_message)) {
      return luaL_error(state, "%s",
                        error_message.empty() ? "failed to register menu entry"
                                              : error_message.c_str());
    }
    return 0;
  }

  static int LuaKeybindingsAdd(lua_State* state) {
    Impl* host = HostFromUpvalue(state);
    luaL_checktype(state, 1, LUA_TTABLE);
    const PluginInstance* plugin = host != nullptr ? host->FindPluginByState(state) : nullptr;
    std::string error_message;
    if (host == nullptr || !lua_provider_registration_interop::RegisterKeybinding(
                               state, plugin, &host->keybindings, &error_message)) {
      return luaL_error(state, "%s",
                        error_message.empty() ? "failed to register keybinding"
                                              : error_message.c_str());
    }
    return 0;
  }

  static int LuaSettingsDeclare(lua_State* state) {
    Impl* host = HostFromUpvalue(state);
    luaL_checktype(state, 1, LUA_TTABLE);
    const PluginInstance* plugin = host != nullptr ? host->FindPluginByState(state) : nullptr;
    std::string error_message;
    if (host == nullptr || !lua_provider_registration_interop::RegisterSetting(
                               state, plugin, &host->settings, &error_message)) {
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

  static int LuaStatusAdd(lua_State* state) {
    Impl* host = HostFromUpvalue(state);
    luaL_checktype(state, 1, LUA_TTABLE);
    const PluginInstance* plugin = host != nullptr ? host->FindPluginByState(state) : nullptr;
    std::string error_message;
    if (host == nullptr || !lua_provider_registration_interop::RegisterStatusItem(
                               state, plugin, &host->status_items, &host->status_item_order,
                               &error_message)) {
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
    lua_provider_registration_interop::UpdateStatusItem(
        state, host->FindPluginByState(state), id, &host->status_items,
        &host->status_item_order, host->callbacks.request_status_redraw);
    return 0;
  }

  static int LuaFormattersAdd(lua_State* state) {
    Impl* host = HostFromUpvalue(state);
    luaL_checktype(state, 1, LUA_TTABLE);
    const PluginInstance* plugin = host->FindPluginByState(state);
    if (plugin == nullptr) {
      return luaL_error(state, "formatter registration requires an active plugin state");
    }

    std::string error_message;
    if (!lua_provider_registration_interop::RegisterFormatter(
            state, plugin->id, &host->formatters, &error_message)) {
      return luaL_error(state, "%s",
                        error_message.empty() ? "failed to parse formatter registration"
                                              : error_message.c_str());
    }
    return 0;
  }

  static int LuaSaveParticipantsAdd(lua_State* state) {
    Impl* host = HostFromUpvalue(state);
    const PluginInstance* plugin = host->FindPluginByState(state);
    if (plugin == nullptr) {
      return luaL_error(state, "save participant registration requires an active plugin state");
    }
    std::string error_message;
    if (!lua_provider_registration_interop::RegisterSaveParticipant(
            state, plugin->id, &host->save_participants, &host->save_participant_runtimes,
            &error_message)) {
      return luaL_error(state, "%s",
                        error_message.empty()
                            ? "failed to parse save participant registration"
                            : error_message.c_str());
    }
    return 0;
  }

  static int LuaCompletionAdd(lua_State* state) {
    Impl* host = HostFromUpvalue(state);
    luaL_checktype(state, 1, LUA_TTABLE);
    const PluginInstance* plugin = host->FindPluginByState(state);
    if (plugin == nullptr) {
      return luaL_error(state, "completion registration requires an active plugin state");
    }
    std::string error_message;
    if (!lua_provider_registration_interop::RegisterCompletion(
            state, plugin->id, &host->completions, &host->completion_runtimes,
            &error_message)) {
      return luaL_error(state, "%s",
                        error_message.empty() ? "failed to parse completion registration"
                                              : error_message.c_str());
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

    std::string error_message;
    if (!lua_provider_registration_interop::RegisterCodeAction(
            state, plugin->id, &host->code_actions, &host->code_action_runtimes,
            &error_message)) {
      return luaL_error(state, "%s",
                        error_message.empty() ? "failed to parse code action registration"
                                              : error_message.c_str());
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

    std::string error_message;
    if (!lua_provider_registration_interop::RegisterTask(state, plugin->id, &host->tasks,
                                                         &error_message)) {
      return luaL_error(state, "%s",
                        error_message.empty() ? "failed to parse task registration"
                                              : error_message.c_str());
    }
    return 0;
  }

  static int LuaLspAdd(lua_State* state) {
    Impl* host = HostFromUpvalue(state);
    luaL_checktype(state, 1, LUA_TTABLE);
    const PluginInstance* plugin = host->FindPluginByState(state);
    if (plugin == nullptr) {
      return luaL_error(state, "language server registration requires an active plugin state");
    }

    std::string error_message;
    if (!lua_provider_registration_interop::RegisterLanguageServer(
            state, plugin->id, &host->language_servers, &error_message)) {
      return luaL_error(state, "%s",
                        error_message.empty() ? "failed to parse language server registration"
                                              : error_message.c_str());
    }
    return 0;
  }

  static int LuaToolAdd(lua_State* state) {
    Impl* host = HostFromUpvalue(state);
    luaL_checktype(state, 1, LUA_TTABLE);
    const PluginInstance* plugin = host->FindPluginByState(state);
    if (plugin == nullptr) {
      return luaL_error(state, "tool registration requires an active plugin state");
    }

    std::string error_message;
    if (!lua_provider_registration_interop::RegisterTool(state, plugin->id, &host->tools,
                                                         &error_message)) {
      return luaL_error(state, "%s",
                        error_message.empty() ? "failed to parse tool registration"
                                              : error_message.c_str());
    }
    return 0;
  }

  static int LuaDebuggerAdd(lua_State* state) {
    Impl* host = HostFromUpvalue(state);
    luaL_checktype(state, 1, LUA_TTABLE);
    const PluginInstance* plugin = host->FindPluginByState(state);
    if (plugin == nullptr) {
      return luaL_error(state, "debugger registration requires an active plugin state");
    }

    std::string error_message;
    if (!lua_provider_registration_interop::RegisterDebugger(state, plugin->id, &host->debuggers,
                                                             &error_message)) {
      return luaL_error(state, "%s",
                        error_message.empty() ? "failed to parse debugger registration"
                                              : error_message.c_str());
    }
    return 0;
  }

  static int LuaTestProviderAdd(lua_State* state) {
    Impl* host = HostFromUpvalue(state);
    luaL_checktype(state, 1, LUA_TTABLE);
    const PluginInstance* plugin = host->FindPluginByState(state);
    if (plugin == nullptr) {
      return luaL_error(state, "test provider registration requires an active plugin state");
    }

    std::string error_message;
    if (!lua_provider_registration_interop::RegisterTestProvider(
            state, plugin->id, &host->test_providers, &host->test_provider_runtimes,
            &error_message)) {
      return luaL_error(state, "%s",
                        error_message.empty() ? "failed to parse test provider registration"
                                              : error_message.c_str());
    }
    return 0;
  }

  static int LuaScmProviderAdd(lua_State* state) {
    Impl* host = HostFromUpvalue(state);
    const PluginInstance* plugin = host->FindPluginByState(state);
    if (plugin == nullptr) {
      return luaL_error(state, "scm provider registration requires an active plugin state");
    }

    std::string error_message;
    if (!lua_provider_registration_interop::RegisterScmProvider(
            state, plugin->id, &host->scm_providers, &host->scm_provider_runtimes,
            &error_message)) {
      return luaL_error(state, "%s",
                        error_message.empty() ? "failed to parse scm provider registration"
                                              : error_message.c_str());
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

    std::string error_message;
    if (!lua_provider_registration_interop::RegisterAnnotationProvider(
            state, plugin->id, &host->annotation_providers, &host->annotation_provider_runtimes,
            &error_message)) {
      return luaL_error(state, "%s",
                        error_message.empty()
                            ? "failed to parse annotation provider registration"
                            : error_message.c_str());
    }
    return 0;
  }

  static int LuaAuthProviderAdd(lua_State* state) {
    Impl* host = HostFromUpvalue(state);
    const PluginInstance* plugin = host->FindPluginByState(state);
    if (plugin == nullptr) {
      return luaL_error(state, "auth provider registration requires an active plugin state");
    }

    std::string error_message;
    if (!lua_provider_registration_interop::RegisterAuthProvider(
            state, plugin->id, &host->auth_providers, &host->auth_provider_runtimes,
            &error_message)) {
      return luaL_error(state, "%s",
                        error_message.empty() ? "failed to parse auth provider registration"
                                              : error_message.c_str());
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

    std::string error_message;
    if (!lua_provider_registration_interop::RegisterAiProvider(
            state, plugin->id, &host->ai_providers, &error_message)) {
      return luaL_error(state, "%s",
                        error_message.empty() ? "failed to parse AI provider registration"
                                              : error_message.c_str());
    }
    return 0;
  }

  static int LuaExternalAgentAdd(lua_State* state) {
    Impl* host = HostFromUpvalue(state);
    luaL_checktype(state, 1, LUA_TTABLE);
    const PluginInstance* plugin = host->FindPluginByState(state);
    if (plugin == nullptr) {
      return luaL_error(state, "external agent registration requires an active plugin state");
    }

    std::string error_message;
    if (!lua_provider_registration_interop::RegisterExternalAgent(
            state, plugin->id, &host->external_agents, &error_message)) {
      return luaL_error(state, "%s",
                        error_message.empty()
                            ? "failed to parse external agent registration"
                            : error_message.c_str());
    }
    return 0;
  }

  static int LuaMcpToolAdd(lua_State* state) {
    Impl* host = HostFromUpvalue(state);
    luaL_checktype(state, 1, LUA_TTABLE);
    const PluginInstance* plugin = host->FindPluginByState(state);
    if (plugin == nullptr) {
      return luaL_error(state, "MCP tool registration requires an active plugin state");
    }

    std::string error_message;
    if (!lua_provider_registration_interop::RegisterMcpTool(
            state, plugin->id, &host->mcp_tools, &host->mcp_tool_runtimes, &error_message)) {
      return luaL_error(state, "%s",
                        error_message.empty() ? "failed to parse MCP tool registration"
                                              : error_message.c_str());
    }
    return 0;
  }

  static int LuaWorkspaceProjectRoot(lua_State* state) {
    Impl* host = HostFromUpvalue(state);
    return workspace_interop::LuaWorkspaceProjectRoot(
        state, host != nullptr ? host->current_project_root : std::filesystem::path{});
  }

  static int LuaWorkspaceOpenFile(lua_State* state) {
    Impl* host = HostFromUpvalue(state);
    return workspace_interop::LuaWorkspaceOpenFile(
        state, host != nullptr ? host->current_project_root : std::filesystem::path{},
        host != nullptr ? host->callbacks : PluginHost::Callbacks{});
  }

  static int LuaWorkspaceActiveBuffer(lua_State* state) {
    Impl* host = HostFromUpvalue(state);
    return workspace_interop::LuaWorkspaceActiveBuffer(
        state, host != nullptr ? host->current_project_root : std::filesystem::path{},
        host != nullptr ? host->callbacks : PluginHost::Callbacks{});
  }

  static int LuaFilesReadText(lua_State* state) {
    Impl* host = HostFromUpvalue(state);
    return workspace_interop::LuaFilesReadText(
        state, host != nullptr ? host->current_project_root : std::filesystem::path{});
  }

  static int LuaFilesWriteText(lua_State* state) {
    Impl* host = HostFromUpvalue(state);
    return workspace_interop::LuaFilesWriteText(
        state, host != nullptr ? host->current_project_root : std::filesystem::path{});
  }

  static int LuaFilesExists(lua_State* state) {
    Impl* host = HostFromUpvalue(state);
    return workspace_interop::LuaFilesExists(
        state, host != nullptr ? host->current_project_root : std::filesystem::path{});
  }

  static int LuaProcessRun(lua_State* state) {
    Impl* host = HostFromUpvalue(state);
    const std::filesystem::path current_project_root =
        host != nullptr ? host->current_project_root : std::filesystem::path{};
    return process_interop::LuaProcessRun(state, current_project_root);
  }

  static int LuaProcessRunAsync(lua_State* state) {
    Impl* host = HostFromUpvalue(state);
    const std::filesystem::path current_project_root =
        host != nullptr ? host->current_project_root : std::filesystem::path{};
    return process_interop::LuaProcessRunAsync(
        state, current_project_root, host != nullptr ? host->async_process_state : nullptr);
  }

  static int LuaDiagnosticsPublish(lua_State* state) {
    Impl* host = HostFromUpvalue(state);
    const char* raw_path = luaL_checkstring(state, 1);
    luaL_checktype(state, 2, LUA_TTABLE);
    const PluginInstance* plugin = host != nullptr ? host->FindPluginByState(state) : nullptr;
    std::string error_message;
    if (host == nullptr || plugin == nullptr ||
        !diagnostics_interop::PublishDiagnostics(state, plugin->id, host->current_project_root,
                                                 raw_path, 2, host->callbacks,
                                                 &error_message)) {
      return luaL_error(state, "%s",
                        error_message.empty() ? "failed to publish diagnostics"
                                              : error_message.c_str());
    }
    return 0;
  }

  static int LuaDiagnosticsClear(lua_State* state) {
    Impl* host = HostFromUpvalue(state);
    const PluginInstance* plugin = host != nullptr ? host->FindPluginByState(state) : nullptr;
    std::optional<std::filesystem::path> path;
    if (lua_gettop(state) >= 1 && !lua_isnil(state, 1)) {
      path = path_interop::ResolveRuntimePath(host != nullptr ? host->current_project_root : std::filesystem::path{},
                                std::filesystem::path(luaL_checkstring(state, 1)));
    }

    std::string error_message;
    if (host == nullptr || plugin == nullptr ||
        !diagnostics_interop::ClearDiagnostics(plugin->id, path, host->callbacks, &error_message)) {
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
    lua_context_interop::PushPluginContext(
        state, this,
        lua_context_interop::ApiFns{
            .log = &LuaLog,
            .commands_add = &LuaCommandsAdd,
            .workspace_project_root = &LuaWorkspaceProjectRoot,
            .workspace_open_file = &LuaWorkspaceOpenFile,
            .workspace_active_buffer = &LuaWorkspaceActiveBuffer,
            .files_read_text = &LuaFilesReadText,
            .files_write_text = &LuaFilesWriteText,
            .files_exists = &LuaFilesExists,
            .process_run = &LuaProcessRun,
            .process_run_async = &LuaProcessRunAsync,
            .diagnostics_publish = &LuaDiagnosticsPublish,
            .diagnostics_clear = &LuaDiagnosticsClear,
            .sidebar_add = &LuaSidebarAdd,
            .sidebar_show = &LuaSidebarShow,
            .hover_add = &LuaHoverAdd,
            .menus_add = &LuaMenusAdd,
            .keybindings_add = &LuaKeybindingsAdd,
            .settings_declare = &LuaSettingsDeclare,
            .settings_get = &LuaSettingsGet,
            .status_add = &LuaStatusAdd,
            .status_update = &LuaStatusUpdate,
            .formatters_add = &LuaFormattersAdd,
            .save_participants_add = &LuaSaveParticipantsAdd,
            .completion_add = &LuaCompletionAdd,
            .code_action_add = &LuaCodeActionAdd,
            .lsp_add = &LuaLspAdd,
            .task_add = &LuaTaskAdd,
            .tool_add = &LuaToolAdd,
            .debugger_add = &LuaDebuggerAdd,
            .test_provider_add = &LuaTestProviderAdd,
            .scm_provider_add = &LuaScmProviderAdd,
            .annotation_provider_add = &LuaAnnotationProviderAdd,
            .auth_provider_add = &LuaAuthProviderAdd,
            .ai_provider_add = &LuaAiProviderAdd,
            .external_agent_add = &LuaExternalAgentAdd,
            .mcp_tool_add = &LuaMcpToolAdd,
        });
  }

  void PushProjectTable(lua_State* state, const std::filesystem::path& project_root) {
    lua_createtable(state, 0, 2);
    lua_pushstring(state, project_root.generic_string().c_str());
    lua_setfield(state, -2, "root");
    lua_pushstring(state, path_interop::Basename(project_root).c_str());
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

  bool SnapshotSidebarProvider(const SidebarProvider& provider,
                               std::vector<SidebarItem>* items,
                               std::string* error_message) {
    return sidebar_hover_interop::SnapshotSidebarProvider(
        provider, current_project_root,
        [](const std::filesystem::path& project_root, const std::filesystem::path& runtime_path) {
          return path_interop::ResolveRuntimePath(project_root, runtime_path);
        },
        [this](lua_State* state) { return FindPluginByState(state); }, items, error_message);
  }

  bool ConfirmSidebarProviderItem(const SidebarProvider& provider,
                                  const SidebarItem& item,
                                  std::string* error_message) {
    return sidebar_hover_interop::ConfirmSidebarProviderItem(
        provider, item, current_project_root,
        [](const std::filesystem::path& project_root, const std::filesystem::path& runtime_path) {
          return path_interop::ResolveRuntimePath(project_root, runtime_path);
        },
        [this](lua_State* state) { return FindPluginByState(state); }, callbacks.open_file,
        error_message);
  }

  bool QueryHoverProvider(const HoverProvider& provider,
                          const std::filesystem::path& path,
                          std::size_t line,
                          std::size_t column,
                          PluginHost::HoverResult* result,
                          std::string* error_message) {
    return sidebar_hover_interop::QueryHoverProvider(
        provider, path, line, column,
        [this](lua_State* state, const std::filesystem::path& buffer_path) {
          PushBufferTable(state, buffer_path);
        },
        [this](lua_State* state) { return FindPluginByState(state); }, result, error_message);
  }

  void ClearPluginDiagnostics(const PluginInstance* plugin) {
    state_teardown_interop::ClearPluginDiagnostics(
        plugin, [this](std::string_view owner) {
          if (callbacks.clear_owner_diagnostics) {
            callbacks.clear_owner_diagnostics(owner);
          }
        });
  }

  void DestroyPluginState(PluginInstance* plugin) {
    state_teardown_interop::DestroyPluginState(plugin);
  }

  void UnregisterContributionsForState(lua_State* state) {
    state_teardown_interop::UnregisterContributionsForState(
        state, FindPluginByState(state), &commands, &command_names, &sidebars, &sidebar_providers,
        &hovers, &hover_provider_order, &menu_entries, &keybindings, &settings, &status_items,
        &status_item_order, &formatters, &save_participants, &save_participant_runtimes,
        &completions, &completion_runtimes, &code_actions, &code_action_runtimes,
        &language_servers, &tasks, &tools, &debuggers, &test_providers, &test_provider_runtimes,
        &scm_providers, &scm_provider_runtimes, &annotation_providers,
        &annotation_provider_runtimes, &auth_providers, &auth_provider_runtimes, &ai_providers,
        &external_agents, &mcp_tools, &mcp_tool_runtimes);
  }

  bool InitializeState(PluginInstance* plugin, std::string* error_message) {
    return lifecycle_load_interop::InitializeState(plugin, &LuaOpenMicroide, error_message);
  }

  bool LoadPluginDescriptor(PluginInstance* plugin, std::string* error_message) {
    return lifecycle_load_interop::LoadPluginDescriptor(plugin, error_message);
  }

  bool CallSetup(PluginInstance* plugin, std::string* error_message) {
    return lifecycle_callback_interop::CallSetup(
        plugin, &active_plugin,
        [this](lua_State* state) { PushPluginContext(state); },
        [this](const PluginInstance* plugin_instance) {
          return FormatPluginPrefix(plugin_instance);
        },
        error_message);
  }

  void CallProjectCallback(PluginInstance* plugin, int ref, const char* callback_name) {
    lifecycle_callback_interop::CallProjectCallback(
        plugin, ref, callback_name, !current_project_root.empty(),
        [this](lua_State* state) { PushPluginContext(state); },
        [this](lua_State* state) { PushProjectTable(state, current_project_root); },
        [this](std::string error) { RecordError(std::move(error)); },
        [this](const PluginInstance* plugin_instance) {
          return FormatPluginPrefix(plugin_instance);
        });
  }

  void CallBufferCallback(PluginInstance* plugin,
                          int ref,
                          const char* callback_name,
                          const std::filesystem::path& path) {
    lifecycle_callback_interop::CallBufferCallback(
        plugin, ref, callback_name, path,
        [this](lua_State* state) { PushPluginContext(state); },
        [this](lua_State* state, const std::filesystem::path& buffer_path) {
          PushBufferTable(state, buffer_path);
        },
        [this](std::string error) { RecordError(std::move(error)); },
        [this](const PluginInstance* plugin_instance) {
          return FormatPluginPrefix(plugin_instance);
        });
  }

  void CallShutdown(PluginInstance* plugin) {
    lifecycle_callback_interop::CallShutdown(
        plugin, [this](lua_State* state) { PushPluginContext(state); },
        [this](std::string error) { RecordError(std::move(error)); },
        [this](const PluginInstance* plugin_instance) {
          return FormatPluginPrefix(plugin_instance);
        });
  }

  void TearDownPlugins() {
    lifecycle_callback_interop::TearDownPlugins(
        &plugins, !current_project_root.empty(),
        [this](PluginInstance* plugin, int ref, const char* callback_name) {
          CallProjectCallback(plugin, ref, callback_name);
        },
        [this](PluginInstance* plugin) { CallShutdown(plugin); },
        [this](lua_State* state) { UnregisterContributionsForState(state); },
        [this](const PluginInstance* plugin) { ClearPluginDiagnostics(plugin); },
        [this](PluginInstance* plugin) { DestroyPluginState(plugin); });
  }

  bool LoadPluginRoot(const std::filesystem::path& plugin_root,
                      bool project_local,
                      std::string* error_message) {
    return lifecycle_callback_interop::LoadPluginRoot(
        plugin_root, project_local, &plugins,
        [this](PluginInstance* plugin, std::string* init_error) {
          return InitializeState(plugin, init_error);
        },
        [this](PluginInstance* plugin, std::string* descriptor_error) {
          return LoadPluginDescriptor(plugin, descriptor_error);
        },
        [this](PluginInstance* plugin, std::string* setup_error) {
          return CallSetup(plugin, setup_error);
        },
        [this](lua_State* state) { UnregisterContributionsForState(state); },
        [this](const PluginInstance* plugin) { ClearPluginDiagnostics(plugin); },
        [this](PluginInstance* plugin) { DestroyPluginState(plugin); }, error_message);
  }
#endif

  void ClearMessages() { messages.clear(); }

#if MICROIDE_HAS_LUA_PLUGINS
  void CancelAsyncProcessCallbacks() {
    if (async_process_state) {
      async_state_interop::CancelCallbacks(*async_process_state);
    }
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
  async_state_interop::SetEventType(*impl_->async_process_state, type);
}

int PluginHost::ConsumeAsyncProcessCallbacks() {
#if MICROIDE_HAS_LUA_PLUGINS
  std::vector<Impl::AsyncProcessCallback> callbacks =
      async_state_interop::TakePendingCallbacks(*impl_->async_process_state);
  for (auto& cb : callbacks) {
    lua_State* state = cb.lua_state;
    const Impl::PluginInstance* plugin = impl_->FindPluginByState(state);
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
    std::string call_error;
    if (plugin == nullptr || !plugin->runtime || !plugin->runtime->PCall(1, 0, &call_error)) {
      if (impl_->callbacks.error_sink && !call_error.empty()) {
        impl_->callbacks.error_sink(std::string("plugin async callback: ") + call_error);
      }
    }
  }
  return static_cast<int>(callbacks.size());
#endif
  return 0;
}

int PluginHost::PendingAsyncProcessCount() const {
  return async_state_interop::PendingCount(*impl_->async_process_state);
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
  if (!impl_->enabled()) {
    if (error_message != nullptr) {
      *error_message = "Lua plugin runtime unavailable";
    }
    return false;
  }

#if MICROIDE_HAS_LUA_PLUGINS
  return provider_query_interop::ExecuteCommand(
      name, args, impl_->commands,
      [this](lua_State* state) { return impl_->FindPluginByState(state); },
      [this](lua_State* state) { impl_->PushPluginContext(state); }, error_message);
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
      path_interop::ResolveRuntimePath(impl_->current_project_root, path).lexically_normal();
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
  return provider_query_interop::RunSaveParticipants(
      path, text, impl_->save_participant_runtimes,
      [this](lua_State* state) { return impl_->FindPluginByState(state); },
      [this](lua_State* state, const std::filesystem::path& buffer_path, std::string_view value) {
        impl_->PushBufferContext(state, buffer_path, value);
      },
      error_message);
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
  results = provider_query_interop::QueryCompletions(
      language_id, path, line, column, trigger_character, impl_->completion_runtimes,
      [this](lua_State* state) { return impl_->FindPluginByState(state); },
      [this](lua_State* state, const std::filesystem::path& buffer_path) {
        impl_->PushBufferContext(state, buffer_path);
      },
      error_message);
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
  results = provider_query_interop::QueryCodeActions(
      language_id, path, start_line, start_column, end_line, end_column,
      impl_->code_action_runtimes,
      [this](lua_State* state) { return impl_->FindPluginByState(state); },
      [this](lua_State* state, const std::filesystem::path& buffer_path) {
        impl_->PushBufferContext(state, buffer_path);
      },
      error_message);
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
  return provider_query_interop::DiscoverTests(
      provider_id, path, impl_->current_project_root, impl_->test_provider_runtimes,
      [this](lua_State* state) { return impl_->FindPluginByState(state); },
      [this](lua_State* state, const std::filesystem::path& buffer_path) {
        impl_->PushBufferContext(state, buffer_path);
      },
      [](const std::filesystem::path& project_root, const std::filesystem::path& runtime_path) {
        return path_interop::ResolveRuntimePath(project_root, runtime_path);
      },
      tests, error_message);
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
  return provider_query_interop::RunTests(
      provider_id, test_ids, impl_->test_provider_runtimes,
      [this](lua_State* state) { return impl_->FindPluginByState(state); }, results,
      error_message);
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
  return provider_query_interop::SnapshotScm(
      provider_id, impl_->current_project_root, impl_->scm_provider_runtimes,
      [this](lua_State* state) { return impl_->FindPluginByState(state); },
      [](const std::filesystem::path& project_root, const std::filesystem::path& runtime_path) {
        return path_interop::ResolveRuntimePath(project_root, runtime_path);
      },
      snapshot, error_message);
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
  lines = provider_query_interop::QueryAnnotations(
      provider_id, path, language_id, visible_start_line, visible_end_line,
      impl_->annotation_provider_runtimes,
      [this](lua_State* state) { return impl_->FindPluginByState(state); },
      [this](lua_State* state, const std::filesystem::path& buffer_path) {
        impl_->PushBufferContext(state, buffer_path);
      },
      error_message);
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
  return provider_query_interop::LoginAuthProvider(
      provider_id, scopes, impl_->auth_provider_runtimes,
      [this](lua_State* state) { return impl_->FindPluginByState(state); }, session,
      error_message);
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
  return provider_query_interop::RefreshAuthSession(
      provider_id, session_id, impl_->auth_provider_runtimes,
      [this](lua_State* state) { return impl_->FindPluginByState(state); }, session,
      error_message);
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
  return provider_query_interop::LogoutAuthSession(
      provider_id, session_id, impl_->auth_provider_runtimes,
      [this](lua_State* state) { return impl_->FindPluginByState(state); }, error_message);
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
  return provider_query_interop::InvokeMcpTool(
      tool_id, input_json, impl_->mcp_tool_runtimes,
      [this](lua_State* state) { return impl_->FindPluginByState(state); }, output_json,
      error_message);
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
