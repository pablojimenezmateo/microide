#include "project/PatternCache.h"

namespace microide::project {

util::CompiledRegex PatternCache::Get(std::string_view pattern,
                                      uint32_t options,
                                      std::string error_prefix) {
  Key key{std::string(pattern), options};
  std::lock_guard lock(mutex_);

  const auto it = map_.find(key);
  if (it != map_.end()) {
    lru_list_.splice(lru_list_.begin(), lru_list_, it->second);
    return it->second->regex;
  }

  util::CompiledRegex compiled(pattern, options, std::move(error_prefix));

  if (lru_list_.size() >= kMaxEntries) {
    const auto& back = lru_list_.back();
    map_.erase(back.key);
    lru_list_.pop_back();
  }

  lru_list_.push_front(Entry{key, compiled});
  map_.emplace(key, lru_list_.begin());

  return compiled;
}

std::size_t PatternCache::Size() const {
  std::lock_guard lock(mutex_);
  return lru_list_.size();
}

PatternCache& GlobalPatternCache() {
  static PatternCache instance;
  return instance;
}

}
