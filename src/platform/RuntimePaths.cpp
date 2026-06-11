#include "platform/RuntimePaths.h"

#include <SDL3/SDL.h>

#include <string>
#include <vector>

namespace microide::platform {

namespace {

std::filesystem::path EnvPath(const char* name) {
  // SDL_getenv_unsafe bypasses SDL's cached copy so it sees changes made by
  // setenv() after SDL initialization (e.g. in tests via ScopedEnvVar).
  const char* value = SDL_getenv_unsafe(name);
  return value != nullptr && value[0] != '\0' ? std::filesystem::path(value)
                                              : std::filesystem::path{};
}

std::filesystem::path BasePath() {
  const char* raw_base_path = SDL_GetBasePath();
  if (raw_base_path == nullptr || raw_base_path[0] == '\0') {
    return {};
  }
  return std::filesystem::path(raw_base_path).lexically_normal();
}

}  // namespace

std::filesystem::path ResolveBundledAssetDirectoryForBasePath(
    const std::filesystem::path& base_path,
    const std::filesystem::path& explicit_asset_root) {
  const std::vector<std::filesystem::path> candidates = {
      explicit_asset_root,
      std::filesystem::path("assets"),
      std::filesystem::path("microide") / "assets",
      base_path / "assets",
      base_path / ".." / "assets",
      base_path / ".." / "share" / "microide" / "assets",
      base_path / ".." / "Resources" / "assets",
      base_path / ".." / ".." / "Resources" / "assets",
      base_path / ".." / ".." / "microide" / "assets",
  };

  for (const auto& candidate : candidates) {
    if (!candidate.empty() && std::filesystem::is_directory(candidate)) {
      return candidate.lexically_normal();
    }
  }
  return {};
}

std::filesystem::path ResolveBundledAssetDirectory() {
  return ResolveBundledAssetDirectoryForBasePath(BasePath(),
                                                 EnvPath("MICROIDE_ASSET_ROOT"));
}

std::filesystem::path ResolveBundledAssetPath(std::string_view relative_path) {
  const std::filesystem::path asset_root = ResolveBundledAssetDirectory();
  if (asset_root.empty()) {
    return {};
  }
  return (asset_root / std::string(relative_path)).lexically_normal();
}

}  // namespace microide::platform
