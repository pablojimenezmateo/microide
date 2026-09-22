#include "platform/RuntimePaths.h"

#include <cstdlib>
#include <string>
#include <system_error>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace microide::platform {

namespace {

std::filesystem::path EnvPath(const char* name) {
  // Plain getenv, not SDL's wrapper: it reads the live environment, so a setenv()
  // from a test's ScopedEnvVar is visible (SDL's cached copy was not, which is why
  // the SDL_getenv_unsafe variant was used here).
  const char* value = std::getenv(name);
  return value != nullptr && value[0] != '\0' ? std::filesystem::path(value)
                                              : std::filesystem::path{};
}

// Directory holding the running executable. This is what SDL_GetBasePath returned;
// reading /proc/self/exe directly keeps the path resolver out of the windowing
// library, which is what lets a headless build resolve its own assets.
std::filesystem::path BasePath() {
#if defined(__linux__)
  std::error_code ec;
  const std::filesystem::path exe = std::filesystem::read_symlink("/proc/self/exe", ec);
  if (ec || exe.empty()) {
    return {};
  }
  return exe.parent_path().lexically_normal();
#else
  return {};
#endif
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
    // Non-throwing probe: one candidate is the MICROIDE_ASSET_ROOT env value, so a
    // hostile/broken path (ELOOP, permission-denied parent, overlong component) must
    // degrade to "next candidate" rather than throw std::filesystem_error at startup.
    std::error_code dir_ec;
    if (!candidate.empty() && std::filesystem::is_directory(candidate, dir_ec) && !dir_ec) {
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

bool EnsureSecurePrivateDirectory(const std::filesystem::path& dir) {
  if (dir.empty()) {
    return false;
  }
#if defined(__unix__) || defined(__APPLE__)
  // Create any missing ancestors with normal permissions, then handle the leaf
  // ourselves so we never follow a symlink or trust a pre-existing foreign leaf.
  std::error_code ec;
  if (dir.has_parent_path()) {
    std::filesystem::create_directories(dir.parent_path(), ec);
  }

  struct stat st{};
  if (::lstat(dir.c_str(), &st) == 0) {
    if (S_ISLNK(st.st_mode)) {
      return false;  // a symlink at the path could redirect state elsewhere
    }
    if (!S_ISDIR(st.st_mode)) {
      return false;  // a regular file / device squatting the directory name
    }
    if (st.st_uid != ::geteuid()) {
      return false;  // owned by another user — do not trust it
    }
    if ((st.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
      // We own it, so tighten group/other bits away; refuse if that fails.
      if (::chmod(dir.c_str(), S_IRWXU) != 0) {
        return false;
      }
    }
    return true;
  }

  // Leaf does not exist: create it owner-only. mkdir is subject to umask, so
  // chmod afterward to guarantee 0700 regardless of the inherited mask.
  if (::mkdir(dir.c_str(), S_IRWXU) != 0) {
    return false;
  }
  return ::chmod(dir.c_str(), S_IRWXU) == 0;
#else
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  return !ec;
#endif
}

}  // namespace microide::platform
