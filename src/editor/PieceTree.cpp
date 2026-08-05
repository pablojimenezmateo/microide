#include "editor/PieceTree.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <limits>
#include <utility>

#include "util/PerformanceCounters.h"
#include "util/PerformanceTrace.h"

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
  // Newline index for the whole original buffer. This is the dominant cost of
  // opening a file (a 5 MB buffer is 5M iterations of a byte-at-a-time loop), so
  // scan with memchr -- the libc implementation is vectorized -- and reserve
  // from a line-length estimate so the index does not grow by reallocation. The
  // estimate is a hint only; a wrong guess costs a normal geometric growth.
  original_newlines_.reserve(original_.size() / 48 + 16);
  for (const char* cursor = original_.data(), *const end = cursor + original_.size();
       cursor != end;) {
    const char* newline = static_cast<const char*>(
        std::memchr(cursor, '\n', static_cast<std::size_t>(end - cursor)));
    if (newline == nullptr) {
      break;
    }
    original_newlines_.push_back(static_cast<std::uint32_t>(newline - original_.data()));
    cursor = newline + 1;
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
  // Same memchr scan as RebuildFromOriginal: a paste appends its whole payload
  // here, so this is O(pasted bytes) on the edit path.
  for (const char* cursor = text.data(), *const end = cursor + text.size(); cursor != end;) {
    const char* newline = static_cast<const char*>(
        std::memchr(cursor, '\n', static_cast<std::size_t>(end - cursor)));
    if (newline == nullptr) {
      break;
    }
    add_newlines_.push_back(start + static_cast<std::uint32_t>(newline - text.data()));
    cursor = newline + 1;
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
  util::AddPerformanceCounter(util::PerfCounterId::DocumentAddBufferCompactions);
  const std::size_t saved_line_count = line_count_;
  std::string materialized;
  const std::uint32_t byte_size = static_cast<std::uint32_t>(ByteSize());
  materialized.reserve(byte_size);
  CopyRange(0, byte_size, materialized);
  original_ = std::move(materialized);
  RebuildFromOriginal();
  // RebuildFromOriginal clear()s these, which keeps their capacity — and the
  // capacity is the entire point here: what is being compacted away is tens of MB
  // of dead edit history. Swap with empty objects so the pages actually go back.
  std::string().swap(add_);
  std::vector<std::uint32_t>().swap(add_newlines_);
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
  } else if (add_.size() >= kAddBufferCompactionFloorBytes &&
             add_.size() / kAddBufferDeadHistoryMultiple > ByteSize()) {
    // Memory-pressure compaction (TD-2026-08-04-130). The 4 GiB guard above is a
    // correctness backstop and essentially never fires; without this, a session
    // doing sustained large multi-line editing grows resident memory with no upper
    // bound and no way for the user to get it back short of restarting.
    //
    // The measurement that found it is worth repeating, because the counters that
    // usually catch this said nothing. Sixteen toggle-line-comment operations over
    // a 1,000-line selection grew RSS 2.68 MB per repetition, indefinitely, while
    // the per-thread allocation counts stayed BALANCED to within 12 and glibc's
    // arena/uordblks were flat. Both are consistent with what was actually
    // happening: `add_` is one std::string doubling itself (17 -> 35 -> 70 MB),
    // which is one allocation and one free each time, and lives in an mmap'd chunk
    // that never touches the sbrk arena. The original hypothesis in the debt entry
    // was heap fragmentation; a backtrace on allocations >= 16 MB named this line
    // instead.
    //
    // Amortized cost is a fraction of a byte-copy per inserted byte: compaction is
    // O(live document) and cannot recur until another
    // kAddBufferDeadHistoryMultiple x live-document bytes have been inserted. The
    // floor keeps small documents from compacting on ordinary typing.
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

  // O(1) ascending step. Every sequential reader -- the fold scans, syntax
  // highlighting, the renderer's row loop -- walks lines in order, and each
  // LineView(i) needs the start of i+1, which the one-entry memo above never
  // holds. That made a sequential walk pay a full descent plus a binary search
  // per line: 99.6% of the indent fold scan's cost on a 50k-line file was this
  // lookup rather than the work it fed.
  //
  // The newline this call wants is the one immediately after the last resolved
  // newline, so while it is still inside the same piece it is simply the next
  // entry of that piece's buffer newline array.
  if (walk_valid_ && line == walk_line_ + 1) {
    const Node& node = nodes_[walk_node_];
    if (walk_target_ + 1 < node.self_newlines) {
      const std::vector<std::uint32_t>& newlines = NewlinesOf(node.buffer);
      const std::uint32_t offset = newlines[walk_nl_pos_ + 1];
      ++walk_target_;
      ++walk_nl_pos_;
      walk_line_ = line;
      return walk_base_ + (offset - node.start) + 1;
    }
  }

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
      // Inlined NthNewlineOffset so the array position is available to seed the
      // ascending-step state above.
      const std::vector<std::uint32_t>& newlines = NewlinesOf(node.buffer);
      const auto first = std::lower_bound(newlines.begin(), newlines.end(), node.start);
      const std::size_t nl_pos = static_cast<std::size_t>(first - newlines.begin()) + target;
      const std::uint32_t offset = newlines[nl_pos];
      walk_valid_ = true;
      walk_line_ = line;
      walk_node_ = id;
      walk_base_ = acc;
      walk_target_ = target;
      walk_nl_pos_ = nl_pos;
      return acc + (offset - node.start) + 1;
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
  // The exact final size is known here, and the walk below appends one piece at a
  // time. Without this a materialization of a 2 MiB line grew its buffer through
  // 1 -> 2 -> 4 MiB, i.e. three allocations and two full copies of what had
  // already been copied, for a result whose size was never in doubt.
  out.reserve(out.size() + length);
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

  // The line-start lookups above leave `walk_node_` holding the piece the last
  // resolved newline lives in. A line whose bytes lie wholly inside that piece --
  // which is every line of an unedited region, and the overwhelming majority
  // everywhere else -- can be viewed straight out of it, skipping TryViewRange's
  // own tree descent. Without this a sequential walk paid a descent per line even
  // once the line-start lookup itself was O(1).
  if (walk_valid_ && length != 0) {
    const Node& node = nodes_[walk_node_];
    if (start >= walk_base_ && start + length <= walk_base_ + node.length) {
      return std::string_view(BufferOf(node.buffer).data() + node.start + (start - walk_base_),
                              length);
    }
  }

  bool ok = false;
  const std::string_view view = TryViewRange(start, length, ok);
  if (ok) return view;
  // Spanning line: materialize once per revision. A same-index re-read within the
  // revision returns the existing slot UNTOUCHED — re-running clear()+CopyRange
  // could reallocate the slot's buffer and dangle a view returned by an earlier
  // same-index call, and re-materializing is wasted work anyway.
  CachedLine& cached = line_view_cache_[index];
  if (cached.revision == revision_) return cached.text;
  // Counted and scoped because this is the one line-sized copy left on the edit
  // path (TD-2026-08-05-131) and nothing measured it: it is charged to whichever
  // consumer happens to ask for the edited line first in a frame, so it moved
  // between scopes in the ranking as the callers around it got faster, and read
  // as that consumer's own cost. On a line with no newlines in it it is megabytes.
  util::PerformanceTrace::Scope perf_scope("PieceTree::MaterializeSpanningLine");
  util::AddPerformanceCounter(util::PerfCounterId::EditorLineMaterializations);
  util::AddPerformanceCounter(util::PerfCounterId::EditorLineMaterializedBytes, length);
  // clear() keeps the capacity, so a line re-materialized at the same size after a
  // keystroke re-copies into the buffer it already had rather than allocating a
  // fresh one and freeing the old.
  cached.text.clear();
  CopyRange(start, length, cached.text);
  cached.revision = revision_;
  return cached.text;
}

std::string_view PieceTree::LineWindow(std::size_t index, std::size_t byte_start,
                                       std::size_t byte_len, std::string& scratch) const {
  const std::uint32_t line_start = LineStartByteMemoized(index);
  const std::uint32_t line_end =
      (index + 1 < line_count_) ? LineStartByteMemoized(index + 1) - 1 : ByteSize();
  const std::uint32_t line_length = line_end - line_start;
  if (byte_start >= line_length) return {};
  const std::uint32_t start = line_start + static_cast<std::uint32_t>(byte_start);
  const std::uint32_t length =
      static_cast<std::uint32_t>(std::min<std::size_t>(byte_len, line_length - byte_start));
  if (length == 0) return {};

  // Same walk-node shortcut LineView uses: the line-start lookups above leave the
  // cursor on the piece the resolved newline lives in, so a window inside that
  // piece needs no tree descent at all.
  if (walk_valid_) {
    const Node& node = nodes_[walk_node_];
    if (start >= walk_base_ && start + length <= walk_base_ + node.length) {
      return std::string_view(BufferOf(node.buffer).data() + node.start + (start - walk_base_),
                              length);
    }
  }
  bool ok = false;
  const std::string_view view = TryViewRange(start, length, ok);
  if (ok) return view;
  scratch.clear();
  CopyRange(start, length, scratch);
  return scratch;
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
  // A whole-document materialization: one std::string per line, O(document).
  // The piece tree exists so the hot paths never need this, so a non-trivial
  // count here means a caller is copying the document per frame or per keystroke.
  // (The per-line read path is deliberately NOT counted -- an atomic add on
  // LineView would cost more than the counter could ever reveal.)
  util::PerformanceTrace::Scope perf_scope("editor::PieceTree::ToVector");
  util::AddPerformanceCounter(util::PerfCounterId::DocumentFullTextMaterializations);
  util::AddPerformanceCounter(util::PerfCounterId::DocumentFullTextBytes, original_.size());
  std::vector<std::string> out;
  if (line_count_ == 0) return out;
  ExtractLineRange(0, line_count_, out);
  return out;
}

// --- mutation ---

void PieceTree::ReplaceLineRange(std::size_t start, std::size_t removed,
                                 std::span<const std::string> inserted) {
  util::AddPerformanceCounter(util::PerfCounterId::DocumentEdits);
  const std::size_t n = line_count_;
  start = std::min(start, n);
  removed = std::min(removed, n - start);
  const std::size_t end_line = start + removed;

  // Decide the surrounding newline BEFORE building the text, and reserve for it.
  // This used to join into `joined` with no reserve and then produce `replacement`
  // as `std::move(joined) + "\n"` (or `"\n" + std::move(joined)`) -- a concatenation
  // that reallocates, because `joined` came out of append growth with no spare
  // capacity. Replacing one 4 MB line therefore allocated 4 MB for the join and
  // then 8 MB for the concatenation. Reserving once and placing the newline while
  // building costs a single exact-sized buffer.
  std::uint32_t old_start = 0;
  std::uint32_t old_end = 0;
  bool newline_before = false;
  bool newline_after = false;
  if (end_line < n) {
    old_start = LineStartByte(start);
    old_end = LineStartByte(end_line);
    newline_after = !inserted.empty();
  } else {
    old_end = ByteSize();
    if (start > 0) {
      old_start = (start < n) ? LineStartByte(start) - 1 : ByteSize();
      newline_before = !inserted.empty();
    } else {
      old_start = 0;
    }
  }

  std::string replacement;
  if (!inserted.empty()) {
    std::size_t total = inserted.size() - 1;  // the '\n' between each pair
    for (const std::string& line : inserted) {
      total += line.size();
    }
    replacement.reserve(total + (newline_before ? 1 : 0) + (newline_after ? 1 : 0));
    if (newline_before) replacement.push_back('\n');
    for (std::size_t i = 0; i < inserted.size(); ++i) {
      if (i != 0) replacement.push_back('\n');
      replacement.append(inserted[i]);
    }
    if (newline_after) replacement.push_back('\n');
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

PieceTree::ByteSpan PieceTree::ResolveByteSpan(std::size_t start_line, std::size_t start_column,
                                               std::size_t end_line,
                                               std::size_t end_column) const {
  if (line_count_ == 0) {
    return ByteSpan{};
  }
  start_line = std::min(start_line, line_count_ - 1);
  end_line = std::clamp(end_line, start_line, line_count_ - 1);

  // Ascending order: LineStartByte's walk state makes line N+1 free once N is
  // resolved, and the memo makes a same-line span a single descent.
  const std::uint32_t start_base = LineStartByteMemoized(start_line);
  const std::uint32_t start_limit = LineEndByte(start_line);
  const std::uint32_t start =
      start_base +
      static_cast<std::uint32_t>(std::min<std::size_t>(start_column, start_limit - start_base));

  std::uint32_t end;
  if (end_line == start_line) {
    end = start_base +
          static_cast<std::uint32_t>(std::min<std::size_t>(end_column, start_limit - start_base));
  } else {
    const std::uint32_t end_base = LineStartByteMemoized(end_line);
    const std::uint32_t end_limit = LineEndByte(end_line);
    end = end_base +
          static_cast<std::uint32_t>(std::min<std::size_t>(end_column, end_limit - end_base));
  }
  return ByteSpan{.start = start, .end = std::max(start, end)};
}

void PieceTree::AppendTextRange(std::size_t start_line, std::size_t start_column,
                                std::size_t end_line, std::size_t end_column,
                                std::string& out) const {
  const ByteSpan span = ResolveByteSpan(start_line, start_column, end_line, end_column);
  CopyRange(span.start, span.end - span.start, out);
}

void PieceTree::ReplaceTextRange(std::size_t start_line, std::size_t start_column,
                                 std::size_t end_line, std::size_t end_column,
                                 std::string_view text) {
  util::AddPerformanceCounter(util::PerfCounterId::DocumentEdits);
  if (line_count_ == 0) {
    return;
  }
  const std::size_t clamped_start_line = std::min(start_line, line_count_ - 1);
  const std::size_t clamped_end_line =
      std::clamp(end_line, clamped_start_line, line_count_ - 1);
  const ByteSpan span =
      ResolveByteSpan(clamped_start_line, start_column, clamped_end_line, end_column);

  // Same live-document byte ceiling as ReplaceLineRange: subtree_length is uint32,
  // so refuse the whole splice before any mutation runs rather than wrapping it.
  const std::uint64_t projected = static_cast<std::uint64_t>(ByteSize()) -
                                  static_cast<std::uint64_t>(span.end - span.start) + text.size();
  if (projected > max_live_document_bytes_) {
    last_mutation_rejected_ = true;
    return;
  }
  last_mutation_rejected_ = false;

  std::size_t inserted_newlines = 0;
  for (const char* cursor = text.data(), *const end = cursor + text.size(); cursor != end;) {
    const char* newline = static_cast<const char*>(
        std::memchr(cursor, '\n', static_cast<std::size_t>(end - cursor)));
    if (newline == nullptr) {
      break;
    }
    ++inserted_newlines;
    cursor = newline + 1;
  }

  DeleteRange(span.start, span.end - span.start);
  InsertText(span.start, text);
  line_count_ = line_count_ - (clamped_end_line - clamped_start_line) + inserted_newlines;
  BumpRevision();
}

void PieceTree::SetLine(std::size_t index, const std::string& value) {
  ReplaceLineRange(index, 1, std::span<const std::string>(&value, 1));
}

void PieceTree::InsertLine(std::size_t index, const std::string& value) {
  ReplaceLineRange(index, 0, std::span<const std::string>(&value, 1));
}

void PieceTree::EraseLine(std::size_t index) {
  if (index >= line_count_) return;
  ReplaceLineRange(index, 1, std::span<const std::string>{});
}

void PieceTree::EraseLineRange(std::size_t begin, std::size_t end) {
  if (begin >= end) return;
  ReplaceLineRange(begin, end - begin, std::span<const std::string>{});
}

void PieceTree::PushBackLine(const std::string& value) {
  ReplaceLineRange(line_count_, 0, std::span<const std::string>(&value, 1));
}

}  // namespace microide::editor
