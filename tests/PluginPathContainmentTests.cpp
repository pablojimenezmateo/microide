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

}  // namespace

void RegisterPluginPathContainmentTests(std::vector<TestCase>& tests) {
  AddTest(tests, "PluginPathContainment/AllowsPlainChildWithinRoot",
          TestContainPathAllowsPlainChildWithinRoot);
  AddTest(tests, "PluginPathContainment/RejectsMissingLeafUnderSymlinkedParent",
          TestContainPathRejectsMissingLeafUnderSymlinkedParent);
  AddTest(tests, "PluginPathContainment/RejectsLexicalEscape",
          TestContainPathRejectsLexicalEscape);
}

}  // namespace microide::tests
