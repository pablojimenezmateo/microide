#include "platform/AppDirectories.h"

#include "platform/HostPlatform.h"

#include <cstdlib>
#include <string>

namespace microide::platform {

namespace {

std::filesystem::path EnvPath(const char* name) {
  const char* value = std::getenv(name);
  return value != nullptr && value[0] != '\0' ? std::filesystem::path(value)
                                              : std::filesystem::path{};
}

}  // namespace

std::filesystem::path ResolveUserHomeDirectory() {
  switch (CurrentHostPlatform()) {
    case HostPlatform::Windows:
      if (const std::filesystem::path profile = EnvPath("USERPROFILE"); !profile.empty()) {
        return profile;
      }
      if (const std::filesystem::path home = EnvPath("HOME"); !home.empty()) {
        return home;
      }
      if (const char* drive = std::getenv("HOMEDRIVE"),
          *path = std::getenv("HOMEPATH");
          drive != nullptr && drive[0] != '\0' && path != nullptr && path[0] != '\0') {
        return std::filesystem::path(std::string(drive) + std::string(path));
      }
      return {};
    case HostPlatform::MacOS:
    case HostPlatform::Linux:
      if (const std::filesystem::path home = EnvPath("HOME"); !home.empty()) {
        return home;
      }
      return {};
  }
  return {};
}

std::filesystem::path ResolveUserDirectory(UserDirectoryKind kind) {
  switch (CurrentHostPlatform()) {
    case HostPlatform::Windows: {
      switch (kind) {
        case UserDirectoryKind::Config:
          if (const std::filesystem::path config = EnvPath("APPDATA"); !config.empty()) {
            return config;
          }
          break;
        case UserDirectoryKind::State:
        case UserDirectoryKind::Data:
        case UserDirectoryKind::Cache:
          if (const std::filesystem::path local = EnvPath("LOCALAPPDATA"); !local.empty()) {
            return local;
          }
          break;
      }
      if (const std::filesystem::path home = ResolveUserHomeDirectory(); !home.empty()) {
        if (kind == UserDirectoryKind::Config) {
          return home / "AppData" / "Roaming";
        }
        return home / "AppData" / "Local";
      }
      return {};
    }
    case HostPlatform::MacOS: {
      const std::filesystem::path home = ResolveUserHomeDirectory();
      if (home.empty()) {
        return {};
      }
      switch (kind) {
        case UserDirectoryKind::Config:
        case UserDirectoryKind::State:
        case UserDirectoryKind::Data:
          return home / "Library" / "Application Support";
        case UserDirectoryKind::Cache:
          return home / "Library" / "Caches";
      }
      return {};
    }
    case HostPlatform::Linux:
      switch (kind) {
        case UserDirectoryKind::Config:
          if (const std::filesystem::path config = EnvPath("XDG_CONFIG_HOME"); !config.empty()) {
            return config;
          }
          if (const std::filesystem::path home = ResolveUserHomeDirectory(); !home.empty()) {
            return home / ".config";
          }
          return {};
        case UserDirectoryKind::State:
          if (const std::filesystem::path state = EnvPath("XDG_STATE_HOME"); !state.empty()) {
            return state;
          }
          if (const std::filesystem::path home = ResolveUserHomeDirectory(); !home.empty()) {
            return home / ".local" / "state";
          }
          return {};
        case UserDirectoryKind::Data:
          if (const std::filesystem::path data = EnvPath("XDG_DATA_HOME"); !data.empty()) {
            return data;
          }
          if (const std::filesystem::path home = ResolveUserHomeDirectory(); !home.empty()) {
            return home / ".local" / "share";
          }
          return {};
        case UserDirectoryKind::Cache:
          if (const std::filesystem::path cache = EnvPath("XDG_CACHE_HOME"); !cache.empty()) {
            return cache;
          }
          if (const std::filesystem::path home = ResolveUserHomeDirectory(); !home.empty()) {
            return home / ".cache";
          }
          return {};
      }
  }

  return {};
}

std::filesystem::path ResolveAppDirectory(UserDirectoryKind kind, std::string_view app_name) {
  const std::filesystem::path root = ResolveUserDirectory(kind);
  if (root.empty()) {
    return {};
  }
  if (app_name.empty()) {
    return root;
  }

  const std::filesystem::path app_root = root / std::string(app_name);
  if (CurrentHostPlatform() != HostPlatform::Windows) {
    return app_root;
  }

  switch (kind) {
    case UserDirectoryKind::Config:
      return app_root;
    case UserDirectoryKind::State:
      return app_root / "State";
    case UserDirectoryKind::Data:
      return app_root / "Data";
    case UserDirectoryKind::Cache:
      return app_root / "Cache";
  }
  return app_root;
}

}  // namespace microide::platform
