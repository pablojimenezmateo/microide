#pragma once

#include "editor/DocumentSnapshot.h"
#include "editor/LineBlob.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace microide::editor {

class PieceTree {
 public:
  PieceTree();
  explicit PieceTree(const std::vector<std::string>& lines);

  void Reset(const std::vector<std::string>& lines);

  void ResetFromText(std::string content);

  std::size_t LineCount() const noexcept { return line_count_; }
  bool Empty() const noexcept { return line_count_ == 0; }

  std::string_view LineView(std::size_t index) const;
  std::size_t LineLength(std::size_t index) const;

  std::string_view LineWindow(std::size_t index, std::size_t byte_start, std::size_t byte_len,
                              std::string& scratch) const;

  const std::string* LineOwnedIfMaterialized(std::size_t index) const {
    const auto it = line_view_cache_.find(index);
    return it != line_view_cache_.end() ? &it->second : nullptr;
  }

  std::vector<std::string> SliceLines(std::size_t begin, std::size_t end) const;
  void AppendLines(std::size_t begin, std::size_t end, std::vector<std::string>& out) const;
  void AppendLines(std::size_t begin, std::size_t end, LineBlob& out) const;
  std::vector<std::string> ToVector() const;
  DocumentSnapshot Snapshot() const;

  void ReplaceLineRange(std::size_t start, std::size_t removed,
                        std::span<const std::string> inserted);
  void ReplaceLineRange(std::size_t start, std::size_t removed,
                        const std::vector<std::string>& inserted) {
    ReplaceLineRange(start, removed, std::span<const std::string>(inserted));
  }
  void ReplaceLineRange(std::size_t start, std::size_t removed, const LineBlob& inserted);

  void ReplaceTextRange(std::size_t start_line, std::size_t start_column,
                        std::size_t end_line, std::size_t end_column,
                        std::string_view text);

  void AppendTextRange(std::size_t start_line, std::size_t start_column,
                       std::size_t end_line, std::size_t end_column,
                       std::string& out) const;
  void SetLine(std::size_t index, const std::string& value);
  void InsertLine(std::size_t index, const std::string& value);
  void EraseLine(std::size_t index);
  void PushBackLine(const std::string& value);

  std::size_t ByteSize() const noexcept { return TreeLength(root_); }

  void AppendWholeText(std::string& out) const {
    const std::uint32_t bytes = TreeLength(root_);
    if (bytes == 0) {
      return;
    }
    out.reserve(out.size() + bytes);
    CopyRange(0, bytes, out);
  }

  void CompactAddBufferForTesting() { CompactAddBuffer(); }
  std::size_t AddBufferSizeForTesting() const noexcept { return add_total_bytes_; }
  std::size_t AddChunkCountForTesting() const noexcept { return add_chunks_.size(); }
  std::size_t DepthForTesting() const { return DepthOf(root_); }

  void InsertTextForTesting(std::uint32_t pos, std::string_view text) {
    InsertText(pos, text);
    BumpRevision();
  }

  bool LastMutationRejectedForByteCeiling() const noexcept { return last_mutation_rejected_; }

  void SetMaxLiveDocumentBytesForTesting(std::uint32_t ceiling) {
    max_live_document_bytes_ = ceiling;
  }

 private:
  using NodeId = std::uint32_t;
  static constexpr NodeId kNull = 0;
  using BufferId = std::uint32_t;
  static constexpr BufferId kOriginal = 0;

  struct Node {
    NodeId left = kNull;
    NodeId right = kNull;
    std::uint32_t priority = 0;
    BufferId buffer = kOriginal;
    std::uint32_t start = 0;
    std::uint32_t length = 0;
    std::uint32_t self_newlines = 0;
    std::uint32_t subtree_length = 0;
    std::uint32_t subtree_newlines = 0;
  };

  static constexpr std::size_t kMinAddChunkBytes = 8u * 1024u;
  static constexpr std::size_t kMaxAddChunkBytes = 64u * 1024u;

  struct AddChunk {
    std::shared_ptr<char[]> bytes;
    std::uint32_t size = 0;
    std::uint32_t capacity = 0;
    std::vector<std::uint32_t> newlines;
  };

  struct TailWritable {
    bool value = true;
    TailWritable() = default;
    TailWritable(const TailWritable&) noexcept : value(false) {}
    TailWritable& operator=(const TailWritable&) noexcept {
      value = false;
      return *this;
    }
    TailWritable(TailWritable&&) noexcept = default;
    TailWritable& operator=(TailWritable&&) noexcept = default;
  };

  struct AddLocation {
    BufferId buffer = kOriginal;
    std::uint32_t offset = 0;
  };

  std::string_view BufferOf(BufferId buffer) const {
    if (buffer == kOriginal) {
      return *original_;
    }
    const AddChunk& chunk = add_chunks_[buffer - 1];
    return std::string_view(chunk.bytes.get(), chunk.size);
  }
  const std::vector<std::uint32_t>& NewlinesOf(BufferId buffer) const {
    return buffer == kOriginal ? original_newlines_ : add_chunks_[buffer - 1].newlines;
  }
  std::uint32_t CountNewlines(BufferId buffer, std::uint32_t from, std::uint32_t to) const;
  AddLocation AppendToAdd(std::string_view text);
  void StartAddChunk(std::size_t needed);
  static constexpr std::size_t kAddBufferCompactionFloorBytes = 4u * 1024u * 1024u;
  static constexpr std::size_t kAddBufferDeadHistoryMultiple = 4;
  void CompactAddBuffer();
  void RebuildFromOriginal();

  NodeId Allocate(BufferId buffer, std::uint32_t start, std::uint32_t length);
  void Free(NodeId id);
  std::uint32_t NextPriority();

  std::uint32_t TreeLength(NodeId id) const { return id == kNull ? 0 : nodes_[id].subtree_length; }
  std::size_t DepthOf(NodeId id) const {
    return id == kNull ? 0 : 1 + std::max(DepthOf(nodes_[id].left), DepthOf(nodes_[id].right));
  }
  std::uint32_t TreeNewlines(NodeId id) const {
    return id == kNull ? 0 : nodes_[id].subtree_newlines;
  }
  void Update(NodeId id);

  void Split(NodeId root, std::uint32_t pos, NodeId& left, NodeId& right);
  NodeId Merge(NodeId left, NodeId right);
  void InsertText(std::uint32_t pos, std::string_view text);
  void DeleteRange(std::uint32_t pos, std::uint32_t length);
  void FreeSubtree(NodeId id);

  struct ByteSpan {
    std::uint32_t start = 0;
    std::uint32_t end = 0;
  };
  ByteSpan ResolveByteSpan(std::size_t start_line, std::size_t start_column,
                           std::size_t end_line, std::size_t end_column) const;
  std::uint32_t LineEndByte(std::size_t line) const {
    return (line + 1 < line_count_) ? LineStartByteMemoized(line + 1) - 1 : TreeLength(root_);
  }
  std::uint32_t LineStartByte(std::size_t line) const;
  void CopyRange(std::uint32_t pos, std::uint32_t length, std::string& out) const;

  struct CopyFrame {
    NodeId id;
    std::uint32_t base;
    bool emit;
  };
  mutable std::vector<CopyFrame> copy_stack_;
  mutable std::vector<CopyFrame> extract_stack_;
  void ExtractLineRange(std::size_t begin_line, std::size_t end_line,
                        std::vector<std::string>& out) const;
  template <typename Sink>
  void ExtractLineRangeInto(std::size_t begin_line, std::size_t end_line, Sink& out) const;
  template <typename Lines>
  void ReplaceLineRangeFrom(std::size_t start, std::size_t removed, const Lines& inserted);
  std::string_view TryViewRange(std::uint32_t pos, std::uint32_t length, bool& ok) const;

  std::uint32_t LineStartByteMemoized(std::size_t line) const;

  void BumpRevision() {
    line_view_cache_.clear();
    cached_line_start_index_ = kNoCachedLine;
    walk_valid_ = false;
  }

  std::shared_ptr<const std::string> original_;
  std::vector<std::uint32_t> original_newlines_;
  std::vector<AddChunk> add_chunks_;
  std::size_t add_total_bytes_ = 0;
  TailWritable tail_writable_;

  std::vector<Node> nodes_;
  std::vector<NodeId> free_list_;
  NodeId root_ = kNull;
  std::size_t line_count_ = 0;
  std::uint32_t priority_state_ = 0x9e3779b9u;
  std::uint32_t max_live_document_bytes_ = std::numeric_limits<std::uint32_t>::max();
  bool last_mutation_rejected_ = false;

  mutable std::unordered_map<std::size_t, std::string> line_view_cache_;

  static constexpr std::size_t kNoCachedLine = std::numeric_limits<std::size_t>::max();
  mutable std::size_t cached_line_start_index_ = kNoCachedLine;
  mutable std::uint32_t cached_line_start_byte_ = 0;

  mutable bool walk_valid_ = false;
  mutable std::size_t walk_line_ = 0;
  mutable NodeId walk_node_ = kNull;
  mutable std::uint32_t walk_base_ = 0;
  mutable std::uint32_t walk_target_ = 0;
  mutable std::size_t walk_nl_pos_ = 0;
};

}
