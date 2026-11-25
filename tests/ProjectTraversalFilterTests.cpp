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

void TestProjectTraversalFilterAncestorMemoIsPerDirectory() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  std::filesystem::create_directories(root / "keep" / "inner");
  std::filesystem::create_directories(root / "vendor" / "inner");
  std::filesystem::create_directories(root / "keep" / "vendor");
  WriteFile(root / ".gitignore", "vendor/\n");

  ProjectTraversalFilter interleaved(root);
  Expect(interleaved.Includes(root / "keep" / "inner" / "a.cpp", PathType::RegularFile),
         "a file under a kept directory is included");
  Expect(!interleaved.Includes(root / "vendor" / "inner" / "a.cpp", PathType::RegularFile),
         "a file under an ignored ancestor is excluded even though its own name is fine");
  Expect(interleaved.Includes(root / "keep" / "inner" / "b.cpp", PathType::RegularFile),
         "the second file in the kept directory reads the memo, not the sibling's answer");
  Expect(!interleaved.Includes(root / "vendor" / "inner" / "b.cpp", PathType::RegularFile),
         "the second file under the ignored ancestor is still excluded");
  Expect(!interleaved.Includes(root / "keep" / "vendor" / "a.cpp", PathType::RegularFile),
         "an ignored directory nested under a kept one still excludes its files");

  ProjectTraversalFilter reversed(root);
  Expect(!reversed.Includes(root / "keep" / "vendor" / "a.cpp", PathType::RegularFile),
         "verdict is independent of query order (nested ignored directory)");
  Expect(!reversed.Includes(root / "vendor" / "inner" / "b.cpp", PathType::RegularFile),
         "verdict is independent of query order (ignored ancestor)");
  Expect(reversed.Includes(root / "keep" / "inner" / "b.cpp", PathType::RegularFile),
         "verdict is independent of query order (kept directory)");

  ProjectTraversalFilter directories(root);
  Expect(directories.Includes(root / "keep" / "inner", PathType::Directory),
         "a kept directory is included");
  Expect(!directories.Includes(root / "vendor" / "inner", PathType::Directory),
         "a directory under an ignored ancestor is excluded");
  Expect(!directories.Includes(root / "vendor", PathType::Directory),
         "the ignored directory itself is excluded");
}

}

void RegisterProjectTraversalFilterTests(std::vector<TestCase>& tests) {
  AddTest(tests, "ProjectTraversalFilter/AncestorMemoIsPerDirectory",
          TestProjectTraversalFilterAncestorMemoIsPerDirectory);
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

}
