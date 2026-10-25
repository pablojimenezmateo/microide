#include "platform/RuntimePaths.h"

#include "platform/AppDirectories.h"

#include <cstdlib>
#include <string>
#include <system_error>
#include <vector>

#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>

namespace microide::platform {

namespace {

// Directory holding the running executable. This is what SDL_GetBasePath returned;
// reading /proc/self/exe directly keeps the path resolver out of the windowing
// library, which is what lets a headless build resolve its own assets.
std::filesystem::path BasePath() {
  std::error_code ec;
  const std::filesystem::path exe = std::filesystem::read_symlink("/proc/self/exe", ec);
  if (ec || exe.empty()) {
    return {};
  }
  return exe.parent_path().lexically_normal();
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
                                                 EnvironmentPath("MICROIDE_ASSET_ROOT"));
}

std::filesystem::path ResolveBundledAssetPath(std::string_view relative_path) {
  const std::filesystem::path asset_root = ResolveBundledAssetDirectory();
  if (asset_root.empty()) {
    return {};
  }
  return (asset_root / std::string(relative_path)).lexically_normal();
}

std::string NormalizeArchitecture(std::string_view uname_m) {
  while (!uname_m.empty() && (uname_m.back() == '\n' || uname_m.back() == '\r' || uname_m.back() == ' ')) {
    uname_m.remove_suffix(1);
  }
  if (uname_m == "arm64") {
    return "aarch64";
  }
  if (uname_m == "amd64") {
    return "x86_64";
  }
  return std::string(uname_m);
}

std::string ThisMachineArchitecture() {
  struct utsname name{};
  if (::uname(&name) == 0) {
    return NormalizeArchitecture(name.machine);
  }
  return {};
}

std::filesystem::path ResolveBundledServerBinary(std::string_view arch) {
  std::error_code ec;
  if (const std::filesystem::path explicit_binary = EnvironmentPath("MICROIDE_SERVER_BINARY");
      !explicit_binary.empty()) {
    return std::filesystem::is_regular_file(explicit_binary, ec) ? explicit_binary
                                                                 : std::filesystem::path();
  }
  return ResolveBundledServerBinaryForBasePath(BasePath(), arch);
}

std::filesystem::path ResolveBundledServerBinaryForBasePath(const std::filesystem::path& base,
                                                            std::string_view arch) {
  std::error_code ec;
  if (base.empty() || arch.empty()) {
    return {};
  }
  // The package's place (/usr/lib/microide/server, beside /usr/bin), then beside
  // the executable.
  const std::string name = "microide-server-" + std::string(arch);
  for (const std::filesystem::path& packaged :
       {base.parent_path() / "lib" / "microide" / "server" / name, base / name}) {
    if (std::filesystem::is_regular_file(packaged, ec)) {
      return packaged;
    }
  }
  const std::filesystem::path beside = base / "microide-server";
  return arch == ThisMachineArchitecture() && std::filesystem::is_regular_file(beside, ec)
             ? beside
             : std::filesystem::path();
}

bool EnsureSecurePrivateDirectory(const std::filesystem::path& dir) {
  if (dir.empty()) {
    return false;
  }
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
}

}  // namespace microide::platform
