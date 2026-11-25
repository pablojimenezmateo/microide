#include "TestSupport.h"

#include "plugin/PluginPathInterop.h"

#include <array>
#include <filesystem>
#include <system_error>

namespace microide::tests {
namespace {

using microide::plugin::path_interop::ContainPath;
using microide::plugin::path_interop::ResolveRuntimePath;

void TestResolveRuntimePathContract() {
  const std::filesystem::path root = "/tmp/project";

  Expect(ResolveRuntimePath(root, {}).empty(), "an empty path resolves to empty");

  Expect(ResolveRuntimePath(root, "src/main.cpp") ==
             std::filesystem::path("/tmp/project/src/main.cpp"),
         "a relative path is joined to the project root");
  Expect(ResolveRuntimePath(root, "./a/../b") == std::filesystem::path("/tmp/project/b"),
         "the join is lexically normalized");

  Expect(ResolveRuntimePath(root, "/etc/passwd") == std::filesystem::path("/etc/passwd"),
         "an absolute path resolves to itself, so containment sees the real target "
         "instead of a path rebased under the project");

  Expect(ResolveRuntimePath({}, "a/../b") == std::filesystem::path("b"),
         "with no project root a relative path is normalized and left relative");
}

void TestResolveThenContainRejectsEscapes() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  std::error_code ec;
  std::filesystem::create_directories(root / "src", ec);
  const std::array<std::filesystem::path, 1> roots{root};

  const auto resolve_and_contain = [&](const std::filesystem::path& requested) {
    return ContainPath(roots, ResolveRuntimePath(root, requested));
  };

  Expect(resolve_and_contain("src/main.cpp").has_value(),
         "a relative path inside the project survives resolve-then-contain");
  Expect(resolve_and_contain("./src/./main.cpp").has_value(),
         "redundant path elements do not break containment");

  Expect(!resolve_and_contain("../outside.txt").has_value(),
         "a relative `..` escape is rejected after resolution");
  Expect(!resolve_and_contain("src/../../outside.txt").has_value(),
         "a `..` escape buried mid-path is rejected");
  Expect(!resolve_and_contain("/etc/passwd").has_value(),
         "an absolute path outside the project is rejected -- and note it is rejected "
         "because the resolver left it absolute rather than rebasing it under the root");
  Expect(!resolve_and_contain({}).has_value(), "an empty request is rejected");
}

void TestContainPathAllowsPlainChildWithinRoot() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  std::error_code ec;
  std::filesystem::create_directories(root, ec);
  const std::array<std::filesystem::path, 1> roots{root};

  const auto contained = ContainPath(roots, root / "new.txt");
  Expect(contained.has_value(), "a plain child of the root is contained even if it does not exist");
}

void TestContainPathRejectsMissingLeafUnderSymlinkedParent() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path base = temp_dir.path();
  const std::filesystem::path root = base / "project";
  const std::filesystem::path outside = base / "outside";
  std::error_code ec;
  std::filesystem::create_directories(root, ec);
  std::filesystem::create_directories(outside, ec);

  std::filesystem::create_directory_symlink(outside, root / "link", ec);
  Expect(!ec, "symlink fixture created");

  const std::array<std::filesystem::path, 1> roots{root};

  const auto escaped = ContainPath(roots, root / "link" / "new.txt");
  Expect(!escaped.has_value(),
         "a missing leaf under a symlinked-out parent must be rejected (containment escape)");

  std::filesystem::create_directory_symlink(root / "real", root / "inner", ec);
  std::filesystem::create_directories(root / "real", ec);
  const auto inside = ContainPath(roots, root / "inner" / "note.txt");
  Expect(inside.has_value(), "a symlink resolving back inside the project stays contained");
}

void TestContainPathRejectsLexicalEscape() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  std::error_code ec;
  std::filesystem::create_directories(root, ec);
  const std::array<std::filesystem::path, 1> roots{root};
  const auto escaped = ContainPath(roots, (root / ".." / "secret.txt").lexically_normal());
  Expect(!escaped.has_value(), "a '..' lexical escape is rejected");
}

void TestContainPathRejectsASiblingSharingTheRootsNamePrefix() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path sibling = temp_dir.path() / "project-evil";
  std::error_code ec;
  std::filesystem::create_directories(root, ec);
  std::filesystem::create_directories(sibling, ec);
  const std::array<std::filesystem::path, 1> roots{root};

  Expect(!ContainPath(roots, sibling / "stolen.txt").has_value(),
         "a sibling whose name merely starts with the root's name must be rejected");
  Expect(!ContainPath(roots, sibling).has_value(),
         "the sibling directory itself must be rejected");
  const std::filesystem::path suffixed = temp_dir.path() / "projectx";
  std::filesystem::create_directories(suffixed, ec);
  Expect(!ContainPath(roots, suffixed / "stolen.txt").has_value(),
         "a directory whose name extends the root's without a separator must be rejected");
}

void TestContainPathFollowsSymlinkChainsOutOfTheRoot() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path outside = temp_dir.path() / "outside";
  std::error_code ec;
  std::filesystem::create_directories(root, ec);
  std::filesystem::create_directories(outside, ec);

  std::filesystem::create_directory_symlink(root / "hop2", root / "hop1", ec);
  std::filesystem::create_directory_symlink(outside, root / "hop2", ec);
  Expect(!ec, "symlink chain fixture created");

  const std::array<std::filesystem::path, 1> roots{root};
  Expect(!ContainPath(roots, root / "hop1" / "stolen.txt").has_value(),
         "a two-hop symlink chain leaving the root must be rejected");
  Expect(!ContainPath(roots, root / "hop1").has_value(),
         "the chained symlink directory itself must be rejected");
}

void TestContainPathHandlesDotDotShapes() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  std::error_code ec;
  std::filesystem::create_directories(root / "a" / "b", ec);
  const std::array<std::filesystem::path, 1> roots{root};

  Expect(!ContainPath(roots, root / ".." / "escaped.txt").has_value(),
         "a single .. above the root escapes");
  Expect(!ContainPath(roots, root / "a" / ".." / ".." / "escaped.txt").has_value(),
         "two .. from one level down escapes");
  Expect(ContainPath(roots, root / "a" / ".." / "b.txt").has_value(),
         "a .. that cancels within the root stays contained");
  Expect(ContainPath(roots, root / "a" / "b" / ".." / ".." / "c.txt").has_value(),
         "..s that cancel back to the root stay contained");
  Expect(ContainPath(roots, root / "." / "a" / "." / "file.txt").has_value(),
         "'.' components are not an escape");
  Expect(ContainPath(roots, root / ".." / "project" / "file.txt").has_value(),
         "leaving and re-entering the root by name resolves inside");
}

void TestContainPathEdgesOfTheRootItself() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  std::error_code ec;
  std::filesystem::create_directories(root, ec);
  const std::array<std::filesystem::path, 1> roots{root};

  Expect(ContainPath(roots, root).has_value(), "the root is contained in itself");
  Expect(!ContainPath(roots, temp_dir.path()).has_value(),
         "the root's parent is not contained");
  Expect(!ContainPath(roots, std::filesystem::path("/etc/passwd")).has_value(),
         "an unrelated absolute path is not contained");
  Expect(!ContainPath(roots, std::filesystem::path("/")).has_value(),
         "the filesystem root is not contained");
  Expect(!ContainPath(roots, std::filesystem::path()).has_value(),
         "an empty path is not contained");
}

void TestContainPathWithNoUsableRootDeniesEverything() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  std::error_code ec;
  std::filesystem::create_directories(root, ec);

  const std::span<const std::filesystem::path> none;
  Expect(!ContainPath(none, root / "file.txt").has_value(),
         "no roots at all must deny, not allow");

  const std::array<std::filesystem::path, 2> empty_roots{std::filesystem::path(),
                                                         std::filesystem::path()};
  Expect(!ContainPath(empty_roots, root / "file.txt").has_value(),
         "roots that are all empty must deny, not allow");
}

void TestContainPathAcceptsAnyOfSeveralRoots() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path first = temp_dir.path() / "one";
  const std::filesystem::path second = temp_dir.path() / "two";
  const std::filesystem::path neither = temp_dir.path() / "three";
  std::error_code ec;
  std::filesystem::create_directories(first, ec);
  std::filesystem::create_directories(second, ec);
  std::filesystem::create_directories(neither, ec);

  const std::array<std::filesystem::path, 3> roots{first, std::filesystem::path(), second};
  Expect(ContainPath(roots, first / "a.txt").has_value(), "contained in the first root");
  Expect(ContainPath(roots, second / "b.txt").has_value(), "contained in the last root");
  Expect(!ContainPath(roots, neither / "c.txt").has_value(),
         "a sibling of both roots is contained in neither -- an empty root is not a wildcard");
}

void TestContainPathRejectsSymlinkedFileAndDanglingEscapes() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path outside = temp_dir.path() / "outside";
  std::error_code ec;
  std::filesystem::create_directories(root, ec);
  std::filesystem::create_directories(outside, ec);
  WriteFile(outside / "secret.txt", "secret\n");

  const std::array<std::filesystem::path, 1> roots{root};

  std::filesystem::create_symlink(outside / "secret.txt", root / "leak.txt", ec);
  Expect(!ec, "file symlink fixture created");
  Expect(!ContainPath(roots, root / "leak.txt").has_value(),
         "a file symlink pointing out of the root must be rejected");

  std::filesystem::create_symlink(outside / "missing.txt", root / "dangling.txt", ec);
  Expect(!ec, "dangling symlink fixture created");
  Expect(!ContainPath(roots, root / "dangling.txt").has_value(),
         "a dangling symlink pointing out of the root must be rejected");
}

}

void RegisterPluginPathContainmentTests(std::vector<TestCase>& tests) {
  AddTest(tests, "PluginPathContainment/RejectsASiblingSharingTheRootsNamePrefix",
          TestContainPathRejectsASiblingSharingTheRootsNamePrefix);
  AddTest(tests, "PluginPathContainment/FollowsSymlinkChainsOutOfTheRoot",
          TestContainPathFollowsSymlinkChainsOutOfTheRoot);
  AddTest(tests, "PluginPathContainment/HandlesDotDotShapes",
          TestContainPathHandlesDotDotShapes);
  AddTest(tests, "PluginPathContainment/EdgesOfTheRootItself",
          TestContainPathEdgesOfTheRootItself);
  AddTest(tests, "PluginPathContainment/WithNoUsableRootDeniesEverything",
          TestContainPathWithNoUsableRootDeniesEverything);
  AddTest(tests, "PluginPathContainment/AcceptsAnyOfSeveralRoots",
          TestContainPathAcceptsAnyOfSeveralRoots);
  AddTest(tests, "PluginPathContainment/RejectsSymlinkedFileAndDanglingEscapes",
          TestContainPathRejectsSymlinkedFileAndDanglingEscapes);
  AddTest(tests, "PluginPathContainment/ResolveRuntimePathContract",
          TestResolveRuntimePathContract);
  AddTest(tests, "PluginPathContainment/ResolveThenContainRejectsEscapes",
          TestResolveThenContainRejectsEscapes);
  AddTest(tests, "PluginPathContainment/AllowsPlainChildWithinRoot",
          TestContainPathAllowsPlainChildWithinRoot);
  AddTest(tests, "PluginPathContainment/RejectsMissingLeafUnderSymlinkedParent",
          TestContainPathRejectsMissingLeafUnderSymlinkedParent);
  AddTest(tests, "PluginPathContainment/RejectsLexicalEscape",
          TestContainPathRejectsLexicalEscape);
}

}
