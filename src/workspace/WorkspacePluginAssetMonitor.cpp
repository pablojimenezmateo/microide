#include "workspace/WorkspacePluginAssetMonitor.h"

#include <vector>

#include "platform/AppDirectories.h"

namespace microide::workspace {

namespace {

std::filesystem::path GlobalPluginDirectory() {
  const std::filesystem::path app_directory =
      platform::ResolveAppDirectory(platform::UserDirectoryKind::Config, "microide");
  return app_directory.empty() ? std::filesystem::path{} : app_directory / "plugins";
}

}  // namespace

void WorkspacePluginAssetMonitor::SetPollInterval(std::chrono::milliseconds poll_interval) {
  watcher_.SetPollInterval(poll_interval);
}

void WorkspacePluginAssetMonitor::SetProjectRoot(const std::filesystem::path& project_root) {
  std::vector<std::filesystem::path> roots;
  roots.push_back(GlobalPluginDirectory());
  if (!project_root.empty()) {
    roots.push_back(project_root.lexically_normal() / ".microide" / "plugins");
  }
  watcher_.SetRoots(std::move(roots));
}

void WorkspacePluginAssetMonitor::Reset() {
  watcher_.Clear();
}

std::optional<std::chrono::milliseconds> WorkspacePluginAssetMonitor::NextPollDelay() const {
  return watcher_.NextPollDelay();
}

bool WorkspacePluginAssetMonitor::PollForChanges() {
  const std::optional<std::chrono::milliseconds> next_delay = watcher_.NextPollDelay();
  return next_delay.has_value() && *next_delay == std::chrono::milliseconds::zero() &&
         watcher_.Poll();
}

}  // namespace microide::workspace
