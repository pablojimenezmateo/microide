#pragma once

#include <SDL3/SDL.h>

#include <chrono>
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

  void ShutdownHost();
  void Shutdown();

 private:
  // Load runtime syntax definitions after the host reload settles and fold the result
  // into the clean/error bool. Runs on the same thread as the ReloadAsync completion.
  bool ApplySyntaxReload(const std::filesystem::path& project_root,
                         bool reload_syntax_definitions, bool clean_reload);

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
};

}  // namespace microide::workspace
