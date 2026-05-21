#pragma once

#include <filesystem>

namespace microide::plugin {

// Canonical user config plugin install directory:
// $XDG_CONFIG_HOME/microide/plugins (or platform fallback equivalent).
std::filesystem::path ResolveUserPluginInstallRoot();

}  // namespace microide::plugin
