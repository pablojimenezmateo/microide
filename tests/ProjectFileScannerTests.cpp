#include "TestSupport.h"

#include "project/ProjectFileScanner.h"

#include <algorithm>
#include <filesystem>
#include <system_error>

namespace microide::tests {
namespace {

using microide::project::CollectProjectFiles;
using microide::project::ProjectFileScanMode;

bool ContainsName(const std::vector<std::filesystem::path>& files, std::string_view name) {
  return std::any_of(files.begin(), files.end(), [&](const std::filesystem::path& p) {
    return p.filename().string() == name;
  });
}

// A directory symlink whose target escapes the project root must NOT be followed:
// otherwise the scanner walks and indexes files outside the project (a
// filesystem-wide DoS and an information-disclosure/traversal escape, since the
// opened project is untrusted).
void TestScannerDoesNotFollowRootEscapingSymlink() {
#if defined(_WIN32)
  return;  // POSIX symlink semantics
#else
  TemporaryDirectory temp_dir;
  const std::filesystem::path project = temp_dir.path() / "project";
  const std::filesystem::path outside = temp_dir.path() / "outside";
  std::filesystem::create_directories(project);
  std::filesystem::create_directories(outside);
  WriteFile(project / "inside.txt", "in");
  WriteFile(outside / "secret.txt", "out");

  std::error_code ec;
  std::filesystem::create_directory_symlink(outside, project / "escape", ec);
  Expect(!ec, "symlink fixture should be created");

  const auto files = CollectProjectFiles(project, ProjectFileScanMode::ExcludeHidden);
  Expect(ContainsName(files, "inside.txt"), "in-project files should still be indexed");
  Expect(!ContainsName(files, "secret.txt"),
         "a root-escaping symlink target must not be indexed");
#endif
}

// A symlink that points to a directory *within* the project is legitimate and
// should still be followed (containment allows it) — the guard only blocks
// escapes and cycles, not all symlinks.
void TestScannerFollowsInProjectSymlink() {
#if defined(_WIN32)
  return;
#else
  TemporaryDirectory temp_dir;
  const std::filesystem::path project = temp_dir.path() / "project";
  const std::filesystem::path sub = project / "sub";
  std::filesystem::create_directories(sub);
  WriteFile(sub / "linked.txt", "x");

  std::error_code ec;
  std::filesystem::create_directory_symlink(sub, project / "alias", ec);
  Expect(!ec, "in-project symlink fixture should be created");

  const auto files = CollectProjectFiles(project, ProjectFileScanMode::ExcludeHidden);
  // The real path is always indexed; the alias may also surface. The key
  // assertion is that in-project symlinks do not crash or get wholesale dropped.
  Expect(ContainsName(files, "linked.txt"), "in-project content should be indexed");
#endif
}

// Opting in (project.follow_out_of_root_symlinks = true) restores following an
// out-of-root symlink target, for monorepos / symlinked dependencies. Cycle
// detection still applies; only the containment guard is relaxed.
void TestScannerFollowsRootEscapingSymlinkWhenOptedIn() {
#if defined(_WIN32)
  return;
#else
  TemporaryDirectory temp_dir;
  const std::filesystem::path project = temp_dir.path() / "project";
  const std::filesystem::path outside = temp_dir.path() / "outside";
  std::filesystem::create_directories(project);
  std::filesystem::create_directories(outside);
  WriteFile(project / "inside.txt", "in");
  WriteFile(outside / "shared.txt", "out");

  std::error_code ec;
  std::filesystem::create_directory_symlink(outside, project / "vendor", ec);
  Expect(!ec, "symlink fixture should be created");

  const auto files = CollectProjectFiles(project, ProjectFileScanMode::ExcludeHidden,
                                         /*follow_out_of_root_symlinks=*/true);
  Expect(ContainsName(files, "inside.txt"), "in-project files should still be indexed");
  Expect(ContainsName(files, "shared.txt"),
         "opting in should follow an out-of-root symlink target");
#endif
}

// Regression: an in-root directory whose name begins with ".." (e.g. "..cache")
// must not be misclassified as a root escape. The containment check compares the
// first path *component*, not a raw string prefix.
void TestScannerFollowsSymlinkToDotDotPrefixedInRootDir() {
#if defined(_WIN32)
  return;
#else
  TemporaryDirectory temp_dir;
  const std::filesystem::path project = temp_dir.path() / "project";
  const std::filesystem::path dotdot_dir = project / "..cache";
  std::filesystem::create_directories(dotdot_dir);
  WriteFile(dotdot_dir / "cached.txt", "c");

  std::error_code ec;
  std::filesystem::create_directory_symlink(dotdot_dir, project / "alias", ec);
  Expect(!ec, "in-project '..'-prefixed symlink fixture should be created");

  // Default containment (opt-in off): the target is in-root, so it must be
  // followed despite the leading ".." in the directory name.
  const auto files = CollectProjectFiles(project, ProjectFileScanMode::ExcludeHidden);
  Expect(ContainsName(files, "cached.txt"),
         "in-root '..'-prefixed directory content should be indexed");
#endif
}

}  // namespace

void RegisterProjectFileScannerTests(std::vector<TestCase>& tests) {
  AddTest(tests, "ProjectFileScanner/DoesNotFollowRootEscapingSymlink",
          TestScannerDoesNotFollowRootEscapingSymlink);
  AddTest(tests, "ProjectFileScanner/FollowsInProjectSymlink",
          TestScannerFollowsInProjectSymlink);
  AddTest(tests, "ProjectFileScanner/FollowsRootEscapingSymlinkWhenOptedIn",
          TestScannerFollowsRootEscapingSymlinkWhenOptedIn);
  AddTest(tests, "ProjectFileScanner/FollowsSymlinkToDotDotPrefixedInRootDir",
          TestScannerFollowsSymlinkToDotDotPrefixedInRootDir);
}

}  // namespace microide::tests
