// Regression coverage for the TD-2026-07-17A focus-pass-2 "bounded resources"
// cluster: helpers that gained per-item caps + truncation flags so hostile or
// accidentally huge inputs cannot grow host memory / per-frame work without bound.

#include "TestSupport.h"

#include <string>

#include "workspace/SettingFlags.h"

namespace microide::tests {
namespace {

using workspace::kMaxExcludeGlobsRules;
using workspace::ParseExcludeGlobs;

// TD-2026-07-17A-106: `project.files_exclude` had no parsed-rule count or byte
// budget, so a persisted/pasted setting could create an unbounded rule vector that
// is then copied into DirectoryTree/FileIndex/the native watcher and scanned per
// traversal predicate.
void TestExcludeGlobsRuleCountIsCapped() {
  // Far more comma-separated rules than the cap; each is a distinct short glob.
  std::string text;
  const std::size_t requested = kMaxExcludeGlobsRules + 500;
  for (std::size_t i = 0; i < requested; ++i) {
    if (i != 0) {
      text.push_back(',');
    }
    text += "rule";
    text += std::to_string(i);
  }
  bool truncated = false;
  const std::vector<std::string> globs = ParseExcludeGlobs(text, &truncated);
  Expect(globs.size() == kMaxExcludeGlobsRules,
         "exclude globs must be capped at kMaxExcludeGlobsRules");
  Expect(truncated, "dropping over-cap exclude rules must set the truncated flag");
}

void TestExcludeGlobsByteBudgetIsCapped() {
  // A single enormous glob line (comment-free) far beyond the byte budget: the raw
  // text is only scanned up to the byte cap, so the whole thing cannot be retained.
  std::string text(4u * 1024 * 1024, 'a');  // 4 MiB single token, no separators
  bool truncated = false;
  const std::vector<std::string> globs = ParseExcludeGlobs(text, &truncated);
  Expect(truncated, "scanning past the byte budget must set the truncated flag");
  // The single surviving glob is bounded by the byte cap, never the full 4 MiB.
  std::size_t total = 0;
  for (const std::string& glob : globs) {
    total += glob.size();
  }
  Expect(total <= workspace::kMaxExcludeGlobsBytes,
         "retained exclude text must never exceed the byte budget");
}

void TestExcludeGlobsNormalInputIsNotFlaggedTruncated() {
  bool truncated = true;
  const std::vector<std::string> globs =
      ParseExcludeGlobs("build/\n# comment\nnode_modules/, dist/", &truncated);
  Expect(globs.size() == 3, "normal exclude parsing keeps all non-comment entries");
  Expect(!truncated, "in-budget input must not be flagged truncated");
  Expect(globs[0] == "build/" && globs[1] == "node_modules/" && globs[2] == "dist/",
         "exclude parsing preserves entries and trims whitespace");
}

}  // namespace

void RegisterBoundedResourceCapsTests(std::vector<TestCase>& tests) {
  AddTest(tests, "BoundedResourceCaps/ExcludeGlobsRuleCountIsCapped",
          TestExcludeGlobsRuleCountIsCapped);
  AddTest(tests, "BoundedResourceCaps/ExcludeGlobsByteBudgetIsCapped",
          TestExcludeGlobsByteBudgetIsCapped);
  AddTest(tests, "BoundedResourceCaps/ExcludeGlobsNormalInputIsNotFlaggedTruncated",
          TestExcludeGlobsNormalInputIsNotFlaggedTruncated);
}

}  // namespace microide::tests
