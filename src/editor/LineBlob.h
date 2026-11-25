#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "util/SmallVector.h"

namespace microide::editor {

class LineBlob {
 public:
  LineBlob() = default;

  explicit LineBlob(std::span<const std::string> lines) {
    reserve_lines(lines.size());
    for (const std::string& line : lines) {
      push_back(line);
    }
  }
  explicit LineBlob(const std::vector<std::string>& lines)
      : LineBlob(std::span<const std::string>(lines)) {}
  static LineBlob Of(std::initializer_list<std::string_view> lines) {
    LineBlob out;
    out.reserve_lines(lines.size());
    for (std::string_view line : lines) {
      out.push_back(line);
    }
    return out;
  }

#if defined(__clang__)
  [[clang::reinitializes]]
#endif
  void clear() {
    data_.clear();
    starts_.clear();
    head_ = 0;
    first_line_ = 0;
  }
  void reserve_lines(std::size_t count) { starts_.reserve(first_line_ + count); }
  void reserve(std::size_t count) { reserve_lines(count); }
  void assign(std::span<const std::string> lines) {
    clear();
    reserve_lines(lines.size());
    for (const std::string& line : lines) {
      push_back(line);
    }
  }
  void reserve_bytes(std::size_t bytes) { data_.reserve(head_ + bytes); }

  void push_back(std::string_view line) {
    starts_.push_back(static_cast<std::uint32_t>(data_.size()));
    data_.append(line);
  }
  void emplace_back(const char* data, std::size_t length) {
    push_back(std::string_view(data, length));
  }
  void begin_line() { starts_.push_back(static_cast<std::uint32_t>(data_.size())); }
  void append_to_last(std::string_view piece) { data_.append(piece); }
  template <typename... Pieces>
  void push_joined(Pieces... pieces) {
    begin_line();
    (append_to_last(pieces), ...);
  }

  std::size_t size() const { return starts_.size() - first_line_; }
  bool empty() const { return size() == 0; }
  std::string_view operator[](std::size_t index) const {
    assert(index < size());
    const std::size_t begin = StartOf(index);
    return std::string_view(data_).substr(begin, EndOf(index) - begin);
  }
  std::string_view front() const { return (*this)[0]; }
  std::string_view back() const { return (*this)[size() - 1]; }

  class const_iterator {
   public:
    using iterator_category = std::forward_iterator_tag;
    using value_type = std::string_view;
    using difference_type = std::ptrdiff_t;
    using pointer = const std::string_view*;
    using reference = std::string_view;

    const_iterator() = default;
    const_iterator(const LineBlob* blob, std::size_t index) : blob_(blob), index_(index) {}
    std::string_view operator*() const { return (*blob_)[index_]; }
    const_iterator& operator++() {
      ++index_;
      return *this;
    }
    const_iterator operator++(int) {
      const_iterator copy = *this;
      ++index_;
      return copy;
    }
    bool operator==(const const_iterator& other) const { return index_ == other.index_; }
    bool operator!=(const const_iterator& other) const { return index_ != other.index_; }

   private:
    const LineBlob* blob_ = nullptr;
    std::size_t index_ = 0;
  };
  const_iterator begin() const { return const_iterator(this, 0); }
  const_iterator end() const { return const_iterator(this, size()); }

  LineBlob SubRange(std::size_t first, std::size_t last) const {
    LineBlob out;
    if (first >= last || first >= size()) {
      return out;
    }
    const std::size_t clamped_last = std::min(last, size());
    const std::size_t byte_begin = StartOf(first);
    const std::size_t byte_end = EndOf(clamped_last - 1);
    out.data_.assign(data_, byte_begin, byte_end - byte_begin);
    out.starts_.reserve(clamped_last - first);
    for (std::size_t i = first; i < clamped_last; ++i) {
      out.starts_.push_back(static_cast<std::uint32_t>(StartOf(i) - byte_begin));
    }
    return out;
  }

  void append(const LineBlob& other) {
    if (&other == this) {
      const LineBlob copy(other);
      append(copy);
      return;
    }
    const std::size_t base = data_.size();
    data_.reserve(base + other.content_bytes());
    data_.append(other.Bytes());
    starts_.reserve(starts_.size() + other.size());
    for (std::size_t i = 0; i < other.size(); ++i) {
      starts_.push_back(static_cast<std::uint32_t>(base + other.LogicalStart(i)));
    }
  }
  void prepend(const LineBlob& other) {
    if (&other == this) {
      const LineBlob copy(other);
      prepend(copy);
      return;
    }
    if (other.empty()) {
      return;
    }
    if (empty()) {
      *this = other;
      return;
    }
    const std::size_t bytes = other.content_bytes();
    const std::size_t lines = other.size();
    if (head_ < bytes || first_line_ < lines) {
      GrowFront(std::max(bytes, content_bytes()), std::max(lines, size()));
    }
    head_ -= bytes;
    first_line_ -= lines;
    std::memcpy(data_.data() + head_, other.Bytes().data(), bytes);
    std::uint32_t* const slots = starts_.data() + first_line_;
    for (std::size_t i = 0; i < lines; ++i) {
      slots[i] = static_cast<std::uint32_t>(head_ + other.LogicalStart(i));
    }
  }
  void replace_range(std::size_t first, std::size_t last, const LineBlob& with) {
    if (&with == this) {
      const LineBlob copy(with);
      replace_range(first, last, copy);
      return;
    }
    const std::size_t line_count = size();
    first = std::min(first, line_count);
    last = std::min(std::max(last, first), line_count);
    const std::string_view bytes = Bytes();
    const std::size_t byte_first = first < line_count ? LogicalStart(first) : bytes.size();
    const std::size_t byte_last = last < line_count ? LogicalStart(last) : bytes.size();

    std::string next;
    next.reserve(bytes.size() - (byte_last - byte_first) + with.content_bytes());
    next.append(bytes.substr(0, byte_first));
    next.append(with.Bytes());
    next.append(bytes.substr(byte_last));

    util::SmallVector<std::uint32_t, kInlineStarts> next_starts;
    next_starts.reserve(first + with.size() + (line_count - last));
    for (std::size_t i = 0; i < first; ++i) {
      next_starts.push_back(static_cast<std::uint32_t>(LogicalStart(i)));
    }
    for (std::size_t i = 0; i < with.size(); ++i) {
      next_starts.push_back(static_cast<std::uint32_t>(byte_first + with.LogicalStart(i)));
    }
    const std::ptrdiff_t shift = static_cast<std::ptrdiff_t>(with.content_bytes()) -
                                 static_cast<std::ptrdiff_t>(byte_last - byte_first);
    for (std::size_t i = last; i < line_count; ++i) {
      next_starts.push_back(static_cast<std::uint32_t>(
          static_cast<std::ptrdiff_t>(LogicalStart(i)) + shift));
    }
    data_ = std::move(next);
    starts_ = std::move(next_starts);
    head_ = 0;
    first_line_ = 0;
  }
  bool range_equals(std::size_t first, const LineBlob& other) const {
    if (first + other.size() > size()) {
      return false;
    }
    for (std::size_t i = 0; i < other.size(); ++i) {
      if ((*this)[first + i] != other[i]) {
        return false;
      }
    }
    return true;
  }

  bool operator==(const LineBlob& other) const {
    if (size() != other.size() || Bytes() != other.Bytes()) {
      return false;
    }
    for (std::size_t i = 0; i < size(); ++i) {
      if (LogicalStart(i) != other.LogicalStart(i)) {
        return false;
      }
    }
    return true;
  }
  bool operator!=(const LineBlob& other) const { return !(*this == other); }

  std::size_t content_bytes() const { return data_.size() - head_; }
  std::size_t ApproximateResidentBytes() const {
    return data_.capacity() + starts_.capacity() * sizeof(std::uint32_t);
  }

  std::vector<std::string> ToVector() const {
    std::vector<std::string> out;
    out.reserve(size());
    for (std::size_t i = 0; i < size(); ++i) {
      out.emplace_back((*this)[i]);
    }
    return out;
  }

 private:
  std::size_t StartOf(std::size_t index) const { return starts_[first_line_ + index]; }
  std::size_t EndOf(std::size_t index) const {
    const std::size_t next = first_line_ + index + 1;
    return next < starts_.size() ? starts_[next] : data_.size();
  }
  std::size_t LogicalStart(std::size_t index) const { return StartOf(index) - head_; }
  std::string_view Bytes() const { return std::string_view(data_).substr(head_); }

  void GrowFront(std::size_t bytes, std::size_t lines) {
    const std::string_view content = Bytes();
    std::string grown;
    grown.reserve(bytes + content.size());
    grown.resize(bytes);
    grown.append(content);
    util::SmallVector<std::uint32_t, kInlineStarts> grown_starts;
    grown_starts.resize_uninitialized(lines + size());
    std::uint32_t* const slots = grown_starts.data();
    for (std::size_t i = 0; i < size(); ++i) {
      slots[lines + i] = static_cast<std::uint32_t>(bytes + LogicalStart(i));
    }
    data_ = std::move(grown);
    starts_ = std::move(grown_starts);
    head_ = bytes;
    first_line_ = lines;
  }

  static constexpr std::size_t kInlineStarts = 6;

  std::string data_;
  std::size_t head_ = 0;
  util::SmallVector<std::uint32_t, kInlineStarts> starts_;
  std::size_t first_line_ = 0;
};

}
