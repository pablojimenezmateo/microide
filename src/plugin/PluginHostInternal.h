#pragma once

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
#include <unordered_set>
#include <utility>
#include <vector>

#include <SDL3/SDL.h>

#include "platform/AppDirectories.h"
#include "platform/Filesystem.h"
#include "platform/Subprocess.h"
#include "plugin/PluginContributionInterop.h"
#include "plugin/PluginContributionLimits.h"
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

constexpr std::chrono::milliseconds kSaveParticipantDeadline{2000};

struct PluginExecContext {
  const PluginHostSnapshot* snapshot = nullptr;
  bool direct = true;
  bool executing = false;
  bool allow_registration = true;
  bool in_reload = false;
};

extern thread_local PluginExecContext g_exec;

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

  Callbacks raw_callbacks{};
  Callbacks callbacks{};
  std::filesystem::path current_project_root;
  const platform::ProcessLauncher* current_project_launcher = &platform::LocalProcessLauncher();
  project::FileWriteGate* current_project_write_gate = &project::LocalFileWriteGate();

  PluginThread* worker_ = nullptr;

  std::chrono::milliseconds save_participant_deadline_ = kSaveParticipantDeadline;

  struct ContextGuard {
    PluginExecContext prev;
    ~ContextGuard() { g_exec = prev; }
  };

  template <typename F>
  void ExecuteWithContext(const PluginHostSnapshot* snapshot, bool direct,
                          bool allow_registration, F&& fn) {
    ContextGuard guard{g_exec};
    g_exec.snapshot = snapshot;
    g_exec.direct = direct;
    g_exec.executing = true;
    g_exec.allow_registration = allow_registration;
    fn();
  }

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
      if (g_exec.snapshot->settings != nullptr) {
        for (const auto& [key, value] : *g_exec.snapshot->settings) {
          if (key == id) {
            return value;
          }
        }
      }
      return std::nullopt;
    }
    return raw_callbacks.get_setting ? raw_callbacks.get_setting(id) : std::nullopt;
  }

  Callbacks BuildRoutedCallbacks();

  PluginHostSnapshot CaptureSnapshot() const;

  std::shared_ptr<const ResolvedPluginSettings> ResolveSettingsSnapshot() const;

  void RunOnWorkerDetached(PluginHostSnapshot snapshot, std::function<void()> fn,
                           std::string coalesce_key = {});

  struct SaveParticipantResult {
    std::string text;
    std::string error;
    bool ok = true;
    bool timed_out = false;
  };

  SaveParticipantResult RunSaveParticipantsBounded(const std::filesystem::path& path,
                                                   std::string input);
  SaveParticipantResult WaitForSaveParticipantsOnWorker(
      PluginHostSnapshot snapshot, const std::filesystem::path& path,
      std::shared_ptr<SaveParticipantResult> shared);
  std::function<SaveParticipantOutcome()> PrepareSaveParticipantsJob(
      const std::filesystem::path& path, std::string text);
  template <typename F>
  void RunOnWorkerBlocking(const PluginHostSnapshot& snapshot, F&& fn,
                           const bool allow_registration = false) {
    if (g_exec.executing || worker_ == nullptr) {
      ExecuteWithContext(&snapshot, true, allow_registration,
                         std::forward<F>(fn));
      return;
    }
    worker_->EnsureStarted();
    std::promise<void> done;
    std::future<void> finished = done.get_future();
    worker_->Post([this, &snapshot, &fn, &done, allow_registration]() {
      struct ReleaseWaiter {
        std::promise<void>& promise;
        ~ReleaseWaiter() { promise.set_value(); }
      } release_waiter{done};
      ExecuteWithContext(&snapshot, true, allow_registration, fn);
    });
    finished.wait();
  }

  struct QueryBuffer {
    std::filesystem::path path;
    std::shared_ptr<const std::string> text;
  };
  QueryBuffer query_buffer_;
  class ScopedQueryBuffer {
   public:
    ScopedQueryBuffer(Impl& impl, QueryBuffer buffer) : impl_(impl) {
      impl_.query_buffer_ = std::move(buffer);
    }
    ~ScopedQueryBuffer() { impl_.query_buffer_ = {}; }
    ScopedQueryBuffer(const ScopedQueryBuffer&) = delete;
    ScopedQueryBuffer& operator=(const ScopedQueryBuffer&) = delete;

   private:
    Impl& impl_;
  };
  bool HasCompletionProvider(std::string_view language_id) const {
    return std::any_of(published_.completions.begin(), published_.completions.end(),
                       [&](const auto& provider) { return provider.language_id == language_id; });
  }
  bool HasCodeActionProvider(std::string_view language_id) const {
    return std::any_of(published_.code_actions.begin(), published_.code_actions.end(),
                       [&](const auto& provider) { return provider.language_id == language_id; });
  }
  QueryBuffer CaptureQueryBuffer(bool has_provider, const std::filesystem::path& path) const {
    if (!has_provider || !raw_callbacks.live_buffer_text) {
      return {};
    }
    std::shared_ptr<const std::string> text = raw_callbacks.live_buffer_text(path);
    if (text == nullptr) {
      return {};
    }
    return QueryBuffer{.path = path.lexically_normal(), .text = std::move(text)};
  }
  const std::shared_ptr<const std::string>* QueryBufferTextFor(
      const std::filesystem::path& normalized_path) const {
    return query_buffer_.text != nullptr && query_buffer_.path == normalized_path
               ? &query_buffer_.text
               : nullptr;
  }

  template <typename Result>
  void RunQueryAsync(std::string dedup_key,
                     std::function<Result()> produce,
                     std::function<void(Result)> deliver,
                     QueryBuffer buffer = {}) {
    PluginHostSnapshot snapshot = CaptureSnapshot();
    if (worker_ == nullptr || g_exec.executing) {
      Result result;
      ExecuteWithContext(&snapshot, true, false, [&]() {
        const ScopedQueryBuffer scope(*this, std::move(buffer));
        result = produce();
      });
      deliver(std::move(result));
      return;
    }
    worker_->EnsureStarted();
    auto task = [this, snapshot = std::move(snapshot), produce = std::move(produce),
                 deliver = std::move(deliver), buffer = std::move(buffer)]() mutable {
      auto result = std::make_shared<Result>();
      ExecuteWithContext(&snapshot, false, false, [&]() {
        const ScopedQueryBuffer scope(*this, std::move(buffer));
        *result = produce();
      });
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

#if MICROIDE_HAS_LUA_PLUGINS
  void RunReloadLoad(const std::filesystem::path& next_project_root,
                     const std::vector<std::filesystem::path>& plugin_roots,
                     const platform::ProcessLauncher* project_launcher,
                     project::FileWriteGate* project_write_gate) {
    g_exec.in_reload = true;
    TearDownPlugins();
    disabled_plugin_meta.clear();
    current_project_root = next_project_root;
    current_project_launcher = project_launcher;
    current_project_write_gate = project_write_gate;
    for (const auto& plugin_root : plugin_roots) {
      std::string error_message;
      if (!LoadPluginRoot(plugin_root, &error_message) && !error_message.empty()) {
        RecordError(std::move(error_message));
      }
    }
    if (!current_project_root.empty()) {
      project_lifecycle_interop::DispatchProjectOpenCallbacks(
          &plugins, [this](runtime_types::PluginInstance* plugin, int callback_ref,
                           const char* callback_name) {
            CallProjectCallback(plugin, callback_ref, callback_name);
          });
    }
  }

  void ReloadDetached(PluginHostSnapshot snapshot,
                      std::vector<std::filesystem::path> plugin_roots,
                      std::function<void(bool)> on_complete) {
    worker_->EnsureStarted();
    worker_->Post([this, snapshot = std::move(snapshot),
                   plugin_roots = std::move(plugin_roots),
                   on_complete = std::move(on_complete)]() mutable {
      ExecuteWithContext(&snapshot, false, true,
                         [&]() {
                           RunReloadLoad(snapshot.project_root, plugin_roots,
                                         snapshot.project_launcher, snapshot.project_write_gate);
                         });
      auto built = std::make_shared<ContributionSnapshot>(BuildContributionSnapshot());
      const std::size_t plugin_count = plugins.size();
      const std::size_t command_count = commands.size();
      const std::size_t sidebar_count = sidebars.size();
      const std::size_t hover_count = hover_provider_order.size();
      worker_->PostToMain([this, built, plugin_count, command_count, sidebar_count, hover_count,
                           on_complete = std::move(on_complete)]() mutable {
        published_ = std::move(*built);
        published_.reload_summary = FormatReloadSummary(plugin_count, command_count, sidebar_count,
                                                        hover_count, errors.size());
        ++status_view_revision;
        on_complete(errors.empty());
      });
    });
  }
#endif

  struct ContributionSnapshot {
    std::vector<std::string> command_names;
    std::vector<SidebarProviderInfo> sidebar_providers;
    std::vector<PluginHost::ContributedMenuEntry> menu_entries;
    std::vector<PluginHost::ContributedKeybinding> keybindings;
    std::vector<PluginHost::ContributedSettingSpec> settings;
    std::vector<PluginHost::ContributedFormatter> formatters;
    std::vector<PluginHost::ContributedSaveParticipant> save_participants;
    std::vector<PluginHost::ContributedVirtualDocument> virtual_documents;
    std::vector<PluginHost::ContributedCompletion> completions;
    std::vector<PluginHost::ContributedCodeAction> code_actions;
    std::vector<PluginHost::ContributedLanguageProvider> language_providers;
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
    std::vector<PluginHost::ContributedStatusItem> status_item_order;
    std::unordered_map<std::string, std::size_t> status_item_index;
    PluginHost::EditorEventInterest editor_event_interests;
    bool has_hover_providers = false;
    std::filesystem::path project_root;
    std::string reload_summary = "Lua plugin runtime unavailable";
  };
  ContributionSnapshot published_;
  std::uint64_t status_view_revision = 0;
  mutable std::shared_ptr<const ResolvedPluginSettings> cached_settings_;
  mutable std::uint64_t cached_settings_revision_ = 0;
  mutable std::uint64_t cached_settings_specs_revision_ = 0;

  std::vector<PluginInstance> plugins;
  std::vector<std::string> disabled_plugins;
  std::vector<PluginHost::LoadedPlugin> disabled_plugin_meta;
  std::unordered_map<std::string, PluginCommand> commands;
  std::vector<std::string> command_names;
  std::unordered_map<std::string, SidebarProvider> sidebars;
  std::vector<SidebarProviderInfo> sidebar_providers;
  bool command_names_dirty = false;
  bool sidebar_providers_dirty = false;
  std::unordered_map<std::string, HoverProvider> hovers;
  std::vector<std::string> hover_provider_order;
  std::vector<PluginHost::ContributedMenuEntry> menu_entries;
  std::vector<PluginHost::ContributedKeybinding> keybindings;
  std::vector<PluginHost::ContributedSettingSpec> settings;
  std::unordered_map<std::string, PluginHost::ContributedStatusItem> status_items;
  std::vector<PluginHost::ContributedStatusItem> status_item_order;
  std::unordered_map<std::string, std::size_t> status_item_index;
  std::vector<PluginHost::ContributedFormatter> formatters;
  std::vector<PluginHost::ContributedSaveParticipant> save_participants;
  std::vector<PluginHost::ContributedVirtualDocument> virtual_documents;
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
  std::unordered_set<std::string> language_server_ids;
  std::unordered_set<std::string> debug_adapter_ids;
  std::unordered_set<std::string> launch_config_ids;
  std::unordered_set<std::string> task_ids;
  std::unordered_set<std::string> tool_ids;
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

  static std::string FormatReloadSummary(std::size_t plugin_count, std::size_t command_count,
                                         std::size_t sidebar_count, std::size_t hover_count,
                                         std::size_t error_count) {
    std::string summary = "Loaded " + std::to_string(plugin_count) + " plugin";
    if (plugin_count != 1) {
      summary += "s";
    }
    summary += " and " + std::to_string(command_count) + " command";
    if (command_count != 1) {
      summary += "s";
    }
    summary += " and " + std::to_string(sidebar_count) + " sidebar";
    if (sidebar_count != 1) {
      summary += "s";
    }
    summary += " and " + std::to_string(hover_count) + " hover provider";
    if (hover_count != 1) {
      summary += "s";
    }
    if (error_count != 0) {
      summary += " with " + std::to_string(error_count) + " error";
      if (error_count != 1) {
        summary += "s";
      }
    }
    return summary;
  }

  void SetReloadSummary() {
    if (!enabled()) {
      reload_summary = "Lua plugin runtime unavailable";
      return;
    }
    reload_summary = FormatReloadSummary(plugins.size(), commands.size(), sidebars.size(),
                                         hover_provider_order.size(), errors.size());
  }

  PluginHost::EditorEventInterest ComputeEditorEventInterests() const {
    PluginHost::EditorEventInterest interest;
#if MICROIDE_HAS_LUA_PLUGINS
    for (const auto& plugin : plugins) {
      interest.buffer_change |= plugin.on_buffer_change_ref != LUA_NOREF;
      interest.cursor_move |= plugin.on_cursor_move_ref != LUA_NOREF;
      interest.selection_change |= plugin.on_selection_change_ref != LUA_NOREF;
      interest.buffer_close |= plugin.on_buffer_close_ref != LUA_NOREF;
      interest.buffer_open |= plugin.on_buffer_open_ref != LUA_NOREF;
      interest.buffer_save |= plugin.on_buffer_save_ref != LUA_NOREF;
    }
#endif
    return interest;
  }

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
    snapshot.virtual_documents = virtual_documents;
    snapshot.completions = completions;
    snapshot.code_actions = code_actions;
    for (const LanguageQueryRuntime& runtime : language_query_runtimes) {
      snapshot.language_providers.push_back(PluginHost::ContributedLanguageProvider{
          .kind = runtime.kind,
          .id = runtime.id,
          .language_id = runtime.language_id,
          .plugin_id = runtime.plugin_id,
          .trigger_characters = runtime.trigger_characters,
      });
    }
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
    snapshot.status_item_order = status_item_order;
    snapshot.editor_event_interests = ComputeEditorEventInterests();
    snapshot.has_hover_providers = !hover_provider_order.empty();
    snapshot.project_root = current_project_root;
    snapshot.reload_summary = reload_summary;
    return snapshot;
  }

  void PublishContributionSnapshot() {
    published_ = BuildContributionSnapshot();
    ++status_view_revision;
  }

  static void TrimRecordedHistory(std::vector<std::string>& entries) {
    if (entries.size() <= PluginHost::kMaxRecordedLogEntries) {
      return;
    }
    const std::size_t keep = PluginHost::kMaxRecordedLogEntries * 3 / 4;
    entries.erase(entries.begin(), entries.begin() + (entries.size() - keep));
  }

  void RecordMessage(std::string message) {
    ApplyHostMutation([this, message = std::move(message)]() mutable {
      messages.push_back(std::move(message));
      if (raw_callbacks.log_sink) {
        raw_callbacks.log_sink(messages.back());
      }
      TrimRecordedHistory(messages);
    });
  }

  void RecordError(std::string error) {
    ApplyHostMutation([this, error = std::move(error)]() mutable {
      errors.push_back(std::move(error));
      if (raw_callbacks.error_sink) {
        raw_callbacks.error_sink(errors.back());
      }
      TrimRecordedHistory(errors);
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
    } else if (const auto* live = QueryBufferTextFor(normalized_path)) {
      lua_buffer_project_interop::PushBufferTextMethods(state, *live);
    }
  }

#endif

  std::vector<std::filesystem::path> DiscoverPluginRoots() const {
    return discovery_interop::DiscoverPluginRoots();
  }

#include "plugin/PluginHostLuaApi.inc"

  void ClearMessages() { messages.clear(); }

};

}
