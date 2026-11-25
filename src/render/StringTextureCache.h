#pragma once

#include <SDL3/SDL.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>

namespace microide::render {

class StringTextureCache {
 public:
  struct Entry {
    SDL_Texture* texture = nullptr;
    int width = 0;
    int height = 0;
  };

  static constexpr std::size_t kMaxEntries = 4096;
  static constexpr std::size_t kMaxBytes = 48ull * 1024 * 1024;

  StringTextureCache() = default;
  ~StringTextureCache();

  StringTextureCache(const StringTextureCache&) = delete;
  StringTextureCache& operator=(const StringTextureCache&) = delete;

  const Entry* Find(std::string_view text, SDL_Color color);
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
    const Key* key = nullptr;
    Node* lru_prev = nullptr;
    Node* lru_next = nullptr;
  };

  using Map = std::unordered_map<Key, Node, KeyHash, KeyEqual>;

  static std::size_t EntryByteCost(int width, int height);
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
  std::size_t bytes_ = 0;
};

}
