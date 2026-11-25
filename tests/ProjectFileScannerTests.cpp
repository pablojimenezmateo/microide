#include "TestSupport.h"

#include "project/ProjectFileScanner.h"

#include <algorithm>
#include <filesystem>
#include <string>
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

void TestScannerDoesNotFollowRootEscapingSymlink() {
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
}

void TestScannerFollowsInProjectSymlink() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path project = temp_dir.path() / "project";
  const std::filesystem::path sub = project / "sub";
  std::filesystem::create_directories(sub);
  WriteFile(sub / "linked.txt", "x");

  std::error_code ec;
  std::filesystem::create_directory_symlink(sub, project / "alias", ec);
  Expect(!ec, "in-project symlink fixture should be created");

  const auto files = CollectProjectFiles(project, ProjectFileScanMode::ExcludeHidden);
  Expect(ContainsName(files, "linked.txt"), "in-project content should be indexed");
}

void TestScannerFollowsRootEscapingSymlinkWhenOptedIn() {
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
                                         true);
  Expect(ContainsName(files, "inside.txt"), "in-project files should still be indexed");
  Expect(ContainsName(files, "shared.txt"),
         "opting in should follow an out-of-root symlink target");
}

void TestScannerFollowsSymlinkToDotDotPrefixedInRootDir() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path project = temp_dir.path() / "project";
  const std::filesystem::path dotdot_dir = project / "..cache";
  std::filesystem::create_directories(dotdot_dir);
  WriteFile(dotdot_dir / "cached.txt", "c");

  std::error_code ec;
  std::filesystem::create_directory_symlink(dotdot_dir, project / "alias", ec);
  Expect(!ec, "in-project '..'-prefixed symlink fixture should be created");

  const auto files = CollectProjectFiles(project, ProjectFileScanMode::ExcludeHidden);
  Expect(ContainsName(files, "cached.txt"),
         "in-root '..'-prefixed directory content should be indexed");
}

void TestScannerSurvivesSymlinkCycle() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path project = temp_dir.path() / "project";
  std::filesystem::create_directories(project);
  WriteFile(project / "real.txt", "r");

  std::error_code ec;
  std::filesystem::create_directory_symlink(project / "loopb", project / "loopa", ec);
  Expect(!ec, "loop symlink 'loopa' should be created");
  std::filesystem::create_directory_symlink(project / "loopa", project / "loopb", ec);
  Expect(!ec, "loop symlink 'loopb' should be created");

  const auto files = CollectProjectFiles(project, ProjectFileScanMode::ExcludeHidden);
  Expect(ContainsName(files, "real.txt"),
         "a symlink cycle must not crash the scan or drop real files");
}

void TestScannerPrunesDefaultsAndUserExcludes() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "src" / "main.cpp", "int main() {}\n");
  WriteFile(root / ".svn" / "wc.db", "svn\n");
  WriteFile(root / "builds" / "artifact.o", "bin\n");
  WriteFile(root / "vendored" / "lib.c", "// v\n");

  const auto default_scan =
      CollectProjectFiles(root, ProjectFileScanMode::IncludeHidden, false);
  Expect(ContainsName(default_scan, "main.cpp"), "real sources should be collected");
  Expect(!ContainsName(default_scan, "wc.db"), "default rules should prune .svn/");
  Expect(!ContainsName(default_scan, "artifact.o"), "default rules should prune builds/");
  Expect(ContainsName(default_scan, "lib.c"),
         "a custom dir is collected until the user excludes it");

  const auto excluded_scan =
      CollectProjectFiles(root, ProjectFileScanMode::IncludeHidden, false, {"vendored/"});
  Expect(!ContainsName(excluded_scan, "lib.c"),
         "a user exclude glob should prune the custom directory");
  Expect(ContainsName(excluded_scan, "main.cpp"), "real sources remain after exclusion");
}

void TestScannerExcludeHiddenDropsDotNamedEntries() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "visible.txt", "v\n");
  WriteFile(root / ".hidden.txt", "h\n");
  WriteFile(root / "src" / "nested.cpp", "int nested() { return 1; }\n");
  WriteFile(root / ".hidden_dir" / "inside.txt", "i\n");
  WriteFile(root / "..cache" / "cached.txt", "c\n");

  const auto included = CollectProjectFiles(root, ProjectFileScanMode::IncludeHidden);
  Expect(ContainsName(included, "visible.txt") && ContainsName(included, ".hidden.txt") &&
             ContainsName(included, "inside.txt") && ContainsName(included, "cached.txt"),
         "IncludeHidden should list dot-named files, dot-named directories and their contents");

  const auto excluded = CollectProjectFiles(root, ProjectFileScanMode::ExcludeHidden);
  Expect(ContainsName(excluded, "visible.txt"), "ExcludeHidden keeps ordinary files");
  Expect(ContainsName(excluded, "nested.cpp"), "ExcludeHidden descends ordinary directories");
  Expect(!ContainsName(excluded, ".hidden.txt"), "ExcludeHidden drops a dot-named file");
  Expect(!ContainsName(excluded, "inside.txt"),
         "ExcludeHidden prunes the subtree under a dot-named directory");
  Expect(!ContainsName(excluded, "cached.txt"),
         "ExcludeHidden treats a '..'-prefixed directory name as hidden");
}

void TestScannerReportsEntryBudgetTruncation() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  for (int i = 0; i < 8; ++i) {
    WriteFile(root / ("f" + std::to_string(i) + ".txt"), "x\n");
  }

  microide::project::ProjectFileScanStatus status;
  const auto complete_scan = CollectProjectFiles(root, ProjectFileScanMode::IncludeHidden, false,
                                                 {}, &status);
  Expect(!status.incomplete(), "a scan under the entry budget reports complete");
  Expect(complete_scan.size() == 8, "a complete scan lists every file");

  microide::project::ProjectFileScanStatus budget_status;
  const auto truncated_scan = CollectProjectFiles(
      root, ProjectFileScanMode::IncludeHidden, false, {}, &budget_status, 3);
  Expect(budget_status.truncated_by_budget,
         "a scan that exhausts the entry budget reports truncated_by_budget");
  Expect(budget_status.incomplete(), "budget truncation is an incomplete scan");
  Expect(!budget_status.permission_limited && !budget_status.error,
         "a pure budget truncation does not spuriously flag permission/error");
  Expect(truncated_scan.size() < complete_scan.size(),
         "a truncated scan returns only a prefix of the tree");
}

void TestScannerReportsPermissionLimited() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "readable.txt", "ok\n");
  const std::filesystem::path locked = root / "locked";
  WriteFile(locked / "secret.txt", "no\n");

  std::error_code ec;
  std::filesystem::permissions(locked, std::filesystem::perms::none, ec);
  Expect(!ec, "permission fixture should apply");

  std::error_code probe_ec;
  std::filesystem::directory_iterator probe(locked, probe_ec);
  const bool actually_locked = static_cast<bool>(probe_ec);

  microide::project::ProjectFileScanStatus status;
  const auto files =
      CollectProjectFiles(root, ProjectFileScanMode::IncludeHidden, false, {}, &status);
  Expect(ContainsName(files, "readable.txt"), "readable siblings are still indexed");
  if (actually_locked) {
    Expect(status.permission_limited,
           "an unreadable directory must be reported as permission_limited");
    Expect(status.incomplete(), "permission-limited is an incomplete scan");
    Expect(!ContainsName(files, "secret.txt"), "an unreadable dir's contents are not indexed");
  }
  std::filesystem::permissions(locked, std::filesystem::perms::owner_all, ec);
}

}

void RegisterProjectFileScannerTests(std::vector<TestCase>& tests) {
  AddTest(tests, "ProjectFileScanner/ExcludeHiddenDropsDotNamedEntries",
          TestScannerExcludeHiddenDropsDotNamedEntries);
  AddTest(tests, "ProjectFileScanner/ReportsEntryBudgetTruncation",
          TestScannerReportsEntryBudgetTruncation);
  AddTest(tests, "ProjectFileScanner/ReportsPermissionLimited",
          TestScannerReportsPermissionLimited);
  AddTest(tests, "ProjectFileScanner/PrunesDefaultsAndUserExcludes",
          TestScannerPrunesDefaultsAndUserExcludes);
  AddTest(tests, "ProjectFileScanner/DoesNotFollowRootEscapingSymlink",
          TestScannerDoesNotFollowRootEscapingSymlink);
  AddTest(tests, "ProjectFileScanner/FollowsInProjectSymlink",
          TestScannerFollowsInProjectSymlink);
  AddTest(tests, "ProjectFileScanner/FollowsRootEscapingSymlinkWhenOptedIn",
          TestScannerFollowsRootEscapingSymlinkWhenOptedIn);
  AddTest(tests, "ProjectFileScanner/FollowsSymlinkToDotDotPrefixedInRootDir",
          TestScannerFollowsSymlinkToDotDotPrefixedInRootDir);
  AddTest(tests, "ProjectFileScanner/SurvivesSymlinkCycle",
          TestScannerSurvivesSymlinkCycle);
}

}
