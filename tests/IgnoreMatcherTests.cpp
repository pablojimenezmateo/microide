#include "TestSupport.h"

#include "project/IgnoreMatcher.h"

#include <filesystem>

namespace microide::tests {
namespace {

using microide::project::IgnoreMatcher;

// Behavior-preservation coverage for the string_view / precomputed base_prefix
// refactor of IgnoreMatcher::Rule::Matches: a nested .gitignore must apply only
// under its own directory, while a root rule applies everywhere.
void TestIgnoreMatcherNestedGitignoreBasePrefix() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  std::filesystem::create_directories(root / "sub");
  WriteFile(root / ".gitignore", "*.log\n");
  WriteFile(root / "sub" / ".gitignore", "ignored_here.txt\n");

  IgnoreMatcher matcher;
  matcher.SetRoot(root);                               // loads root/.gitignore
  matcher.LoadIgnoreFile(root / "sub" / ".gitignore");  // base directory = "sub"

  Expect(matcher.Ignored("debug.log", false), "root rule should ignore *.log anywhere");
  Expect(matcher.Ignored("sub/debug.log", false), "root rule should also match nested *.log");
  Expect(matcher.Ignored("sub/ignored_here.txt", false),
         "a nested gitignore rule should match under its base directory");
  Expect(!matcher.Ignored("ignored_here.txt", false),
         "a nested gitignore rule should not match outside its base directory");
  Expect(!matcher.Ignored("sub/keep.txt", false), "unmatched files should not be ignored");
}

void TestIgnoreMatcherNegationAndDirectoryRules() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  std::filesystem::create_directories(root);
  WriteFile(root / ".gitignore", "build/\n*.tmp\n!keep.tmp\n");

  IgnoreMatcher matcher;
  matcher.SetRoot(root);

  Expect(matcher.Ignored("build", true), "directory-only rule should ignore the directory");
  Expect(!matcher.Ignored("build", false),
         "directory-only rule should not ignore a like-named file");
  Expect(matcher.Ignored("scratch.tmp", false), "glob rule should ignore matching files");
  Expect(!matcher.Ignored("keep.tmp", false), "a later negation should un-ignore the file");
}

}  // namespace

void RegisterIgnoreMatcherTests(std::vector<TestCase>& tests) {
  AddTest(tests, "IgnoreMatcher/NestedGitignoreBasePrefix",
          TestIgnoreMatcherNestedGitignoreBasePrefix);
  AddTest(tests, "IgnoreMatcher/NegationAndDirectoryRules",
          TestIgnoreMatcherNegationAndDirectoryRules);
}

}  // namespace microide::tests
