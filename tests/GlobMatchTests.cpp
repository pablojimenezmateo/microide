#include "TestSupport.h"

#include "project/GlobMatch.h"

#include <string>
#include <string_view>
#include <vector>

namespace microide::tests {
namespace {

using microide::project::GlobMatches;
using microide::project::GlobSet;

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

void TestGlobSetBoundsBraceExpansion() {
  std::string pattern;
  for (int index = 0; index < 12; ++index) {
    pattern += "{a,b}";
  }
  const GlobSet bounded = GlobSet::Parse(pattern);
  Expect(bounded.size() <= 256, "brace expansion should be capped");
  Expect(!bounded.empty(), "a capped expansion should still yield usable patterns");
}

void TestGlobMatchesDoubleStarConventions() {
  using microide::project::GlobDoubleStar;

  Expect(!GlobMatches("lib/**.c", "lib/deep/x.c"),
         "a non-segment '**' must not cross '/' under the gitignore rule");
  Expect(GlobMatches("lib/**.c", "lib/x.c"),
         "a non-segment '**' still matches within one segment");
  Expect(GlobMatches("lib/**/*.c", "lib/deep/x.c"),
         "a segment-anchored '**' crosses '/' under the gitignore rule");

  Expect(GlobMatches("lib/**.c", "lib/deep/x.c", GlobDoubleStar::Always),
         "EditorConfig's '**' crosses '/' wherever it appears");
  Expect(GlobMatches("lib/**.c", "lib/x.c", GlobDoubleStar::Always),
         "EditorConfig's '**' also matches zero directories deep");
  Expect(!GlobMatches("lib/**.c", "other/deep/x.c", GlobDoubleStar::Always),
         "a crossing '**' must still respect the anchored prefix");

  Expect(!GlobMatches("*.c", "src/x.c", GlobDoubleStar::Always),
         "a single '*' must not cross '/' under either convention");
}

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
  Expect(!GlobMatches("**/*/b", "ab"), "'**/*/b' needs a real segment before b");
  Expect(GlobMatches("**/*/b", "x/a/b"), "'**/*/b' matches with a directory in between");
  Expect(GlobMatches("**/x*/b", "q/xa/b"), "'**/x*/b' resumes on a segment boundary");
  Expect(!GlobMatches("**/x*/b", "q/axa/b"), "'**/x*/b' must not start x mid-segment");
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

void TestExpandGlobBracesBoundsPatternLength() {
  std::string deep;
  for (int index = 0; index < 300000; ++index) {
    deep += "{a}";
  }
  std::vector<std::string> out;
  microide::project::ExpandGlobBraces(deep, out);
  Expect(out.empty(), "a pattern past the length cap expands to nothing");

  std::string fits(microide::project::kMaxBraceExpansionPatternBytes - 6, 'x');
  fits += "{a,b}";
  out.clear();
  microide::project::ExpandGlobBraces(fits, out);
  Expect(out.size() == 2, "a pattern at the cap still expands");
}

}

void RegisterGlobMatchTests(std::vector<TestCase>& tests) {
  AddTest(tests, "GlobMatch/DoubleStarSlashMatchesTheEmptyRemainder",
          TestGlobDoubleStarSlashMatchesTheEmptyRemainder);
  AddTest(tests, "GlobMatch/DoubleStarConventions", TestGlobMatchesDoubleStarConventions);
  AddTest(tests, "GlobMatch/DoubleStarConsumesWholeSegments",
          TestGlobMatchesDoubleStarConsumesWholeSegments);
  AddTest(tests, "GlobMatch/AgreesWithRecursiveReference",
          TestGlobMatchesAgreesWithRecursiveReference);
  AddTest(tests, "GlobMatch/PathnameSemantics", TestGlobMatchesPathnameSemantics);
  AddTest(tests, "GlobMatch/ExpandBracesBoundsPatternLength",
          TestExpandGlobBracesBoundsPatternLength);
  AddTest(tests, "GlobSet/FloatsBareNamesAndAnchorsPaths",
          TestGlobSetFloatsBareNamesAndAnchorsPaths);
  AddTest(tests, "GlobSet/WildcardFreeEntriesAreSubtrees",
          TestGlobSetTreatsWildcardFreeEntriesAsSubtrees);
  AddTest(tests, "GlobSet/ExpandsBracesAndSplitsTopLevelCommas",
          TestGlobSetExpandsBracesAndSplitsTopLevelCommas);
  AddTest(tests, "GlobSet/NormalizesEntryDecoration", TestGlobSetNormalizesEntryDecoration);
  AddTest(tests, "GlobSet/BoundsBraceExpansion", TestGlobSetBoundsBraceExpansion);
}

}
