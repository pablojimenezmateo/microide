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

void TestIgnoreMatcherDefaultRulesGrayVcsAndBuildDirs() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  std::filesystem::create_directories(root);

  IgnoreMatcher matcher;
  matcher.SetRoot(root);
  matcher.AddDefaultRules();

  // VCS metadata + build-output dirs are ignored as directories, at any depth.
  Expect(matcher.Ignored(".svn", true), "default rules should ignore .svn/");
  Expect(matcher.Ignored(".git", true), "default rules should ignore .git/");
  Expect(matcher.Ignored("builds", true), "default rules should ignore builds/");
  Expect(matcher.Ignored("out", true), "default rules should ignore out/");
  Expect(matcher.Ignored("node_modules", true), "default rules should ignore node_modules/");
  Expect(matcher.Ignored("Visum/src/node_modules", true),
         "default (basename) rules should match a nested node_modules");
  Expect(matcher.Ignored("cmake-build-debug", true),
         "glob default rule should match cmake-build-* dirs");

  // Directory-only: a like-named regular file is not ignored, and real source is untouched.
  Expect(!matcher.Ignored("builds", false), "a file named 'builds' should not be ignored");
  Expect(!matcher.Ignored("src/main.cpp", false), "ordinary source must not be ignored");
}

void TestIgnoreMatcherExcludeGlobsAndReinclude() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  std::filesystem::create_directories(root);

  IgnoreMatcher matcher;
  matcher.SetRoot(root);
  matcher.AddDefaultRules();
  // A user exclude adds a custom dir; a later "!build/" re-includes a default-ignored one.
  matcher.AddExcludeGlobs({"vendored/", "!build/"});

  Expect(matcher.Ignored("vendored", true), "a user exclude glob should ignore its directory");
  Expect(!matcher.Ignored("build", true),
         "a trailing !build/ exclude should re-include the default-ignored build dir");
  // The default .svn ignore still applies (not re-included).
  Expect(matcher.Ignored(".svn", true), "unrelated defaults remain in effect");
}

}  // namespace

void TestIgnoreMatcherDoubleStarCrossesDirectories() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  std::filesystem::create_directories(root);
  // `**/build` must match build at any depth (git semantics), including the repo
  // root; a nested `a/**/b` must match across zero or more directories; `logs/**`
  // must match everything under logs. A single '*' must still stay in one segment.
  WriteFile(root / ".gitignore",
            "**/build\nsrc/**/generated\nlogs/**\ndocs/*.md\n");

  IgnoreMatcher matcher;
  matcher.SetRoot(root);

  Expect(matcher.Ignored("build", true), "**/build should match build at the repo root");
  Expect(matcher.Ignored("a/build", true), "**/build should match a depth-1 build");
  Expect(matcher.Ignored("a/b/build", true), "**/build should match a deeply nested build");

  Expect(matcher.Ignored("src/generated", true), "a/**/b should match with zero directories");
  Expect(matcher.Ignored("src/x/generated", true), "a/**/b should match one directory");
  Expect(matcher.Ignored("src/x/y/generated", true), "a/**/b should match many directories");
  Expect(!matcher.Ignored("other/generated", false),
         "src/**/generated must not match a different top segment");

  Expect(matcher.Ignored("logs/today.txt", false), "logs/** should match a direct child");
  Expect(matcher.Ignored("logs/2026/07/today.txt", false),
         "logs/** should match a deeply nested child");

  Expect(matcher.Ignored("docs/readme.md", false), "docs/*.md should match a direct child");
  Expect(!matcher.Ignored("docs/sub/readme.md", false),
         "a single '*' must not cross a '/' boundary");
}

void RegisterIgnoreMatcherTests(std::vector<TestCase>& tests) {
  AddTest(tests, "IgnoreMatcher/NestedGitignoreBasePrefix",
          TestIgnoreMatcherNestedGitignoreBasePrefix);
  AddTest(tests, "IgnoreMatcher/NegationAndDirectoryRules",
          TestIgnoreMatcherNegationAndDirectoryRules);
  AddTest(tests, "IgnoreMatcher/DefaultRulesGrayVcsAndBuildDirs",
          TestIgnoreMatcherDefaultRulesGrayVcsAndBuildDirs);
  AddTest(tests, "IgnoreMatcher/ExcludeGlobsAndReinclude",
          TestIgnoreMatcherExcludeGlobsAndReinclude);
  AddTest(tests, "IgnoreMatcher/DoubleStarCrossesDirectories",
          TestIgnoreMatcherDoubleStarCrossesDirectories);
}

}  // namespace microide::tests
