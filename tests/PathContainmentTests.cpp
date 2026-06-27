#include "TestSupport.h"

#include <filesystem>
#include <system_error>
#include <vector>

#include "plugin/PluginPathInterop.h"
#include "util/PathContainment.h"

namespace microide::tests {
namespace {

namespace path_interop = microide::plugin::path_interop;

void TestPathWithinRootRejectsParentEscapesAndEmptyRoots() {
  const std::filesystem::path root = "/home/user/project";
  Expect(util::PathWithinRoot(root / "src" / "main.cpp", root),
         "a path under the root should be contained");
  Expect(!util::PathWithinRoot("/home/user/other/file", root),
         "a sibling directory escapes the root");
  Expect(!util::PathWithinRoot(root / ".." / "secret", root),
         "a `..` segment that climbs out of the root is rejected");
  Expect(!util::PathWithinRoot(root / "src", std::filesystem::path()),
         "an empty root never contains anything");
}

void TestResolveWithinRootAllowsMissingInRootLeaf() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  std::filesystem::create_directories(root);

  // A not-yet-created file under a (not-yet-created) in-root subdirectory must still
  // resolve: weakly_canonical canonicalizes the existing prefix and appends the rest.
  const std::optional<std::filesystem::path> contained =
      util::ResolveWithinRoot(root / "sub" / "new.txt", root);
  Expect(contained.has_value(), "a missing in-root leaf should be allowed");
  Expect(util::PathWithinRoot(*contained, std::filesystem::canonical(root)),
         "the resolved leaf should stay within the canonical root");
}

void TestResolveWithinRootRejectsSymlinkParentEscape() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path outside = temp_dir.path() / "outside";
  std::filesystem::create_directories(root);
  std::filesystem::create_directories(outside);

  std::error_code link_error;
  std::filesystem::create_directory_symlink(outside, root / "link", link_error);
  Expect(!link_error, "fixture should create an escaping directory symlink");

  // The leaf does not exist yet (a create), but the parent component is a symlink whose
  // target is outside the root. Containment must still fail closed.
  Expect(!util::ResolveWithinRoot(root / "link" / "evil.txt", root).has_value(),
         "a missing leaf reached through a symlinked parent escaping the root is rejected");
}

void TestContainPathRejectsMissingLeafThroughSymlinkParent() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path outside = temp_dir.path() / "outside";
  std::filesystem::create_directories(root);
  std::filesystem::create_directories(outside);

  std::error_code link_error;
  std::filesystem::create_directory_symlink(outside, root / "link", link_error);
  Expect(!link_error, "fixture should create an escaping directory symlink");

  const std::vector<std::filesystem::path> roots = {root};

  // Regression: a not-yet-existing write target reached through a symlinked parent
  // directory that escapes the sandbox must be denied (previously allowed on create).
  Expect(!path_interop::ContainPath(roots, root / "link" / "implant").has_value(),
         "ContainPath should reject a missing leaf escaping via a symlinked parent");

  // A normal missing in-root leaf (a legitimate create) is still allowed.
  Expect(path_interop::ContainPath(roots, root / "src" / "fresh.txt").has_value(),
         "ContainPath should still allow a missing in-root write target");

  // An existing in-root file resolves to its canonical form within the root.
  WriteFile(root / "real.txt", "data\n");
  const std::optional<std::filesystem::path> existing =
      path_interop::ContainPath(roots, root / "real.txt");
  Expect(existing.has_value(), "ContainPath should allow an existing in-root file");
  Expect(util::PathWithinRoot(*existing, std::filesystem::canonical(root)),
         "an allowed existing path should resolve within the canonical root");

  // A lexical `..` escape is rejected by Tier 1 without touching the filesystem.
  Expect(!path_interop::ContainPath(roots, root / ".." / "outside" / "x").has_value(),
         "ContainPath should reject a lexical parent escape");
}

}  // namespace

void RegisterPathContainmentTests(std::vector<TestCase>& tests) {
  AddTest(tests, "PathContainment/PathWithinRootRejectsParentEscapesAndEmptyRoots",
          TestPathWithinRootRejectsParentEscapesAndEmptyRoots);
  AddTest(tests, "PathContainment/ResolveWithinRootAllowsMissingInRootLeaf",
          TestResolveWithinRootAllowsMissingInRootLeaf);
  AddTest(tests, "PathContainment/ResolveWithinRootRejectsSymlinkParentEscape",
          TestResolveWithinRootRejectsSymlinkParentEscape);
  AddTest(tests, "PathContainment/ContainPathRejectsMissingLeafThroughSymlinkParent",
          TestContainPathRejectsMissingLeafThroughSymlinkParent);
}

}  // namespace microide::tests
