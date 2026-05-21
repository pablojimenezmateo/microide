#include "plugin/PluginInstallRoot.h"

#include "platform/AppDirectories.h"

namespace microide::plugin {

std::filesystem::path ResolveUserPluginInstallRoot() {
  const std::filesystem::path config_root =
      platform::ResolveAppDirectory(platform::UserDirectoryKind::Config, "microide");
  return config_root.empty() ? std::filesystem::path{} : config_root / "plugins";
}

}  // namespace microide::plugin
