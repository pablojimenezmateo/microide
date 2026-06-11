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

}  // namespace

void RegisterRuntimePathsTests(std::vector<TestCase>& tests) {
  AddTest(tests, "RuntimePaths/PreferExplicitAssetRootOverride",
          TestRuntimePathsPreferExplicitAssetRootOverride);
  AddTest(tests, "RuntimePaths/ResolveInstalledShareLayout",
          TestRuntimePathsResolveInstalledShareLayout);
}

}  // namespace microide::tests
