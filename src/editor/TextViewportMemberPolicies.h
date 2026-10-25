#pragma once

// How the TextViewport members that must NOT be copied or moved memberwise do
// travel, each said once at its declaration. TextViewport used to spell its four
// special members out by hand, listing every one of ~70 members three times, so a
// member added without updating all three lists would have been silently
// default-initialized by the copy. With these wrappers the special members are
// defaulted and a new member needs no edit beyond its own declaration.

#include <memory>
#include <utility>
#include <vector>

#include "editor/TextLayoutCache.h"

namespace microide::editor::detail {

// A scratch buffer holds no state between calls -- every use clears it on entry --
// so a copy starts empty rather than allocating to carry bytes the next call
// throws away (the capacity rebuilds on first use). A move steals the buffer,
// which is free and keeps the destination's next rebuild allocation-free.
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

// A value that belongs to one object and never travels: a copy or a move starts
// the destination at `T{}`, and a move leaves the source at `T{}` too. For a
// non-owning pointer the destination must re-attach itself, and for a transient
// flag a copy must not inherit a scope it is not in.
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

// A shared handle that a move SHARES instead of stealing, so a moved-from owner
// still holds it. TextViewport's document: a moved-from viewport keeps a document
// to answer from rather than one that most of its methods would dereference null.
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

// The viewport's layout cache. Widths are a function of the document's bytes and
// the tab size, and a copy or move carries both, so the per-line width table and
// the visible-line cache entries keyed on them stay exactly right -- dropping them
// cost an O(document) rebuild per copy, and reloading an already-open buffer
// copies a viewport twice (TD-2026-08-06-138). Only the wrapped-row table also
// depends on the folding model, which does not travel (NotTransferred), so that
// half is dropped. A moved-from cache is reset to empty.
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

}  // namespace microide::editor::detail
