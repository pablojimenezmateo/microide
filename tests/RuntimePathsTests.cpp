#include "TestSupport.h"

#include "util/PathMatch.h"

#include "platform/HostPlatform.h"
#include "platform/RuntimePaths.h"

#include <clocale>
#include <langinfo.h>

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

void TestEnsureSecurePrivateDirectory() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path base = temp_dir.path();

  const std::filesystem::path fresh = base / "run" / "microide";
  Expect(platform::EnsureSecurePrivateDirectory(fresh),
         "a fresh runtime directory should be created and accepted");
  Expect(std::filesystem::is_directory(fresh), "the directory should exist after creation");
  Expect((std::filesystem::status(fresh).permissions() &
          (std::filesystem::perms::group_all | std::filesystem::perms::others_all)) ==
             std::filesystem::perms::none,
         "the created directory must be owner-only (no group/other bits)");

  Expect(platform::EnsureSecurePrivateDirectory(fresh),
         "re-securing an already-private directory should succeed");

  const std::filesystem::path link_target = base / "elsewhere";
  std::filesystem::create_directories(link_target);
  const std::filesystem::path link_path = base / "linked";
  std::filesystem::create_symlink(link_target, link_path);
  Expect(!platform::EnsureSecurePrivateDirectory(link_path),
         "a symlink at the runtime path must be refused");

  const std::filesystem::path loose = base / "loose";
  std::filesystem::create_directories(loose);
  std::filesystem::permissions(loose, std::filesystem::perms::all);
  Expect(platform::EnsureSecurePrivateDirectory(loose),
         "a loose directory we own should be tightened and accepted");
  Expect((std::filesystem::status(loose).permissions() &
          (std::filesystem::perms::group_all | std::filesystem::perms::others_all)) ==
             std::filesystem::perms::none,
         "the loose directory must be tightened to owner-only");

  const std::filesystem::path file_path = base / "afile";
  WriteFile(file_path, "not a dir");
  Expect(!platform::EnsureSecurePrivateDirectory(file_path),
         "a non-directory at the runtime path must be refused");
}

void TestBundledServerBinaryIsChosenByArchitecture() {
  TemporaryDirectory temp;
  const std::filesystem::path usr = temp.path() / "usr";
  const std::filesystem::path bin = usr / "bin";
  WriteFile(usr / "lib/microide/server/microide-server-aarch64", "arm");
  WriteFile(bin / "microide-server-riscv64", "riscv");
  WriteFile(bin / "microide-server", "native");
  Expect(platform::ResolveBundledServerBinaryForBasePath(bin, "aarch64") ==
             usr / "lib/microide/server/microide-server-aarch64",
         "the packaged per-architecture server");
  Expect(platform::ResolveBundledServerBinaryForBasePath(bin, "riscv64") == bin / "microide-server-riscv64",
         "else one beside the executable");
  Expect(platform::ResolveBundledServerBinaryForBasePath(bin, platform::ThisMachineArchitecture()) ==
             bin / "microide-server",
         "the build tree's own server serves this machine's architecture");
  Expect(platform::ResolveBundledServerBinaryForBasePath(bin, "mips64").empty(),
         "and nothing serves an architecture that is not shipped");
}

void TestCharacterLocaleIsUtf8EvenFromAPosixEnvironment() {
  ScopedEnvVar lc_all("LC_ALL", "C");
  ScopedEnvVar lang("LANG", "C");
  const std::string locale = platform::UseUtf8CharacterLocale();
  const std::string codeset = nl_langinfo(CODESET);
  (void)std::setlocale(LC_CTYPE, "C");
  Expect(codeset == "UTF-8", "LC_CTYPE " + locale + " should be UTF-8, its codeset is " + codeset);
}

void TestRelativeClimbOutIsAboutTheFirstComponent() {
  using microide::util::RelativePathClimbsOut;
  using microide::util::RelativeTextClimbsOut;
  for (const char* outside : {"..", "../x", "../../a/b"}) {
    Expect(RelativeTextClimbsOut(outside) && RelativePathClimbsOut(outside), std::string(outside) + " climbs out");
  }
  for (const char* inside : {"..cache", "..cache/x", "...", "a/../b", ".", "x/.."}) {
    Expect(!RelativeTextClimbsOut(inside) && !RelativePathClimbsOut(inside), std::string(inside) + " stays in");
  }
  Expect(RelativePathClimbsOut(std::filesystem::path("/p/q").lexically_relative("/p/r")), "a sibling climbs out");
  Expect(!RelativePathClimbsOut(std::filesystem::path("/p/..q/x").lexically_relative("/p")),
         "a child named ..q does not");
}

void TestPathContainmentHelpersAgreeOnTrailingSeparators() {
  struct Case {
    const char* candidate;
    const char* root;
    bool within;
  };
  const Case cases[] = {
      {"/a/b/c", "/a/b/", true}, {"/a/b", "/a/b/", true},  {"/a/b/", "/a/b", true},
      {"/a/b/", "/a/b/", true},  {"/a/bc", "/a/b/", false}, {"/a/b/c", "/a/b", true},
      {"/a/bc", "/a/b", false},  {"/x", "/", true},         {"/", "/", true},
      {"/a/b/../c", "/a/b", false}, {"/a", "/a/b/", false},
  };
  for (const Case& c : cases) {
    const std::filesystem::path candidate(c.candidate);
    const std::filesystem::path root(c.root);
    const std::string what = std::string(c.candidate) + " in " + c.root;
    Expect(microide::util::PathEqualsOrWithin(candidate, root) == c.within, "PathEqualsOrWithin: " + what);
    Expect(microide::util::PathEqualsOrWithinNormalized(candidate, root.lexically_normal()) == c.within,
           "PathEqualsOrWithinNormalized: " + what);
    Expect(microide::util::NormalizedPathEqualsOrWithin(candidate.lexically_normal(), root.lexically_normal()) ==
               c.within,
           "NormalizedPathEqualsOrWithin: " + what);
  }
}

void TestPortableRelativePathIsRelativeNormalAndBounded() {
  using microide::util::IsPortableRelativePath;
  for (const char* good : {"a", "src/main.cpp", "..cache/x", "a/.b", "...", "x/y/z"}) {
    Expect(IsPortableRelativePath(good, 64), std::string(good) + " is portable");
  }
  for (const char* bad : {"", "/etc/passwd", "../x", "a/../b", "./a", "a/.", "a//b", "a/", ".."}) {
    Expect(!IsPortableRelativePath(bad, 64), std::string("'") + bad + "' is not portable");
  }
  Expect(!IsPortableRelativePath(std::string_view("a\0b", 3), 64), "an embedded NUL is not portable");
  Expect(IsPortableRelativePath("abcd", 4) && !IsPortableRelativePath("abcde", 4), "the byte cap is inclusive");
}

}

void RegisterRuntimePathsTests(std::vector<TestCase>& tests) {
  AddTest(tests, "PathMatch/RelativeClimbOutIsAboutTheFirstComponent",
          TestRelativeClimbOutIsAboutTheFirstComponent);
  AddTest(tests, "PathMatch/ContainmentHelpersAgreeOnTrailingSeparators",
          TestPathContainmentHelpersAgreeOnTrailingSeparators);
  AddTest(tests, "PathMatch/PortableRelativePathIsRelativeNormalAndBounded",
          TestPortableRelativePathIsRelativeNormalAndBounded);
  AddTest(tests, "RuntimePaths/PreferExplicitAssetRootOverride",
          TestRuntimePathsPreferExplicitAssetRootOverride);
  AddTest(tests, "RuntimePaths/BundledServerBinaryIsChosenByArchitecture",
          TestBundledServerBinaryIsChosenByArchitecture);
  AddTest(tests, "RuntimePaths/ResolveInstalledShareLayout",
          TestRuntimePathsResolveInstalledShareLayout);
  AddTest(tests, "RuntimePaths/EnsureSecurePrivateDirectory",
          TestEnsureSecurePrivateDirectory);
  AddTest(tests, "HostPlatform/CharacterLocaleIsUtf8EvenFromAPosixEnvironment",
          TestCharacterLocaleIsUtf8EvenFromAPosixEnvironment);
}

}
