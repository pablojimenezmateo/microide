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
  // Leaves the blob empty and usable, a moved-from one included: a loop reuses one
  // blob per region, moving it out at the end of each (clang-tidy's
  // bugprone-use-after-move learns that only from the attribute).
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
  void reserve_bytes(std::size_t bytes) { data_.reserve(head_ + bytes); }

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

  // ---- slice / compare ------------------------------------------------
  // Lines [first, last) as a new blob. One memcpy of the byte range plus a
  // rebased offset table: 2 allocations, not (last - first).
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

  // ---- splice ---------------------------------------------------------
  // The three shapes the undo history's group merge needs. Each is one pass over
  // the bytes into a buffer reserved to its final size, where the vector form was
  // an insert/erase that moved n std::strings around.
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
  // Insert `other`'s lines before this blob's, in amortized O(other).
  //
  // An undo GROUP merges its children into one running aggregate, and a
  // multi-region shaping verb applies its regions HIGHEST-FIRST (so each region's
  // line indices stay valid against the still-unedited buffer below it), which
  // means every child abuts the aggregate on the LOW side and every merge is a
  // prepend. Rebuilding the aggregate per merge was O(total bytes) in allocations
  // (a block comment toggled at 1,201 cursors allocated 253 MB, TD-2026-09-20-301);
  // inserting at the front of the one buffer fixed the allocations and left the
  // shift, O(total) per merge, O(regions^2) per group: ~1.7 GB of memmove at the
  // 10,000-cursor cap. So the blob is double-ended: both buffers keep slack at the
  // FRONT too, a prepend writes into it without moving anything, and when it runs
  // out the blob regrows with at least as much front slack as it has content, so
  // each byte is moved O(1) times however many prepends follow.
  void prepend(const LineBlob& other) {
    if (&other == this) {
      // Self-prepend doubles the blob; writing into the front would read lines
      // this call is placing. Nothing does it today; take the copy rather than
      // leave a shape that is correct only for as long as nobody tries it.
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
  // Lines [first, last) become `with`.
  void replace_range(std::size_t first, std::size_t last, const LineBlob& with) {
    if (&with == this) {
      const LineBlob copy(with);
      replace_range(first, last, copy);
      return;
    }
    // Built compact (no front slack), in logical offsets throughout.
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

  // By content: where the lines sit in either blob's storage is not part of it.
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

  // ---- accounting -----------------------------------------------------
  // Content bytes, for the undo history's byte budget. The offset table is
  // bookkeeping, not content, and the budget has always counted content.
  // It is also what the undo group's merge picks its direction by: merging into
  // the side that already owns the bigger buffer is what keeps a many-child
  // group linear.
  std::size_t content_bytes() const { return data_.size() - head_; }
  // Retained heap, by capacity: what this blob keeps hold of, not what it uses.
  std::size_t ApproximateResidentBytes() const {
    return data_.capacity() + starts_.capacity() * sizeof(std::uint32_t);
  }

  // ---- boundaries -----------------------------------------------------
  // For the callers that still hand around `std::vector<std::string>` (the LSP
  // whole-document sync path). Materializes, so it is only for those.
  std::vector<std::string> ToVector() const {
    std::vector<std::string> out;
    out.reserve(size());
    for (std::size_t i = 0; i < size(); ++i) {
      out.emplace_back((*this)[i]);
    }
    return out;
  }

 private:
  // Line `index`'s first byte and one past its last, as positions in `data_`.
  std::size_t StartOf(std::size_t index) const { return starts_[first_line_ + index]; }
  std::size_t EndOf(std::size_t index) const {
    const std::size_t next = first_line_ + index + 1;
    return next < starts_.size() ? starts_[next] : data_.size();
  }
  // Line `index`'s first byte relative to the content's first byte.
  std::size_t LogicalStart(std::size_t index) const { return StartOf(index) - head_; }
  // The content, without the front slack.
  std::string_view Bytes() const { return std::string_view(data_).substr(head_); }

  // Reallocates both buffers with (at least) `bytes` of front slack and `lines`
  // offset slots before the content, which keeps its own order and values.
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

  // The content is `data_` past its first `head_` bytes, and the line table is
  // `starts_` past its first `first_line_` slots: the front slack a prepend writes
  // into (see `prepend`). Both are 0 for a blob nothing has prepended to, which is
  // every blob but a group's aggregate.
  std::string data_;
  std::size_t head_ = 0;
  // Position in `data_` of each line's first byte. Line i ends where line i + 1
  // starts, or at data_.size() for the last one -- no sentinel, so an empty blob
  // is two empty containers.
  util::SmallVector<std::uint32_t, kInlineStarts> starts_;
  std::size_t first_line_ = 0;
};

}  // namespace microide::editor
