#include "platform/AppDirectories.h"

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
  if (const std::filesystem::path home = EnvPath("HOME"); !home.empty()) {
    return home;
  }
#if defined(_WIN32)
  if (const std::filesystem::path profile = EnvPath("USERPROFILE"); !profile.empty()) {
    return profile;
  }
  const char* drive = std::getenv("HOMEDRIVE");
  const char* path = std::getenv("HOMEPATH");
  if (drive != nullptr && drive[0] != '\0' && path != nullptr && path[0] != '\0') {
    return std::filesystem::path(std::string(drive) + std::string(path));
  }
#endif
  return {};
}

std::filesystem::path ResolveUserDirectory(UserDirectoryKind kind) {
  switch (kind) {
    case UserDirectoryKind::Config:
      if (const std::filesystem::path config = EnvPath("XDG_CONFIG_HOME"); !config.empty()) {
        return config;
      }
#if defined(_WIN32)
      if (const std::filesystem::path config = EnvPath("APPDATA"); !config.empty()) {
        return config;
      }
#endif
      if (const std::filesystem::path home = ResolveUserHomeDirectory(); !home.empty()) {
        return home / ".config";
      }
      return {};
    case UserDirectoryKind::State:
      if (const std::filesystem::path state = EnvPath("XDG_STATE_HOME"); !state.empty()) {
        return state;
      }
#if defined(_WIN32)
      if (const std::filesystem::path state = EnvPath("LOCALAPPDATA"); !state.empty()) {
        return state / "State";
      }
#endif
      if (const std::filesystem::path home = ResolveUserHomeDirectory(); !home.empty()) {
        return home / ".local" / "state";
      }
      return {};
    case UserDirectoryKind::Data:
      if (const std::filesystem::path data = EnvPath("XDG_DATA_HOME"); !data.empty()) {
        return data;
      }
#if defined(_WIN32)
      if (const std::filesystem::path data = EnvPath("LOCALAPPDATA"); !data.empty()) {
        return data / "Data";
      }
#endif
      if (const std::filesystem::path home = ResolveUserHomeDirectory(); !home.empty()) {
        return home / ".local" / "share";
      }
      return {};
    case UserDirectoryKind::Cache:
      if (const std::filesystem::path cache = EnvPath("XDG_CACHE_HOME"); !cache.empty()) {
        return cache;
      }
#if defined(_WIN32)
      if (const std::filesystem::path cache = EnvPath("LOCALAPPDATA"); !cache.empty()) {
        return cache / "Cache";
      }
#endif
      if (const std::filesystem::path home = ResolveUserHomeDirectory(); !home.empty()) {
        return home / ".cache";
      }
      return {};
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
  return root / std::string(app_name);
}

}  // namespace microide::platform
