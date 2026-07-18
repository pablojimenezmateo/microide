#include "workspace/WorkspacePluginRuntime.h"

#include <algorithm>
#include <cstdint>
#include <unordered_set>
#include <utility>

#include "editor/SyntaxDefinitionLoader.h"
#include "util/StartupTrace.h"

namespace microide::workspace {

void WorkspacePluginRuntime::SetCallbacks(plugin::PluginHost::Callbacks callbacks) {
  plugin_host_.SetCallbacks(std::move(callbacks));
}

bool WorkspacePluginRuntime::enabled() const {
  return plugin_host_.enabled();
}

plugin::PluginHost& WorkspacePluginRuntime::Host() {
  return plugin_host_;
}

const plugin::PluginHost& WorkspacePluginRuntime::Host() const {
  return plugin_host_;
}

void WorkspacePluginRuntime::AppendLog(std::string text) {
  output_channels_.AppendLine("plugins.log", "Plugin Log", std::move(text));
}

void WorkspacePluginRuntime::AppendError(std::string text) {
  output_channels_.AppendLine("plugins.error", "Plugin Errors", std::move(text));
}

const std::vector<std::string>* WorkspacePluginRuntime::OutputChannelEntries(
    std::string_view id) const {
  return output_channels_.Entries(id);
}

void WorkspacePluginRuntime::SetWakeEventType(Uint32 event_type) {
  asset_monitor_.SetWakeEventType(event_type);
}

bool WorkspacePluginRuntime::ConsumeWakeEvent(Uint32 type) {
  return asset_monitor_.ConsumeWakeEvent(type);
}

void WorkspacePluginRuntime::SetPluginThreadEventType(Uint32 event_type) {
  plugin_thread_.SetWakeEventType(event_type);
}

int WorkspacePluginRuntime::DrainPluginThreadActions() {
  return plugin_thread_.DrainMainThreadActions();
}

int WorkspacePluginRuntime::PendingPluginThreadActionCount() const {
  return plugin_thread_.PendingMainThreadActionCount();
}

void WorkspacePluginRuntime::SetPollInterval(std::chrono::milliseconds poll_interval) {
  asset_monitor_.SetPollInterval(poll_interval);
}

std::optional<std::chrono::milliseconds> WorkspacePluginRuntime::NextPollDelay() const {
  return asset_monitor_.NextPollDelay();
}

bool WorkspacePluginRuntime::ConsumeAssetChanges(bool force_check) {
  return force_check ? asset_monitor_.ConsumePendingChanges() : asset_monitor_.PollForChanges();
}

void WorkspacePluginRuntime::ReloadAsync(const std::filesystem::path& project_root,
                                         bool reload_syntax_definitions,
                                         std::function<void(bool)> on_complete) {
  syntax_definitions_changed_ = false;
  changed_syntax_languages_.clear();
  changed_syntax_language_views_.clear();
  const std::uint64_t generation = ++syntax_reload_generation_;
  // The host reload runs the plugin Lua off the UI thread; its completion fires on the
  // main thread (mailbox drain) or inline when no worker is wired. From that completion
  // the runtime-syntax sources are stat/read/parsed on the plugin worker and only the
  // built registry is swapped in on the main thread (TD-2026-07-17A-108).
  plugin_host_.ReloadAsync(
      project_root, [this, project_root, reload_syntax_definitions, generation,
                     on_complete = std::move(on_complete)](bool clean_reload) mutable {
        if (!reload_syntax_definitions) {
          asset_monitor_.SetProjectRoot(project_root);
          on_complete(clean_reload);
          return;
        }
        if (!plugin_thread_.started()) {
          // No plugin worker (no plugins to load): stay fully synchronous, exactly as
          // before — nothing to gain from a worker hop and most tests run this path.
          on_complete(ApplySyntaxReload(project_root, /*reload_syntax_definitions=*/true,
                                        clean_reload));
          return;
        }
        // DataDirectories reads the UI-thread-owned published plugin snapshot, so it is
        // resolved here on the main thread and the resulting paths are handed to the
        // worker (which never touches host state).
        std::vector<std::filesystem::path> directories = plugin_host_.DataDirectories("syntax");
        const std::uint64_t last_fingerprint = syntax_source_fingerprint_;
        const bool has_last_fingerprint = syntax_fingerprint_initialized_;
        plugin_thread_.Post([this, project_root, clean_reload, generation,
                             directories = std::move(directories), last_fingerprint,
                             has_last_fingerprint,
                             on_complete = std::move(on_complete)]() mutable {
          SyntaxLoadResult result =
              LoadSyntaxDefinitionsOffThread(directories, last_fingerprint, has_last_fingerprint);
          plugin_thread_.PostToMain([this, project_root, clean_reload, generation,
                                     result = std::move(result),
                                     on_complete = std::move(on_complete)]() mutable {
            on_complete(PublishSyntaxReload(project_root, clean_reload, std::move(result),
                                            generation));
          });
        });
      });
}

WorkspacePluginRuntime::SyntaxLoadResult WorkspacePluginRuntime::LoadSyntaxDefinitionsOffThread(
    const std::vector<std::filesystem::path>& directories, std::uint64_t last_fingerprint,
    bool has_last_fingerprint) {
  off_thread_syntax_load_count_.fetch_add(1, std::memory_order_relaxed);
  SyntaxLoadResult result;
  result.fingerprint = syntax_fingerprint_cache_.Compute(directories);
  if (has_last_fingerprint && last_fingerprint == result.fingerprint) {
    // Sources unchanged since the last publish: skip the load + registry rebuild.
    result.unchanged = true;
    return result;
  }
  {
    util::StartupTrace::Scope load_scope("LoadSyntaxDefinitions");
    result.definitions = editor::runtime_syntax::LoadDefinitionsFromDirectories(
        directories, &result.loader_errors);
  }
  return result;
}

bool WorkspacePluginRuntime::PublishSyntaxReload(const std::filesystem::path& project_root,
                                                 bool clean_reload, SyntaxLoadResult result,
                                                 std::uint64_t generation) {
  asset_monitor_.SetProjectRoot(project_root);
  // Drop a superseded reload's publish: a newer ReloadAsync bumped the generation, so
  // its (latest) worker result must own the global registry — never this stale one.
  if (generation != syntax_reload_generation_) {
    return clean_reload && runtime_syntax_errors_.empty();
  }
  if (result.unchanged) {
    syntax_fingerprint_initialized_ = true;
    syntax_source_fingerprint_ = result.fingerprint;
    syntax_definitions_changed_ = false;
    return clean_reload && runtime_syntax_errors_.empty();
  }

  runtime_syntax_errors_.clear();
  {
    util::StartupTrace::Scope reload_scope("ReloadSyntaxDefinitions");
    const editor::runtime_syntax::RuntimeSyntaxReloadResult syntax_reload =
        editor::runtime_syntax::ReloadDefinitions(result.definitions, &runtime_syntax_errors_);
    runtime_syntax_plugin_definition_count_ = syntax_reload.plugin_definition_count;
  }
  runtime_syntax_errors_.insert(runtime_syntax_errors_.end(), result.loader_errors.begin(),
                                result.loader_errors.end());
  std::unordered_set<std::string> unique_languages;
  for (const auto& definition : result.definitions) {
    if (!definition.filetype.empty()) {
      unique_languages.insert(definition.filetype);
    }
  }
  changed_syntax_languages_.assign(unique_languages.begin(), unique_languages.end());
  std::sort(changed_syntax_languages_.begin(), changed_syntax_languages_.end());
  changed_syntax_language_views_.clear();
  changed_syntax_language_views_.reserve(changed_syntax_languages_.size());
  for (const std::string& language : changed_syntax_languages_) {
    changed_syntax_language_views_.push_back(language);
  }
  syntax_fingerprint_initialized_ = true;
  syntax_source_fingerprint_ = result.fingerprint;
  syntax_definitions_changed_ = true;
  return clean_reload && runtime_syntax_errors_.empty();
}

bool WorkspacePluginRuntime::ApplySyntaxReload(const std::filesystem::path& project_root,
                                               bool reload_syntax_definitions, bool clean_reload) {
  if (!reload_syntax_definitions) {
    asset_monitor_.SetProjectRoot(project_root);
    return clean_reload;
  }

  std::vector<std::string> syntax_loader_errors;
  std::vector<editor::runtime_syntax::RuntimeSyntaxDefinitionData> syntax_definitions;
  const std::vector<std::filesystem::path> syntax_directories = plugin_host_.DataDirectories("syntax");
  const std::uint64_t fingerprint = syntax_fingerprint_cache_.Compute(syntax_directories);
  if (syntax_fingerprint_initialized_ && syntax_source_fingerprint_ == fingerprint) {
    asset_monitor_.SetProjectRoot(project_root);
    return clean_reload && runtime_syntax_errors_.empty();
  }
  {
    util::StartupTrace::Scope load_scope("LoadSyntaxDefinitions");
    syntax_definitions = editor::runtime_syntax::LoadDefinitionsFromDirectories(
        syntax_directories, &syntax_loader_errors);
  }

  runtime_syntax_errors_.clear();
  {
    util::StartupTrace::Scope reload_scope("ReloadSyntaxDefinitions");
    const editor::runtime_syntax::RuntimeSyntaxReloadResult syntax_reload =
        editor::runtime_syntax::ReloadDefinitions(syntax_definitions, &runtime_syntax_errors_);
    runtime_syntax_plugin_definition_count_ = syntax_reload.plugin_definition_count;
  }
  runtime_syntax_errors_.insert(runtime_syntax_errors_.end(), syntax_loader_errors.begin(),
                                syntax_loader_errors.end());
  std::unordered_set<std::string> unique_languages;
  for (const auto& definition : syntax_definitions) {
    if (!definition.filetype.empty()) {
      unique_languages.insert(definition.filetype);
    }
  }
  changed_syntax_languages_.assign(unique_languages.begin(), unique_languages.end());
  std::sort(changed_syntax_languages_.begin(), changed_syntax_languages_.end());
  changed_syntax_language_views_.reserve(changed_syntax_languages_.size());
  for (const std::string& language : changed_syntax_languages_) {
    changed_syntax_language_views_.push_back(language);
  }
  syntax_fingerprint_initialized_ = true;
  syntax_source_fingerprint_ = fingerprint;
  syntax_definitions_changed_ = true;
  asset_monitor_.SetProjectRoot(project_root);
  return clean_reload && runtime_syntax_errors_.empty();
}

std::string WorkspacePluginRuntime::ReloadSummary() const {
  std::string summary = plugin_host_.ReloadSummary();
  summary += " and " + std::to_string(runtime_syntax_plugin_definition_count_) +
             " syntax definition";
  if (runtime_syntax_plugin_definition_count_ != 1) {
    summary += "s";
  }
  if (!runtime_syntax_errors_.empty()) {
    summary += " with " + std::to_string(runtime_syntax_errors_.size()) + " syntax error";
    if (runtime_syntax_errors_.size() != 1) {
      summary += "s";
    }
  }
  return summary;
}

void WorkspacePluginRuntime::ShutdownHost() {
  // Project switch: quiesce the worker so no in-flight/queued plugin job is
  // touching the lua_State while the host runs on_project_close / shutdown
  // callbacks and destroys plugin state on this (UI) thread. Unlike full
  // Shutdown() we keep the worker ALIVE — the next project's reload posts to it —
  // so we Drain() rather than permanently Shutdown() the queue. Host teardown
  // itself runs plugin callbacks inline (g_exec.executing), so any synchronous
  // host API they call takes the inline branch instead of re-posting to the
  // worker, leaving the worker quiescent for the whole teardown.
  plugin_thread_.Drain();
  plugin_host_.Shutdown();
  // Discard any deferred plugin→UI mutations still queued for the project we just
  // tore down (including any posted by the on_project_close/shutdown callbacks
  // above). Otherwise the next mailbox drain would replay the old project's
  // diagnostics / decorations / surfaces / open_file into the newly active project.
  plugin_thread_.ClearMainThreadActions();
}

void WorkspacePluginRuntime::Shutdown() {
  // Join the worker before tearing down the host so no in-flight job touches a
  // lua_State the host is about to destroy.
  plugin_thread_.Shutdown();
  // The worker is now joined; drop the host's pointer to it so the host's own
  // teardown (and any late call) runs inline with exclusive access rather than
  // dead-locking on a post to a stopped worker.
  plugin_host_.SetWorker(nullptr);
  asset_monitor_.SetWakeEventType(0);
  asset_monitor_.Reset();
  plugin_host_.Shutdown();
  runtime_syntax_plugin_definition_count_ = 0;
  runtime_syntax_errors_.clear();
  syntax_definitions_changed_ = false;
  changed_syntax_languages_.clear();
  changed_syntax_language_views_.clear();
  syntax_fingerprint_initialized_ = false;
  syntax_source_fingerprint_ = 0;
  syntax_fingerprint_cache_.Clear();
}

}  // namespace microide::workspace
