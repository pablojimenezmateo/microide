#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "editor/PieceTree.h"

namespace microide::editor {

class TextBuffer {
 public:
  using const_iterator = std::vector<std::string>::const_iterator;

  TextBuffer() = default;
  explicit TextBuffer(const std::vector<std::string>& lines) : tree_(lines) {}

  void Reset(const std::vector<std::string>& lines) {
    tree_.Reset(lines);
    InvalidateSnapshot();
  }

  void ResetFromText(std::string content) {
    tree_.ResetFromText(std::move(content));
    InvalidateSnapshot();
  }

  std::size_t LineCount() const noexcept { return tree_.LineCount(); }
  std::size_t size() const noexcept { return tree_.LineCount(); }
  bool empty() const noexcept { return tree_.Empty(); }

  std::string_view LineView(std::size_t index) const { return tree_.LineView(index); }
  std::size_t LineLength(std::size_t index) const { return tree_.LineLength(index); }

  std::string_view LineWindow(std::size_t index, std::size_t byte_start, std::size_t byte_len,
                              std::string& scratch) const {
    return tree_.LineWindow(index, byte_start, byte_len, scratch);
  }

  const std::string& operator[](std::size_t index) const { return LineRef(index); }
  const std::string& front() const { return LineRef(0); }
  const std::string& back() const { return LineRef(tree_.LineCount() - 1); }
  const_iterator begin() const { return Snapshot().begin(); }
  const_iterator end() const { return Snapshot().end(); }

  LineBlob SliceLinesBlob(std::size_t begin, std::size_t end) const {
    LineBlob out;
    tree_.AppendLines(begin, end, out);
    return out;
  }
  std::vector<std::string> SliceLines(std::size_t begin, std::size_t end) const {
    return tree_.SliceLines(begin, end);
  }
  void AppendLines(std::size_t begin, std::size_t end, std::vector<std::string>& out) const {
    tree_.AppendLines(begin, end, out);
  }
  void AppendLines(std::size_t begin, std::size_t end, LineBlob& out) const {
    tree_.AppendLines(begin, end, out);
  }
  std::vector<std::string> ToVector() const { return tree_.ToVector(); }

  void AppendWholeText(std::string& out) const {
    tree_.AppendWholeText(out);
  }

  DocumentSnapshot SnapshotDocument() const { return tree_.Snapshot(); }

  const std::vector<std::string>& Snapshot() const {
    if (!snapshot_valid_) {
      ++s_snapshot_builds_;
      snapshot_ = tree_.ToVector();
      snapshot_valid_ = true;
    }
    return snapshot_;
  }

  static std::size_t snapshot_build_count() { return s_snapshot_builds_; }
  static void reset_snapshot_build_count() { s_snapshot_builds_ = 0; }

  std::size_t materialized_line_count() const { return line_cache_.size(); }

  void ReplaceLineRange(std::size_t start, std::size_t removed,
                        const std::vector<std::string>& inserted) {
    tree_.ReplaceLineRange(start, removed, inserted);
    InvalidateSnapshot();
  }
  void ReplaceLineRange(std::size_t start, std::size_t removed, const LineBlob& inserted) {
    tree_.ReplaceLineRange(start, removed, inserted);
    InvalidateSnapshot();
  }

  void ReplaceTextRange(std::size_t start_line, std::size_t start_column,
                        std::size_t end_line, std::size_t end_column,
                        std::string_view text) {
    tree_.ReplaceTextRange(start_line, start_column, end_line, end_column, text);
    InvalidateSnapshot();
  }

  void AppendTextRange(std::size_t start_line, std::size_t start_column,
                       std::size_t end_line, std::size_t end_column,
                       std::string& out) const {
    tree_.AppendTextRange(start_line, start_column, end_line, end_column, out);
  }

  void SetLine(std::size_t index, const std::string& value) {
    tree_.SetLine(index, value);
    InvalidateSnapshot();
  }
  void InsertLine(std::size_t index, const std::string& value) {
    tree_.InsertLine(index, value);
    InvalidateSnapshot();
  }
  void EraseLine(std::size_t index) {
    tree_.EraseLine(index);
    InvalidateSnapshot();
  }
  void PushBackLine(const std::string& value) {
    tree_.PushBackLine(value);
    InvalidateSnapshot();
  }

 private:
  const std::string& LineRef(std::size_t index) const {
    const auto it = line_cache_.find(index);
    if (it != line_cache_.end()) return it->second;
    const std::string_view view = tree_.LineView(index);
    if (const std::string* owned = tree_.LineOwnedIfMaterialized(index);
        owned != nullptr && owned->data() == view.data() && owned->size() == view.size()) {
      return *owned;
    }
    return line_cache_.emplace(index, std::string(view)).first->second;
  }

  void InvalidateSnapshot() {
    snapshot_valid_ = false;
    snapshot_.clear();
    line_cache_.clear();
  }

  PieceTree tree_;
  mutable std::vector<std::string> snapshot_;
  mutable bool snapshot_valid_ = false;
  mutable std::unordered_map<std::size_t, std::string> line_cache_;
  inline static std::size_t s_snapshot_builds_ = 0;
};

}
