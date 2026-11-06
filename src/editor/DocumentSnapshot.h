#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace microide::editor {

// An immutable view of a whole document at one revision: its '\n'-joined text as
// a list of byte spans, plus whatever keeps those bytes alive. Copying one is a
// reference-count increment, and it may be read from any thread while the
// document it came from goes on being edited.
//
// A PieceTree hands one out in O(pieces) without copying a byte
// (PieceTree::Snapshot): its buffers are immutable once written -- the original
// is shared, add chunks never reallocate and are only appended past every
// snapshot's spans -- so the snapshot shares them. That is what lets the
// crash-safety flush give every unsaved buffer to the persistence worker without
// copying it on the shell thread (TD-2026-11-02-429 item 3).
class DocumentSnapshot {
 public:
  // The empty document (no lines).
  DocumentSnapshot() = default;

  // Owns `text`, the '\n'-joined lines of a `line_count`-line document (0 lines
  // only for empty text).
  static DocumentSnapshot FromText(std::string text, std::size_t line_count);
  // Joins `lines`.
  static DocumentSnapshot FromLines(const std::vector<std::string>& lines);

  // Spans of the '\n'-joined text, in order, and the owners that keep them valid.
  // For PieceTree::Snapshot; nothing else needs to build one from parts.
  static DocumentSnapshot FromSpans(std::vector<std::string_view> spans,
                                    std::vector<std::shared_ptr<const void>> owners,
                                    std::size_t line_count);

  std::size_t LineCount() const { return impl_ != nullptr ? impl_->line_count : 0; }
  std::size_t ByteSize() const { return impl_ != nullptr ? impl_->bytes : 0; }
  bool empty() const { return LineCount() == 0; }

  // Calls `fn(std::string_view line)` for each line, without its '\n'. A line
  // that spans two spans is assembled in one scratch buffer reused across lines;
  // the view passed is valid for that call only.
  template <typename Fn>
  void ForEachLine(Fn&& fn) const;

  void AppendText(std::string& out) const;
  std::vector<std::string> ToLines() const;

 private:
  struct Impl {
    std::vector<std::string_view> spans;
    std::vector<std::shared_ptr<const void>> owners;
    std::size_t line_count = 0;
    std::size_t bytes = 0;
  };
  std::shared_ptr<const Impl> impl_;
};

template <typename Fn>
void DocumentSnapshot::ForEachLine(Fn&& fn) const {
  if (impl_ == nullptr || impl_->line_count == 0) {
    return;
  }
  std::string scratch;
  bool spanning = false;  // `scratch` holds the start of the current line
  std::size_t emitted = 0;
  for (std::string_view span : impl_->spans) {
    while (!span.empty()) {
      const std::size_t newline = span.find('\n');
      if (newline == std::string_view::npos) {
        scratch.append(span);
        spanning = true;
        break;
      }
      if (spanning) {
        scratch.append(span.substr(0, newline));
        fn(std::string_view(scratch));
        scratch.clear();
        spanning = false;
      } else {
        fn(span.substr(0, newline));
      }
      ++emitted;
      span.remove_prefix(newline + 1);
    }
  }
  // The last line has no '\n' after it (possibly empty: a trailing newline).
  if (emitted < impl_->line_count) {
    fn(std::string_view(scratch));
  }
}

}  // namespace microide::editor
