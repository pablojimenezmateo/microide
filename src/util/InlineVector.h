#pragma once

#include <array>
#include <cassert>
#include <cstddef>
#include <initializer_list>
#include <type_traits>
#include <utility>

namespace microide::util {

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

  template <typename... Args>
  constexpr reference emplace_back(Args&&... args) {
    assert(size_ < N && "InlineVector capacity exceeded");
    if (size_ >= N) {
      return storage_[N - 1];
    }
    storage_[size_] = T{std::forward<Args>(args)...};
    return storage_[size_++];
  }

  constexpr void insert(size_type index, T value) {
    assert(size_ < N && "InlineVector capacity exceeded");
    assert(index <= size_ && "InlineVector insert out of range");
    if (size_ >= N || index > size_) {
      return;
    }
    for (size_type i = size_; i > index; --i) {
      storage_[i] = std::move(storage_[i - 1]);
    }
    storage_[index] = std::move(value);
    ++size_;
  }

  constexpr void erase(size_type index) {
    assert(index < size_ && "InlineVector erase out of range");
    if (index >= size_) {
      return;
    }
    for (size_type i = index + 1; i < size_; ++i) {
      storage_[i - 1] = std::move(storage_[i]);
    }
    storage_[--size_] = T{};
  }

  constexpr void pop_back() {
    assert(size_ > 0 && "InlineVector pop_back on empty");
    if (size_ > 0) {
      storage_[--size_] = T{};
    }
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

}
