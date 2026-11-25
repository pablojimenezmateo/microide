#include "TestSupport.h"

#include "project/PatternCache.h"
#include "util/RegexUtil.h"

#include <string>

namespace microide::tests {
namespace {

void TestSamePatternReturnsCachedResult() {
  project::PatternCache cache;
  const auto regex1 = cache.Get("hello", 0, "test");
  const auto regex2 = cache.Get("hello", 0, "test");

  Expect(regex1.valid(), "first Get should return a valid CompiledRegex");
  Expect(regex2.valid(), "second Get of same pattern should return a valid CompiledRegex");
  Expect(cache.Size() == 1, "cache size should be 1 after two Gets of the same pattern");
}

void TestDistinctPatternsGrowCache() {
  project::PatternCache cache;
  for (int i = 0; i < 10; ++i) {
    const std::string pattern = "pattern_" + std::to_string(i);
    (void)cache.Get(pattern, 0, "test");
  }
  Expect(cache.Size() == 10, "cache should contain one entry per distinct pattern");
}

void TestLruEvictionKeepsSizeAtMax() {
  project::PatternCache cache;
  const std::size_t n = project::PatternCache::kMaxEntries + 1;
  for (std::size_t i = 0; i < n; ++i) {
    const std::string pattern = "evict_" + std::to_string(i);
    (void)cache.Get(pattern, 0, "test");
  }
  Expect(cache.Size() == project::PatternCache::kMaxEntries,
         "cache size should not exceed kMaxEntries after LRU eviction");
}

void TestAccessPromotesEntryToMru() {
  project::PatternCache cache;
  for (std::size_t i = 0; i < project::PatternCache::kMaxEntries; ++i) {
    (void)cache.Get("mru_" + std::to_string(i), 0, "test");
  }
  const auto promoted = cache.Get("mru_0", 0, "test");
  Expect(promoted.valid(), "re-accessed pattern should still be valid");

  (void)cache.Get("mru_new", 0, "test");
  Expect(cache.Size() == project::PatternCache::kMaxEntries,
         "cache size should stay at kMaxEntries after insert post-promotion");

  const auto still_present = cache.Get("mru_0", 0, "test");
  Expect(still_present.valid(), "promoted pattern should survive the next eviction");
}

void TestInvalidPatternReturnsInvalidRegex() {
  project::PatternCache cache;
  const auto bad = cache.Get("[invalid", 0, "bad pattern");
  Expect(!bad.valid(), "invalid regex pattern should produce an invalid CompiledRegex");
  Expect(!bad.error().empty(), "invalid CompiledRegex should carry an error message");
}

void TestDifferentOptionsAreDifferentEntries() {
  project::PatternCache cache;
  (void)cache.Get("abc", 0, "test");
  (void)cache.Get("abc", PCRE2_CASELESS, "test");
  Expect(cache.Size() == 2,
         "same pattern with different options should produce two distinct cache entries");
}

void TestGlobalPatternCacheIsUsable() {
  project::PatternCache& global = project::GlobalPatternCache();
  const auto regex = global.Get("microide", 0, "global cache test");
  Expect(regex.valid(), "GlobalPatternCache should compile a simple pattern successfully");
}

}

void RegisterPatternCacheTests(std::vector<TestCase>& tests) {
  AddTest(tests, "PatternCache/SamePatternReturnsCachedResult",
          TestSamePatternReturnsCachedResult);
  AddTest(tests, "PatternCache/DistinctPatternsGrowCache",
          TestDistinctPatternsGrowCache);
  AddTest(tests, "PatternCache/LruEvictionKeepsSizeAtMax",
          TestLruEvictionKeepsSizeAtMax);
  AddTest(tests, "PatternCache/AccessPromotesEntryToMru",
          TestAccessPromotesEntryToMru);
  AddTest(tests, "PatternCache/InvalidPatternReturnsInvalidRegex",
          TestInvalidPatternReturnsInvalidRegex);
  AddTest(tests, "PatternCache/DifferentOptionsAreDifferentEntries",
          TestDifferentOptionsAreDifferentEntries);
  AddTest(tests, "PatternCache/GlobalPatternCacheIsUsable",
          TestGlobalPatternCacheIsUsable);
}

}
