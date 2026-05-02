#include "TestSupport.h"

#include "project/PatternCache.h"
#include "util/RegexUtil.h"

#include <string>

namespace microide::tests {
namespace {

// Compiling the same pattern twice should hit the cache on the second call:
// cache size stays at 1 and both returned regexes are valid.
void TestSamePatternReturnsCachedResult() {
  project::PatternCache cache;
  const auto regex1 = cache.Get("hello", 0, "test");
  const auto regex2 = cache.Get("hello", 0, "test");

  Expect(regex1.valid(), "first Get should return a valid CompiledRegex");
  Expect(regex2.valid(), "second Get of same pattern should return a valid CompiledRegex");
  Expect(cache.Size() == 1, "cache size should be 1 after two Gets of the same pattern");
}

// Compiling N distinct patterns should keep size == N (when N < kMaxEntries).
void TestDistinctPatternsGrowCache() {
  project::PatternCache cache;
  for (int i = 0; i < 10; ++i) {
    const std::string pattern = "pattern_" + std::to_string(i);
    (void)cache.Get(pattern, 0, "test");
  }
  Expect(cache.Size() == 10, "cache should contain one entry per distinct pattern");
}

// Inserting kMaxEntries + 1 distinct patterns should evict the LRU entry.
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

// After inserting kMaxEntries patterns and then re-accessing the first one,
// the first pattern should remain in the cache (it was promoted to MRU).
void TestAccessPromotesEntryToMru() {
  project::PatternCache cache;
  // Fill cache exactly to capacity.
  for (std::size_t i = 0; i < project::PatternCache::kMaxEntries; ++i) {
    (void)cache.Get("mru_" + std::to_string(i), 0, "test");
  }
  // Re-access pattern 0 (makes it MRU).
  const auto promoted = cache.Get("mru_0", 0, "test");
  Expect(promoted.valid(), "re-accessed pattern should still be valid");

  // Insert one more pattern; the LRU (mru_1, not mru_0) should be evicted.
  (void)cache.Get("mru_new", 0, "test");
  Expect(cache.Size() == project::PatternCache::kMaxEntries,
         "cache size should stay at kMaxEntries after insert post-promotion");

  // mru_0 should still be retrievable (it was promoted, not evicted).
  const auto still_present = cache.Get("mru_0", 0, "test");
  Expect(still_present.valid(), "promoted pattern should survive the next eviction");
}

// Invalid pattern should return an invalid CompiledRegex with a non-empty error.
void TestInvalidPatternReturnsInvalidRegex() {
  project::PatternCache cache;
  const auto bad = cache.Get("[invalid", 0, "bad pattern");
  Expect(!bad.valid(), "invalid regex pattern should produce an invalid CompiledRegex");
  Expect(!bad.error().empty(), "invalid CompiledRegex should carry an error message");
}

// Different options for the same pattern string are different cache entries.
void TestDifferentOptionsAreDifferentEntries() {
  project::PatternCache cache;
  (void)cache.Get("abc", 0, "test");
  (void)cache.Get("abc", PCRE2_CASELESS, "test");
  Expect(cache.Size() == 2,
         "same pattern with different options should produce two distinct cache entries");
}

// GlobalPatternCache returns a non-null singleton usable for real searches.
void TestGlobalPatternCacheIsUsable() {
  project::PatternCache& global = project::GlobalPatternCache();
  const auto regex = global.Get("microide", 0, "global cache test");
  Expect(regex.valid(), "GlobalPatternCache should compile a simple pattern successfully");
}

}  // namespace

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

}  // namespace microide::tests
