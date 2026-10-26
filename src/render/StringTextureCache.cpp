#include "render/StringTextureCache.h"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <functional>
#include <utility>

#include "util/PerformanceCounters.h"

namespace microide::render {

namespace {

// Give a recycled cache key a capacity that the NEXT row is likely to fit too.
//
// The eviction path hands the victim's map node back so the key string's buffer
// is reused instead of freed and re-allocated (TD-2026-08-15-236). That only
// pays off if the buffer refits: a string sized exactly to the row it last held
// fits the next row only when that row is no longer, which on real text is a
// coin flip. Rounding to a power of two collapses the cache's key buffers onto a
// handful of sizes; the backend's `ResolveEntry` was the #1 allocator of six
// render-frame phases with the exact sizing.
//
// libstdc++'s 15-byte SSO covers short runs, so the floor starts above it.
void ReserveRoundedKeyCapacity(std::string& key_text, std::size_t needed) {
  if (key_text.capacity() >= needed) {
    return;
  }
  key_text.reserve(std::bit_ceil(std::max<std::size_t>(needed, 32)));
}

}  // namespace

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

  // Evict FIRST when this insert would put the cache over either budget, and
  // build into the victim's node instead of erasing it and emplacing a fresh one.
  //
  // The pair is the steady state, not a corner: a scroll through fresh content
  // misses on every row, and once the VRAM budget binds every miss IS an eviction
  // — measured at 685 misses and 685 evictions across one scroll scenario. So the
  // erase-then-emplace cost a map node plus a fresh copy of the row's own text,
  // per row, per frame, and freed the identical pair one row later. `extract`
  // hands the node back with the key string's capacity intact; assigning over it
  // reuses that too (TD-2026-08-15-236).
  //
  // The `!entries_.empty()` guard is the counterpart of the trailing loop's `> 1`:
  // a single composite that alone exceeds kMaxBytes must not evict itself and
  // dangle the pointer this returns. Here it cannot — the victim is chosen before
  // the new entry is in the map.
  Map::node_type recycled;
  if (!entries_.empty() &&
      (entries_.size() + 1 > kMaxEntries || bytes_ + entry_bytes > kMaxBytes)) {
    if (const auto victim_it = EvictOldest(); victim_it != entries_.end()) {
      recycled = entries_.extract(victim_it);
    }
  }

  Map::iterator map_it;
  if (!recycled.empty()) {
    // `text` is a view of the caller's row text, never of a key in this map (we
    // are here because `Find` missed), so overwriting the node's key cannot
    // invalidate it.
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
      // Unreachable in practice — we are here because `Find` missed — but the
      // assignment below overwrites the LRU links, so the existing entry has to
      // leave the list before it stops describing where it sits in it.
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

  // Evict oldest entries until both the count and VRAM budgets are satisfied.
  // Keep at least the just-inserted entry (>1 guard) so a single oversized
  // composite that alone exceeds kMaxBytes never evicts itself, which would
  // dangle the returned pointer.
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
  // `find` before `extract`/`erase`, not by key: the key lives inside the node
  // they consume, so looking it up by reference afterwards would hash freed memory.
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

}  // namespace microide::render
