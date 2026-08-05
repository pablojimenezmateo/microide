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
        length_fn_(&VectorLength),
        window_fn_(&VectorWindow) {}

  LineSpan(const TextBuffer& buffer)  // NOLINT(google-explicit-constructor)
      : source_(&buffer),
        size_fn_(&BufferSize),
        at_fn_(&BufferAt),
        length_fn_(&BufferLength),
        window_fn_(&BufferWindow) {}

  std::size_t size() const { return size_fn_(source_); }
  bool empty() const { return size() == 0; }
  std::string_view operator[](std::size_t index) const { return at_fn_(source_, index); }
  std::size_t LineLength(std::size_t index) const { return length_fn_(source_, index); }

  // Bounded slice of one line: `[byte_start, byte_start + byte_len)` clamped to
  // the line, zero-copy whenever the source can serve it that way. `scratch` is
  // only written when the underlying store cannot view the window contiguously
  // (a piece-tree line that spans pieces, i.e. an edited one); the returned view
  // is then into `scratch` and lives until it is reused.
  //
  // `operator[]` is the whole line, and on a piece-tree source that means
  // materializing the whole line even when the caller wanted 4 KiB of it. Any
  // caller with a bound belongs here instead (TD-2026-08-05-133).
  std::string_view LineWindow(std::size_t index, std::size_t byte_start, std::size_t byte_len,
                              std::string& scratch) const {
    return window_fn_(source_, index, byte_start, byte_len, scratch);
  }

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
  static std::string_view VectorWindow(const void* p, std::size_t i, std::size_t byte_start,
                                       std::size_t byte_len, std::string&) {
    const std::string& line = (*static_cast<const std::vector<std::string>*>(p))[i];
    if (byte_start >= line.size()) return {};
    return std::string_view(line).substr(byte_start, byte_len);
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
  static std::string_view BufferWindow(const void* p, std::size_t i, std::size_t byte_start,
                                       std::size_t byte_len, std::string& scratch) {
    return static_cast<const TextBuffer*>(p)->LineWindow(i, byte_start, byte_len, scratch);
  }

  const void* source_;
  std::size_t (*size_fn_)(const void*);
  std::string_view (*at_fn_)(const void*, std::size_t);
  std::size_t (*length_fn_)(const void*, std::size_t);
  std::string_view (*window_fn_)(const void*, std::size_t, std::size_t, std::size_t, std::string&);
};

}  // namespace microide::editor
