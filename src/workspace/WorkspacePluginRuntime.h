#pragma once

#include <SDL3/SDL.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "editor/SyntaxDefinitionLoader.h"
#include "editor/TextViewport.h"
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

  void SetWakeChannel(Uint32 event_type);
  bool ConsumeWakeEvent(Uint32 type);

  plugin::PluginThread& Thread() { return plugin_thread_; }
  void SetPluginThreadEventType(Uint32 event_type);
  int DrainPluginThreadActions();
  int PendingPluginThreadActionCount() const;
  void SetPollInterval(std::chrono::milliseconds poll_interval);
  std::optional<std::chrono::milliseconds> NextPollDelay() const;
  bool ConsumeAssetChanges(bool force_check);

  void ReloadAsync(const std::filesystem::path& project_root,
                   plugin::PluginHost::ProjectLocality locality, bool reload_syntax_definitions,
                   std::function<void(bool)> on_complete);
  std::span<const std::string_view> ChangedSyntaxLanguages() const {
    return changed_syntax_language_views_;
  }
  bool syntax_definitions_changed() const { return syntax_definitions_changed_; }
  std::string ReloadSummary() const;

  std::uint64_t OffThreadSyntaxLoadCount() const {
    return off_thread_syntax_load_count_.load(std::memory_order_relaxed);
  }

  void ShutdownHost();
  void Shutdown();

  std::shared_ptr<const std::string> LiveBufferText(const editor::TextViewport& viewport) {
    if (live_text_ == nullptr || live_text_revision_ != viewport.content_revision()) {
      live_text_ = std::make_shared<const std::string>(viewport.SerializeText());
      live_text_revision_ = viewport.content_revision();
    }
    return live_text_;
  }

 private:
  bool ApplySyntaxReload(const std::filesystem::path& project_root,
                         bool reload_syntax_definitions, bool clean_reload);

  struct SyntaxLoadResult {
    bool unchanged = false;
    std::uint64_t fingerprint = 0;
    std::vector<editor::runtime_syntax::RuntimeSyntaxDefinitionData> definitions;
    std::vector<std::string> loader_errors;
  };
  SyntaxLoadResult LoadSyntaxDefinitionsOffThread(
      const std::vector<std::filesystem::path>& directories, std::uint64_t last_fingerprint,
      bool has_last_fingerprint);
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
  std::uint64_t syntax_reload_generation_ = 0;
  std::atomic<std::uint64_t> off_thread_syntax_load_count_{0};
  std::shared_ptr<const std::string> live_text_;
  std::uint64_t live_text_revision_ = 0;
};

}
