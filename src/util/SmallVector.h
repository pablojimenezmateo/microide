#pragma once

#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <memory>
#include <type_traits>
#include <utility>

namespace microide::util {

// Growable sequence with `N` elements of inline storage, spilling to the heap
// only past that.
//
// The difference from `InlineVector` (same directory) is the spill: use
// `InlineVector` when the maximum size is a structural cap and exceeding it is a
// bug, and this when the size is usually small but genuinely unbounded.
// Per-event redraw damage is the shape that motivated it — a mouse move queues
// one rect, but a sidebar refresh can queue many, so a hard cap would silently
// drop damage and paint stale pixels.
//
// Restricted to trivially copyable, trivially destructible `T`. That is what
// keeps the implementation short enough to trust: growth is a copy of `size()`
// objects with no per-element construction, destruction, or exception-safety
// window, and copy/move assignment are the same. Nothing here needs to be
// general — it needs to be right.
//
// The heap buffer goes through `new[]`/`delete[]` rather than malloc so that a
// spill is visible to the perf harness's counting `operator new`. A container
// that hides its allocations from the allocation gate is worse than one that
// allocates.
template <typename T, std::size_t N>
class SmallVector {
  static_assert(N > 0, "SmallVector needs a non-zero inline capacity");
  static_assert(std::is_trivially_copyable_v<T>,
                "SmallVector grows by copying raw elements");
  static_assert(std::is_trivially_destructible_v<T>,
                "SmallVector does not run element destructors");
  static_assert(std::is_default_constructible_v<T>,
                "SmallVector value-initialises its inline slots");

 public:
  using value_type = T;
  using size_type = std::size_t;
  using reference = T&;
  using const_reference = const T&;
  using iterator = T*;
  using const_iterator = const T*;

  SmallVector() = default;

  SmallVector(std::initializer_list<T> init) { Assign(init.begin(), init.size()); }

  SmallVector(const SmallVector& other) { Assign(other.data(), other.size_); }

  SmallVector& operator=(const SmallVector& other) {
    if (this != &other) {
      Assign(other.data(), other.size_);
    }
    return *this;
  }

  SmallVector(SmallVector&& other) noexcept { Steal(other); }

  SmallVector& operator=(SmallVector&& other) noexcept {
    if (this != &other) {
      FreeHeap();
      size_ = 0;
      Steal(other);
    }
    return *this;
  }

  ~SmallVector() { FreeHeap(); }

  size_type size() const { return size_; }
  size_type capacity() const { return capacity_; }
  bool empty() const { return size_ == 0; }
  // Whether the elements currently live on the heap. Diagnostic — a caller that
  // believed its inline capacity covered the common case can check.
  bool spilled() const { return heap_ != nullptr; }

  T* data() { return heap_ != nullptr ? heap_ : inline_storage_; }
  const T* data() const { return heap_ != nullptr ? heap_ : inline_storage_; }

  iterator begin() { return data(); }
  iterator end() { return data() + size_; }
  const_iterator begin() const { return data(); }
  const_iterator end() const { return data() + size_; }
  const_iterator cbegin() const { return begin(); }
  const_iterator cend() const { return end(); }

  reference operator[](size_type index) { return data()[index]; }
  const_reference operator[](size_type index) const { return data()[index]; }
  reference front() { return data()[0]; }
  const_reference front() const { return data()[0]; }
  reference back() { return data()[size_ - 1]; }
  const_reference back() const { return data()[size_ - 1]; }

  // Keeps the current buffer (inline or heap). A cleared-and-refilled instance
  // is allocation-free once it has seen its high-water mark, which is the whole
  // point of reusing one rather than constructing a fresh one per event.
  void clear() { size_ = 0; }

  void push_back(const T& value) {
    if (size_ == capacity_) {
      Grow(capacity_ * 2);
    }
    data()[size_++] = value;
  }

  template <typename... Args>
  reference emplace_back(Args&&... args) {
    if (size_ == capacity_) {
      Grow(capacity_ * 2);
    }
    data()[size_] = T{std::forward<Args>(args)...};
    return data()[size_++];
  }

  void reserve(size_type required) {
    if (required > capacity_) {
      Grow(required);
    }
  }

  template <typename InputIt>
  void append(InputIt first, InputIt last) {
    for (; first != last; ++first) {
      push_back(*first);
    }
  }

  // Exchanges contents in O(1) when both sides are on the heap, and by copying
  // the inline slabs otherwise. Used to hand a filled buffer to a consumer while
  // taking its (already-grown) buffer back, so neither side ever reallocates.
  void swap(SmallVector& other) noexcept {
    if (heap_ != nullptr && other.heap_ != nullptr) {
      std::swap(heap_, other.heap_);
      std::swap(capacity_, other.capacity_);
      std::swap(size_, other.size_);
      return;
    }
    SmallVector temp(std::move(other));
    other = std::move(*this);
    *this = std::move(temp);
  }

  friend bool operator==(const SmallVector& lhs, const SmallVector& rhs) {
    return lhs.size_ == rhs.size_ && std::equal(lhs.begin(), lhs.end(), rhs.begin());
  }

 private:
  void Assign(const T* source, size_type count) {
    if (count > capacity_) {
      Grow(count);
    }
    std::copy_n(source, count, data());
    size_ = count;
  }

  void Steal(SmallVector& other) {
    if (other.heap_ != nullptr) {
      heap_ = other.heap_;
      capacity_ = other.capacity_;
      size_ = other.size_;
      other.heap_ = nullptr;
      other.capacity_ = N;
      other.size_ = 0;
      return;
    }
    std::copy_n(other.inline_storage_, other.size_, inline_storage_);
    size_ = other.size_;
    other.size_ = 0;
  }

  // `std::allocator`, not `new T[]`.
  //
  // Two reasons, and the second is the one that bit. `new T[n]` VALUE-INITIALISES
  // all n slots, which for a spilled buffer is a memset of capacity the caller has
  // not written yet -- pure waste for a type this container already requires to be
  // trivially copyable and trivially destructible.
  //
  // And `delete[]` on a trivially-destructible type compiles to the UNSIZED
  // `operator delete[](void*)`, which the perf harness's counting allocator can
  // only record as "freed 0 bytes" -- it has no size to report. `std::allocator`
  // routes to the SIZED overload where the compiler supports it, portably: a
  // direct `::operator delete(p, n)` does not compile under clang, which defaults
  // to `-fno-sized-deallocation` (caught by the second-compiler lane). So a SmallVector
  // that spilled looked, to `p50_net_heap_bytes`, like memory that was allocated
  // and never released: moving `LineBlob::starts_` onto this container moved
  // `editor_sort_lines_large` from 2,476 to 162,476 net bytes with its allocation
  // count and its resident growth both unchanged (TD-2026-08-15-249). Sized
  // deallocation reports the real number.
  void Grow(size_type required) {
    const size_type new_capacity = std::max<size_type>(required, capacity_ * 2);
    T* buffer = std::allocator<T>{}.allocate(new_capacity);
    std::copy_n(data(), size_, buffer);
    FreeHeap();
    heap_ = buffer;
    capacity_ = new_capacity;
  }

  // Frees the spilled buffer, if any, passing the size the sized-deallocation
  // overload needs. Deliberately does NOT touch `size_`: `Grow` still holds the
  // elements it just copied into the new buffer.
  void FreeHeap() noexcept {
    if (heap_ != nullptr) {
      std::allocator<T>{}.deallocate(heap_, capacity_);
      heap_ = nullptr;
    }
    capacity_ = N;
  }

  T inline_storage_[N]{};
  T* heap_ = nullptr;
  size_type size_ = 0;
  size_type capacity_ = N;
};

}  // namespace microide::util
