#include "platform/AppDirectories.h"

#include <cstdlib>
#include <string>

namespace microide::platform {

std::filesystem::path EnvironmentPath(const char* name) {
  const char* value = std::getenv(name);
  return value != nullptr && value[0] != '\0' ? std::filesystem::path(value)
                                              : std::filesystem::path{};
}

std::filesystem::path ResolveUserHomeDirectory() { return EnvironmentPath("HOME"); }

std::filesystem::path ResolveUserDirectory(UserDirectoryKind kind) {
  // The XDG base directories, each falling back to its spec default under $HOME.
  const auto xdg = [](const char* variable, const char* home_relative) -> std::filesystem::path {
    if (std::filesystem::path explicit_dir = EnvironmentPath(variable); !explicit_dir.empty()) {
      return explicit_dir;
    }
    if (const std::filesystem::path home = ResolveUserHomeDirectory(); !home.empty()) {
      return home / home_relative;
    }
    return {};
  };
  switch (kind) {
    case UserDirectoryKind::Config:
      return xdg("XDG_CONFIG_HOME", ".config");
    case UserDirectoryKind::State:
      return xdg("XDG_STATE_HOME", ".local/state");
    case UserDirectoryKind::Data:
      return xdg("XDG_DATA_HOME", ".local/share");
    case UserDirectoryKind::Cache:
      return xdg("XDG_CACHE_HOME", ".cache");
  }
  return {};
}

std::filesystem::path ResolveAppDirectory(UserDirectoryKind kind, std::string_view app_name) {
  std::filesystem::path root = ResolveUserDirectory(kind);
  if (root.empty() || app_name.empty()) {
    return root;
  }
  return root / std::string(app_name);
}

}  // namespace microide::platform
