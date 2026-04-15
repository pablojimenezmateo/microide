#include "workspace/WorkspaceShell.h"

#include <filesystem>
#include <string_view>

#include "workspace/WorkspaceCommandRegistry.h"

namespace microide::workspace {

WorkspaceShell::WorkspaceShell() {
  plugin_host_.SetCallbacks(plugin::PluginHost::Callbacks{
      .is_command_name_available =
          [](std::string_view name) { return FindWorkspaceActionByCommand(name) == nullptr; },
      .open_file =
          [this](const std::filesystem::path& path) {
            const std::filesystem::path normalized_path = path.lexically_normal();
            return OpenFileInNewTab(normalized_path);
          },
      .log_sink = {},
  });
}

bool WorkspaceShell::ReloadPluginsForCurrentProject() {
  if (!plugin_host_.enabled()) {
    return false;
  }
  const bool clean_reload = plugin_host_.Reload(project_root_);
  NotifyPluginsAboutOpenBuffers();
  return clean_reload;
}

void WorkspaceShell::NotifyPluginsAboutOpenBuffers() {
  if (!plugin_host_.enabled()) {
    return;
  }
  for (const auto& tab : open_tabs_) {
    if (tab.kind != TabEntry::Kind::Editor || tab.path.empty()) {
      continue;
    }
    NotifyPluginBufferOpen(tab.path);
  }
}

void WorkspaceShell::NotifyPluginBufferOpen(const std::filesystem::path& path) {
  if (!plugin_host_.enabled() || path.empty()) {
    return;
  }
  plugin_host_.OnBufferOpen(path.lexically_normal());
}

void WorkspaceShell::NotifyPluginBufferSave(const std::filesystem::path& path) {
  if (!plugin_host_.enabled() || path.empty()) {
    return;
  }
  plugin_host_.OnBufferSave(path.lexically_normal());
}

}  // namespace microide::workspace
