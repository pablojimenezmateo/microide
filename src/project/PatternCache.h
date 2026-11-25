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

  util::CompiledRegex Get(std::string_view pattern, uint32_t options,
                          std::string error_prefix = {});

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

PatternCache& GlobalPatternCache();

}
