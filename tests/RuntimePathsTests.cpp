#include "TestSupport.h"

#include "platform/RuntimePaths.h"

namespace microide::tests {
namespace {

class ScopedCurrentPath {
 public:
  explicit ScopedCurrentPath(const std::filesystem::path& path)
      : previous_(std::filesystem::current_path()) {
    std::filesystem::current_path(path);
  }

  ~ScopedCurrentPath() { std::filesystem::current_path(previous_); }

 private:
  std::filesystem::path previous_;
};

void TestRuntimePathsPreferExplicitAssetRootOverride() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path assets_root = temp_dir.path() / "assets";
  std::filesystem::create_directories(assets_root / "themes");
  WriteFile(assets_root / "themes" / "test.microide", "background=#000000\n");

  ScopedEnvVar scoped_asset_root("MICROIDE_ASSET_ROOT", assets_root.string());

  Expect(platform::ResolveBundledAssetDirectory() == assets_root.lexically_normal(),
         "runtime asset lookup should respect MICROIDE_ASSET_ROOT");
  Expect(platform::ResolveBundledAssetPath("themes/test.microide") ==
             (assets_root / "themes" / "test.microide").lexically_normal(),
         "runtime asset lookup should resolve asset-relative paths from the explicit root");
}

void TestRuntimePathsResolveInstalledShareLayout() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path install_root = temp_dir.path() / "usr";
  const std::filesystem::path base_path = install_root / "bin";
  const std::filesystem::path assets_root = install_root / "share" / "microide" / "assets";
  const std::filesystem::path working_dir = temp_dir.path() / "cwd";
  std::filesystem::create_directories(base_path);
  std::filesystem::create_directories(assets_root / "themes");
  std::filesystem::create_directories(working_dir);
  ScopedCurrentPath scoped_current_path(working_dir);

  Expect(platform::ResolveBundledAssetDirectoryForBasePath(base_path) ==
             assets_root.lexically_normal(),
         "runtime asset lookup should resolve Debian-style shared asset installs");
  Expect(platform::ResolveBundledAssetDirectoryForBasePath(base_path,
                                                           temp_dir.path() / "override") ==
             assets_root.lexically_normal(),
         "runtime asset lookup should fall back when an explicit asset root is missing");
}

// TD-2026-07-17-040: the control-socket runtime dir must be created private and a
// pre-existing untrusted directory must be refused, so the /tmp fallback cannot be
// hijacked to redirect state or plant forged descriptors.
void TestEnsureSecurePrivateDirectory() {
#if defined(__unix__) || defined(__APPLE__)
  TemporaryDirectory temp_dir;
  const std::filesystem::path base = temp_dir.path();

  // Fresh directory: created owner-only (0700).
  const std::filesystem::path fresh = base / "run" / "microide";
  Expect(platform::EnsureSecurePrivateDirectory(fresh),
         "a fresh runtime directory should be created and accepted");
  Expect(std::filesystem::is_directory(fresh), "the directory should exist after creation");
  Expect((std::filesystem::status(fresh).permissions() &
          (std::filesystem::perms::group_all | std::filesystem::perms::others_all)) ==
             std::filesystem::perms::none,
         "the created directory must be owner-only (no group/other bits)");

  // Idempotent on an already-secure directory.
  Expect(platform::EnsureSecurePrivateDirectory(fresh),
         "re-securing an already-private directory should succeed");

  // A symlink squatting the path must be refused (it could redirect state).
  const std::filesystem::path link_target = base / "elsewhere";
  std::filesystem::create_directories(link_target);
  const std::filesystem::path link_path = base / "linked";
  std::filesystem::create_symlink(link_target, link_path);
  Expect(!platform::EnsureSecurePrivateDirectory(link_path),
         "a symlink at the runtime path must be refused");

  // A world/group-accessible directory we own is tightened to owner-only, not refused.
  const std::filesystem::path loose = base / "loose";
  std::filesystem::create_directories(loose);
  std::filesystem::permissions(loose, std::filesystem::perms::all);
  Expect(platform::EnsureSecurePrivateDirectory(loose),
         "a loose directory we own should be tightened and accepted");
  Expect((std::filesystem::status(loose).permissions() &
          (std::filesystem::perms::group_all | std::filesystem::perms::others_all)) ==
             std::filesystem::perms::none,
         "the loose directory must be tightened to owner-only");

  // A regular file squatting the directory name must be refused.
  const std::filesystem::path file_path = base / "afile";
  WriteFile(file_path, "not a dir");
  Expect(!platform::EnsureSecurePrivateDirectory(file_path),
         "a non-directory at the runtime path must be refused");
#endif
}

}  // namespace

void RegisterRuntimePathsTests(std::vector<TestCase>& tests) {
  AddTest(tests, "RuntimePaths/PreferExplicitAssetRootOverride",
          TestRuntimePathsPreferExplicitAssetRootOverride);
  AddTest(tests, "RuntimePaths/ResolveInstalledShareLayout",
          TestRuntimePathsResolveInstalledShareLayout);
  AddTest(tests, "RuntimePaths/EnsureSecurePrivateDirectory",
          TestEnsureSecurePrivateDirectory);
}

}  // namespace microide::tests
