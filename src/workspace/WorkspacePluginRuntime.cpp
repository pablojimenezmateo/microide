#include "workspace/WorkspacePluginRuntime.h"

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

void WorkspacePluginRuntime::SetPollInterval(std::chrono::milliseconds poll_interval) {
  asset_monitor_.SetPollInterval(poll_interval);
}

std::optional<std::chrono::milliseconds> WorkspacePluginRuntime::NextPollDelay() const {
  return asset_monitor_.NextPollDelay();
}

bool WorkspacePluginRuntime::ConsumeAssetChanges(bool force_check) {
  return force_check ? asset_monitor_.ConsumePendingChanges() : asset_monitor_.PollForChanges();
}

bool WorkspacePluginRuntime::Reload(const std::filesystem::path& project_root) {
  bool clean_reload = false;
  {
    util::StartupTrace::Scope host_scope("PluginHost::Reload");
    clean_reload = plugin_host_.enabled() ? plugin_host_.Reload(project_root) : false;
  }

  std::vector<std::string> syntax_loader_errors;
  std::vector<editor::runtime_syntax::RuntimeSyntaxDefinitionData> syntax_definitions;
  {
    util::StartupTrace::Scope load_scope("LoadSyntaxDefinitions");
    syntax_definitions = editor::runtime_syntax::LoadDefinitionsFromDirectories(
        plugin_host_.DataDirectories("syntax"), &syntax_loader_errors);
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
  plugin_host_.Shutdown();
}

void WorkspacePluginRuntime::Shutdown() {
  asset_monitor_.SetWakeEventType(0);
  asset_monitor_.Reset();
  plugin_host_.Shutdown();
  runtime_syntax_plugin_definition_count_ = 0;
  runtime_syntax_errors_.clear();
}

}  // namespace microide::workspace
