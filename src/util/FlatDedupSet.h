#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string_view>
#include <utility>
#include <vector>

namespace microide::util {

template <typename Key, typename Hash = std::hash<Key>, typename Eq = std::equal_to<Key>>
class FlatDedupSet {
 public:
  explicit FlatDedupSet(std::size_t max_elements) {
    Resize(TableSizeFor(max_elements));
    keys_.reserve(max_elements);
  }

  bool Insert(Key key) {
    std::size_t hash = 0;
    const std::size_t slot = Probe(key, hash);
    if (slots_[slot].index_plus_one != 0) {
      return false;
    }
    Occupy(slot, hash, std::move(key));
    return true;
  }

  std::size_t Intern(Key key) {
    std::size_t hash = 0;
    const std::size_t slot = Probe(key, hash);
    if (slots_[slot].index_plus_one != 0) {
      return slots_[slot].index_plus_one - 1;
    }
    const std::size_t id = keys_.size();
    Occupy(slot, hash, std::move(key));
    return id;
  }

  bool Contains(const Key& key) const {
    std::size_t hash = 0;
    return slots_[Probe(key, hash)].index_plus_one != 0;
  }

  void Reset(std::size_t max_elements) {
    keys_.clear();
    const std::size_t table_size = TableSizeFor(max_elements);
    slots_.assign(table_size, Slot{});
    mask_ = table_size - 1;
    if (keys_.capacity() < max_elements) {
      keys_.reserve(max_elements);
    }
  }

  std::size_t size() const { return keys_.size(); }
  bool empty() const { return keys_.empty(); }
  const std::vector<Key>& keys() const { return keys_; }

 private:
  struct Slot {
    std::size_t hash = 0;
    std::size_t index_plus_one = 0;
  };

  std::size_t Probe(const Key& key, std::size_t& hash) const {
    hash = Hash{}(key);
    std::size_t index = hash & mask_;
    while (slots_[index].index_plus_one != 0) {
      const Slot& slot = slots_[index];
      if (slot.hash == hash && Eq{}(keys_[slot.index_plus_one - 1], key)) {
        break;
      }
      index = (index + 1) & mask_;
    }
    return index;
  }

  void Occupy(std::size_t slot, std::size_t hash, Key key) {
    keys_.push_back(std::move(key));
    slots_[slot] = Slot{.hash = hash, .index_plus_one = keys_.size()};
    if (keys_.size() * 2 > slots_.size()) {
      Resize(slots_.size() * 2);
    }
  }

  static std::size_t TableSizeFor(std::size_t max_elements) {
    std::size_t size = 8;
    while (size < max_elements * 2) {
      size *= 2;
    }
    return size;
  }

  void Resize(std::size_t table_size) {
    slots_.assign(table_size, Slot{});
    mask_ = table_size - 1;
    for (std::size_t i = 0; i < keys_.size(); ++i) {
      const std::size_t hash = Hash{}(keys_[i]);
      std::size_t index = hash & mask_;
      while (slots_[index].index_plus_one != 0) {
        index = (index + 1) & mask_;
      }
      slots_[index] = Slot{.hash = hash, .index_plus_one = i + 1};
    }
  }

  std::vector<Slot> slots_;
  std::vector<Key> keys_;
  std::size_t mask_ = 0;
};

inline constexpr std::size_t kFlatKeyNotFound = static_cast<std::size_t>(-1);

template <typename KeyOf>
class FlatKeyIndex {
 public:
  static constexpr std::size_t kNotFound = kFlatKeyNotFound;

  FlatKeyIndex(std::size_t expected_elements, KeyOf key_of) : key_of_(std::move(key_of)) {
    Resize(TableSizeFor(expected_elements));
  }

  std::size_t Find(std::string_view key) const {
    const std::size_t hash = std::hash<std::string_view>{}(key);
    const std::size_t slot = Probe(key, hash);
    return slots_[slot].index_plus_one == 0 ? kNotFound : slots_[slot].index_plus_one - 1;
  }

  void Insert(std::size_t index) {
    const std::string_view key = key_of_(index);
    const std::size_t hash = std::hash<std::string_view>{}(key);
    const std::size_t slot = Probe(key, hash);
    slots_[slot] = Slot{.hash = hash, .index_plus_one = index + 1};
    ++size_;
    if (size_ * 2 > slots_.size()) {
      Resize(slots_.size() * 2);
    }
  }

  std::size_t size() const { return size_; }

 private:
  struct Slot {
    std::size_t hash = 0;
    std::size_t index_plus_one = 0;
  };

  std::size_t Probe(std::string_view key, std::size_t hash) const {
    std::size_t index = hash & mask_;
    while (slots_[index].index_plus_one != 0) {
      const Slot& slot = slots_[index];
      if (slot.hash == hash && key_of_(slot.index_plus_one - 1) == key) {
        break;
      }
      index = (index + 1) & mask_;
    }
    return index;
  }

  static std::size_t TableSizeFor(std::size_t expected_elements) {
    std::size_t size = 8;
    while (size < expected_elements * 2) {
      size *= 2;
    }
    return size;
  }

  void Resize(std::size_t table_size) {
    std::vector<Slot> previous;
    previous.swap(slots_);
    slots_.assign(table_size, Slot{});
    mask_ = table_size - 1;
    for (const Slot& slot : previous) {
      if (slot.index_plus_one == 0) {
        continue;
      }
      std::size_t index = slot.hash & mask_;
      while (slots_[index].index_plus_one != 0) {
        index = (index + 1) & mask_;
      }
      slots_[index] = slot;
    }
  }

  KeyOf key_of_;
  std::vector<Slot> slots_;
  std::size_t mask_ = 0;
  std::size_t size_ = 0;
};

}
