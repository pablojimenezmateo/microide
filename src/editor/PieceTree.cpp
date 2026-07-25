#include "editor/PieceTree.h"

#include <algorithm>
#include <cassert>
#include <limits>
#include <utility>

namespace microide::editor {

namespace {

std::string JoinLines(const std::vector<std::string>& lines) {
  std::size_t total = 0;
  for (const std::string& line : lines) total += line.size() + 1;
  std::string joined;
  joined.reserve(total);
  for (std::size_t i = 0; i < lines.size(); ++i) {
    if (i != 0) joined.push_back('\n');
    joined.append(lines[i]);
  }
  return joined;
}

}  // namespace

PieceTree::PieceTree() {
  nodes_.emplace_back();  // index 0 == null sentinel
  Reset({});
}

PieceTree::PieceTree(const std::vector<std::string>& lines) {
  nodes_.emplace_back();
  Reset(lines);
}

void PieceTree::Reset(const std::vector<std::string>& lines) {
  original_ = JoinLines(lines);
  RebuildFromOriginal();
  // A vector of N lines joins to N-1 newlines, so the newline-derived count
  // would be N. The one case the join cannot express is the empty document
  // (0 lines vs the single empty line ""), so honor the caller's count here.
  line_count_ = lines.size();
}

void PieceTree::ResetFromText(std::string content) {
  original_ = std::move(content);
  RebuildFromOriginal();
  // Canonical '\n'-joined text: N newlines means N+1 lines (the final line may
  // be empty), and empty content is one empty line -- matching DecodeLines.
  line_count_ = original_newlines_.size() + 1;
}

void PieceTree::RebuildFromOriginal() {
  add_.clear();
  add_newlines_.clear();
  original_newlines_.clear();
  // Piece offsets and lengths are 32-bit. The editor caps file loads far below
  // this (kMaxTextFileBytes = 512 MiB), but guard here too so PieceTree stays
  // self-defending: without this, a >=4 GiB buffer (a future cap change or a
  // non-file caller) would wrap the loop counter into an infinite loop and
  // silently truncate the length cast below.
  if (original_.size() > std::numeric_limits<std::uint32_t>::max()) {
    original_.clear();
  }
  for (std::size_t i = 0; i < original_.size(); ++i) {
    if (original_[i] == '\n') original_newlines_.push_back(static_cast<std::uint32_t>(i));
  }
  nodes_.resize(1);  // keep only the sentinel
  free_list_.clear();
  root_ = kNull;
  priority_state_ = 0x9e3779b9u;
  BumpRevision();
  if (!original_.empty()) {
    root_ = Allocate(kOriginal, 0, static_cast<std::uint32_t>(original_.size()));
  }
}

// --- buffer helpers ---

std::uint32_t PieceTree::CountNewlines(std::uint8_t buffer, std::uint32_t from,
                                       std::uint32_t to) const {
  const std::vector<std::uint32_t>& nls = NewlinesOf(buffer);
  const auto lo = std::lower_bound(nls.begin(), nls.end(), from);
  const auto hi = std::lower_bound(nls.begin(), nls.end(), to);
  return static_cast<std::uint32_t>(hi - lo);
}

std::uint32_t PieceTree::NthNewlineOffset(std::uint8_t buffer, std::uint32_t from,
                                          std::uint32_t nth) const {
  const std::vector<std::uint32_t>& nls = NewlinesOf(buffer);
  const auto lo = std::lower_bound(nls.begin(), nls.end(), from);
  return *(lo + nth);
}

std::uint32_t PieceTree::AppendToAdd(std::string_view text) {
  const std::uint32_t start = static_cast<std::uint32_t>(add_.size());
  add_.append(text);
  for (std::uint32_t i = 0; i < text.size(); ++i) {
    if (text[i] == '\n') add_newlines_.push_back(start + i);
  }
  return start;
}

// --- node pool ---

std::uint32_t PieceTree::NextPriority() {
  std::uint32_t x = priority_state_;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  priority_state_ = x;
  return x;
}

PieceTree::NodeId PieceTree::Allocate(std::uint8_t buffer, std::uint32_t start,
                                      std::uint32_t length) {
  NodeId id;
  if (!free_list_.empty()) {
    id = free_list_.back();
    free_list_.pop_back();
    nodes_[id] = Node{};
  } else {
    id = static_cast<NodeId>(nodes_.size());
    nodes_.emplace_back();
  }
  Node& node = nodes_[id];
  node.buffer = buffer;
  node.start = start;
  node.length = length;
  node.priority = NextPriority();
  node.self_newlines = CountNewlines(buffer, start, start + length);
  node.subtree_length = length;
  node.subtree_newlines = node.self_newlines;
  return id;
}

void PieceTree::Free(NodeId id) {
  if (id == kNull) return;
  free_list_.push_back(id);
}

void PieceTree::FreeSubtree(NodeId id) {
  if (id == kNull) return;
  FreeSubtree(nodes_[id].left);
  FreeSubtree(nodes_[id].right);
  Free(id);
}

void PieceTree::Update(NodeId id) {
  Node& node = nodes_[id];
  node.subtree_length = node.length + TreeLength(node.left) + TreeLength(node.right);
  node.subtree_newlines = node.self_newlines + TreeNewlines(node.left) + TreeNewlines(node.right);
}

// --- treap core ---

void PieceTree::Split(NodeId t, std::uint32_t pos, NodeId& left, NodeId& right) {
  if (t == kNull) {
    left = kNull;
    right = kNull;
    return;
  }
  const std::uint32_t left_len = TreeLength(nodes_[t].left);
  if (pos <= left_len) {
    NodeId ll = kNull;
    NodeId lr = kNull;
    Split(nodes_[t].left, pos, ll, lr);
    nodes_[t].left = lr;
    Update(t);
    left = ll;
    right = t;
  } else if (pos >= left_len + nodes_[t].length) {
    NodeId rl = kNull;
    NodeId rr = kNull;
    Split(nodes_[t].right, pos - left_len - nodes_[t].length, rl, rr);
    nodes_[t].right = rl;
    Update(t);
    left = t;
    right = rr;
  } else {
    // Split this node's piece at byte `within`.
    const std::uint32_t within = pos - left_len;
    const NodeId right_child = nodes_[t].right;
    const std::uint8_t buffer = nodes_[t].buffer;
    const std::uint32_t piece_start = nodes_[t].start;
    const std::uint32_t piece_len = nodes_[t].length;
    const std::uint32_t priority = nodes_[t].priority;
    const NodeId nr = Allocate(buffer, piece_start + within, piece_len - within);
    // Allocate may have reallocated nodes_; index by id below.
    nodes_[t].length = within;
    nodes_[t].self_newlines = CountNewlines(buffer, piece_start, piece_start + within);
    nodes_[t].right = kNull;
    nodes_[nr].priority = priority;  // preserve heap order vs the former right child
    nodes_[nr].left = kNull;
    nodes_[nr].right = right_child;
    Update(nr);
    Update(t);
    left = t;
    right = nr;
  }
}

PieceTree::NodeId PieceTree::Merge(NodeId left, NodeId right) {
  if (left == kNull) return right;
  if (right == kNull) return left;
  if (nodes_[left].priority >= nodes_[right].priority) {
    nodes_[left].right = Merge(nodes_[left].right, right);
    Update(left);
    return left;
  }
  nodes_[right].left = Merge(left, nodes_[right].left);
  Update(right);
  return right;
}

void PieceTree::CompactAddBuffer() {
  // The live document is uint32-bounded, so materialize it into a fresh original_
  // buffer and drop the accumulated add_ history. Content and line count are
  // invariant (representation only changes), so keep the authoritative line_count_
  // rather than re-deriving it (which would mishandle the empty-document edge case).
  const std::size_t saved_line_count = line_count_;
  std::string materialized;
  const std::uint32_t byte_size = static_cast<std::uint32_t>(ByteSize());
  materialized.reserve(byte_size);
  CopyRange(0, byte_size, materialized);
  original_ = std::move(materialized);
  RebuildFromOriginal();
  line_count_ = saved_line_count;
}

void PieceTree::InsertText(std::uint32_t pos, std::string_view text) {
  if (text.empty()) return;
  // add_ is append-only: deletes never reclaim it and only Reset*/compaction
  // clears it, so add_.size() tracks *cumulative* inserted bytes over the
  // document's lifetime, decoupled from the uint32-bounded live size. A formatter
  // plugin or the --control channel repeatedly rewriting a large document can
  // drive it past 4 GiB while the live document stays small; then AppendToAdd's
  // static_cast<uint32_t>(add_.size()) would wrap and later pieces would reference
  // stale early bytes -> silent corruption. Compact first (mirrors
  // RebuildFromOriginal's original_ self-defense) so add_ offsets stay in range.
  if (add_.size() + text.size() > std::numeric_limits<std::uint32_t>::max()) {
    CompactAddBuffer();
  }
  const std::uint32_t start = AppendToAdd(text);
  const NodeId node = Allocate(kAdd, start, static_cast<std::uint32_t>(text.size()));
  NodeId left = kNull;
  NodeId right = kNull;
  Split(root_, pos, left, right);
  root_ = Merge(Merge(left, node), right);
}

void PieceTree::DeleteRange(std::uint32_t pos, std::uint32_t length) {
  if (length == 0) return;
  NodeId left = kNull;
  NodeId rest = kNull;
  Split(root_, pos, left, rest);
  NodeId mid = kNull;
  NodeId right = kNull;
  Split(rest, length, mid, right);
  FreeSubtree(mid);
  root_ = Merge(left, right);
}

// --- navigation / extraction ---

std::uint32_t PieceTree::LineStartByte(std::size_t line) const {
  if (line == 0) return 0;
  std::uint32_t target = static_cast<std::uint32_t>(line - 1);  // which '\n'
  NodeId id = root_;
  std::uint32_t acc = 0;
  while (id != kNull) {
    const Node& node = nodes_[id];
    const std::uint32_t left_nl = TreeNewlines(node.left);
    if (target < left_nl) {
      id = node.left;
      continue;
    }
    target -= left_nl;
    acc += TreeLength(node.left);
    if (target < node.self_newlines) {
      const std::uint32_t off = NthNewlineOffset(node.buffer, node.start, target);
      return acc + (off - node.start) + 1;
    }
    target -= node.self_newlines;
    acc += node.length;
    id = node.right;
  }
  return acc;  // unreachable for valid input
}

std::uint32_t PieceTree::LineStartByteMemoized(std::size_t line) const {
  if (line == cached_line_start_index_) {
    return cached_line_start_byte_;
  }
  const std::uint32_t start = LineStartByte(line);
  cached_line_start_index_ = line;
  cached_line_start_byte_ = start;
  return start;
}

void PieceTree::CopyRange(std::uint32_t pos, std::uint32_t length, std::string& out) const {
  if (length == 0) return;
  const std::uint32_t end = pos + length;
  // Pruned in-order walk: each frame carries the global base offset of its
  // subtree; subtrees that cannot overlap [pos, end) are skipped.
  struct Frame {
    NodeId id;
    std::uint32_t base;
    bool emit;  // false: expand children; true: append this node's overlap
  };
  std::vector<Frame> stack;
  stack.push_back({root_, 0, false});
  while (!stack.empty()) {
    const Frame frame = stack.back();
    stack.pop_back();
    if (frame.id == kNull) continue;
    const Node& node = nodes_[frame.id];
    const std::uint32_t left_len = TreeLength(node.left);
    const std::uint32_t piece_start = frame.base + left_len;
    const std::uint32_t piece_end = piece_start + node.length;
    if (frame.emit) {
      const std::uint32_t s = std::max(pos, piece_start);
      const std::uint32_t e = std::min(end, piece_end);
      if (s < e) {
        out.append(BufferOf(node.buffer).data() + node.start + (s - piece_start), e - s);
      }
      continue;
    }
    if (frame.base >= end || frame.base + node.subtree_length <= pos) continue;  // prune
    // Push in reverse so the in-order sequence (left, self, right) pops in order.
    stack.push_back({node.right, piece_end, false});
    stack.push_back({frame.id, frame.base, true});
    stack.push_back({node.left, frame.base, false});
  }
}

std::string_view PieceTree::TryViewRange(std::uint32_t pos, std::uint32_t length,
                                         bool& ok) const {
  ok = true;
  if (length == 0) return {};
  NodeId id = root_;
  while (id != kNull) {
    const Node& node = nodes_[id];
    const std::uint32_t left_len = TreeLength(node.left);
    if (pos < left_len) {
      id = node.left;
    } else if (pos < left_len + node.length) {
      const std::uint32_t within = pos - left_len;
      if (within + length <= node.length) {
        return std::string_view(BufferOf(node.buffer).data() + node.start + within, length);
      }
      ok = false;
      return {};
    } else {
      pos -= left_len + node.length;
      id = node.right;
    }
  }
  ok = false;
  return {};
}

std::string_view PieceTree::LineView(std::size_t index) const {
  // Ask for index + 1 LAST so the memo is left holding it: the next call in an
  // ascending walk is LineView(index + 1), which then needs zero descents to
  // find its start.
  const std::uint32_t start = LineStartByteMemoized(index);
  const std::uint32_t end =
      (index + 1 < line_count_) ? LineStartByteMemoized(index + 1) - 1 : ByteSize();
  const std::uint32_t length = end - start;
  bool ok = false;
  const std::string_view view = TryViewRange(start, length, ok);
  if (ok) return view;
  // Spanning line: materialize once per revision. If this index is already
  // cached (the whole map is cleared on every mutation via BumpRevision), return
  // the existing slot untouched — re-running clear()+CopyRange could reallocate
  // the slot's buffer and dangle a view returned by an earlier same-index call,
  // and re-materializing is wasted work anyway.
  auto it = line_view_cache_.find(index);
  if (it != line_view_cache_.end()) return it->second;
  std::string& cached = line_view_cache_[index];
  CopyRange(start, length, cached);
  return cached;
}

std::size_t PieceTree::LineLength(std::size_t index) const {
  const std::uint32_t start = LineStartByteMemoized(index);
  const std::uint32_t end =
      (index + 1 < line_count_) ? LineStartByteMemoized(index + 1) - 1 : ByteSize();
  return end - start;
}

void PieceTree::ExtractLineRange(std::size_t begin_line, std::size_t end_line,
                                 std::vector<std::string>& out) const {
  const std::size_t count = end_line - begin_line;
  const std::size_t target = out.size() + count;
  out.reserve(target);
  const std::uint32_t start = LineStartByte(begin_line);

  // In-order pruned walk over [start, ByteSize()). Each visited piece contributes
  // a contiguous byte span; we split those bytes on '\n' straight into `out`
  // (newline excluded), stopping the instant `count` lines have been emitted. The
  // final requested line (when end_line == line_count_) has no terminating '\n' in
  // the byte stream, so it is pushed once after the walk drains.
  std::string current;
  bool done = false;
  struct Frame {
    NodeId id;
    std::uint32_t base;
    bool emit;  // false: expand children; true: consume this node's span
  };
  std::vector<Frame> stack;
  stack.push_back({root_, 0, false});
  while (!stack.empty() && !done) {
    const Frame frame = stack.back();
    stack.pop_back();
    if (frame.id == kNull) continue;
    const Node& node = nodes_[frame.id];
    const std::uint32_t left_len = TreeLength(node.left);
    const std::uint32_t piece_start = frame.base + left_len;
    const std::uint32_t piece_end = piece_start + node.length;
    if (frame.emit) {
      const std::uint32_t s = std::max(start, piece_start);
      if (s < piece_end) {
        const char* data = BufferOf(node.buffer).data() + node.start + (s - piece_start);
        const std::uint32_t len = piece_end - s;
        std::uint32_t seg_begin = 0;
        for (std::uint32_t i = 0; i < len; ++i) {
          if (data[i] == '\n') {
            current.append(data + seg_begin, i - seg_begin);
            out.push_back(std::move(current));
            current.clear();
            seg_begin = i + 1;
            if (out.size() == target) {
              done = true;
              break;
            }
          }
        }
        if (!done) {
          current.append(data + seg_begin, len - seg_begin);
        }
      }
      continue;
    }
    if (frame.base + node.subtree_length <= start) continue;  // prune: entirely before start
    // Push in reverse so the in-order sequence (left, self, right) pops in order.
    stack.push_back({node.right, piece_end, false});
    stack.push_back({frame.id, frame.base, true});
    stack.push_back({node.left, frame.base, false});
  }
  if (!done && out.size() < target) {
    out.push_back(std::move(current));
  }
}

std::vector<std::string> PieceTree::SliceLines(std::size_t begin, std::size_t end) const {
  if (begin >= end || begin >= line_count_) return {};
  end = std::min(end, line_count_);
  std::vector<std::string> out;
  ExtractLineRange(begin, end, out);
  return out;
}

std::vector<std::string> PieceTree::ToVector() const {
  std::vector<std::string> out;
  if (line_count_ == 0) return out;
  ExtractLineRange(0, line_count_, out);
  return out;
}

// --- mutation ---

void PieceTree::ReplaceLineRange(std::size_t start, std::size_t removed,
                                 const std::vector<std::string>& inserted) {
  const std::size_t n = line_count_;
  start = std::min(start, n);
  removed = std::min(removed, n - start);
  const std::size_t end_line = start + removed;

  std::string joined;
  for (std::size_t i = 0; i < inserted.size(); ++i) {
    if (i != 0) joined.push_back('\n');
    joined.append(inserted[i]);
  }

  std::uint32_t old_start = 0;
  std::uint32_t old_end = 0;
  std::string replacement;
  if (end_line < n) {
    old_start = LineStartByte(start);
    old_end = LineStartByte(end_line);
    if (!inserted.empty()) replacement = std::move(joined) + "\n";
  } else {
    old_end = ByteSize();
    if (start > 0) {
      old_start = (start < n) ? LineStartByte(start) - 1 : ByteSize();
      if (!inserted.empty()) replacement = "\n" + std::move(joined);
    } else {
      old_start = 0;
      replacement = std::move(joined);
    }
  }

  // Live-document byte ceiling: subtree_length/TreeLength are uint32, so a document
  // grown past the ceiling wraps and corrupts split/extract/serialize. Individual edits
  // are bounded, but many bounded edits can accumulate past it. Compute the projected
  // size (current − removed bytes + replacement) and refuse the WHOLE replace before any
  // mutation runs, keeping the tree and line_count_ consistent. (TD-2026-07-16-35.)
  const std::uint64_t projected = static_cast<std::uint64_t>(ByteSize()) -
                                  static_cast<std::uint64_t>(old_end - old_start) +
                                  replacement.size();
  if (projected > max_live_document_bytes_) {
    last_mutation_rejected_ = true;
    return;
  }
  last_mutation_rejected_ = false;

  DeleteRange(old_start, old_end - old_start);
  InsertText(old_start, replacement);
  line_count_ = n - removed + inserted.size();
  BumpRevision();
}

void PieceTree::SetLine(std::size_t index, const std::string& value) {
  ReplaceLineRange(index, 1, {value});
}

void PieceTree::InsertLine(std::size_t index, const std::string& value) {
  ReplaceLineRange(index, 0, {value});
}

void PieceTree::EraseLine(std::size_t index) {
  if (index >= line_count_) return;
  ReplaceLineRange(index, 1, {});
}

void PieceTree::EraseLineRange(std::size_t begin, std::size_t end) {
  if (begin >= end) return;
  ReplaceLineRange(begin, end - begin, {});
}

void PieceTree::PushBackLine(const std::string& value) {
  ReplaceLineRange(line_count_, 0, {value});
}

}  // namespace microide::editor
