#pragma once

#include <memory>
#include <utility>
#include <vector>

#include "editor/TextLayoutCache.h"

namespace microide::editor::detail {

template <typename T>
class ScratchBuffer : public std::vector<T> {
 public:
  ScratchBuffer() = default;
  ScratchBuffer(const ScratchBuffer&) : std::vector<T>() {}
  ScratchBuffer& operator=(const ScratchBuffer&) {
    this->clear();
    return *this;
  }
  ScratchBuffer(ScratchBuffer&&) noexcept = default;
  ScratchBuffer& operator=(ScratchBuffer&&) noexcept = default;
};

template <typename T>
class NotTransferred {
 public:
  NotTransferred() = default;
  NotTransferred(T value) : value_(value) {}  // NOLINT(google-explicit-constructor)
  NotTransferred(const NotTransferred&) {}
  NotTransferred(NotTransferred&& other) noexcept { other.value_ = T{}; }
  NotTransferred& operator=(const NotTransferred&) {
    value_ = T{};
    return *this;
  }
  NotTransferred& operator=(NotTransferred&& other) noexcept {
    value_ = T{};
    other.value_ = T{};
    return *this;
  }
  NotTransferred& operator=(T value) {
    value_ = value;
    return *this;
  }
  operator T() const { return value_; }  // NOLINT(google-explicit-constructor)
  T operator->() const { return value_; }

 private:
  T value_{};
};

template <typename T>
class SharedOnMove : public std::shared_ptr<T> {
 public:
  SharedOnMove() = default;
  SharedOnMove(std::shared_ptr<T> pointer)  // NOLINT(google-explicit-constructor)
      : std::shared_ptr<T>(std::move(pointer)) {}
  SharedOnMove(const SharedOnMove&) = default;
  SharedOnMove(SharedOnMove&& other) noexcept : std::shared_ptr<T>(other) {}
  SharedOnMove& operator=(const SharedOnMove&) = default;
  SharedOnMove& operator=(SharedOnMove&& other) noexcept {
    std::shared_ptr<T>::operator=(other);
    return *this;
  }
  SharedOnMove& operator=(std::shared_ptr<T> pointer) noexcept {
    std::shared_ptr<T>::operator=(std::move(pointer));
    return *this;
  }
};

class TransferredLayoutCache : public TextLayoutCache {
 public:
  TransferredLayoutCache() = default;
  TransferredLayoutCache(const TransferredLayoutCache& other) : TextLayoutCache(other) {
    DropWrappedRowLayouts();
  }
  TransferredLayoutCache(TransferredLayoutCache&& other) noexcept
      : TextLayoutCache(std::move(other)) {
    other.Reset();
    DropWrappedRowLayouts();
  }
  TransferredLayoutCache& operator=(const TransferredLayoutCache& other) {
    TextLayoutCache::operator=(other);
    DropWrappedRowLayouts();
    return *this;
  }
  TransferredLayoutCache& operator=(TransferredLayoutCache&& other) noexcept {
    TextLayoutCache::operator=(std::move(other));
    other.Reset();
    DropWrappedRowLayouts();
    return *this;
  }

 private:
  void Reset() { static_cast<TextLayoutCache&>(*this) = TextLayoutCache{}; }
};

}
