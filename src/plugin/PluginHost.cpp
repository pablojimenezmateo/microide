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
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <SDL3/SDL.h>

#include "platform/AppDirectories.h"
#include "platform/Filesystem.h"
#include "platform/Subprocess.h"
#include "plugin/PluginAsyncStateInterop.h"
#include "plugin/PluginAsyncCallbackInterop.h"
#include "plugin/PluginContributionInterop.h"
#include "plugin/PluginDataDirectoryInterop.h"
#include "plugin/PluginBufferLifecycleInterop.h"
#include "plugin/PluginHoverQueryInterop.h"
#include "plugin/PluginLuaBufferProjectInterop.h"
#include "plugin/PluginLuaInterop.h"
#include "plugin/PluginLuaContextInterop.h"
#include "plugin/PluginDiscoveryInterop.h"
#include "plugin/PluginLifecycleCallbackInterop.h"
#include "plugin/PluginLifecycleLoadInterop.h"
#include "plugin/PluginLifecycleResetInterop.h"
#include "plugin/PluginPathInterop.h"
#include "plugin/PluginProcessInterop.h"
#include "plugin/PluginProviderQueryInterop.h"
#include "plugin/PluginRegistrationParsers.h"
#include "plugin/PluginRegistryInterop.h"
#include "plugin/PluginProjectLifecycleInterop.h"
#include "plugin/PluginRuntimeApiInterop.h"
#include "plugin/PluginSidebarHoverInterop.h"
#include "plugin/PluginStatusInterop.h"
#include "plugin/PluginStateTeardownInterop.h"
#include "plugin/PluginWorkspaceInterop.h"
#include "plugin/PluginHostRuntimeTypes.h"
#include "plugin/LuaError.h"
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

  Callbacks callbacks{};
  std::filesystem::path current_project_root;

  using AsyncProcessCallback = runtime_types::AsyncProcessCallback;
  using AsyncProcessRequest = runtime_types::AsyncProcessRequest;
  using AsyncProcessState = runtime_types::AsyncProcessState;
  std::shared_ptr<AsyncProcessState> async_process_state = std::make_shared<AsyncProcessState>();
  std::vector<PluginInstance> plugins;
  // Plugin ids the user has disabled: their setup is skipped on Reload. disabled_plugin_meta
  // records {id, root} for the ones actually skipped this reload so the UI can list them.
  std::vector<std::string> disabled_plugins;
  std::vector<PluginHost::LoadedPlugin> disabled_plugin_meta;
  std::unordered_map<std::string, PluginCommand> commands;
  std::vector<std::string> command_names;
  std::unordered_map<std::string, SidebarProvider> sidebars;
  std::vector<SidebarProviderInfo> sidebar_providers;
  // Ordered views are sorted projections of the maps above. Registration only
  // flips these dirty bits (O(1)); the const accessors rebuild lazily on the next
  // read, so a plugin registering N commands sorts once instead of N times.
  bool command_names_dirty = false;
  bool sidebar_providers_dirty = false;
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
  std::vector<PluginHost::ContributedTestProvider> test_providers;
  std::vector<TestProviderRuntime> test_provider_runtimes;
  std::vector<PluginHost::ContributedScmProvider> scm_providers;
  std::vector<ScmProviderRuntime> scm_provider_runtimes;
  std::vector<PluginHost::ContributedAnnotationProvider> annotation_providers;
  std::vector<AnnotationProviderRuntime> annotation_provider_runtimes;
  std::vector<PluginHost::ContributedAuthProvider> auth_providers;
  std::vector<AuthProviderRuntime> auth_provider_runtimes;
  std::vector<PluginHost::ContributedBracketSet> bracket_sets;
  std::vector<PluginHost::ContributedCommentMarkers> comment_markers;
  std::vector<PluginHost::ContributedIndentRules> indent_rules;
  std::vector<PluginHost::ContributedSnippet> snippets;
  std::vector<std::string> messages;
  std::vector<std::string> errors;
  std::string reload_summary = "Lua plugin runtime unavailable";
  bool startup_plugins_enabled = true;
#if MICROIDE_HAS_LUA_PLUGINS
  PluginInstance* active_plugin = nullptr;
#endif

  [[nodiscard]] bool enabled() const {
#if MICROIDE_HAS_LUA_PLUGINS
    return startup_plugins_enabled;
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
    // Read the maps, not the ordered views: the latter rebuild lazily and may be
    // dirty here, but their element counts always match the source maps.
    reload_summary += " and " + std::to_string(commands.size()) + " command";
    if (commands.size() != 1) {
      reload_summary += "s";
    }
    reload_summary += " and " + std::to_string(sidebars.size()) + " sidebar";
    if (sidebars.size() != 1) {
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
    const std::filesystem::path relative =
        path.lexically_normal().lexically_relative(current_project_root.lexically_normal());
    if (relative.empty()) {
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
    const std::string path_string = normalized_path.generic_string();
    lua_pushlstring(state, path_string.c_str(), path_string.size());
    lua_setfield(state, -2, "path");

    const std::string relative_path = RelativePathString(normalized_path).value_or(
        normalized_path.filename().empty() ? normalized_path.generic_string()
                                           : normalized_path.filename().string());
    lua_pushlstring(state, relative_path.c_str(), relative_path.size());
    lua_setfield(state, -2, "relative_path");

    if (text.has_value()) {
      lua_pushlstring(state, text->data(), text->size());
      lua_setfield(state, -2, "text");
    }
  }

#endif

  std::vector<std::filesystem::path> DiscoverPluginRoots() const {
    return discovery_interop::DiscoverPluginRoots();
  }

#include "plugin/PluginHostLuaApi.inc"

  void ClearMessages() { messages.clear(); }

#if MICROIDE_HAS_LUA_PLUGINS
  void CancelAsyncProcessCallbacks() {
    if (async_process_state) {
      async_state_interop::CancelCallbacks(*async_process_state);
    }
  }

  void DrainAsyncProcessWorkers() {
    if (!async_process_state) {
      return;
    }
    const bool drained = async_state_interop::DrainAndJoinWorkers(
        *async_process_state, runtime_types::kPluginHostDrainDeadline);
    if (!drained) {
      SDL_Log(
          "PluginHost: async worker drain exceeded %lld ms deadline; proceeding "
          "with teardown (cancelled callbacks remain inert)",
          static_cast<long long>(runtime_types::kPluginHostDrainDeadline.count()));
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

#include "plugin/PluginHostPublicApi.inc"
