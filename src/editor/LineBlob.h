#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "util/SmallVector.h"

namespace microide::editor {

// A run of lines stored as ONE byte buffer plus a line-start table, instead of
// one owned `std::string` per line.
//
// Undo entries are the reason this exists. A line-shaped edit -- toggle comment,
// sort lines, move line, indent/dedent, any multi-caret shaping op -- records
// the lines it replaced and the lines it installed, and as a
// `std::vector<std::string>` that is one heap block per line in each direction:
// 2n allocations for an n-line edit, n destructor calls per undo/redo, and n
// separate malloc headers held for the life of the entry. The constant is not
// reducible while the entry IS a vector of strings (TD-2026-08-11-182).
//
// As a blob it is 2 allocations per direction regardless of n (the byte buffer
// and the offset table, each with geometric growth), the entry's footprint is
// two contiguous buffers instead of n scattered blocks, and undo/redo frees two
// blocks instead of n.
//
// Lines never contain '\n' -- the newline is the separator the buffer splits on,
// and is not stored.
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
  // Deliberately a named factory rather than an initializer_list constructor:
  // an implicit one makes every `ReplaceLines(a, b, {"x", "y"})` call ambiguous
  // between the vector and blob overloads.
  static LineBlob Of(std::initializer_list<std::string_view> lines) {
    LineBlob out;
    out.reserve_lines(lines.size());
    for (std::string_view line : lines) {
      out.push_back(line);
    }
    return out;
  }

  // ---- build ----------------------------------------------------------
  void clear() {
    data_.clear();
    starts_.clear();
  }
  void reserve_lines(std::size_t count) { starts_.reserve(count); }
  // Element-count reserve, spelled like the vector sink's so one templated
  // extractor serves both.
  void reserve(std::size_t count) { reserve_lines(count); }
  void assign(std::span<const std::string> lines) {
    clear();
    reserve_lines(lines.size());
    for (const std::string& line : lines) {
      push_back(line);
    }
  }
  void reserve_bytes(std::size_t bytes) { data_.reserve(bytes); }

  void push_back(std::string_view line) {
    starts_.push_back(static_cast<std::uint32_t>(data_.size()));
    data_.append(line);
  }
  // Sink shape the piece tree's line walk emits into, mirroring
  // `std::vector<std::string>`'s so one templated extractor serves both.
  void emplace_back(const char* data, std::size_t length) {
    push_back(std::string_view(data, length));
  }
  // Compose a line from pieces without a temporary `std::string`: `begin_line()`
  // opens it, `append_to_last()` adds to whichever line is open. A composed line
  // (prefix + replacement + suffix) used to build its own buffer and then be
  // moved into the vector; here it is appended straight into the blob.
  void begin_line() { starts_.push_back(static_cast<std::uint32_t>(data_.size())); }
  void append_to_last(std::string_view piece) { data_.append(piece); }
  // Line assembled from exactly these pieces.
  template <typename... Pieces>
  void push_joined(Pieces... pieces) {
    begin_line();
    (append_to_last(pieces), ...);
  }

  // ---- read -----------------------------------------------------------
  std::size_t size() const { return starts_.size(); }
  bool empty() const { return starts_.empty(); }
  std::string_view operator[](std::size_t index) const {
    assert(index < starts_.size());
    const std::size_t begin = starts_[index];
    const std::size_t end = index + 1 < starts_.size() ? starts_[index + 1] : data_.size();
    return std::string_view(data_).substr(begin, end - begin);
  }
  std::string_view front() const { return (*this)[0]; }
  std::string_view back() const { return (*this)[starts_.size() - 1]; }

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
  const_iterator end() const { return const_iterator(this, starts_.size()); }

  // ---- slice / compare ------------------------------------------------
  // Lines [first, last) as a new blob. One memcpy of the byte range plus a
  // rebased offset table: 2 allocations, not (last - first).
  LineBlob SubRange(std::size_t first, std::size_t last) const {
    LineBlob out;
    if (first >= last || first >= starts_.size()) {
      return out;
    }
    const std::size_t clamped_last = std::min(last, starts_.size());
    const std::size_t byte_begin = starts_[first];
    const std::size_t byte_end =
        clamped_last < starts_.size() ? starts_[clamped_last] : data_.size();
    out.data_.assign(data_, byte_begin, byte_end - byte_begin);
    out.starts_.reserve(clamped_last - first);
    for (std::size_t i = first; i < clamped_last; ++i) {
      out.starts_.push_back(static_cast<std::uint32_t>(starts_[i] - byte_begin));
    }
    return out;
  }

  // ---- splice ---------------------------------------------------------
  // The three shapes the undo history's group merge needs. Each is one pass over
  // the bytes into a buffer reserved to its final size, where the vector form was
  // an insert/erase that moved n std::strings around.
  void append(const LineBlob& other) {
    const std::size_t base = data_.size();
    data_.reserve(base + other.data_.size());
    data_.append(other.data_);
    starts_.reserve(starts_.size() + other.starts_.size());
    for (const std::uint32_t start : other.starts_) {
      starts_.push_back(static_cast<std::uint32_t>(base + start));
    }
  }
  void prepend(const LineBlob& other) { replace_range(0, 0, other); }
  // Lines [first, last) become `with`.
  void replace_range(std::size_t first, std::size_t last, const LineBlob& with) {
    const std::size_t line_count = starts_.size();
    first = std::min(first, line_count);
    last = std::min(std::max(last, first), line_count);
    const std::size_t byte_first = first < line_count ? starts_[first] : data_.size();
    const std::size_t byte_last = last < line_count ? starts_[last] : data_.size();

    std::string next;
    next.reserve(data_.size() - (byte_last - byte_first) + with.data_.size());
    next.append(std::string_view(data_).substr(0, byte_first));
    next.append(with.data_);
    next.append(std::string_view(data_).substr(byte_last));

    util::SmallVector<std::uint32_t, kInlineStarts> next_starts;
    next_starts.reserve(first + with.starts_.size() + (line_count - last));
    next_starts.append(starts_.begin(), starts_.begin() + static_cast<std::ptrdiff_t>(first));
    for (const std::uint32_t start : with.starts_) {
      next_starts.push_back(static_cast<std::uint32_t>(byte_first + start));
    }
    const std::ptrdiff_t shift = static_cast<std::ptrdiff_t>(with.data_.size()) -
                                 static_cast<std::ptrdiff_t>(byte_last - byte_first);
    for (std::size_t i = last; i < line_count; ++i) {
      next_starts.push_back(static_cast<std::uint32_t>(static_cast<std::ptrdiff_t>(starts_[i]) +
                                                        shift));
    }
    data_ = std::move(next);
    starts_ = std::move(next_starts);
  }
  // Do `other.size()` lines starting at `first` equal `other`?
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
    return starts_.size() == other.starts_.size() && data_ == other.data_ &&
           starts_ == other.starts_;
  }
  bool operator!=(const LineBlob& other) const { return !(*this == other); }

  // ---- accounting -----------------------------------------------------
  // Content bytes, for the undo history's byte budget. The offset table is
  // bookkeeping, not content, and the budget has always counted content.
  std::size_t content_bytes() const { return data_.size(); }
  // Retained heap, by capacity: what this blob keeps hold of, not what it uses.
  std::size_t ApproximateResidentBytes() const {
    return data_.capacity() + starts_.capacity() * sizeof(std::uint32_t);
  }

  // ---- boundaries -----------------------------------------------------
  // For the callers that still hand around `std::vector<std::string>` (the LSP
  // whole-document sync path). Materializes, so it is only for those.
  std::vector<std::string> ToVector() const {
    std::vector<std::string> out;
    out.reserve(starts_.size());
    for (std::size_t i = 0; i < starts_.size(); ++i) {
      out.emplace_back((*this)[i]);
    }
    return out;
  }

 private:
  // Inline offset slots. A blob covering a handful of lines -- which is what a
  // multi-caret line op builds, one per caret per keystroke -- then costs the
  // offset table nothing at all: `move_line_down.multi_caret_burst` spent 52 % of
  // its allocations on EIGHT-byte `starts_` tables, two offsets for a one-line
  // region plus the neighbour it swaps with (TD-2026-08-15-244). Beyond the
  // inline slots it spills to the heap and behaves as before, so the big
  // whole-selection ops (toggle comment, sort) are unchanged.
  //
  // Six, not four: the shaping ops build (region + 1 neighbour), and a region is
  // commonly a handful of lines. Costs 24 bytes inside a LineBlob, which undo
  // entries hold two of.
  static constexpr std::size_t kInlineStarts = 6;

  std::string data_;
  // Byte offset of each line's first byte. Line i ends at starts_[i + 1], or at
  // data_.size() for the last one -- no sentinel, so `size()` is `starts_.size()`
  // and an empty blob is two empty containers.
  util::SmallVector<std::uint32_t, kInlineStarts> starts_;
};

}  // namespace microide::editor
