#pragma once

#include <SDL3/SDL.h>

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "plugin/PluginHost.h"
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
  void SetPollInterval(std::chrono::milliseconds poll_interval);
  std::optional<std::chrono::milliseconds> NextPollDelay() const;
  bool ConsumeAssetChanges(bool force_check);

  bool Reload(const std::filesystem::path& project_root, bool reload_syntax_definitions = true);
  std::string ReloadSummary() const;

  void ShutdownHost();
  void Shutdown();

 private:
  WorkspaceOutputChannels output_channels_;
  WorkspacePluginAssetMonitor asset_monitor_;
  plugin::PluginHost plugin_host_;
  std::size_t runtime_syntax_plugin_definition_count_ = 0;
  std::vector<std::string> runtime_syntax_errors_;
};

}  // namespace microide::workspace
