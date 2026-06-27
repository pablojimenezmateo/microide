#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "editor/TextBuffer.h"

namespace microide::editor {

// Non-owning, representation-agnostic view over a sequence of text lines.
//
// Layout/scroll/render code must read lines without caring whether the source is
// a piece-tree-backed `TextBuffer` (the live document) or a plain
// `std::vector<std::string>` (compare/merge models, undo deltas). LineSpan is the
// seam: it adapts either source behind `size()` + `operator[]` returning a
// `std::string_view`, with no allocation and no virtual dispatch -- just a data
// pointer and two free-function thunks. For a `TextBuffer` source, `operator[]`
// goes straight through `LineView`, so hot paths never materialize the whole
// document the way `Snapshot()` would.
//
// A LineSpan does not own its source; it must not outlive it, and (like the
// underlying line storage) returned views are invalidated by any mutation of the
// source.
class LineSpan {
 public:
  LineSpan(const std::vector<std::string>& lines)  // NOLINT(google-explicit-constructor)
      : source_(&lines),
        size_fn_(&VectorSize),
        at_fn_(&VectorAt),
        length_fn_(&VectorLength) {}

  LineSpan(const TextBuffer& buffer)  // NOLINT(google-explicit-constructor)
      : source_(&buffer),
        size_fn_(&BufferSize),
        at_fn_(&BufferAt),
        length_fn_(&BufferLength) {}

  std::size_t size() const { return size_fn_(source_); }
  bool empty() const { return size() == 0; }
  std::string_view operator[](std::size_t index) const { return at_fn_(source_, index); }
  std::size_t LineLength(std::size_t index) const { return length_fn_(source_, index); }

 private:
  static std::size_t VectorSize(const void* p) {
    return static_cast<const std::vector<std::string>*>(p)->size();
  }
  static std::string_view VectorAt(const void* p, std::size_t i) {
    return (*static_cast<const std::vector<std::string>*>(p))[i];
  }
  static std::size_t VectorLength(const void* p, std::size_t i) {
    return (*static_cast<const std::vector<std::string>*>(p))[i].size();
  }
  static std::size_t BufferSize(const void* p) {
    return static_cast<const TextBuffer*>(p)->size();
  }
  static std::string_view BufferAt(const void* p, std::size_t i) {
    return static_cast<const TextBuffer*>(p)->LineView(i);
  }
  static std::size_t BufferLength(const void* p, std::size_t i) {
    return static_cast<const TextBuffer*>(p)->LineLength(i);
  }

  const void* source_;
  std::size_t (*size_fn_)(const void*);
  std::string_view (*at_fn_)(const void*, std::size_t);
  std::size_t (*length_fn_)(const void*, std::size_t);
};

}  // namespace microide::editor
