#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "editor/TextBuffer.h"

namespace microide::editor {

class LineSpan {
 public:
  LineSpan(const std::vector<std::string>& lines)  // NOLINT(google-explicit-constructor)
      : source_(&lines),
        size_fn_(&VectorSize),
        at_fn_(&VectorAt),
        length_fn_(&VectorLength),
        window_fn_(&VectorWindow) {}

  LineSpan(const std::vector<std::string_view>& lines)  // NOLINT(google-explicit-constructor)
      : source_(&lines),
        size_fn_(&ViewVectorSize),
        at_fn_(&ViewVectorAt),
        length_fn_(&ViewVectorLength),
        window_fn_(&ViewVectorWindow) {}

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
  static std::size_t ViewVectorSize(const void* p) {
    return static_cast<const std::vector<std::string_view>*>(p)->size();
  }
  static std::string_view ViewVectorAt(const void* p, std::size_t i) {
    return (*static_cast<const std::vector<std::string_view>*>(p))[i];
  }
  static std::size_t ViewVectorLength(const void* p, std::size_t i) {
    return (*static_cast<const std::vector<std::string_view>*>(p))[i].size();
  }
  static std::string_view ViewVectorWindow(const void* p, std::size_t i, std::size_t byte_start,
                                           std::size_t byte_len, std::string&) {
    const std::string_view line = (*static_cast<const std::vector<std::string_view>*>(p))[i];
    if (byte_start >= line.size()) return {};
    return line.substr(byte_start, byte_len);
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

}
