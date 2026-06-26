#pragma once

#include <SDL3/SDL.h>

#include <chrono>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

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

  bool Reload(const std::filesystem::path& project_root, bool reload_syntax_definitions = true);
  std::span<const std::string_view> ChangedSyntaxLanguages() const {
    return changed_syntax_language_views_;
  }
  bool syntax_definitions_changed() const { return syntax_definitions_changed_; }
  std::string ReloadSummary() const;

  void ShutdownHost();
  void Shutdown();

 private:
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
};

}  // namespace microide::workspace
