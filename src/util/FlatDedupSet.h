#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

namespace microide::util {

// First-occurrence-wins de-duplication with a constant number of allocations.
//
// `std::unordered_set` is a node-per-element container: inserting N keys costs N
// heap nodes (~48 bytes each for a std::string key, plus the string's own buffer)
// on top of the bucket array, and every probe is a pointer chase into separately
// allocated memory. For the shape TD-2026-08-06-159's grep list names — a set
// built inside a function, used once, and thrown away — that node cost IS the
// cost of the function. `assist_merge::RankedUnion` merging two 6,000-item
// provider result lists was 6,002 allocations, one per distinct key.
//
// This is open addressing over two flat arrays instead: the slot table (sized
// once from the caller's upper bound, load factor <= 0.5) and the key storage.
// Two allocations for the whole operation, and the probe stays in cache.
//
// The stored hash in each slot means a probe compares keys only when the hashes
// match, so a colliding-bucket walk over string keys does not run memcmp per
// step. Growth exists so an under-estimated bound stays correct rather than
// spinning; a caller that passes a real upper bound never pays it.
//
// `Intern` makes this a value map as well, without a second container: because
// keys live in an append-only vector, a key's index in `keys()` IS its dense
// equality-class id, so a caller that wants a value per distinct key keeps a
// parallel `std::vector<V>` indexed by that id — one allocation instead of a
// node per element. That is what the compare path uses in place of the two
// `unordered_map`s it used to build per diff (TD-2026-08-07-164).
template <typename Key, typename Hash = std::hash<Key>, typename Eq = std::equal_to<Key>>
class FlatDedupSet {
 public:
  // `max_elements` is the expected upper bound on distinct keys. The table is
  // sized to the next power of two at or above 2x it, so a correct bound means
  // exactly one slot-table allocation for the lifetime of the set.
  explicit FlatDedupSet(std::size_t max_elements) {
    Resize(TableSizeFor(max_elements));
    keys_.reserve(max_elements);
  }

  // Returns true when `key` was not already present (and takes ownership of it),
  // false when it was (and `key` is left untouched, so the caller's move is not
  // consumed on the duplicate path).
  bool Insert(Key key) {
    std::size_t hash = 0;
    const std::size_t slot = Probe(key, hash);
    if (slots_[slot].index_plus_one != 0) {
      return false;
    }
    Occupy(slot, hash, std::move(key));
    return true;
  }

  // Equality-class id for `key`: its index in `keys()`. A key seen for the first
  // time is appended (taking ownership) and gets the next id, so ids are dense
  // and assigned in first-occurrence order.
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

  std::size_t size() const { return keys_.size(); }
  bool empty() const { return keys_.empty(); }
  // Distinct keys in first-occurrence order — the insertion order is preserved
  // because keys live in their own append-only vector.
  const std::vector<Key>& keys() const { return keys_; }

 private:
  struct Slot {
    std::size_t hash = 0;
    std::size_t index_plus_one = 0;  // 0 = empty; otherwise index into keys_ + 1
  };

  // Linear probe to `key`'s slot: either the one holding it, or the first empty
  // slot on its chain (the insertion point). Also reports the key's hash so a
  // caller that inserts does not compute it twice.
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
    // Keep the load factor at or below 1/2: linear probing degrades sharply past
    // that, and an under-estimated bound must not turn into a quadratic walk.
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

}  // namespace microide::util
