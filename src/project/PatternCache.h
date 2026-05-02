#pragma once

#include <cstddef>
#include <cstdint>
#include <list>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

#include "util/RegexUtil.h"

namespace microide::project {

// PatternCache: process-wide LRU cache of PCRE2 compiled patterns (max 64 entries).
// Thread-safe with std::mutex.
// Key: (pattern_string, flags uint32_t)
// Value: CompiledRegex (shared_ptr to pcre2_code, including JIT if available)
class PatternCache {
 public:
  static constexpr std::size_t kMaxEntries = 64;

  struct Key {
    std::string pattern;
    uint32_t options = 0;

    bool operator==(const Key& other) const {
      return options == other.options && pattern == other.pattern;
    }
  };

  struct KeyHash {
    std::size_t operator()(const Key& k) const noexcept {
      const std::size_t h1 = std::hash<std::string>{}(k.pattern);
      const std::size_t h2 = std::hash<uint32_t>{}(k.options);
      return h1 ^ (h2 << 1);
    }
  };

  // Returns a cached CompiledRegex for the given pattern+options, or compiles and caches it.
  // Thread-safe.
  util::CompiledRegex Get(std::string_view pattern, uint32_t options,
                          std::string error_prefix = {});

  // Returns the number of cached entries. Thread-safe.
  std::size_t Size() const;

 private:
  struct Entry {
    Key key;
    util::CompiledRegex regex;
  };

  mutable std::mutex mutex_;
  std::list<Entry> lru_list_;
  std::unordered_map<Key, std::list<Entry>::iterator, KeyHash> map_;
};

// Process-wide singleton pattern cache for search operations.
PatternCache& GlobalPatternCache();

}  // namespace microide::project
