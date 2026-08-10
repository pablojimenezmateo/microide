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

// The per-entry hot path derives the relative text, the parent directory and the
// ancestor chain from views into the candidate's own text rather than from
// `path` operations (TD-2026-08-10-174). Those derivations are what every
// .gitignore rule is matched against, so this pins the cases where a view-based
// derivation could differ from the path-based one it replaced: a deep ancestor
// that is itself ignored, an unnormalized input, and a path escaping the root.
void TestProjectTraversalFilterDeepAncestorAndUnnormalizedInput() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  std::filesystem::create_directories(root / "a" / "b" / "c");
  WriteFile(root / ".gitignore", "/b/\n");

  ProjectTraversalFilter filter(root);

  Expect(filter.Includes(root / "a" / "b" / "c" / "deep.cpp", PathType::RegularFile),
         "a root-anchored '/b/' rule must not match the nested a/b, so the file stays included");

  WriteFile(root / ".gitignore", "b/\n");
  ProjectTraversalFilter nested_filter(root);
  Expect(!nested_filter.Includes(root / "a" / "b" / "c" / "deep.cpp", PathType::RegularFile),
         "a file three levels under an ignored ancestor directory is excluded — the ancestor "
         "walk must reach a/b, not stop at the immediate parent");

  ProjectTraversalFilter unnormalized_filter(root);
  Expect(!unnormalized_filter.Includes(root / "a" / "." / "b" / "c" / "deep.cpp",
                                       PathType::RegularFile),
         "an unnormalized spelling of the same path reaches the same verdict");
  Expect(unnormalized_filter.Includes(root / "a" / "x" / ".." / "keep.cpp",
                                      PathType::RegularFile),
         "an unnormalized spelling of an included path is still included");

  Expect(!unnormalized_filter.Includes(temp_dir.path() / "outside.cpp", PathType::RegularFile),
         "a sibling of the root escapes the project boundary");
  Expect(!unnormalized_filter.Includes(root / ".." / "outside.cpp", PathType::RegularFile),
         "a path climbing out of the root via .. escapes the project boundary");
  Expect(!unnormalized_filter.Includes(std::filesystem::path(root.generic_string() + "ile") /
                                           "x.cpp",
                                       PathType::RegularFile),
         "a sibling directory sharing the root's string prefix is not inside it");
}

}  // namespace

void RegisterProjectTraversalFilterTests(std::vector<TestCase>& tests) {
  AddTest(tests, "ProjectTraversalFilter/DeepAncestorAndUnnormalizedInput",
          TestProjectTraversalFilterDeepAncestorAndUnnormalizedInput);
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
