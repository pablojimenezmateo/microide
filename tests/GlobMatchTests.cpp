#include "TestSupport.h"

#include "project/GlobMatch.h"

#include <string>
#include <string_view>
#include <vector>

namespace microide::tests {
namespace {

using microide::project::GlobMatches;
using microide::project::GlobSet;

// The matcher itself moved out of IgnoreMatcher.cpp's anonymous namespace so both
// ignore rules and search scope filters share one definition of "what a glob
// means". Lock in the FNM_PATHNAME semantics directly, independent of the
// gitignore parsing layered on top in IgnoreMatcherTests.
// git's wildmatch lets a leading `**/` stand for zero directories even when
// there is nothing left to match, so `**/*` matches the empty string. That is
// the remainder a rule like `a**/*` leaves against the directory `a` once the
// literal prefix is stripped, and git ignores `a` by it. Found against
// `git check-ignore`.
void TestGlobDoubleStarSlashMatchesTheEmptyRemainder() {
  Expect(GlobMatches("**/*", ""), "`**/*` matches an empty remainder");
  Expect(GlobMatches("**/", ""), "`**/` matches an empty remainder");
  Expect(!GlobMatches("*/", ""), "a plain `*` cannot skip the separator");
  Expect(!GlobMatches("**/b", ""), "a literal after the wildcard still needs text");
  Expect(!GlobMatches("a/**/*", "a"), "a separator before the wildcard still needs its text");
}

void TestGlobMatchesPathnameSemantics() {
  Expect(GlobMatches("*.cpp", "main.cpp"), "'*' should match within a segment");
  Expect(!GlobMatches("*.cpp", "src/main.cpp"), "'*' must not cross a '/'");
  Expect(GlobMatches("src/*.cpp", "src/main.cpp"), "an anchored '*' matches its own segment");
  Expect(!GlobMatches("src/*.cpp", "src/util/main.cpp"), "'*' must not cross a nested '/'");

  Expect(GlobMatches("**/*.cpp", "src/util/main.cpp"), "'**/' should cross directories");
  Expect(GlobMatches("**/*.cpp", "main.cpp"), "'**/' should also match zero directories");
  Expect(GlobMatches("src/**", "src/a/b/c.txt"), "a trailing '**' should match a whole subtree");
  Expect(!GlobMatches("src/**", "tests/a.txt"), "'src/**' must stay anchored to src");

  Expect(GlobMatches("a?c", "abc"), "'?' should match one character");
  Expect(!GlobMatches("a?c", "a/c"), "'?' must not match a '/'");

  Expect(GlobMatches("file[0-9].txt", "file7.txt"), "character ranges should match");
  Expect(!GlobMatches("file[0-9].txt", "filex.txt"), "out-of-range characters should not match");
  Expect(GlobMatches("file[!0-9].txt", "filex.txt"), "'!' should negate a class");

  Expect(GlobMatches("a\\*b", "a*b"), "'\\\\' should escape a wildcard into a literal");
  Expect(!GlobMatches("a\\*b", "axb"), "an escaped '*' must not behave as a wildcard");
}

// VSCode's search box: a '/'-free entry floats to any depth, a '/'-bearing entry
// is anchored to the workspace root.
void TestGlobSetFloatsBareNamesAndAnchorsPaths() {
  const GlobSet includes = GlobSet::Parse("*.cpp");
  Expect(!includes.empty(), "a non-empty entry should produce patterns");
  Expect(includes.Matches("main.cpp"), "'*.cpp' should match at the root");
  Expect(includes.Matches("src/util/main.cpp"), "'*.cpp' should float to any depth");
  Expect(!includes.Matches("src/main.h"), "'*.cpp' should not match other extensions");

  const GlobSet anchored = GlobSet::Parse("src/*.cpp");
  Expect(anchored.Matches("src/main.cpp"), "an anchored entry should match its own directory");
  Expect(!anchored.Matches("tests/src/main.cpp"),
         "a '/'-bearing entry must stay anchored to the root, not float");

  Expect(GlobSet::Parse("").empty(), "an empty box should produce an inactive set");
  Expect(GlobSet::Parse("  ,  ,").empty(), "whitespace-only entries should be dropped");
}

// A wildcard-free entry names a file OR a directory subtree, so selecting a folder
// and typing its path scopes the search to everything beneath it.
void TestGlobSetTreatsWildcardFreeEntriesAsSubtrees() {
  const GlobSet folder = GlobSet::Parse("src/util");
  Expect(folder.Matches("src/util"), "the exact path should match");
  Expect(folder.Matches("src/util/Parse.cpp"), "everything under the folder should match");
  Expect(folder.Matches("src/util/deep/nested.cpp"), "the whole subtree should match");
  Expect(!folder.Matches("src/utility/Parse.cpp"),
         "a sibling with a longer name must not match the subtree");
  Expect(!folder.Matches("src/project/Parse.cpp"), "an unrelated folder should not match");

  const GlobSet bare = GlobSet::Parse("tests");
  Expect(bare.Matches("tests/Foo.cpp"), "a bare folder name should float and cover its subtree");
  Expect(bare.Matches("src/tests/Foo.cpp"), "a bare folder name should match at any depth");
  Expect(!bare.Matches("src/testsuite/Foo.cpp"), "a bare name must match a whole segment");
}

// Brace alternation is expanded at parse time (the underlying matcher has no brace
// support), and a comma inside braces is alternation rather than an entry split.
void TestGlobSetExpandsBracesAndSplitsTopLevelCommas() {
  const GlobSet braces = GlobSet::Parse("*.{cpp,h}");
  Expect(braces.Matches("src/main.cpp"), "the first alternative should match");
  Expect(braces.Matches("src/main.h"), "the second alternative should match");
  Expect(!braces.Matches("src/main.py"), "an unlisted extension should not match");

  const GlobSet nested = GlobSet::Parse("src/**/*.{c,cp{p,c}}");
  Expect(nested.Matches("src/a/main.c"), "a nested brace group should expand");
  Expect(nested.Matches("src/a/main.cpp"), "nested alternatives should expand");
  Expect(nested.Matches("src/a/main.cpc"), "nested alternatives should expand fully");
  Expect(!nested.Matches("src/a/main.cxx"), "unexpanded combinations should not match");

  const GlobSet multi = GlobSet::Parse("*.md, src/*.cpp");
  Expect(multi.Matches("README.md"), "the first comma-separated entry should match");
  Expect(multi.Matches("src/main.cpp"), "the second comma-separated entry should match");
  Expect(!multi.Matches("src/main.h"), "an entry outside both patterns should not match");
}

// Leading "./" and "/" and a trailing "/" are cosmetic in VSCode's boxes; an
// unbalanced brace stays literal instead of dropping the whole entry.
void TestGlobSetNormalizesEntryDecoration() {
  Expect(GlobSet::Parse("./src/*.cpp").Matches("src/main.cpp"),
         "a leading './' should be stripped");
  Expect(GlobSet::Parse("/src/*.cpp").Matches("src/main.cpp"), "a leading '/' should be stripped");
  Expect(GlobSet::Parse("src/util/").Matches("src/util/Parse.cpp"),
         "a trailing '/' should still cover the subtree");
  Expect(GlobSet::Parse("**/*.cpp").Matches("src/main.cpp"),
         "an explicit '**/' prefix should not be doubled");

  const GlobSet unbalanced = GlobSet::Parse("weird{name");
  Expect(!unbalanced.empty(), "an unbalanced brace should not drop the entry");
  Expect(unbalanced.Matches("weird{name"), "an unbalanced '{' should stay a literal character");
}

// Brace alternation is multiplicative, so a pathological pasted entry must be
// bounded rather than expanded into an unbounded pattern list.
void TestGlobSetBoundsBraceExpansion() {
  std::string pattern;
  for (int index = 0; index < 12; ++index) {
    pattern += "{a,b}";
  }  // 2^12 = 4096 combinations if unbounded
  const GlobSet bounded = GlobSet::Parse(pattern);
  Expect(bounded.size() <= 256, "brace expansion should be capped");
  Expect(!bounded.empty(), "a capped expansion should still yield usable patterns");
}

// The two '**' conventions the matcher has to serve. gitignore/VSCode only let
// '**' cross '/' when it forms a whole path segment; EditorConfig defines it as
// "any string of characters" wherever it appears. Getting this wrong is silent —
// the EditorConfig section simply never matches — so pin both directions.
void TestGlobMatchesDoubleStarConventions() {
  using microide::project::GlobDoubleStar;

  // Segment-anchored (the default): a non-segment '**' degrades to a single '*'.
  Expect(!GlobMatches("lib/**.c", "lib/deep/x.c"),
         "a non-segment '**' must not cross '/' under the gitignore rule");
  Expect(GlobMatches("lib/**.c", "lib/x.c"),
         "a non-segment '**' still matches within one segment");
  Expect(GlobMatches("lib/**/*.c", "lib/deep/x.c"),
         "a segment-anchored '**' crosses '/' under the gitignore rule");

  // EditorConfig: every '**' crosses.
  Expect(GlobMatches("lib/**.c", "lib/deep/x.c", GlobDoubleStar::Always),
         "EditorConfig's '**' crosses '/' wherever it appears");
  Expect(GlobMatches("lib/**.c", "lib/x.c", GlobDoubleStar::Always),
         "EditorConfig's '**' also matches zero directories deep");
  Expect(!GlobMatches("lib/**.c", "other/deep/x.c", GlobDoubleStar::Always),
         "a crossing '**' must still respect the anchored prefix");

  // A single '*' is unaffected by the convention.
  Expect(!GlobMatches("*.c", "src/x.c", GlobDoubleStar::Always),
         "a single '*' must not cross '/' under either convention");
}

// A segment-anchored '**' stands for WHOLE directories. The matcher's "**/"
// restart used to step one byte at a time, so the wildcard could end inside a
// segment: "a/**/b" matched "a/ab", "**/build" matched "prebuild", and the
// floating scope entry "tests" ("**/tests/**") caught "mytests/x".
void TestGlobMatchesDoubleStarConsumesWholeSegments() {
  Expect(!GlobMatches("a/**/b", "a/ab"), "'a/**/b' must not match 'a/ab'");
  Expect(!GlobMatches("a/**/b", "a/xb"), "'a/**/b' must not match 'a/xb'");
  Expect(!GlobMatches("a/**/b", "a/x/yb"), "'a/**/b' must not end '**' mid-segment");
  Expect(GlobMatches("a/**/b", "a/b"), "'a/**/b' matches zero directories");
  Expect(GlobMatches("a/**/b", "a/x/b"), "'a/**/b' matches one directory");
  Expect(GlobMatches("a/**/b", "a/x/y/b"), "'a/**/b' matches many directories");
  Expect(!GlobMatches("**/build", "prebuild"), "'**/build' must not match 'prebuild'");
  Expect(!GlobMatches("**/build", "a/rebuild"), "'**/build' must not match 'a/rebuild'");
  Expect(GlobMatches("**/build", "a/b/build"), "'**/build' matches a nested build");
  Expect(GlobMatches("logs/**", "logs/a/b.txt"), "a trailing '**' still takes the subtree");
  Expect(!GlobMatches("logs/**", "logs"), "'logs/**' does not name the directory itself");
  Expect(GlobMatches("**", "a/b/c"), "a lone '**' matches everything");
  // A later single '*' must not smuggle the '**' restart back to a byte step:
  // this is the '**/' -> '*' -> literal shape the search scope uses.
  Expect(!GlobMatches("**/*/b", "ab"), "'**/*/b' needs a real segment before b");
  Expect(GlobMatches("**/*/b", "x/a/b"), "'**/*/b' matches with a directory in between");
  Expect(GlobMatches("**/x*/b", "q/xa/b"), "'**/x*/b' resumes on a segment boundary");
  Expect(!GlobMatches("**/x*/b", "q/axa/b"), "'**/x*/b' must not start x mid-segment");
  // EditorConfig's convention: a '**' that does not fold a '/' is any string of
  // characters, but its "**/" (what a slash-free section header becomes) is
  // still whole directories — "[x.py]" must not apply to "ax.py".
  using microide::project::GlobDoubleStar;
  Expect(GlobMatches("a/**b", "a/x/yb", GlobDoubleStar::Always),
         "EditorConfig's unfolded '**' still ends anywhere");
  Expect(!GlobMatches("**/x.py", "ax.py", GlobDoubleStar::Always),
         "EditorConfig's '**/' must not end inside a segment");
  Expect(GlobMatches("**/x.py", "a/x.py", GlobDoubleStar::Always),
         "EditorConfig's '**/' matches at depth");
  Expect(GlobMatches("**/x.py", "x.py", GlobDoubleStar::Always),
         "EditorConfig's '**/' matches at the top level");

  const GlobSet scope = GlobSet::Parse("tests");
  Expect(scope.Matches("tests/a.cpp"), "'tests' scopes its own subtree");
  Expect(scope.Matches("src/tests/a.cpp"), "'tests' floats to any depth");
  Expect(!scope.Matches("mytests/a.cpp"), "'tests' must not match 'mytests'");
  Expect(!scope.Matches("src/mytests"), "'tests' must not match a 'mytests' leaf");
}

// Exhaustive differential check against a plain recursive matcher over a tiny
// alphabet: every pattern up to 5 bytes of {a, b, /, *, ?} against every
// root-relative path up to 5 bytes of {a, b, /}. The greedy single-restart
// matcher is the fast path; the recursion is the definition. Patterns ending in
// '/', texts ending in '/' and the empty text are excluded: every caller strips a
// trailing separator before matching and a root-relative path names something,
// so none of them is a real input.
void TestGlobMatchesAgreesWithRecursiveReference() {
  struct Reference {
    static bool Match(std::string_view p, std::string_view t, bool segment_start) {
      if (p.empty()) {
        return t.empty();
      }
      if (p[0] == '*') {
        std::size_t stars = 0;
        while (stars < p.size() && p[stars] == '*') {
          ++stars;
        }
        const std::string_view rest = p.substr(stars);
        const bool after_ok = rest.empty() || rest[0] == '/';
        if (stars >= 2 && segment_start && after_ok) {
          if (rest.empty()) {
            return true;
          }
          const std::string_view after_slash = rest.substr(1);
          if (Match(after_slash, t, true)) {
            return true;
          }
          for (std::size_t k = 1; k <= t.size(); ++k) {
            if (t[k - 1] == '/' && Match(after_slash, t.substr(k), true)) {
              return true;
            }
          }
          return false;
        }
        for (std::size_t k = 0; k <= t.size(); ++k) {
          if (Match(rest, t.substr(k), false)) {
            return true;
          }
          if (k < t.size() && t[k] == '/') {
            break;
          }
        }
        return false;
      }
      if (t.empty()) {
        return false;
      }
      if (p[0] == '?') {
        return t[0] != '/' && Match(p.substr(1), t.substr(1), false);
      }
      return p[0] == t[0] && Match(p.substr(1), t.substr(1), p[0] == '/');
    }
  };
  std::vector<std::string> patterns;
  std::vector<std::string> texts;
  const auto enumerate = [](std::string_view alphabet, std::size_t max_length,
                            std::vector<std::string>& out) {
    out.emplace_back();
    for (std::size_t begin = 0; begin < out.size(); ++begin) {
      if (out[begin].size() >= max_length) {
        continue;
      }
      for (const char c : alphabet) {
        out.push_back(out[begin] + c);
      }
    }
  };
  enumerate("ab/*?", 5, patterns);
  enumerate("ab/", 5, texts);
  std::size_t mismatches = 0;
  for (const std::string& pattern : patterns) {
    if (pattern.ends_with('/')) {
      continue;
    }
    for (const std::string& text : texts) {
      if (text.empty() || text.ends_with('/')) {
        continue;
      }
      if (GlobMatches(pattern, text) != Reference::Match(pattern, text, true)) {
        ++mismatches;
      }
    }
  }
  Expect(mismatches == 0, "GlobMatches must agree with the recursive reference everywhere");
}

}  // namespace

void RegisterGlobMatchTests(std::vector<TestCase>& tests) {
  AddTest(tests, "GlobMatch/DoubleStarSlashMatchesTheEmptyRemainder",
          TestGlobDoubleStarSlashMatchesTheEmptyRemainder);
  AddTest(tests, "GlobMatch/DoubleStarConventions", TestGlobMatchesDoubleStarConventions);
  AddTest(tests, "GlobMatch/DoubleStarConsumesWholeSegments",
          TestGlobMatchesDoubleStarConsumesWholeSegments);
  AddTest(tests, "GlobMatch/AgreesWithRecursiveReference",
          TestGlobMatchesAgreesWithRecursiveReference);
  AddTest(tests, "GlobMatch/PathnameSemantics", TestGlobMatchesPathnameSemantics);
  AddTest(tests, "GlobSet/FloatsBareNamesAndAnchorsPaths",
          TestGlobSetFloatsBareNamesAndAnchorsPaths);
  AddTest(tests, "GlobSet/WildcardFreeEntriesAreSubtrees",
          TestGlobSetTreatsWildcardFreeEntriesAsSubtrees);
  AddTest(tests, "GlobSet/ExpandsBracesAndSplitsTopLevelCommas",
          TestGlobSetExpandsBracesAndSplitsTopLevelCommas);
  AddTest(tests, "GlobSet/NormalizesEntryDecoration", TestGlobSetNormalizesEntryDecoration);
  AddTest(tests, "GlobSet/BoundsBraceExpansion", TestGlobSetBoundsBraceExpansion);
}

}  // namespace microide::tests
