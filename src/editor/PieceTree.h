#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace microide::editor {

// A line-indexed text store backed by the "piece tree" design (VS Code's model):
// text lives in two contiguous buffers -- an immutable `original` buffer (the
// loaded file) and an append-only `add` buffer (everything typed since) -- and
// the logical document is an ordered sequence of *pieces*, each a [offset, len)
// slice into one of those buffers. The pieces are held in a balanced tree so
// insert/delete/line-lookup are O(log p) in the piece count, and text is never
// shuffled the way `vector<std::string>` shuffles line pointers on a mid-file
// edit. No per-line heap allocation: a line is computed from cached newline
// offsets, not stored as its own string.
//
// The balancing structure here is an *implicit treap* (a rope): split/merge by
// byte position, with each node caching its subtree byte length and newline
// count. Treaps need far fewer special cases than a red-black tree, which keeps
// the load-bearing data structure auditable; the fuzz target
// (PieceTreeEquivalenceFuzz) pins it to a naive `vector<std::string>` oracle.
//
// This class speaks *lines*, because that is the document model the editor,
// undo history, layout, and syntax caches are built on. `ReplaceLineRange` is
// the single mutation primitive (mirrors the undo-entry apply model); every
// other mutator is a thin wrapper. Reads return a `std::string_view`:
//   * Zero-copy when the line lies within a single piece -- the common case for
//     unedited original-buffer lines and freshly typed lines.
//   * Materialized into a per-revision cache when the line spans a piece
//     boundary (only edited regions create spans). Views stay valid until the
//     next *mutation* -- the same reference-invalidation contract a
//     `vector<std::string>` already imposes.
class PieceTree {
 public:
  PieceTree();
  explicit PieceTree(const std::vector<std::string>& lines);

  // Replace the whole document with `lines` (load / ResetState).
  void Reset(const std::vector<std::string>& lines);

  // Replace the whole document by taking ownership of `content` directly as the
  // original buffer -- no split-into-lines / rejoin round-trip. `content` must
  // already be the canonical document representation (lines joined by '\n', i.e.
  // it contains no '\r'); the line count is derived from its '\n' count. This is
  // the large-file load fast path: the file bytes are moved in once and scanned
  // for newlines a single time, instead of being copied through a
  // vector<string>. Empty content yields a single empty line.
  void ResetFromText(std::string content);

  // --- Read ---
  std::size_t LineCount() const noexcept { return line_count_; }
  bool Empty() const noexcept { return line_count_ == 0; }

  // Zero-copy view of line `index` (no trailing newline). `index` < LineCount().
  // Valid until the next mutation.
  std::string_view LineView(std::size_t index) const;
  std::size_t LineLength(std::size_t index) const;

  // Copy lines [begin, end) into a fresh vector.
  std::vector<std::string> SliceLines(std::size_t begin, std::size_t end) const;
  // Full materialized copy of every line.
  std::vector<std::string> ToVector() const;

  // --- Mutate ---
  // Universal primitive: erase `removed` lines starting at `start`, then insert
  // `inserted` at `start`. Identical in effect to a vector erase+insert.
  void ReplaceLineRange(std::size_t start, std::size_t removed,
                        const std::vector<std::string>& inserted);
  void SetLine(std::size_t index, const std::string& value);
  void InsertLine(std::size_t index, const std::string& value);
  void EraseLine(std::size_t index);
  void EraseLineRange(std::size_t begin, std::size_t end);
  void PushBackLine(const std::string& value);

  // Total bytes of the logical document (lines joined by '\n').
  std::size_t ByteSize() const noexcept { return TreeLength(root_); }

  // Testing seam: force the add-buffer compaction that InsertText performs
  // automatically when the append-only add_ buffer would overflow the 32-bit
  // offset space. Content and line count are invariant across a compaction; the
  // add buffer is emptied. Exposed so the overflow guard's correctness can be
  // tested without allocating 4 GiB.
  void CompactAddBufferForTesting() { CompactAddBuffer(); }
  std::size_t AddBufferSizeForTesting() const noexcept { return add_.size(); }

 private:
  // 0 = null sentinel; real nodes are indices >= 1 into nodes_.
  using NodeId = std::uint32_t;
  static constexpr NodeId kNull = 0;
  static constexpr std::uint8_t kOriginal = 0;
  static constexpr std::uint8_t kAdd = 1;

  struct Node {
    NodeId left = kNull;
    NodeId right = kNull;
    std::uint32_t priority = 0;
    std::uint8_t buffer = kOriginal;
    std::uint32_t start = 0;        // byte offset into the buffer
    std::uint32_t length = 0;       // piece length in bytes
    std::uint32_t self_newlines = 0;
    std::uint32_t subtree_length = 0;
    std::uint32_t subtree_newlines = 0;
  };

  // --- buffer access ---
  const std::string& BufferOf(std::uint8_t buffer) const {
    return buffer == kOriginal ? original_ : add_;
  }
  const std::vector<std::uint32_t>& NewlinesOf(std::uint8_t buffer) const {
    return buffer == kOriginal ? original_newlines_ : add_newlines_;
  }
  // Number of '\n' in buffer `b` within byte range [from, to).
  std::uint32_t CountNewlines(std::uint8_t buffer, std::uint32_t from, std::uint32_t to) const;
  // Byte offset (within the buffer) of the `nth` (0-based) '\n' inside [from, to).
  std::uint32_t NthNewlineOffset(std::uint8_t buffer, std::uint32_t from, std::uint32_t nth) const;
  // Append `text` to the add buffer; returns its start offset there.
  std::uint32_t AppendToAdd(std::string_view text);
  // Materialize the live document into `original_` and clear the append-only
  // `add_` buffer, preserving content and line_count_. Called by InsertText when
  // add_ would otherwise overflow the 32-bit offset space.
  void CompactAddBuffer();
  // Rebuild the newline index + single root piece from the current `original_`
  // (shared by Reset / ResetFromText). Does not set line_count_.
  void RebuildFromOriginal();

  // --- node pool ---
  NodeId Allocate(std::uint8_t buffer, std::uint32_t start, std::uint32_t length);
  void Free(NodeId id);
  std::uint32_t NextPriority();

  std::uint32_t TreeLength(NodeId id) const { return id == kNull ? 0 : nodes_[id].subtree_length; }
  std::uint32_t TreeNewlines(NodeId id) const {
    return id == kNull ? 0 : nodes_[id].subtree_newlines;
  }
  void Update(NodeId id);

  // --- treap core (split by byte position; can split a node's piece) ---
  void Split(NodeId root, std::uint32_t pos, NodeId& left, NodeId& right);
  NodeId Merge(NodeId left, NodeId right);
  void InsertText(std::uint32_t pos, std::string_view text);
  void DeleteRange(std::uint32_t pos, std::uint32_t length);
  void FreeSubtree(NodeId id);

  // --- navigation / extraction ---
  std::uint32_t LineStartByte(std::size_t line) const;
  void CopyRange(std::uint32_t pos, std::uint32_t length, std::string& out) const;
  // Append lines [begin_line, end_line) (newlines excluded) to `out` in a single
  // in-order treap walk -- O(N + p) instead of the O(N log p) that per-line
  // LineView() extraction costs (two tree descents per line). Callers guarantee
  // begin_line < end_line <= line_count_.
  void ExtractLineRange(std::size_t begin_line, std::size_t end_line,
                        std::vector<std::string>& out) const;
  // If [pos, pos+length) lies wholly inside one piece, return a view into the
  // backing buffer; otherwise return an empty optional-substitute via `ok`.
  std::string_view TryViewRange(std::uint32_t pos, std::uint32_t length, bool& ok) const;

  void BumpRevision() { line_view_cache_.clear(); }

  std::string original_;
  std::string add_;
  std::vector<std::uint32_t> original_newlines_;  // offsets of each '\n' in original_
  std::vector<std::uint32_t> add_newlines_;       // offsets of each '\n' in add_

  std::vector<Node> nodes_;        // index 0 is the null sentinel
  std::vector<NodeId> free_list_;
  NodeId root_ = kNull;
  std::size_t line_count_ = 0;     // authoritative line count (0 == empty document)
  std::uint32_t priority_state_ = 0x9e3779b9u;  // deterministic treap priorities

  // Spanning-line materialization, valid for the current revision only.
  mutable std::unordered_map<std::size_t, std::string> line_view_cache_;
};

}  // namespace microide::editor
