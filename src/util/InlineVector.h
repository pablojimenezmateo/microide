#pragma once

#include <array>
#include <cassert>
#include <cstddef>
#include <initializer_list>
#include <type_traits>
#include <utility>

namespace microide::util {

// Fixed-capacity, heap-free sequence for containers with a *structural* cap.
//
// `std::vector` is the wrong shape for a container whose maximum size is a
// property of the design rather than of the data: it costs a heap round-trip
// per construction to carry one or two small structs. The editor-pane layouts
// are the case that motivated this — a mouse drag rebuilt them three times per
// motion event, and each rebuild was two `std::vector` allocations to carry a
// single-element result (TD-2026-08-06-145).
//
// This is NOT a small-vector with heap spill. `N` is a hard capacity; exceeding
// it is a programming error, asserted in debug and clamped (the extra element is
// dropped) in release. Only use it where the cap is enforced upstream by a
// shared constant — `kMaxEditorGroups`, not "2 is probably enough".
//
// All `N` slots are value-initialised on construction, so `T` must be default
// constructible and cheap to default construct. That keeps the implementation
// free of manual lifetime management (no placement new, no launder, no
// exception-safety window) at the cost of constructing slots that may go
// unused — the right trade for the small POD-ish payloads this exists for.
template <typename T, std::size_t N>
class InlineVector {
  static_assert(N > 0, "InlineVector needs a non-zero capacity");
  static_assert(std::is_default_constructible_v<T>,
                "InlineVector value-initialises every slot");

 public:
  using value_type = T;
  using size_type = std::size_t;
  using reference = T&;
  using const_reference = const T&;
  using iterator = T*;
  using const_iterator = const T*;

  constexpr InlineVector() = default;

  constexpr InlineVector(std::initializer_list<T> init) {
    for (const T& value : init) {
      push_back(value);
    }
  }

  static constexpr size_type capacity() { return N; }
  constexpr size_type size() const { return size_; }
  constexpr bool empty() const { return size_ == 0; }

  constexpr T* data() { return storage_.data(); }
  constexpr const T* data() const { return storage_.data(); }

  constexpr iterator begin() { return storage_.data(); }
  constexpr iterator end() { return storage_.data() + size_; }
  constexpr const_iterator begin() const { return storage_.data(); }
  constexpr const_iterator end() const { return storage_.data() + size_; }
  constexpr const_iterator cbegin() const { return begin(); }
  constexpr const_iterator cend() const { return end(); }

  constexpr reference operator[](size_type index) { return storage_[index]; }
  constexpr const_reference operator[](size_type index) const { return storage_[index]; }
  constexpr reference front() { return storage_[0]; }
  constexpr const_reference front() const { return storage_[0]; }
  constexpr reference back() { return storage_[size_ - 1]; }
  constexpr const_reference back() const { return storage_[size_ - 1]; }

  constexpr void clear() { size_ = 0; }

  constexpr void push_back(const T& value) {
    assert(size_ < N && "InlineVector capacity exceeded");
    if (size_ >= N) {
      return;
    }
    storage_[size_++] = value;
  }

  constexpr void push_back(T&& value) {
    assert(size_ < N && "InlineVector capacity exceeded");
    if (size_ >= N) {
      return;
    }
    storage_[size_++] = std::move(value);
  }

  // Mirrors `std::vector::emplace_back` for the aggregate case: the slot is
  // already constructed, so this assigns a freshly built `T` into it and returns
  // the slot. Returns `back()` unchanged when the capacity is exceeded.
  template <typename... Args>
  constexpr reference emplace_back(Args&&... args) {
    assert(size_ < N && "InlineVector capacity exceeded");
    if (size_ >= N) {
      return storage_[N - 1];
    }
    storage_[size_] = T{std::forward<Args>(args)...};
    return storage_[size_++];
  }

  friend constexpr bool operator==(const InlineVector& lhs, const InlineVector& rhs) {
    if (lhs.size_ != rhs.size_) {
      return false;
    }
    for (size_type i = 0; i < lhs.size_; ++i) {
      if (!(lhs.storage_[i] == rhs.storage_[i])) {
        return false;
      }
    }
    return true;
  }

 private:
  std::array<T, N> storage_{};
  size_type size_ = 0;
};

}  // namespace microide::util
