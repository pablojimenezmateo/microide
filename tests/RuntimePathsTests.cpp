#include "TestSupport.h"

#include "platform/RuntimePaths.h"

namespace microide::tests {
namespace {

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

}  // namespace

void RegisterRuntimePathsTests(std::vector<TestCase>& tests) {
  AddTest(tests, "RuntimePaths/PreferExplicitAssetRootOverride",
          TestRuntimePathsPreferExplicitAssetRootOverride);
}

}  // namespace microide::tests
