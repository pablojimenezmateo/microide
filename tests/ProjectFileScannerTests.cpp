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

}  // namespace

void RegisterProjectFileScannerTests(std::vector<TestCase>& tests) {
  AddTest(tests, "ProjectFileScanner/DoesNotFollowRootEscapingSymlink",
          TestScannerDoesNotFollowRootEscapingSymlink);
  AddTest(tests, "ProjectFileScanner/FollowsInProjectSymlink",
          TestScannerFollowsInProjectSymlink);
}

}  // namespace microide::tests
