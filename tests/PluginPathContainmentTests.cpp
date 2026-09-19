// plugin::path_interop::ContainPath tests — the sandbox filesystem containment gate.
//
// Focus: a plugin with project-scoped write capability must not escape the project via
// a symlinked parent directory, even when the write target itself does not exist yet.

#include "TestSupport.h"

#include "plugin/PluginPathInterop.h"

#include <array>
#include <filesystem>
#include <system_error>

namespace microide::tests {
namespace {

using microide::plugin::path_interop::ContainPath;
using microide::plugin::path_interop::ResolveRuntimePath;

// ResolveRuntimePath is what PRODUCES the path ContainPath then gates, and it had
// no test naming it -- so the ten cases below all hand ContainPath a path built by
// hand, and the composition that actually runs in production was unchecked.
//
// Its contract has one clause that is easy to "simplify" wrongly: an ABSOLUTE
// path ignores project_root entirely. `project_root / path` would look
// equivalent and is not -- std::filesystem::operator/ REPLACES the whole left
// side when the right side is absolute, which is the exact shape that let a
// theme `include` name any file on the system (TD-2026-09-07-292). Here the
// branch is explicit; this pins that it stays so.
void TestResolveRuntimePathContract() {
  const std::filesystem::path root = "/tmp/project";

  Expect(ResolveRuntimePath(root, {}).empty(), "an empty path resolves to empty");

  // Relative: joined to the root and normalized.
  Expect(ResolveRuntimePath(root, "src/main.cpp") ==
             std::filesystem::path("/tmp/project/src/main.cpp"),
         "a relative path is joined to the project root");
  Expect(ResolveRuntimePath(root, "./a/../b") == std::filesystem::path("/tmp/project/b"),
         "the join is lexically normalized");

  // Absolute: the root is ignored, NOT prefixed.
  Expect(ResolveRuntimePath(root, "/etc/passwd") == std::filesystem::path("/etc/passwd"),
         "an absolute path resolves to itself, so containment sees the real target "
         "instead of a path rebased under the project");

  // No root: nothing to join to, so the path is only normalized.
  Expect(ResolveRuntimePath({}, "a/../b") == std::filesystem::path("b"),
         "with no project root a relative path is normalized and left relative");
}

// The composition that runs in production: resolve, then contain. Each hostile
// shape has to be rejected AFTER going through the resolver, not just when a
// test hands ContainPath a pre-built absolute path.
void TestResolveThenContainRejectsEscapes() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  std::error_code ec;
  std::filesystem::create_directories(root / "src", ec);
  const std::array<std::filesystem::path, 1> roots{root};

  const auto resolve_and_contain = [&](const std::filesystem::path& requested) {
    return ContainPath(roots, ResolveRuntimePath(root, requested));
  };

  // Legitimate: a relative path inside the project.
  Expect(resolve_and_contain("src/main.cpp").has_value(),
         "a relative path inside the project survives resolve-then-contain");
  Expect(resolve_and_contain("./src/./main.cpp").has_value(),
         "redundant path elements do not break containment");

  // Hostile shapes, each rejected.
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

  // An ordinary, not-yet-created file directly under the root stays contained.
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

  // project/link -> ../outside (a symlink escaping the project).
  std::filesystem::create_directory_symlink(outside, root / "link", ec);
  Expect(!ec, "symlink fixture created");

  const std::array<std::filesystem::path, 1> roots{root};

  // The escape target does NOT exist yet: this is the regression. The lexical check
  // passes (no ".."), but the existing parent `link` resolves outside the project, so
  // containment must reject it — otherwise the plugin write lands beside the symlink
  // target, outside the project.
  const auto escaped = ContainPath(roots, root / "link" / "new.txt");
  Expect(!escaped.has_value(),
         "a missing leaf under a symlinked-out parent must be rejected (containment escape)");

  // A symlink that stays inside the project remains allowed.
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

// ---- adversarial sweep -------------------------------------------------------
//
// Containment is the plugin sandbox's filesystem boundary, so the interesting
// inputs are the ones an escape would be written as, not the ones a well-behaved
// plugin produces. Each case below is a shape that has defeated a containment
// check somewhere.

// The classic string-prefix bug: a SIBLING directory whose name starts with the
// root's name. "/tmp/x/project-evil" begins with "/tmp/x/project", so any check
// built on starts_with accepts it, and the plugin writes outside the project.
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
  // Same shape one level deeper, and with no separator at the split point.
  const std::filesystem::path suffixed = temp_dir.path() / "projectx";
  std::filesystem::create_directories(suffixed, ec);
  Expect(!ContainPath(roots, suffixed / "stolen.txt").has_value(),
         "a directory whose name extends the root's without a separator must be rejected");
}

// A chain of symlinks, and a symlink whose target is itself a symlink out. One
// level of resolution is not enough.
void TestContainPathFollowsSymlinkChainsOutOfTheRoot() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path outside = temp_dir.path() / "outside";
  std::error_code ec;
  std::filesystem::create_directories(root, ec);
  std::filesystem::create_directories(outside, ec);

  // project/hop1 -> project/hop2 -> outside
  std::filesystem::create_directory_symlink(root / "hop2", root / "hop1", ec);
  std::filesystem::create_directory_symlink(outside, root / "hop2", ec);
  Expect(!ec, "symlink chain fixture created");

  const std::array<std::filesystem::path, 1> roots{root};
  Expect(!ContainPath(roots, root / "hop1" / "stolen.txt").has_value(),
         "a two-hop symlink chain leaving the root must be rejected");
  Expect(!ContainPath(roots, root / "hop1").has_value(),
         "the chained symlink directory itself must be rejected");
}

// `..` shapes: escaping, escaping-and-returning, and cancelling out. Only the
// ones that actually resolve outside may be rejected -- over-rejecting breaks
// ordinary relative paths a plugin legitimately builds.
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
  // Out and back in by name: resolves inside, so it is allowed.
  Expect(ContainPath(roots, root / ".." / "project" / "file.txt").has_value(),
         "leaving and re-entering the root by name resolves inside");
}

// The root itself, and paths that are not below any root at all.
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

// An empty or missing root list must deny everything, not allow everything --
// the "no capability configured" case has to fail closed.
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

// Several roots: a path contained in any one of them is allowed, and one empty
// root among real ones must not turn into a wildcard.
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

// A symlinked FILE (not directory) pointing out of the root, and a dangling
// symlink. Both are write targets a plugin can name.
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

  // Dangling: the target does not exist, but the link still resolves outside.
  std::filesystem::create_symlink(outside / "missing.txt", root / "dangling.txt", ec);
  Expect(!ec, "dangling symlink fixture created");
  Expect(!ContainPath(roots, root / "dangling.txt").has_value(),
         "a dangling symlink pointing out of the root must be rejected");
}

}  // namespace

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

}  // namespace microide::tests
