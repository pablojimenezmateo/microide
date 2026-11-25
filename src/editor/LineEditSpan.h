#pragma once

#include <algorithm>
#include <cstddef>
#include <limits>

namespace microide::editor {

class LineEditSpan {
 public:
  static constexpr std::size_t kNone = std::numeric_limits<std::size_t>::max();
  static constexpr std::size_t kToEnd = std::numeric_limits<std::size_t>::max();

  bool empty() const noexcept { return begin_ == kNone; }

  std::size_t begin() const noexcept { return begin_; }
  std::size_t cached_end() const noexcept { return cached_end_; }
  std::size_t current_end() const noexcept { return current_end_; }

  void Clear() noexcept {
    begin_ = kNone;
    cached_end_ = 0;
    current_end_ = 0;
  }

  void NoteSplice(std::size_t start, std::size_t removed, std::size_t inserted) noexcept {
    if (begin_ == kNone) {
      begin_ = start;
      cached_end_ = start + removed;
      current_end_ = start + inserted;
      return;
    }
    begin_ = std::min(begin_, start);
    if (current_end_ == kToEnd) {
      return;
    }
    const std::size_t splice_end = start + removed;
    if (splice_end > current_end_) {
      cached_end_ += splice_end - current_end_;
      current_end_ = splice_end;
    }
    current_end_ = current_end_ - removed + inserted;
  }

  void NoteSuffixReplaced(std::size_t start) noexcept {
    begin_ = begin_ == kNone ? start : std::min(begin_, start);
    cached_end_ = kToEnd;
    current_end_ = kToEnd;
  }

  static LineEditSpan SuffixReplacedFrom(std::size_t start) noexcept {
    LineEditSpan span;
    span.NoteSuffixReplaced(start);
    return span;
  }
  static LineEditSpan FullRebuild() noexcept { return SuffixReplacedFrom(0); }

  std::size_t ResolvedCachedEnd(std::size_t cache_line_count) const noexcept {
    return cached_end_ == kToEnd ? cache_line_count : std::min(cached_end_, cache_line_count);
  }
  std::size_t ResolvedCurrentEnd(std::size_t document_line_count) const noexcept {
    return current_end_ == kToEnd ? document_line_count
                                  : std::min(current_end_, document_line_count);
  }

 private:
  std::size_t begin_ = kNone;
  std::size_t cached_end_ = 0;
  std::size_t current_end_ = 0;
};

}
