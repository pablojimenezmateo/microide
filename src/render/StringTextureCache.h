#pragma once

#include <SDL3/SDL.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>

namespace microide::render {

// Whole-string glyph composites, one texture per (text, colour), bounded LRU.
//
// A backend's glyph face -- its fonts, metrics and coverage atlases -- is the same
// for every renderer that draws in that font, so renderers share it
// (TextBackendPool). What each one draws is not: the chrome's labels and the
// editor's rows have nothing in common, and one cache for both makes them evict
// each other under one budget, so the document scrolling past evicts the menu bar
// (TD-2026-10-26-394). This cache is therefore the renderer's, handed to the
// backend with every draw, and it never outlives the glyphs it was filled with:
// TextRenderer clears it whenever its backend, font or scale changes.
//
// Owns its textures; the SDL renderer that created them must outlive it.
class StringTextureCache {
 public:
  struct Entry {
    SDL_Texture* texture = nullptr;
    int width = 0;
    int height = 0;
  };

  // Count and approximate-VRAM bounds; eviction enforces whichever binds first.
  // The entry cap alone does not bound memory: composites have arbitrary width,
  // so a few thousand wide entries can pin hundreds of MB. In normal use most text
  // goes through the ASCII atlas, so the cache stays small and neither bound is hit.
  static constexpr std::size_t kMaxEntries = 4096;
  static constexpr std::size_t kMaxBytes = 48ull * 1024 * 1024;

  StringTextureCache() = default;
  ~StringTextureCache();

  // Non-copyable for the reason SdlTtfTextBackend states: the LRU is intrusive
  // (every entry's links and key pointer point into this object's own map nodes),
  // and the textures would be destroyed twice.
  StringTextureCache(const StringTextureCache&) = delete;
  StringTextureCache& operator=(const StringTextureCache&) = delete;

  // The cached composite for (text, color), touched as most recently used, or
  // nullptr on a miss. Counts the hit or the miss.
  const Entry* Find(std::string_view text, SDL_Color color);
  // Takes ownership of `texture` (width x height device pixels) as the composite
  // for (text, color), which must have just missed, evicting least recently used
  // entries to the budget. The returned entry stays valid until the next Insert
  // or Clear.
  const Entry* Insert(std::string_view text, SDL_Color color, SDL_Texture* texture, int width,
                      int height);
  void Clear();

  std::size_t size() const { return entries_.size(); }

 private:
  struct KeyView {
    std::string_view text;
    SDL_Color color{};
  };

  struct Key {
    std::string text;
    SDL_Color color{};

    operator KeyView() const noexcept { return KeyView{.text = text, .color = color}; }
  };

  struct KeyHash {
    using is_transparent = void;
    std::size_t operator()(const Key& key) const noexcept { return (*this)(static_cast<KeyView>(key)); }
    std::size_t operator()(const KeyView& key) const noexcept;
  };

  struct KeyEqual {
    using is_transparent = void;
    bool operator()(const Key& lhs, const Key& rhs) const noexcept {
      return (*this)(static_cast<KeyView>(lhs), static_cast<KeyView>(rhs));
    }
    bool operator()(const Key& lhs, const KeyView& rhs) const noexcept {
      return (*this)(static_cast<KeyView>(lhs), rhs);
    }
    bool operator()(const KeyView& lhs, const Key& rhs) const noexcept {
      return (*this)(lhs, static_cast<KeyView>(rhs));
    }
    bool operator()(const KeyView& lhs, const KeyView& rhs) const noexcept;
  };

  struct Node {
    Entry entry;
    // Intrusive LRU. The order used to be a `std::list<CacheKey>`, so every
    // insert allocated a list node AND a second copy of the key's string — two
    // allocations per cache MISS, on a path where scrolling through fresh
    // content misses on every row (TD-2026-08-14-224). `unordered_map` never
    // moves its elements, so links into them stay valid across rehash.
    const Key* key = nullptr;  // back-pointer into the map, for eviction
    Node* lru_prev = nullptr;
    Node* lru_next = nullptr;
  };

  using Map = std::unordered_map<Key, Node, KeyHash, KeyEqual>;

  // Approximate VRAM footprint of an entry (RGBA texture: w * h * 4).
  static std::size_t EntryByteCost(int width, int height);
  // Unlinks and destroys the least recently used entry's texture; returns its map
  // position (end() when the LRU is empty). The caller extracts or erases it.
  Map::iterator EvictOldest();
  void LruUnlink(Node& node);
  void LruPushBack(Node& node);
  void LruTouch(Node& node) {
    if (lru_tail_ != &node) {
      LruUnlink(node);
      LruPushBack(node);
    }
  }

  Map entries_;
  Node* lru_head_ = nullptr;
  Node* lru_tail_ = nullptr;
  // Running sum of EntryByteCost over every live entry.
  std::size_t bytes_ = 0;
};

}  // namespace microide::render
