#include "plugin/PluginLifecycleResetInterop.h"

namespace microide::plugin::lifecycle_reset_interop {

void ResetForDisabledRuntime(
    const std::filesystem::path& project_root,
    std::filesystem::path* current_project_root,
    std::unordered_map<std::string, runtime_types::PluginCommand>* commands,
    std::vector<std::string>* command_names,
    std::unordered_map<std::string, runtime_types::SidebarProvider>* sidebars,
    std::vector<PluginHost::SidebarProviderInfo>* sidebar_providers,
    std::unordered_map<std::string, runtime_types::HoverProvider>* hovers,
    std::vector<std::string>* hover_provider_order,
    std::vector<runtime_types::PluginInstance>* plugins) {
  *current_project_root = project_root.empty() ? std::filesystem::path{} : project_root.lexically_normal();
  commands->clear();
  command_names->clear();
  sidebars->clear();
  sidebar_providers->clear();
  hovers->clear();
  hover_provider_order->clear();
  plugins->clear();
}

void ShutdownForDisabledRuntime(
    std::filesystem::path* current_project_root,
    std::unordered_map<std::string, runtime_types::PluginCommand>* commands,
    std::vector<std::string>* command_names,
    std::unordered_map<std::string, runtime_types::SidebarProvider>* sidebars,
    std::vector<PluginHost::SidebarProviderInfo>* sidebar_providers,
    std::unordered_map<std::string, runtime_types::HoverProvider>* hovers,
    std::vector<std::string>* hover_provider_order,
    std::vector<runtime_types::PluginInstance>* plugins) {
  plugins->clear();
  commands->clear();
  command_names->clear();
  sidebars->clear();
  sidebar_providers->clear();
  hovers->clear();
  hover_provider_order->clear();
  current_project_root->clear();
}

}  // namespace microide::plugin::lifecycle_reset_interop
