#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
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
  // Valid until the next mutation, including across repeated calls for the same
  // `index` (a spanning line is materialized once per revision and its slot is
  // returned unchanged thereafter, so an earlier view never dangles).
  std::string_view LineView(std::size_t index) const;
  std::size_t LineLength(std::size_t index) const;

  // Bounded read: the bytes of line `index` in [byte_start, byte_start + byte_len),
  // clamped to the line. Zero-copy when that window lies inside a single piece
  // (the common case even on an edited line, because an in-line edit splits its
  // line into exactly three pieces); otherwise the WINDOW -- never the line -- is
  // copied into `scratch` and viewed from there.
  //
  // This is what a caller that only wants part of a line must use. `LineView` on
  // a line that spans pieces materializes the whole line into a per-revision
  // cache, so "read the first 4 KiB" spelled as `LineView(i).substr(0, 4096)`
  // copies megabytes on a minified bundle and merely throws them away
  // (TD-2026-08-05-133). The returned view is valid until `scratch` is reused or
  // the tree mutates.
  std::string_view LineWindow(std::size_t index, std::size_t byte_start, std::size_t byte_len,
                              std::string& scratch) const;

  // The owned copy of line `index` if this revision already had to make one, else
  // nullptr. Exists so a caller that must hand out a `const std::string&` can
  // reference the copy the tree already paid for instead of making a second one;
  // both die at the same mutation, so the lifetime contract is unchanged. Call
  // `LineView(index)` first -- this only reports what is cached, it does not
  // materialize.
  const std::string* LineOwnedIfMaterialized(std::size_t index) const {
    const auto it = line_view_cache_.find(index);
    return it != line_view_cache_.end() ? &it->second : nullptr;
  }

  // Copy lines [begin, end) into a fresh vector.
  std::vector<std::string> SliceLines(std::size_t begin, std::size_t end) const;
  // Full materialized copy of every line.
  std::vector<std::string> ToVector() const;

  // --- Mutate ---
  // Universal primitive: erase `removed` lines starting at `start`, then insert
  // `inserted` at `start`. Identical in effect to a vector erase+insert.
  // `inserted` is a span so the single-line mutators below can pass the caller's
  // own string without wrapping it in a temporary vector -- that wrap was a full
  // copy of the line, paid on every SetLine, i.e. on every in-line keystroke.
  void ReplaceLineRange(std::size_t start, std::size_t removed,
                        std::span<const std::string> inserted);
  // Convenience overload so callers (and braced initializer lists) keep working.
  void ReplaceLineRange(std::size_t start, std::size_t removed,
                        const std::vector<std::string>& inserted) {
    ReplaceLineRange(start, removed, std::span<const std::string>(inserted));
  }

  // Byte-range splice in (line, column) coordinates: replace the document bytes
  // between (start_line, start_column) and (end_line, end_column) with `text`.
  //
  // This is the mutation the tree actually performs -- a split/insert/merge at two
  // byte offsets. `ReplaceLineRange` is the line-shaped wrapper around it, and
  // expressing an in-line edit through that wrapper costs two copies of the whole
  // affected line: the caller must compose the post-edit line, and this class then
  // joins it into `replacement` and appends it to `add_`. On a file with no line
  // breaks in it (a minified bundle) that is two multi-megabyte copies per
  // keystroke. This form copies only `text`.
  //
  // Columns are byte offsets, clamped to their line's length; an `end` that
  // precedes `start` collapses to an insertion at `start`. `text` may contain
  // '\n' (the line count is updated from its newline count).
  void ReplaceTextRange(std::size_t start_line, std::size_t start_column,
                        std::size_t end_line, std::size_t end_column,
                        std::string_view text);

  // Append the document bytes in [(start_line, start_column), (end_line, end_column))
  // to `out`, '\n'-joined exactly as the tree stores them. O(range) -- unlike
  // LineView it never materializes a whole line that happens to span pieces, which
  // is every edited line.
  void AppendTextRange(std::size_t start_line, std::size_t start_column,
                       std::size_t end_line, std::size_t end_column,
                       std::string& out) const;
  void SetLine(std::size_t index, const std::string& value);
  void InsertLine(std::size_t index, const std::string& value);
  void EraseLine(std::size_t index);
  void EraseLineRange(std::size_t begin, std::size_t end);
  void PushBackLine(const std::string& value);

  // Total bytes of the logical document (lines joined by '\n').
  std::size_t ByteSize() const noexcept { return TreeLength(root_); }

  // Append the whole document ('\n'-joined, exactly the tree's own
  // representation) to `out`. One pruned in-order walk with a memcpy per piece.
  // Prefer this over looping LineView to build whole-buffer text: that costs two
  // tree descents per line and, for lines that span pieces, materializes them
  // into the per-line cache — a second full copy of the document retained until
  // the next mutation.
  void AppendWholeText(std::string& out) const {
    const std::uint32_t bytes = TreeLength(root_);
    if (bytes == 0) {
      return;
    }
    out.reserve(out.size() + bytes);
    CopyRange(0, bytes, out);
  }

  // Testing seam: force the add-buffer compaction that InsertText performs
  // automatically when the append-only add_ buffer would overflow the 32-bit
  // offset space. Content and line count are invariant across a compaction; the
  // add buffer is emptied. Exposed so the overflow guard's correctness can be
  // tested without allocating 4 GiB.
  void CompactAddBufferForTesting() { CompactAddBuffer(); }
  std::size_t AddBufferSizeForTesting() const noexcept { return add_.size(); }

  // Testing seam: byte-level mid-line insertion (the shape TextViewport character
  // editing produces) so a single line's content can be forced to span multiple
  // pieces, exercising LineView's non-contiguous materialization path. `text` must
  // not contain '\n' (line_count_ stays valid); the caller passes a pos strictly
  // inside a line. Bumps the revision so LineView re-materializes.
  void InsertTextForTesting(std::uint32_t pos, std::string_view text) {
    InsertText(pos, text);
    BumpRevision();
  }

  // True when the last mutation was refused because it would push the live document
  // past the byte ceiling (see max_live_document_bytes_). Lets edit callers detect a
  // dropped mutation instead of silently desyncing cursor/undo state.
  bool LastMutationRejectedForByteCeiling() const noexcept { return last_mutation_rejected_; }

  // Testing seam: lower the live-document byte ceiling so the overflow-refusal path can
  // be exercised without allocating ~4 GiB. Restore with uint32_max after the test.
  void SetMaxLiveDocumentBytesForTesting(std::uint32_t ceiling) {
    max_live_document_bytes_ = ceiling;
  }

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
  // Below this, `add_` is too small to be worth a compaction pass; ordinary typing
  // in a small document never reaches it.
  static constexpr std::size_t kAddBufferCompactionFloorBytes = 4u * 1024u * 1024u;
  // Compact once the dead edit history exceeds this multiple of the LIVE document,
  // so the bound on resident text is proportional to the document rather than to
  // how long the session has been editing it. Larger = fewer compactions and more
  // retained history; smaller = tighter memory and more O(document) rebuilds.
  static constexpr std::size_t kAddBufferDeadHistoryMultiple = 4;
  // Materialize the live document into `original_` and clear the append-only
  // `add_` buffer, preserving content and line_count_. Called by InsertText when
  // add_ would otherwise overflow the 32-bit offset space, and when the dead
  // history has outgrown the live document by kAddBufferDeadHistoryMultiple.
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
  // Clamped byte offsets of a (line, column) span. Resolved through the
  // sequential line-start memo, in ascending line order, so a same-line span
  // costs one descent rather than two.
  struct ByteSpan {
    std::uint32_t start = 0;
    std::uint32_t end = 0;
  };
  ByteSpan ResolveByteSpan(std::size_t start_line, std::size_t start_column,
                           std::size_t end_line, std::size_t end_column) const;
  // Byte offset one past the last byte of `line` (its '\n' excluded).
  std::uint32_t LineEndByte(std::size_t line) const {
    return (line + 1 < line_count_) ? LineStartByteMemoized(line + 1) - 1 : TreeLength(root_);
  }
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

  // Byte offset of line `line`'s first byte, memoized for the immediately
  // preceding query. Every caller reads lines in ascending order (the render
  // loop over the viewport, whole-buffer serialization, search, save), and
  // LineView/LineLength each need BOTH LineStartByte(index) and
  // LineStartByte(index + 1) -- so caching the second one turns the next line's
  // two tree descents into one. Valid for the current revision only.
  std::uint32_t LineStartByteMemoized(std::size_t line) const;

  // THE single revision-invalidation point. Every derived cache below must be
  // reset here, and every mutation must route through it — RebuildFromOriginal
  // used to clear `line_view_cache_` inline instead, which is exactly how a
  // second cache silently ends up half-invalidated.
  void BumpRevision() {
    line_view_cache_.clear();
    cached_line_start_index_ = kNoCachedLine;
    walk_valid_ = false;
  }

  std::string original_;
  std::string add_;
  std::vector<std::uint32_t> original_newlines_;  // offsets of each '\n' in original_
  std::vector<std::uint32_t> add_newlines_;       // offsets of each '\n' in add_

  std::vector<Node> nodes_;        // index 0 is the null sentinel
  std::vector<NodeId> free_list_;
  NodeId root_ = kNull;
  std::size_t line_count_ = 0;     // authoritative line count (0 == empty document)
  std::uint32_t priority_state_ = 0x9e3779b9u;  // deterministic treap priorities
  // Live-document byte ceiling. subtree_length/TreeLength are uint32, so a live
  // document above this wraps and corrupts split/extract/serialize. Defaults to the
  // uint32 max (the true wrap point); a test can lower it. (TD-2026-07-16-35.)
  std::uint32_t max_live_document_bytes_ = std::numeric_limits<std::uint32_t>::max();
  // Set when the most recent ReplaceLineRange was refused because it would exceed the
  // ceiling (so the mutation was a no-op), cleared when a mutation is applied.
  bool last_mutation_rejected_ = false;

  // Spanning-line materialization, valid for the current revision only.
  mutable std::unordered_map<std::size_t, std::string> line_view_cache_;

  // Sequential line-start memo (see LineStartByteMemoized). kNoCachedLine marks
  // "empty" — a real line index can never equal it.
  static constexpr std::size_t kNoCachedLine = std::numeric_limits<std::size_t>::max();
  mutable std::size_t cached_line_start_index_ = kNoCachedLine;
  mutable std::uint32_t cached_line_start_byte_ = 0;

  // Ascending-walk state for LineStartByte.
  //
  // Resolving line N means finding the (N-1)-th newline of the document, which
  // costs a tree descent plus a binary search over the source buffer's newline
  // offsets. Line N+1 wants the very next newline -- so when it is still inside
  // the same piece, it is the next entry of that same array and needs neither.
  // This records exactly enough to take that step: which node the last resolved
  // newline was in, that node's byte base, its index within the node, and its
  // absolute index in the buffer's newline array.
  //
  // Every mutation routes through BumpRevision (see the note there), which is
  // what makes holding a NodeId here safe.
  mutable bool walk_valid_ = false;
  mutable std::size_t walk_line_ = 0;
  mutable NodeId walk_node_ = kNull;
  mutable std::uint32_t walk_base_ = 0;
  mutable std::uint32_t walk_target_ = 0;
  mutable std::size_t walk_nl_pos_ = 0;
};

}  // namespace microide::editor
