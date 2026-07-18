#pragma once

#include <SDL3/SDL.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "editor/SyntaxDefinitionLoader.h"
#include "plugin/PluginHost.h"
#include "plugin/PluginThread.h"
#include "workspace/WorkspaceOutputChannels.h"
#include "workspace/WorkspacePluginAssetMonitor.h"

namespace microide::workspace {

class WorkspacePluginRuntime {
 public:
  void SetCallbacks(plugin::PluginHost::Callbacks callbacks);

  bool enabled() const;
  plugin::PluginHost& Host();
  const plugin::PluginHost& Host() const;

  void AppendLog(std::string text);
  void AppendError(std::string text);
  const std::vector<std::string>* OutputChannelEntries(std::string_view id) const;

  void SetWakeEventType(Uint32 event_type);
  bool ConsumeWakeEvent(Uint32 type);

  // Dedicated worker thread that runs plugin Lua off the UI thread. Spawned
  // lazily on the first Reload that loads a plugin; never created otherwise.
  plugin::PluginThread& Thread() { return plugin_thread_; }
  void SetPluginThreadEventType(Uint32 event_type);
  int DrainPluginThreadActions();
  int PendingPluginThreadActionCount() const;
  void SetPollInterval(std::chrono::milliseconds poll_interval);
  std::optional<std::chrono::milliseconds> NextPollDelay() const;
  bool ConsumeAssetChanges(bool force_check);

  // Non-blocking reload: forwards to PluginHost::ReloadAsync, then loads runtime syntax
  // definitions and invokes `on_complete` with the clean/error result. `on_complete`
  // runs on the UI thread during the mailbox drain (or synchronously when no worker is
  // wired / nothing needs loading).
  void ReloadAsync(const std::filesystem::path& project_root, bool reload_syntax_definitions,
                   std::function<void(bool)> on_complete);
  std::span<const std::string_view> ChangedSyntaxLanguages() const {
    return changed_syntax_language_views_;
  }
  bool syntax_definitions_changed() const { return syntax_definitions_changed_; }
  std::string ReloadSummary() const;

  // Test/telemetry seam: how many times the runtime-syntax discovery+load ran on
  // the plugin worker (off the main thread) rather than the synchronous fallback.
  // Lets a test prove the reload actually moved that work off the UI thread.
  std::uint64_t OffThreadSyntaxLoadCount() const {
    return off_thread_syntax_load_count_.load(std::memory_order_relaxed);
  }

  void ShutdownHost();
  void Shutdown();

 private:
  // Load runtime syntax definitions after the host reload settles and fold the result
  // into the clean/error bool. Synchronous fallback used only when no plugin worker is
  // running (no plugins to load); the worker path splits into the two helpers below.
  bool ApplySyntaxReload(const std::filesystem::path& project_root,
                         bool reload_syntax_definitions, bool clean_reload);

  // Result of the off-thread syntax discovery/load (TD-2026-07-17A-108). Plain data
  // only — safe to move from the plugin worker back to the main thread through the
  // mailbox (captures no lua_State / Lua ref).
  struct SyntaxLoadResult {
    bool unchanged = false;  // fingerprint matched the last publish -> skip the swap
    std::uint64_t fingerprint = 0;
    std::vector<editor::runtime_syntax::RuntimeSyntaxDefinitionData> definitions;
    std::vector<std::string> loader_errors;
  };
  // WORKER thread: stat/read/parse the syntax sources (each file compiled in its own
  // throwaway lua_State, so this is independent of the host's shared runtime) and
  // fingerprint them. No global registry mutation happens here.
  SyntaxLoadResult LoadSyntaxDefinitionsOffThread(
      const std::vector<std::filesystem::path>& directories, std::uint64_t last_fingerprint,
      bool has_last_fingerprint);
  // MAIN thread: swap the built definitions into the global runtime-syntax registry
  // (kept on the main thread so lock-free main-thread registry readers stay correct)
  // and update the changed-language bookkeeping. Drops the publish when `generation`
  // is stale so a superseded reload can't overwrite a newer one's registry.
  bool PublishSyntaxReload(const std::filesystem::path& project_root, bool clean_reload,
                           SyntaxLoadResult result, std::uint64_t generation);

  WorkspaceOutputChannels output_channels_;
  WorkspacePluginAssetMonitor asset_monitor_;
  plugin::PluginHost plugin_host_;
  plugin::PluginThread plugin_thread_;
  std::size_t runtime_syntax_plugin_definition_count_ = 0;
  std::vector<std::string> runtime_syntax_errors_;
  bool syntax_definitions_changed_ = false;
  std::vector<std::string> changed_syntax_languages_;
  std::vector<std::string_view> changed_syntax_language_views_;
  bool syntax_fingerprint_initialized_ = false;
  std::uint64_t syntax_source_fingerprint_ = 0;
  editor::runtime_syntax::SyntaxSourceFingerprint syntax_fingerprint_cache_;
  // Monotonic per-reload token: bumped on every ReloadAsync, captured by the
  // off-thread load, and re-checked in PublishSyntaxReload so a superseded reload's
  // late worker result cannot swap the global registry back (TD-2026-07-17A-108).
  std::uint64_t syntax_reload_generation_ = 0;
  // Incremented on the plugin worker each time LoadSyntaxDefinitionsOffThread runs.
  std::atomic<std::uint64_t> off_thread_syntax_load_count_{0};
};

}  // namespace microide::workspace
