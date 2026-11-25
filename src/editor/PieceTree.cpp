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

}

PieceTree::PieceTree() {
  nodes_.emplace_back();
  Reset({});
}

PieceTree::PieceTree(const std::vector<std::string>& lines) {
  nodes_.emplace_back();
  Reset(lines);
}

void PieceTree::Reset(const std::vector<std::string>& lines) {
  original_ = std::make_shared<const std::string>(JoinLines(lines));
  RebuildFromOriginal();
  line_count_ = lines.size();
}

void PieceTree::ResetFromText(std::string content) {
  original_ = std::make_shared<const std::string>(std::move(content));
  RebuildFromOriginal();
  line_count_ = original_newlines_.size() + 1;
}

void PieceTree::RebuildFromOriginal() {
  add_chunks_.clear();
  add_total_bytes_ = 0;
  original_newlines_.clear();
  if (original_ == nullptr || original_->size() > std::numeric_limits<std::uint32_t>::max()) {
    original_ = std::make_shared<const std::string>();
  }
  const std::string& original = *original_;
  original_newlines_.reserve(original.size() / 48 + 16);
  for (const char* cursor = original.data(), *const end = cursor + original.size();
       cursor != end;) {
    const char* newline = static_cast<const char*>(
        std::memchr(cursor, '\n', static_cast<std::size_t>(end - cursor)));
    if (newline == nullptr) {
      break;
    }
    original_newlines_.push_back(static_cast<std::uint32_t>(newline - original.data()));
    cursor = newline + 1;
  }
  nodes_.resize(1);
  free_list_.clear();
  root_ = kNull;
  priority_state_ = 0x9e3779b9u;
  BumpRevision();
  if (!original.empty()) {
    root_ = Allocate(kOriginal, 0, static_cast<std::uint32_t>(original.size()));
  }
}

std::uint32_t PieceTree::CountNewlines(BufferId buffer, std::uint32_t from,
                                       std::uint32_t to) const {
  const std::vector<std::uint32_t>& nls = NewlinesOf(buffer);
  const auto lo = std::lower_bound(nls.begin(), nls.end(), from);
  const auto hi = std::lower_bound(nls.begin(), nls.end(), to);
  return static_cast<std::uint32_t>(hi - lo);
}

void PieceTree::StartAddChunk(std::size_t needed) {
  std::size_t capacity = kMinAddChunkBytes;
  if (!add_chunks_.empty()) {
    capacity = std::min<std::size_t>(kMaxAddChunkBytes, std::size_t{add_chunks_.back().capacity} * 2);
  }
  capacity = std::max(capacity, needed);
  AddChunk& chunk = add_chunks_.emplace_back();
  chunk.bytes = std::make_shared_for_overwrite<char[]>(capacity);
  chunk.capacity = static_cast<std::uint32_t>(capacity);
  tail_writable_.value = true;
}

PieceTree::AddLocation PieceTree::AppendToAdd(std::string_view text) {
  if (add_chunks_.empty() || !tail_writable_.value ||
      std::size_t{add_chunks_.back().size} + text.size() > add_chunks_.back().capacity) {
    StartAddChunk(text.size());
  }
  AddChunk& chunk = add_chunks_.back();
  const std::uint32_t start = chunk.size;
  std::memcpy(chunk.bytes.get() + start, text.data(), text.size());
  chunk.size += static_cast<std::uint32_t>(text.size());
  add_total_bytes_ += text.size();
  for (const char* cursor = text.data(), *const end = cursor + text.size(); cursor != end;) {
    const char* newline = static_cast<const char*>(
        std::memchr(cursor, '\n', static_cast<std::size_t>(end - cursor)));
    if (newline == nullptr) {
      break;
    }
    chunk.newlines.push_back(start + static_cast<std::uint32_t>(newline - text.data()));
    cursor = newline + 1;
  }
  return AddLocation{.buffer = static_cast<BufferId>(add_chunks_.size()), .offset = start};
}

std::uint32_t PieceTree::NextPriority() {
  std::uint32_t x = priority_state_;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  priority_state_ = x;
  return x;
}

PieceTree::NodeId PieceTree::Allocate(BufferId buffer, std::uint32_t start,
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
    nodes_[t].left = kNull;
    Update(t);
    left = ll;
    right = Merge(lr, t);
  } else if (pos >= left_len + nodes_[t].length) {
    NodeId rl = kNull;
    NodeId rr = kNull;
    Split(nodes_[t].right, pos - left_len - nodes_[t].length, rl, rr);
    nodes_[t].right = kNull;
    Update(t);
    left = Merge(t, rl);
    right = rr;
  } else {
    const std::uint32_t within = pos - left_len;
    const NodeId right_child = nodes_[t].right;
    const BufferId buffer = nodes_[t].buffer;
    const std::uint32_t piece_start = nodes_[t].start;
    const std::uint32_t piece_len = nodes_[t].length;
    const NodeId nr = Allocate(buffer, piece_start + within, piece_len - within);
    nodes_[t].length = within;
    nodes_[t].self_newlines = CountNewlines(buffer, piece_start, piece_start + within);
    nodes_[t].right = kNull;
    Update(nr);
    Update(t);
    left = t;
    right = Merge(nr, right_child);
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
  util::AddPerformanceCounter(util::PerfCounterId::DocumentAddBufferCompactions);
  const std::size_t saved_line_count = line_count_;
  std::string materialized;
  const std::uint32_t byte_size = static_cast<std::uint32_t>(ByteSize());
  materialized.reserve(byte_size);
  CopyRange(0, byte_size, materialized);
  original_ = std::make_shared<const std::string>(std::move(materialized));
  RebuildFromOriginal();
  std::vector<AddChunk>().swap(add_chunks_);
  line_count_ = saved_line_count;
}

void PieceTree::InsertText(std::uint32_t pos, std::string_view text) {
  if (text.empty()) return;
  if (add_total_bytes_ >= kAddBufferCompactionFloorBytes &&
      add_total_bytes_ / kAddBufferDeadHistoryMultiple > ByteSize()) {
    CompactAddBuffer();
  }
  const AddLocation location = AppendToAdd(text);
  const NodeId node =
      Allocate(location.buffer, location.offset, static_cast<std::uint32_t>(text.size()));
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

std::uint32_t PieceTree::LineStartByte(std::size_t line) const {
  if (line == 0) return 0;

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

  std::uint32_t target = static_cast<std::uint32_t>(line - 1);
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
  return acc;
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
  out.reserve(out.size() + length);
  const std::uint32_t end = pos + length;
  std::vector<CopyFrame>& stack = copy_stack_;
  stack.clear();
  stack.push_back({root_, 0, false});
  while (!stack.empty()) {
    const CopyFrame frame = stack.back();
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
    if (frame.base >= end || frame.base + node.subtree_length <= pos) continue;
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
  const std::uint32_t start = LineStartByteMemoized(index);
  const std::uint32_t end =
      (index + 1 < line_count_) ? LineStartByteMemoized(index + 1) - 1 : ByteSize();
  const std::uint32_t length = end - start;

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
  const auto it = line_view_cache_.find(index);
  if (it != line_view_cache_.end()) return it->second;
  util::PerformanceTrace::Scope perf_scope("PieceTree::MaterializeSpanningLine");
  util::AddPerformanceCounter(util::PerfCounterId::EditorLineMaterializations);
  util::AddPerformanceCounter(util::PerfCounterId::EditorLineMaterializedBytes, length);
  std::string& cached = line_view_cache_[index];
  CopyRange(start, length, cached);
  return cached;
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

template <typename Sink>
void PieceTree::ExtractLineRangeInto(std::size_t begin_line, std::size_t end_line,
                                     Sink& out) const {
  const std::size_t count = end_line - begin_line;
  const std::size_t target = out.size() + count;
  out.reserve(target);
  const std::uint32_t start = LineStartByte(begin_line);

  if constexpr (requires(Sink& sink) { sink.reserve_bytes(std::size_t{}); }) {
    const std::uint32_t end_byte = end_line >= line_count_
                                       ? static_cast<std::uint32_t>(ByteSize())
                                       : LineStartByte(end_line);
    if (end_byte > start) {
      out.reserve_bytes(out.content_bytes() + (end_byte - start));
    }
  }

  std::string current;
  bool done = false;
  std::vector<CopyFrame>& stack = extract_stack_;
  stack.clear();
  stack.push_back({root_, 0, false});
  while (!stack.empty() && !done) {
    const CopyFrame frame = stack.back();
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
        while (seg_begin < len) {
          const char* segment = data + seg_begin;
          const auto* newline = static_cast<const char*>(
              std::memchr(segment, '\n', len - seg_begin));
          if (newline == nullptr) break;
          const std::size_t segment_length = static_cast<std::size_t>(newline - segment);
          if (current.empty()) {
            out.emplace_back(segment, segment_length);
          } else {
            current.append(segment, segment_length);
            out.push_back(std::move(current));
            current.clear();
          }
          seg_begin = static_cast<std::uint32_t>(newline - data) + 1;
          if (out.size() == target) {
            done = true;
            break;
          }
        }
        if (!done && seg_begin < len) {
          current.append(data + seg_begin, len - seg_begin);
        }
      }
      continue;
    }
    if (frame.base + node.subtree_length <= start) continue;
    stack.push_back({node.right, piece_end, false});
    stack.push_back({frame.id, frame.base, true});
    stack.push_back({node.left, frame.base, false});
  }
  if (!done && out.size() < target) {
    out.push_back(std::move(current));
  }
}

template void PieceTree::ExtractLineRangeInto<std::vector<std::string>>(
    std::size_t, std::size_t, std::vector<std::string>&) const;
template void PieceTree::ExtractLineRangeInto<LineBlob>(std::size_t, std::size_t, LineBlob&) const;

void PieceTree::ExtractLineRange(std::size_t begin_line, std::size_t end_line,
                                 std::vector<std::string>& out) const {
  ExtractLineRangeInto(begin_line, end_line, out);
}

void PieceTree::AppendLines(std::size_t begin, std::size_t end, LineBlob& out) const {
  if (begin >= end || begin >= line_count_) return;
  ExtractLineRangeInto(begin, std::min(end, line_count_), out);
}

std::vector<std::string> PieceTree::SliceLines(std::size_t begin, std::size_t end) const {
  if (begin >= end || begin >= line_count_) return {};
  end = std::min(end, line_count_);
  std::vector<std::string> out;
  ExtractLineRange(begin, end, out);
  return out;
}

void PieceTree::AppendLines(std::size_t begin, std::size_t end,
                            std::vector<std::string>& out) const {
  if (begin >= end || begin >= line_count_) return;
  ExtractLineRange(begin, std::min(end, line_count_), out);
}

DocumentSnapshot PieceTree::Snapshot() const {
  util::PerformanceTrace::Scope perf_scope("editor::PieceTree::Snapshot");
  std::vector<std::string_view> spans;
  std::vector<std::shared_ptr<const void>> owners;
  std::vector<bool> owned(add_chunks_.size() + 1, false);
  std::vector<NodeId> stack;
  for (NodeId id = root_; id != kNull || !stack.empty();) {
    if (id != kNull) {
      stack.push_back(id);
      id = nodes_[id].left;
      continue;
    }
    id = stack.back();
    stack.pop_back();
    const Node& node = nodes_[id];
    if (node.length != 0) {
      spans.push_back(BufferOf(node.buffer).substr(node.start, node.length));
      if (!owned[node.buffer]) {
        owned[node.buffer] = true;
        if (node.buffer == kOriginal) {
          owners.push_back(original_);
        } else {
          owners.push_back(add_chunks_[node.buffer - 1].bytes);
        }
      }
    }
    id = node.right;
  }
  return DocumentSnapshot::FromSpans(std::move(spans), std::move(owners), line_count_);
}

std::vector<std::string> PieceTree::ToVector() const {
  util::PerformanceTrace::Scope perf_scope("editor::PieceTree::ToVector");
  util::AddPerformanceCounter(util::PerfCounterId::DocumentFullTextMaterializations);
  util::AddPerformanceCounter(util::PerfCounterId::DocumentFullTextBytes, original_->size());
  std::vector<std::string> out;
  if (line_count_ == 0) return out;
  ExtractLineRange(0, line_count_, out);
  return out;
}

template <typename Lines>
void PieceTree::ReplaceLineRangeFrom(std::size_t start, std::size_t removed,
                                     const Lines& inserted) {
  util::AddPerformanceCounter(util::PerfCounterId::DocumentEdits);
  const std::size_t n = line_count_;
  start = std::min(start, n);
  removed = std::min(removed, n - start);
  const std::size_t end_line = start + removed;

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
    std::size_t total = inserted.size() - 1;
    for (const std::string_view line : inserted) {
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

void PieceTree::ReplaceLineRange(std::size_t start, std::size_t removed,
                                 std::span<const std::string> inserted) {
  ReplaceLineRangeFrom(start, removed, inserted);
}

void PieceTree::ReplaceLineRange(std::size_t start, std::size_t removed,
                                 const LineBlob& inserted) {
  ReplaceLineRangeFrom(start, removed, inserted);
}

PieceTree::ByteSpan PieceTree::ResolveByteSpan(std::size_t start_line, std::size_t start_column,
                                               std::size_t end_line,
                                               std::size_t end_column) const {
  if (line_count_ == 0) {
    return ByteSpan{};
  }
  start_line = std::min(start_line, line_count_ - 1);
  end_line = std::clamp(end_line, start_line, line_count_ - 1);

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

void PieceTree::PushBackLine(const std::string& value) {
  ReplaceLineRange(line_count_, 0, std::span<const std::string>(&value, 1));
}

}
