#include "TestSupport.h"

#include "project/IgnoreMatcher.h"

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

#include "project/GlobMatch.h"

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

// TD-2026-07-17A-055: a directory's matcher inherits its ancestors as a shared,
// immutable parent layer (not a full copy of the rule set) via MakeChild. The
// parent-then-local evaluation must be byte-identical to the old flattened
// last-match-wins matcher, including negations that cross layer boundaries and a
// multi-level ancestor chain.
void TestIgnoreMatcherParentLinkedLayersMatchFlattened() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  std::filesystem::create_directories(root / "sub" / "deep");
  WriteFile(root / ".gitignore", "*.log\nsecret/\n");
  WriteFile(root / "sub" / ".gitignore", "!important.log\nlocal.tmp\n");
  WriteFile(root / "sub" / "deep" / ".gitignore", "*.bin\n");

  // Parent-linked chain: root -> sub -> sub/deep, each holding only its own rules.
  auto root_matcher = std::make_shared<IgnoreMatcher>();
  root_matcher->SetRoot(root);
  std::shared_ptr<IgnoreMatcher> sub_matcher = IgnoreMatcher::MakeChild(root_matcher);
  sub_matcher->LoadIgnoreFile(root / "sub" / ".gitignore");
  std::shared_ptr<IgnoreMatcher> deep_matcher = IgnoreMatcher::MakeChild(sub_matcher);
  deep_matcher->LoadIgnoreFile(root / "sub" / "deep" / ".gitignore");

  // Old flattened equivalent: one matcher with every ancestor's rules loaded in order.
  IgnoreMatcher flat;
  flat.SetRoot(root);
  flat.LoadIgnoreFile(root / "sub" / ".gitignore");
  flat.LoadIgnoreFile(root / "sub" / "deep" / ".gitignore");

  const struct {
    const char* path;
    bool is_dir;
  } cases[] = {
      {"debug.log", false},          {"sub/debug.log", false},
      {"sub/important.log", false},  {"sub/local.tmp", false},
      {"local.tmp", false},          {"secret", true},
      {"sub/deep/x.bin", false},     {"x.bin", false},
      {"sub/deep/important.log", false}, {"sub/keep.txt", false},
  };
  for (const auto& c : cases) {
    Expect(deep_matcher->Ignored(c.path, c.is_dir) == flat.Ignored(c.path, c.is_dir),
           "parent-linked layers must decide identically to the flattened matcher");
  }

  // Spot-check the load-bearing semantics directly (not just parity):
  Expect(deep_matcher->Ignored("sub/debug.log", false),
         "an inherited root rule still ignores a nested match");
  Expect(!deep_matcher->Ignored("sub/important.log", false),
         "a child negation crosses the layer boundary to un-ignore an ancestor-ignored path");
  Expect(deep_matcher->Ignored("sub/deep/x.bin", false),
         "the deepest layer's own rule applies under its base directory");
  Expect(!deep_matcher->Ignored("x.bin", false),
         "the deepest layer's rule must not apply outside its base directory");
  // The intermediate matcher does not see the grandchild's rules.
  Expect(!sub_matcher->Ignored("sub/deep/x.bin", false),
         "an ancestor matcher must not see a descendant layer's rules");
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

// git's verdict for a path inside a directory: the directory's own verdict wins
// (gitignore(5): a parent's exclusion cannot be undone for its children), and a
// slash-free rule names the ENTRY, not its ancestors. The old form tested a
// basename rule against every path component, so `!a?` re-included `ab/a` by
// matching the parent `ab`, and a directory-only `build/` never reached
// `build/x.o` because the file's type was checked first. Found against
// `git check-ignore` over generated rule sets.
void TestIgnoreMatcherAncestorVerdictWinsAndNamesMatchTheEntry() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  std::filesystem::create_directories(root);
  WriteFile(root / ".gitignore", "?\n!a?\nbuild/\n!build/keep.txt\nout\n!out/keep.txt\n");
  IgnoreMatcher matcher;
  matcher.SetRoot(root);

  // `?` ignores the one-letter entry `a`; `!a?` names two-letter entries and must
  // not re-include `a` for sitting under `ab`.
  Expect(matcher.Ignored("ab", true) == false, "`!a?` re-includes the two-letter directory");
  Expect(matcher.Ignored("ab/a", true), "`?` ignores ab/a; the parent's `!a?` does not reach it");
  Expect(matcher.Ignored("ab/a/x.txt", false), "everything under an ignored directory is ignored");

  // A directory-only rule reaches the files beneath the directory it names.
  Expect(matcher.Ignored("build", true), "`build/` ignores the directory");
  Expect(!matcher.Ignored("build", false), "`build/` does not ignore a file named build");
  Expect(matcher.Ignored("build/x.o", false), "a file under `build/` is ignored");
  Expect(matcher.Ignored("build/keep.txt", false),
         "`!build/keep.txt` cannot re-include a file whose parent is excluded");
  Expect(matcher.Ignored("out/keep.txt", false),
         "the same holds for a slash-free `out` rule on the parent");

  // The entry-only form is for callers that pruned ignored directories: it
  // answers for the entry alone and never walks ancestors.
  Expect(!matcher.IgnoredEntryNormalized("build/x.o", false),
         "the entry form does not apply the parent's verdict");
  Expect(!matcher.IgnoredEntryNormalized("ab/a/x.txt", false),
         "the entry form does not apply the grandparent's verdict");
  Expect(matcher.IgnoredEntryNormalized("ab/a", true), "the entry form still matches the entry");
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
  Expect(!matcher.Ignored("prebuild", true), "**/build must not match 'prebuild'");
  Expect(!matcher.Ignored("a/rebuild", true), "**/build must not match a nested 'rebuild'");
  Expect(!matcher.Ignored("src/xgenerated", true),
         "a/**/b must not let '**' end inside a segment");

  Expect(matcher.Ignored("src/generated", true), "a/**/b should match with zero directories");
  Expect(matcher.Ignored("src/x/generated", true), "a/**/b should match one directory");
  Expect(matcher.Ignored("src/x/y/generated", true), "a/**/b should match many directories");
  Expect(!matcher.Ignored("other/generated", false),
         "src/**/generated must not match a different top segment");

  Expect(matcher.Ignored("logs", true),
         "logs/** ignores the logs directory itself, as git does");
  Expect(!matcher.Ignored("logs", false), "logs/** does not ignore a FILE named logs");
  Expect(matcher.Ignored("logs/today.txt", false), "logs/** should match a direct child");
  Expect(matcher.Ignored("logs/2026/07/today.txt", false),
         "logs/** should match a deeply nested child");

  Expect(matcher.Ignored("docs/readme.md", false), "docs/*.md should match a direct child");
  Expect(!matcher.Ignored("docs/sub/readme.md", false),
         "a single '*' must not cross a '/' boundary");
}

// A pattern with a slash at the beginning or middle is anchored to the .gitignore's
// directory (gitignore(5)); only a slash-free pattern floats and matches by basename
// at any depth. Regression for the bug where a leading '/' was stripped without
// recording anchoring (so `/build` matched at every depth) and where mid-slash
// patterns were floated across path suffixes (so `a/b` matched `x/a/b`).
void TestIgnoreMatcherAnchoredPatternsDoNotFloat() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  std::filesystem::create_directories(root);
  WriteFile(root / ".gitignore", "/build\na/b\nnode_modules\n");

  IgnoreMatcher matcher;
  matcher.SetRoot(root);

  // Leading-slash pattern: anchored to the root only.
  Expect(matcher.Ignored("build", true), "/build should ignore the root build directory");
  Expect(!matcher.Ignored("src/build", true),
         "/build is anchored: it must NOT match a nested build directory");
  Expect(!matcher.Ignored("packages/x/build", true),
         "/build must not match a deeply nested build directory");

  // Mid-slash pattern: anchored, must not float across suffixes.
  Expect(matcher.Ignored("a/b", true), "a/b should match at the anchored location");
  Expect(!matcher.Ignored("x/a/b", true),
         "a/b is anchored: it must NOT float to match x/a/b");

  // A slash-free pattern still floats by basename at any depth (unchanged behavior).
  Expect(matcher.Ignored("node_modules", true), "node_modules matches at the root");
  Expect(matcher.Ignored("src/node_modules", true),
         "a slash-free pattern still matches by basename at any depth");
}

// Regression: a `**/` segment followed later by another wildcard segment (`*`/`?`)
// must still cross directory boundaries. The single-star backtrack model let the
// trailing non-crossing `*` clobber the `**` restart point, so `**/*.ext` degraded
// to matching at the top level only. git matches all of these across directories.
void TestIgnoreMatcherDoubleStarBeforeWildcardCrossesDirectories() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  std::filesystem::create_directories(root);
  WriteFile(root / ".gitignore", "**/*.txt\nsrc/**/*.cpp\n**/*.min.js\na/**/b/*.md\n");

  IgnoreMatcher matcher;
  matcher.SetRoot(root);

  // A '**' bounded by literals on both sides that also precedes a trailing wildcard
  // must re-extend across directories even when an earlier literal ('b/') matched at a
  // false position. git ignores all three of these for `a/**/b/*.md`.
  Expect(matcher.Ignored("a/b/direct.md", false), "a/**/b/*.md matches with zero middle dirs");
  Expect(matcher.Ignored("a/q/b/n.md", false), "a/**/b/*.md matches with one middle dir");
  Expect(matcher.Ignored("a/q/b/w/b/n.md", false),
         "a/**/b/*.md must re-extend '**' past a false 'b/' match");

  // `**/*.txt` matches a matching file at any depth, including the root.
  Expect(matcher.Ignored("b.txt", false), "**/*.txt should match a root .txt file");
  Expect(matcher.Ignored("a/b.txt", false), "**/*.txt should match a depth-1 .txt file");
  Expect(matcher.Ignored("a/c/b.txt", false), "**/*.txt should match a deeply nested .txt file");
  Expect(!matcher.Ignored("a/b.md", false), "**/*.txt must not match a non-.txt file");

  // `src/**/*.cpp` descends through directories under src.
  Expect(matcher.Ignored("src/x.cpp", false), "src/**/*.cpp should match a direct child");
  Expect(matcher.Ignored("src/a/b/x.cpp", false),
         "src/**/*.cpp should match a deeply nested child");
  Expect(!matcher.Ignored("lib/a/x.cpp", false),
         "src/**/*.cpp must not match a different top segment");

  // A multi-dot suffix wildcard still crosses directories.
  Expect(matcher.Ignored("dist/app.min.js", false), "**/*.min.js should match under a directory");
}

// gitignore(5) escapes: "\#literal"/"\!literal" are literal patterns (not a comment or
// negation), and a backslash-escaped trailing space is preserved as part of the name.
// Regression for inventory I10 (trimming/escape handling ran in the wrong order).
void TestIgnoreMatcherHonorsGitignoreEscapes() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  std::filesystem::create_directories(root);
  // Lines (raw bytes on disk):
  //   \#foo        -> ignore a file literally named "#foo"
  //   \!bar        -> ignore a file literally named "!bar"
  //   trailer\     -> ignore "trailer " (escaped trailing space kept)
  //   plain        -> ignore "plain" (an unescaped trailing space is trimmed)
  WriteFile(root / ".gitignore", "\\#foo\n\\!bar\ntrailer\\ \nplain \n");

  IgnoreMatcher matcher;
  matcher.SetRoot(root);

  Expect(matcher.Ignored("#foo", false),
         "\\#foo should ignore a file literally named '#foo' (not treated as a comment)");
  Expect(!matcher.Ignored("foo", false), "\\#foo must not ignore 'foo'");

  Expect(matcher.Ignored("!bar", false),
         "\\!bar should ignore a file literally named '!bar' (not treated as negation)");
  Expect(!matcher.Ignored("bar", false), "\\!bar must not ignore 'bar'");

  Expect(matcher.Ignored("trailer ", false),
         "an escaped trailing space must be preserved: 'trailer ' should be ignored");
  Expect(!matcher.Ignored("trailer", false),
         "the escaped-space pattern must not match the space-less name 'trailer'");

  Expect(matcher.Ignored("plain", false),
         "an unescaped trailing space is trimmed: 'plain' should be ignored");
  Expect(!matcher.Ignored("plain ", false),
         "the trimmed pattern must not match a trailing-space name 'plain '");
}

// A '[' character class matches a single char; an UNTERMINATED '[' (no closing
// ']') is an ordinary literal per POSIX fnmatch / gitignore. Before the fix the
// unterminated case failed to match, so a file literally named "weird[name" was
// never ignored by a "weird[name" pattern.
void TestIgnoreMatcherHonorsCharacterClasses() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  std::filesystem::create_directories(root);
  //   file[0-9].log -> a real character class (matches one digit)
  //   weird[name    -> an unterminated '[' is a literal '['
  WriteFile(root / ".gitignore", "file[0-9].log\nweird[name\n");

  IgnoreMatcher matcher;
  matcher.SetRoot(root);

  Expect(matcher.Ignored("file7.log", false),
         "a [0-9] class should ignore file7.log");
  Expect(!matcher.Ignored("fileX.log", false),
         "a [0-9] class must not match a non-digit");
  Expect(matcher.Ignored("weird[name", false),
         "an unterminated '[' is a literal char: 'weird[name' should be ignored");
  Expect(!matcher.Ignored("weirdXname", false),
         "the literal '[' pattern must not match as if '[' were a class");
}

// A rule's pattern is classified once at parse time into an exact / suffix /
// prefix / general shape so the per-entry test is a compare rather than a run
// through the backtracking glob matcher (that classification is what took the
// project walk's ignore filter from 6.9 us to 1.6 us per entry). The whole risk of
// that optimization is a shape that decides something GlobMatches would not, and
// it is silent: the tree just quietly gains or loses files.
//
// So sweep a pattern corpus against a path corpus and require the matcher to agree
// with GlobMatches applied the long way round, for every pair. The corpus keeps to
// patterns whose parse is trivially reproducible here (no '!', no leading or
// trailing '/', no backslash escapes) — the escape and character-class cases have
// their own dedicated tests above, which is where a hand-written expectation is
// worth more than a differential.
void TestIgnoreMatcherShapeClassificationMatchesGlob() {
  static constexpr std::string_view kPatterns[] = {
      "node_modules", "build", "*.pyc",    "*",       "*~",     "cmake-build-*",
      "build*",       "a*b",   "*a*",      "?x",      "**",     "x**",
      "**x",          "a/b",   "src/*.h",  "tests/fuzz/corpora/*/*",
      "**/gen/*.h",   "lib/**.c",          "file[0-9].log",
  };
  static constexpr std::string_view kPaths[] = {
      "node_modules",
      "src/node_modules/index.js",
      "a.pyc",
      "src/deep/a.pyc",
      "a/b",
      "x/a/b",
      "src/x.h",
      "src/deep/x.h",
      "tests/fuzz/corpora/seed/case_1",
      "tests/fuzz/corpora/case_1",
      "gen/a.h",
      "pkg/gen/a.h",
      "lib/x.c",
      "lib/deep/x.c",
      "file7.log",
      "fileX.log",
      "cmake-build-debug/CMakeCache.txt",
      "buildx/y",
      "editor.swp~",
      "x",
      "deep/nested/tree/with/many/components/leaf.txt",
      // A component that is EXACTLY the literal a shape compares against. These
      // are the pairs that separate "ends_with" from "ends_with and is longer",
      // "starts_with" from "starts_with and is longer", and an exact compare from
      // a prefix compare — a corpus without them cannot see those mistakes.
      ".pyc",
      "src/.pyc",
      "~",
      "cmake-build-",
      "b",
      "ab",
      "aab",
      "abb",
      "node_modules_extra",
      "extra_node_modules",
      "src/build",
      "buildbuild",
  };

  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  std::filesystem::create_directories(root);

  for (const std::string_view pattern : kPatterns) {
    WriteFile(root / ".gitignore", std::string(pattern) + "\n");
    IgnoreMatcher matcher;
    matcher.SetRoot(root);

    // The pre-classification shape of Rule::Matches, spelled out: a pattern with a
    // separator is anchored to the whole relative path, one without it floats and
    // is tried against each path component.
    const bool anchored = pattern.find('/') != std::string_view::npos;
    for (const std::string_view path : kPaths) {
      bool expected = false;
      if (anchored) {
        expected = project::GlobMatches(pattern, path);
      } else {
        for (std::size_t start = 0; start < path.size();) {
          const std::size_t end = path.find('/', start);
          const std::string_view part =
              end == std::string_view::npos ? path.substr(start) : path.substr(start, end - start);
          if (project::GlobMatches(pattern, part)) {
            expected = true;
            break;
          }
          if (end == std::string_view::npos) {
            break;
          }
          start = end + 1;
        }
      }
      for (const bool is_directory : {false, true}) {
        Expect(matcher.Ignored(std::filesystem::path(std::string(path)), is_directory) == expected,
               std::string("pattern '") + std::string(pattern) + "' vs path '" +
                   std::string(path) + "' (is_directory=" + (is_directory ? "true" : "false") +
                   "): the precompiled shape disagrees with GlobMatches");
      }
    }
  }
}

void RegisterIgnoreMatcherTests(std::vector<TestCase>& tests) {
  AddTest(tests, "IgnoreMatcher/ShapeClassificationMatchesGlob",
          TestIgnoreMatcherShapeClassificationMatchesGlob);
  AddTest(tests, "IgnoreMatcher/HonorsCharacterClasses",
          TestIgnoreMatcherHonorsCharacterClasses);
  AddTest(tests, "IgnoreMatcher/HonorsGitignoreEscapes",
          TestIgnoreMatcherHonorsGitignoreEscapes);
  AddTest(tests, "IgnoreMatcher/DoubleStarBeforeWildcardCrossesDirectories",
          TestIgnoreMatcherDoubleStarBeforeWildcardCrossesDirectories);
  AddTest(tests, "IgnoreMatcher/AnchoredPatternsDoNotFloat",
          TestIgnoreMatcherAnchoredPatternsDoNotFloat);
  AddTest(tests, "IgnoreMatcher/NestedGitignoreBasePrefix",
          TestIgnoreMatcherNestedGitignoreBasePrefix);
  AddTest(tests, "IgnoreMatcher/ParentLinkedLayersMatchFlattened",
          TestIgnoreMatcherParentLinkedLayersMatchFlattened);
  AddTest(tests, "IgnoreMatcher/NegationAndDirectoryRules",
          TestIgnoreMatcherNegationAndDirectoryRules);
  AddTest(tests, "IgnoreMatcher/DefaultRulesGrayVcsAndBuildDirs",
          TestIgnoreMatcherDefaultRulesGrayVcsAndBuildDirs);
  AddTest(tests, "IgnoreMatcher/AncestorVerdictWinsAndNamesMatchTheEntry",
          TestIgnoreMatcherAncestorVerdictWinsAndNamesMatchTheEntry);
  AddTest(tests, "IgnoreMatcher/ExcludeGlobsAndReinclude",
          TestIgnoreMatcherExcludeGlobsAndReinclude);
  AddTest(tests, "IgnoreMatcher/DoubleStarCrossesDirectories",
          TestIgnoreMatcherDoubleStarCrossesDirectories);
}

}  // namespace microide::tests
