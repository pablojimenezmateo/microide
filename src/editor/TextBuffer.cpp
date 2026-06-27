#include "editor/TextBuffer.h"

#include <algorithm>

namespace microide::editor {

std::vector<std::string> TextBuffer::SliceLines(std::size_t begin, std::size_t end) const {
  if (begin >= end || begin >= lines_.size()) {
    return {};
  }
  end = std::min(end, lines_.size());
  return std::vector<std::string>(lines_.begin() + static_cast<std::ptrdiff_t>(begin),
                                  lines_.begin() + static_cast<std::ptrdiff_t>(end));
}

void TextBuffer::ReplaceLineRange(std::size_t start, std::size_t removed,
                                  const std::vector<std::string>& inserted) {
  start = std::min(start, lines_.size());
  const std::size_t erase_end = std::min(start + removed, lines_.size());
  const auto erase_begin_it = lines_.begin() + static_cast<std::ptrdiff_t>(start);
  const auto erase_end_it = lines_.begin() + static_cast<std::ptrdiff_t>(erase_end);
  lines_.erase(erase_begin_it, erase_end_it);
  lines_.insert(lines_.begin() + static_cast<std::ptrdiff_t>(start), inserted.begin(),
                inserted.end());
}

void TextBuffer::InsertLine(std::size_t index, std::string value) {
  index = std::min(index, lines_.size());
  lines_.insert(lines_.begin() + static_cast<std::ptrdiff_t>(index), std::move(value));
}

void TextBuffer::EraseLine(std::size_t index) {
  if (index >= lines_.size()) {
    return;
  }
  lines_.erase(lines_.begin() + static_cast<std::ptrdiff_t>(index));
}

void TextBuffer::EraseLineRange(std::size_t begin, std::size_t end) {
  if (begin >= end || begin >= lines_.size()) {
    return;
  }
  end = std::min(end, lines_.size());
  lines_.erase(lines_.begin() + static_cast<std::ptrdiff_t>(begin),
               lines_.begin() + static_cast<std::ptrdiff_t>(end));
}

}  // namespace microide::editor
