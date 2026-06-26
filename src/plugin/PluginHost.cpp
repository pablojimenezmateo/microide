#include "plugin/PluginHost.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
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
#include "plugin/PluginLanguageProviderQueryInterop.h"
#include "plugin/PluginPathInterop.h"
#include "plugin/PluginPresentationRegistrationParsers.h"
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
#include "plugin/PluginThread.h"
#include "plugin/PluginThreadTypes.h"
#include "plugin/LuaError.h"
#include "plugin/LuaRuntime.h"
#include "util/TextFileIO.h"

#if MICROIDE_HAS_LUA_PLUGINS
#include <lua.hpp>
#endif

namespace microide::plugin {

namespace {

// Per-plugin-call execution context. This is THREAD-LOCAL, not shared host state:
// re-entrancy ("am I already inside a plugin call on THIS thread") and the active
// snapshot / direct-access flags are inherently per-thread. A shared member would
// race — the worker writes the flags while the main thread reads them to decide
// inline-vs-post, and a stale read of "executing" set by the busy worker would make
// the main thread wrongly run Lua inline. Keeping it thread-local makes the worker's
// context invisible to the main thread and vice versa.
struct PluginExecContext {
  const PluginHostSnapshot* snapshot = nullptr;
  bool direct = true;
  bool executing = false;
};
thread_local PluginExecContext g_exec;

}  // namespace

struct PluginHost::Impl {
  using PluginInstance = runtime_types::PluginInstance;
  using PluginCommand = runtime_types::PluginCommand;
  using SidebarProvider = runtime_types::SidebarProvider;
  using HoverProvider = runtime_types::HoverProvider;
  using SaveParticipantRuntime = runtime_types::SaveParticipantRuntime;
  using CompletionRuntime = runtime_types::CompletionRuntime;
  using CodeActionRuntime = runtime_types::CodeActionRuntime;
  using LanguageQueryRuntime = runtime_types::LanguageQueryRuntime;
  using TestProviderRuntime = runtime_types::TestProviderRuntime;
  using ScmProviderRuntime = runtime_types::ScmProviderRuntime;
  using AnnotationProviderRuntime = runtime_types::AnnotationProviderRuntime;
  using AuthProviderRuntime = runtime_types::AuthProviderRuntime;

  // `callbacks` is the THREAD-ROUTED view the Lua verbs see: read verbs resolve
  // from the active per-call snapshot, write verbs either run directly (round-trip
  // ops, where the UI thread is parked and the worker has exclusive shell access)
  // or post a main-thread action to the worker mailbox (fire-and-forget events).
  // `raw_callbacks` is the unwrapped shell binding the routed view dispatches to.
  Callbacks raw_callbacks{};
  Callbacks callbacks{};
  std::filesystem::path current_project_root;

  // The dedicated worker that runs plugin Lua. Set once at wiring time and read
  // from both threads as a stable pointer; the per-call execution context lives in
  // the thread-local g_exec, not here.
  PluginThread* worker_ = nullptr;

  // Restores the per-thread execution context on scope exit so nested plugin calls
  // and any early return leave the thread-local flags as they were.
  struct ContextGuard {
    PluginExecContext prev;
    ~ContextGuard() { g_exec = prev; }
  };

  template <typename F>
  void ExecuteWithContext(const PluginHostSnapshot* snapshot, bool direct, F&& fn) {
    ContextGuard guard{g_exec};
    g_exec.snapshot = snapshot;
    g_exec.direct = direct;
    g_exec.executing = true;
    fn();
  }

  // Apply a host-side mutation: run it now when the worker holds exclusive shell
  // access (round-trip with the UI thread parked, or the un-wired inline path),
  // otherwise marshal it to the UI thread via the mailbox.
  void ApplyHostMutation(std::function<void()> fn) {
    if (g_exec.direct || worker_ == nullptr) {
      fn();
    } else {
      worker_->PostToMain(std::move(fn));
    }
  }

  std::optional<PluginHost::ActiveBuffer> ResolveActiveBuffer() const {
    if (g_exec.snapshot != nullptr) {
      if (!g_exec.snapshot->active_buffer.present) {
        return std::nullopt;
      }
      return PluginHost::ActiveBuffer{
          .path = g_exec.snapshot->active_buffer.path,
          .line = g_exec.snapshot->active_buffer.line,
          .column = g_exec.snapshot->active_buffer.column,
      };
    }
    return raw_callbacks.active_buffer ? raw_callbacks.active_buffer() : std::nullopt;
  }

  std::optional<std::string> ResolveSetting(std::string_view id) const {
    if (g_exec.snapshot != nullptr) {
      for (const auto& [key, value] : g_exec.snapshot->settings) {
        if (key == id) {
          return value;
        }
      }
      return std::nullopt;
    }
    return raw_callbacks.get_setting ? raw_callbacks.get_setting(id) : std::nullopt;
  }

  // Build the thread-routed Callbacks view over `raw_callbacks`. Read verbs resolve
  // from the snapshot; write/request verbs go through ApplyHostMutation. Closures
  // capture only owning copies (never a lua_State / Lua ref) so a deferred action
  // is safe to run later on the UI thread.
  Callbacks BuildRoutedCallbacks();

  // Capture the immutable host view a plugin call may read. Runs on the UI thread.
  PluginHostSnapshot CaptureSnapshot() const;

  // Dispatch a plugin call onto the worker. Detached = fire-and-forget (events);
  // Blocking = bounded synchronous round-trip preserving the synchronous API.
  void RunOnWorkerDetached(PluginHostSnapshot snapshot, std::function<void()> fn);
  template <typename F>
  void RunOnWorkerBlocking(const PluginHostSnapshot& snapshot, F&& fn) {
    if (g_exec.executing || worker_ == nullptr) {
      // Already on the worker (re-entrant) or no worker wired: run inline with
      // exclusive access rather than dead-locking on a self-post.
      ExecuteWithContext(&snapshot, /*direct=*/true, std::forward<F>(fn));
      return;
    }
    worker_->EnsureStarted();
    std::promise<void> done;
    std::future<void> finished = done.get_future();
    // Captures by reference are safe: the UI thread blocks on `finished` until the
    // job completes, so every referent outlives the call.
    worker_->Post([this, &snapshot, &fn, &done]() {
      ExecuteWithContext(&snapshot, /*direct=*/true, fn);
      done.set_value();
    });
    finished.wait();
  }

  // Dispatch a result-returning call onto the worker without blocking the UI.
  // `produce` runs on the worker (reads resolve from the captured snapshot) and
  // returns the POD result; `deliver` runs on the UI thread during the mailbox
  // drain. A non-empty `dedup_key` collapses superseded in-flight requests of the
  // same kind (a stale cursor-driven query is dropped before it runs); an empty
  // key means every call must run (e.g. command execution), so it is plain-posted.
  // With no worker wired (tests / pre-wire) or when already on the worker, runs
  // inline and delivers synchronously.
  template <typename Result>
  void RunQueryAsync(std::string dedup_key,
                     std::function<Result()> produce,
                     std::function<void(Result)> deliver) {
    PluginHostSnapshot snapshot = CaptureSnapshot();
    if (worker_ == nullptr || g_exec.executing) {
      Result result;
      ExecuteWithContext(&snapshot, /*direct=*/true, [&]() { result = produce(); });
      deliver(std::move(result));
      return;
    }
    worker_->EnsureStarted();
    auto task = [this, snapshot = std::move(snapshot), produce = std::move(produce),
                 deliver = std::move(deliver)]() mutable {
      auto result = std::make_shared<Result>();
      ExecuteWithContext(&snapshot, /*direct=*/false, [&]() { *result = produce(); });
      worker_->PostToMain([result, deliver = std::move(deliver)]() mutable {
        deliver(std::move(*result));
      });
    };
    if (dedup_key.empty()) {
      worker_->Post(std::move(task));
    } else {
      worker_->PostLatest(std::move(dedup_key), std::move(task));
    }
  }

  // Immutable, UI-thread-owned view of every setup-time contribution the UI reads
  // (everything that changes only on Reload; the runtime-mutating status items,
  // messages, and errors are not here). The worker builds it from the live
  // registries at the end of a reload; the UI accessors read it. This decouples UI
  // reads from the worker's registry mutations so Reload can stop blocking the UI
  // (the worker rebuilds the live registries and republishes this view without the
  // UI ever touching live mutable plugin state).
  struct ContributionSnapshot {
    std::vector<std::string> command_names;
    std::vector<SidebarProviderInfo> sidebar_providers;
    std::vector<PluginHost::ContributedMenuEntry> menu_entries;
    std::vector<PluginHost::ContributedKeybinding> keybindings;
    std::vector<PluginHost::ContributedSettingSpec> settings;
    std::vector<PluginHost::ContributedFormatter> formatters;
    std::vector<PluginHost::ContributedSaveParticipant> save_participants;
    std::vector<PluginHost::ContributedCompletion> completions;
    std::vector<PluginHost::ContributedCodeAction> code_actions;
    std::vector<PluginHost::ContributedLanguageServer> language_servers;
    std::vector<PluginHost::ContributedDebugAdapter> debug_adapters;
    std::vector<PluginHost::ContributedLaunchConfig> launch_configs;
    std::vector<PluginHost::ContributedTask> tasks;
    std::vector<PluginHost::ContributedTool> tools;
    std::vector<PluginHost::ContributedTestProvider> test_providers;
    std::vector<PluginHost::ContributedScmProvider> scm_providers;
    std::vector<PluginHost::ContributedAnnotationProvider> annotation_providers;
    std::vector<PluginHost::ContributedAuthProvider> auth_providers;
    std::vector<PluginHost::ContributedBracketSet> bracket_sets;
    std::vector<PluginHost::ContributedCommentMarkers> comment_markers;
    std::vector<PluginHost::ContributedIndentRules> indent_rules;
    std::vector<PluginHost::ContributedSnippet> snippets;
    std::vector<PluginHost::ContributedTheme> themes;
    std::vector<PluginHost::ContributedFileIconTheme> file_icon_themes;
    std::vector<PluginHost::LoadedPlugin> loaded_plugins;
    std::string reload_summary = "Lua plugin runtime unavailable";
  };
  ContributionSnapshot published_;

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
  // Monotonic stamp bumped on every status-item mutation (add/update/teardown).
  // The render/hit-test/hover paths resolve the sorted view once per change
  // instead of rebuilding it every frame.
  std::uint64_t status_items_revision = 0;
  std::vector<PluginHost::ContributedFormatter> formatters;
  std::vector<PluginHost::ContributedSaveParticipant> save_participants;
  std::vector<SaveParticipantRuntime> save_participant_runtimes;
  std::vector<PluginHost::ContributedCompletion> completions;
  std::vector<CompletionRuntime> completion_runtimes;
  std::vector<PluginHost::ContributedCodeAction> code_actions;
  std::vector<CodeActionRuntime> code_action_runtimes;
  std::vector<LanguageQueryRuntime> language_query_runtimes;
  std::vector<PluginHost::ContributedLanguageServer> language_servers;
  std::vector<PluginHost::ContributedDebugAdapter> debug_adapters;
  std::vector<PluginHost::ContributedLaunchConfig> launch_configs;
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
  std::vector<PluginHost::ContributedTheme> themes;
  std::vector<PluginHost::ContributedFileIconTheme> file_icon_themes;
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

  // Compute the {id, root, enabled} list the UI shows for loaded + disabled plugins.
  std::vector<PluginHost::LoadedPlugin> ComputeLoadedPlugins() const {
    std::vector<PluginHost::LoadedPlugin> result;
    result.reserve(plugins.size() + disabled_plugin_meta.size());
    for (const auto& plugin : plugins) {
      result.push_back(PluginHost::LoadedPlugin{.id = plugin.id, .root = plugin.root, .enabled = true});
    }
    for (const auto& meta : disabled_plugin_meta) {
      result.push_back(meta);
    }
    std::sort(result.begin(), result.end(),
              [](const PluginHost::LoadedPlugin& a, const PluginHost::LoadedPlugin& b) {
                return a.id < b.id;
              });
    return result;
  }

  // Build the immutable UI view from the live setup-time registries, resolving the
  // lazy ordered views first. Called when a reload/shutdown settles; the result is
  // assigned to `published_` (in 4a inline while the UI is parked on the round-trip;
  // a later phase posts it to the UI thread so Reload need not block).
  ContributionSnapshot BuildContributionSnapshot() {
#if MICROIDE_HAS_LUA_PLUGINS
    if (command_names_dirty) {
      registry_interop::RebuildCommandNames(commands, &command_names);
      command_names_dirty = false;
    }
    if (sidebar_providers_dirty) {
      registry_interop::RebuildSidebarProviders(sidebars, &sidebar_providers);
      sidebar_providers_dirty = false;
    }
#endif
    ContributionSnapshot snapshot;
    snapshot.command_names = command_names;
    snapshot.sidebar_providers = sidebar_providers;
    snapshot.menu_entries = menu_entries;
    snapshot.keybindings = keybindings;
    snapshot.settings = settings;
    snapshot.formatters = formatters;
    snapshot.save_participants = save_participants;
    snapshot.completions = completions;
    snapshot.code_actions = code_actions;
    snapshot.language_servers = language_servers;
    snapshot.debug_adapters = debug_adapters;
    snapshot.launch_configs = launch_configs;
    snapshot.tasks = tasks;
    snapshot.tools = tools;
    snapshot.test_providers = test_providers;
    snapshot.scm_providers = scm_providers;
    snapshot.annotation_providers = annotation_providers;
    snapshot.auth_providers = auth_providers;
    snapshot.bracket_sets = bracket_sets;
    snapshot.comment_markers = comment_markers;
    snapshot.indent_rules = indent_rules;
    snapshot.snippets = snippets;
    snapshot.themes = themes;
    snapshot.file_icon_themes = file_icon_themes;
    snapshot.loaded_plugins = ComputeLoadedPlugins();
    snapshot.reload_summary = reload_summary;
    return snapshot;
  }

  void PublishContributionSnapshot() { published_ = BuildContributionSnapshot(); }

  // RecordMessage/RecordError mutate the host-owned messages/errors vectors that the
  // UI thread reads, so the whole record-and-sink runs through ApplyHostMutation:
  // inline under exclusive access (setup/round-trip), or marshalled to the UI thread
  // when a fire-and-forget event records on the worker.
  void RecordMessage(std::string message) {
    ApplyHostMutation([this, message = std::move(message)]() mutable {
      messages.push_back(std::move(message));
      if (raw_callbacks.log_sink) {
        raw_callbacks.log_sink(messages.back());
      }
    });
  }

  void RecordError(std::string error) {
    ApplyHostMutation([this, error = std::move(error)]() mutable {
      errors.push_back(std::move(error));
      if (raw_callbacks.error_sink) {
        raw_callbacks.error_sink(errors.back());
      }
    });
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

};

PluginHost::Callbacks PluginHost::Impl::BuildRoutedCallbacks() {
  Callbacks routed;

  // Each routed wrapper is assigned only when the corresponding raw callback is
  // wired, so existence checks (`if (callbacks.x)`) keep their original meaning.

  // Reads resolve from the per-call snapshot (or live shell, on the inline path).
  if (raw_callbacks.active_buffer) {
    routed.active_buffer = [this]() { return ResolveActiveBuffer(); };
  }
  if (raw_callbacks.get_setting) {
    routed.get_setting = [this](std::string_view id) { return ResolveSetting(id); };
  }
  if (raw_callbacks.is_command_name_available) {
    // Consulted only during setup-time registration, which runs with direct access.
    routed.is_command_name_available = [this](std::string_view name) -> bool {
      return g_exec.direct && raw_callbacks.is_command_name_available(name);
    };
  }

  // Requests/writes: run directly when the worker holds exclusive shell access,
  // otherwise marshal an owning copy to the UI thread via the mailbox. Closures
  // never capture a lua_State / Lua ref.
  if (raw_callbacks.open_file) {
    routed.open_file = [this](const OpenFileRequest& request) -> bool {
      if (g_exec.direct) {
        return raw_callbacks.open_file(request);
      }
      ApplyHostMutation([this, request]() { raw_callbacks.open_file(request); });
      return true;
    };
  }
  if (raw_callbacks.show_sidebar) {
    routed.show_sidebar = [this](std::string_view id) -> bool {
      if (g_exec.direct) {
        return raw_callbacks.show_sidebar(id);
      }
      ApplyHostMutation([this, id = std::string(id)]() { raw_callbacks.show_sidebar(id); });
      return true;
    };
  }
  if (raw_callbacks.publish_diagnostics) {
    routed.publish_diagnostics = [this](std::string_view owner, const std::filesystem::path& path,
                                        std::vector<editor::Diagnostic> diagnostics) {
      ApplyHostMutation([this, owner = std::string(owner), path,
                         diagnostics = std::move(diagnostics)]() mutable {
        raw_callbacks.publish_diagnostics(owner, path, std::move(diagnostics));
      });
    };
  }
  if (raw_callbacks.clear_file_diagnostics) {
    routed.clear_file_diagnostics = [this](std::string_view owner,
                                           const std::filesystem::path& path) {
      ApplyHostMutation([this, owner = std::string(owner), path]() {
        raw_callbacks.clear_file_diagnostics(owner, path);
      });
    };
  }
  if (raw_callbacks.clear_owner_diagnostics) {
    routed.clear_owner_diagnostics = [this](std::string_view owner) {
      ApplyHostMutation(
          [this, owner = std::string(owner)]() { raw_callbacks.clear_owner_diagnostics(owner); });
    };
  }
  if (raw_callbacks.publish_decorations) {
    routed.publish_decorations = [this](std::string_view owner, const std::filesystem::path& path,
                                        editor::PluginDecorationData decorations) {
      ApplyHostMutation([this, owner = std::string(owner), path,
                         decorations = std::move(decorations)]() mutable {
        raw_callbacks.publish_decorations(owner, path, std::move(decorations));
      });
    };
  }
  if (raw_callbacks.clear_file_decorations) {
    routed.clear_file_decorations = [this](std::string_view owner,
                                           const std::filesystem::path& path) {
      ApplyHostMutation([this, owner = std::string(owner), path]() {
        raw_callbacks.clear_file_decorations(owner, path);
      });
    };
  }
  if (raw_callbacks.clear_owner_decorations) {
    routed.clear_owner_decorations = [this](std::string_view owner) {
      ApplyHostMutation(
          [this, owner = std::string(owner)]() { raw_callbacks.clear_owner_decorations(owner); });
    };
  }
  if (raw_callbacks.publish_surface) {
    routed.publish_surface = [this](std::string_view owner, std::string_view surface_id,
                                    editor::SurfaceContent content) {
      ApplyHostMutation([this, owner = std::string(owner), surface_id = std::string(surface_id),
                         content = std::move(content)]() mutable {
        raw_callbacks.publish_surface(owner, surface_id, std::move(content));
      });
    };
  }
  if (raw_callbacks.clear_surface) {
    routed.clear_surface = [this](std::string_view owner, std::string_view surface_id) {
      ApplyHostMutation(
          [this, owner = std::string(owner), surface_id = std::string(surface_id)]() {
            raw_callbacks.clear_surface(owner, surface_id);
          });
    };
  }
  if (raw_callbacks.clear_owner_surfaces) {
    routed.clear_owner_surfaces = [this](std::string_view owner) {
      ApplyHostMutation(
          [this, owner = std::string(owner)]() { raw_callbacks.clear_owner_surfaces(owner); });
    };
  }
  if (raw_callbacks.decode_raster) {
    routed.decode_raster = [this](std::uint64_t hash, int format, std::vector<std::byte> bytes,
                                  int width, int height) {
      ApplyHostMutation([this, hash, format, bytes = std::move(bytes), width, height]() mutable {
        raw_callbacks.decode_raster(hash, format, std::move(bytes), width, height);
      });
    };
  }
  if (raw_callbacks.apply_workspace_edit) {
    routed.apply_workspace_edit = [this](std::string_view owner,
                                         const WorkspaceEditRequest& request) -> bool {
      if (g_exec.direct) {
        return raw_callbacks.apply_workspace_edit(owner, request);
      }
      // Posted, not awaited: the UI thread re-validates against the live buffer at
      // apply time and drops the edit if stale. Report optimistic acceptance.
      ApplyHostMutation([this, owner = std::string(owner), request]() {
        raw_callbacks.apply_workspace_edit(owner, request);
      });
      return true;
    };
  }
  if (raw_callbacks.publish_ghost_text) {
    routed.publish_ghost_text = [this](std::string_view owner, const GhostTextRequest& request) {
      ApplyHostMutation([this, owner = std::string(owner), request]() {
        raw_callbacks.publish_ghost_text(owner, request);
      });
    };
  }
  if (raw_callbacks.clear_ghost_text) {
    routed.clear_ghost_text = [this](std::string_view owner) {
      ApplyHostMutation(
          [this, owner = std::string(owner)]() { raw_callbacks.clear_ghost_text(owner); });
    };
  }
  if (raw_callbacks.error_sink) {
    routed.error_sink = [this](const std::string& message) {
      ApplyHostMutation([this, message]() { raw_callbacks.error_sink(message); });
    };
  }
  if (raw_callbacks.log_sink) {
    routed.log_sink = [this](const std::string& message) {
      ApplyHostMutation([this, message]() { raw_callbacks.log_sink(message); });
    };
  }
  if (raw_callbacks.request_status_redraw) {
    routed.request_status_redraw = [this]() {
      ApplyHostMutation([this]() { raw_callbacks.request_status_redraw(); });
    };
  }
  if (raw_callbacks.show_notification) {
    routed.show_notification = [this](const std::string& level, const std::string& message) {
      ApplyHostMutation([this, level, message]() { raw_callbacks.show_notification(level, message); });
    };
  }

  return routed;
}

PluginHostSnapshot PluginHost::Impl::CaptureSnapshot() const {
  PluginHostSnapshot snapshot;
  snapshot.project_root = current_project_root;
  if (raw_callbacks.active_buffer) {
    if (const std::optional<PluginHost::ActiveBuffer> active = raw_callbacks.active_buffer();
        active.has_value() && !active->path.empty()) {
      snapshot.active_buffer = PluginHostSnapshot::ActiveBuffer{
          .path = active->path,
          .line = active->line,
          .column = active->column,
          .present = true,
      };
    }
  }
  if (raw_callbacks.get_setting) {
    snapshot.settings.reserve(settings.size());
    for (const auto& spec : settings) {
      if (const std::optional<std::string> value = raw_callbacks.get_setting(spec.id);
          value.has_value()) {
        snapshot.settings.emplace_back(spec.id, *value);
      }
    }
  }
  return snapshot;
}

void PluginHost::Impl::RunOnWorkerDetached(PluginHostSnapshot snapshot, std::function<void()> fn) {
  if (worker_ == nullptr) {
    // No worker wired: run inline with exclusive access (legacy behavior).
    ExecuteWithContext(&snapshot, /*direct=*/true, fn);
    return;
  }
  worker_->EnsureStarted();
  worker_->Post([this, snapshot = std::move(snapshot), fn = std::move(fn)]() mutable {
    ExecuteWithContext(&snapshot, /*direct=*/false, fn);
  });
}

PluginHost::PluginHost() : impl_(std::make_unique<Impl>()) {}

PluginHost::~PluginHost() {
  Shutdown();
}

PluginHost::PluginHost(PluginHost&& other) noexcept = default;

PluginHost& PluginHost::operator=(PluginHost&& other) noexcept = default;

void PluginHost::SetCallbacks(Callbacks callbacks) {
  impl_->raw_callbacks = std::move(callbacks);
  impl_->callbacks = impl_->BuildRoutedCallbacks();
}

void PluginHost::SetWorker(PluginThread* worker) {
  impl_->worker_ = worker;
}

#include "plugin/PluginHostPublicApi.inc"
