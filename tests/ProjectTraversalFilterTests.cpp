#include "TestSupport.h"

#include "project/ProjectTraversalFilter.h"

#include <filesystem>

namespace microide::tests {
namespace {

using microide::platform::PathType;
using microide::project::ProjectTraversalFilter;

void TestProjectTraversalFilterExcludesVcsAndBuildDirs() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  std::filesystem::create_directories(root);

  ProjectTraversalFilter filter(root);

  Expect(filter.Includes(root, PathType::Directory), "the project root is always included");
  Expect(filter.ShouldSkipDirectory(root / ".svn"), ".svn/ should be skipped");
  Expect(filter.ShouldSkipDirectory(root / ".git"), ".git/ should be skipped");
  Expect(filter.ShouldSkipDirectory(root / "builds"), "builds/ should be skipped");
  Expect(filter.ShouldSkipDirectory(root / "src" / "node_modules"),
         "a nested node_modules should be skipped");
  Expect(!filter.ShouldSkipDirectory(root / "src"), "an ordinary source dir is not skipped");
  Expect(filter.Includes(root / "src" / "main.cpp", PathType::RegularFile),
         "an ordinary source file is included");
}

void TestProjectTraversalFilterHonorsNestedGitignore() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  std::filesystem::create_directories(root / "sub");
  WriteFile(root / "sub" / ".gitignore", "*.gen\n");

  ProjectTraversalFilter filter(root);

  Expect(!filter.Includes(root / "sub" / "code.gen", PathType::RegularFile),
         "a nested .gitignore rule should exclude matching files under its dir");
  Expect(filter.Includes(root / "sub" / "code.cpp", PathType::RegularFile),
         "unmatched files under the nested dir remain included");
  Expect(filter.Includes(root / "top.gen", PathType::RegularFile),
         "the nested rule should not apply outside its directory");
}

void TestProjectTraversalFilterUserExcludesAndReinclude() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  std::filesystem::create_directories(root);

  ProjectTraversalFilter filter(root, {"vendored/", "!build/"});

  Expect(filter.ShouldSkipDirectory(root / "vendored"), "a user exclude glob skips its dir");
  Expect(!filter.ShouldSkipDirectory(root / "build"),
         "a '!build/' exclude re-includes the default-ignored build dir");
}

// The extraction must preserve the trailing-separator root normalization: a root
// passed with a trailing slash must still terminate the ancestor walk correctly.
void TestProjectTraversalFilterTrailingSeparatorRoot() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path base = temp_dir.path() / "repo";
  std::filesystem::create_directories(base / "src");
  const std::filesystem::path root_with_slash = base.generic_string() + "/";

  ProjectTraversalFilter filter(root_with_slash);

  Expect(filter.Includes(base / "src" / "main.cpp", PathType::RegularFile),
         "a trailing-slash root should still include ordinary files");
  Expect(filter.ShouldSkipDirectory(base / ".git"),
         "a trailing-slash root should still skip VCS metadata");
}

}  // namespace

void RegisterProjectTraversalFilterTests(std::vector<TestCase>& tests) {
  AddTest(tests, "ProjectTraversalFilter/ExcludesVcsAndBuildDirs",
          TestProjectTraversalFilterExcludesVcsAndBuildDirs);
  AddTest(tests, "ProjectTraversalFilter/HonorsNestedGitignore",
          TestProjectTraversalFilterHonorsNestedGitignore);
  AddTest(tests, "ProjectTraversalFilter/UserExcludesAndReinclude",
          TestProjectTraversalFilterUserExcludesAndReinclude);
  AddTest(tests, "ProjectTraversalFilter/TrailingSeparatorRoot",
          TestProjectTraversalFilterTrailingSeparatorRoot);
}

}  // namespace microide::tests
