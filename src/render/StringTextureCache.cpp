#include "render/StringTextureCache.h"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <functional>
#include <utility>

#include "util/PerformanceCounters.h"

namespace microide::render {

namespace {

void ReserveRoundedKeyCapacity(std::string& key_text, std::size_t needed) {
  if (key_text.capacity() >= needed) {
    return;
  }
  key_text.reserve(std::bit_ceil(std::max<std::size_t>(needed, 32)));
}

}

StringTextureCache::~StringTextureCache() {
  Clear();
}

std::size_t StringTextureCache::KeyHash::operator()(const KeyView& key) const noexcept {
  auto mix = [](std::size_t seed, std::size_t value) {
    return seed ^ (value + 0x9e3779b9ULL + (seed << 6) + (seed >> 2));
  };

  std::size_t h = std::hash<std::string_view>{}(key.text);
  const std::uint32_t packed_foreground =
      (static_cast<std::uint32_t>(key.color.r) << 24) |
      (static_cast<std::uint32_t>(key.color.g) << 16) |
      (static_cast<std::uint32_t>(key.color.b) << 8) |
      static_cast<std::uint32_t>(key.color.a);
  h = mix(h, packed_foreground);
  return h;
}

bool StringTextureCache::KeyEqual::operator()(const KeyView& lhs, const KeyView& rhs) const noexcept {
  return lhs.text == rhs.text && lhs.color.r == rhs.color.r && lhs.color.g == rhs.color.g &&
         lhs.color.b == rhs.color.b && lhs.color.a == rhs.color.a;
}

std::size_t StringTextureCache::EntryByteCost(int width, int height) {
  const std::size_t w = width > 0 ? static_cast<std::size_t>(width) : 0;
  const std::size_t h = height > 0 ? static_cast<std::size_t>(height) : 0;
  return w * h * 4;
}

const StringTextureCache::Entry* StringTextureCache::Find(std::string_view text, SDL_Color color) {
  if (auto it = entries_.find(KeyView{.text = text, .color = color}); it != entries_.end()) {
    util::AddPerformanceCounter(util::PerfCounterId::RenderTextTextureCacheHits);
    LruTouch(it->second);
    return &it->second.entry;
  }
  util::AddPerformanceCounter(util::PerfCounterId::RenderTextTextureCacheMisses);
  return nullptr;
}

const StringTextureCache::Entry* StringTextureCache::Insert(std::string_view text, SDL_Color color,
                                                            SDL_Texture* texture, int width,
                                                            int height) {
  const Entry entry{.texture = texture, .width = width, .height = height};
  const std::size_t entry_bytes = EntryByteCost(width, height);

  Map::node_type recycled;
  if (!entries_.empty() &&
      (entries_.size() + 1 > kMaxEntries || bytes_ + entry_bytes > kMaxBytes)) {
    if (const auto victim_it = EvictOldest(); victim_it != entries_.end()) {
      recycled = entries_.extract(victim_it);
    }
  }

  Map::iterator map_it;
  if (!recycled.empty()) {
    ReserveRoundedKeyCapacity(recycled.key().text, text.size());
    recycled.key().text.assign(text);
    recycled.key().color = color;
    recycled.mapped() = Node{.entry = entry};
    util::AddPerformanceCounter(util::PerfCounterId::RenderTextTextureCacheRecycled);
    map_it = entries_.insert(std::move(recycled)).position;
  } else {
    Key key;
    ReserveRoundedKeyCapacity(key.text, text.size());
    key.text.assign(text);
    key.color = color;
    const auto [it, inserted] = entries_.emplace(std::move(key), Node{.entry = entry});
    map_it = it;
    if (!inserted) {
      LruUnlink(map_it->second);
      bytes_ -= EntryByteCost(map_it->second.entry.width, map_it->second.entry.height);
      if (map_it->second.entry.texture != nullptr) {
        SDL_DestroyTexture(map_it->second.entry.texture);
      }
      map_it->second = Node{.entry = entry};
    }
  }
  bytes_ += entry_bytes;
  map_it->second.key = &map_it->first;
  LruPushBack(map_it->second);

  while (entries_.size() > 1 && (entries_.size() > kMaxEntries || bytes_ > kMaxBytes)) {
    const auto evict_it = EvictOldest();
    if (evict_it == entries_.end()) {
      break;
    }
    entries_.erase(evict_it);
  }

  return &map_it->second.entry;
}

StringTextureCache::Map::iterator StringTextureCache::EvictOldest() {
  Node* victim = lru_head_;
  if (victim == nullptr || victim->key == nullptr) {
    return entries_.end();
  }
  const auto victim_it = entries_.find(*victim->key);
  LruUnlink(*victim);
  if (victim_it != entries_.end()) {
    util::AddPerformanceCounter(util::PerfCounterId::RenderTextTextureCacheEvictions);
    bytes_ -= EntryByteCost(victim_it->second.entry.width, victim_it->second.entry.height);
    if (victim_it->second.entry.texture != nullptr) {
      SDL_DestroyTexture(victim_it->second.entry.texture);
      victim_it->second.entry.texture = nullptr;
    }
  }
  return victim_it;
}

void StringTextureCache::Clear() {
  for (auto& [_, node] : entries_) {
    if (node.entry.texture != nullptr) {
      SDL_DestroyTexture(node.entry.texture);
      node.entry.texture = nullptr;
    }
  }
  entries_.clear();
  lru_head_ = nullptr;
  lru_tail_ = nullptr;
  bytes_ = 0;
}

void StringTextureCache::LruUnlink(Node& node) {
  if (node.lru_prev != nullptr) {
    node.lru_prev->lru_next = node.lru_next;
  } else if (lru_head_ == &node) {
    lru_head_ = node.lru_next;
  }
  if (node.lru_next != nullptr) {
    node.lru_next->lru_prev = node.lru_prev;
  } else if (lru_tail_ == &node) {
    lru_tail_ = node.lru_prev;
  }
  node.lru_prev = nullptr;
  node.lru_next = nullptr;
}

void StringTextureCache::LruPushBack(Node& node) {
  node.lru_prev = lru_tail_;
  node.lru_next = nullptr;
  if (lru_tail_ != nullptr) {
    lru_tail_->lru_next = &node;
  }
  lru_tail_ = &node;
  if (lru_head_ == nullptr) {
    lru_head_ = &node;
  }
}

}
