// Equivalence fuzzer: drives a PieceTree and a naive oracle with the same random
// mutations (decoded from the fuzz input) and aborts if any observable -- line
// count, per-line content/length, or full materialization -- ever diverges. This
// is the load-bearing safety net for the piece-tree data structure (Phase 3 of
// the large-file overhaul).
//
// Both mutation primitives are driven against the SAME document, interleaved:
// the line-shaped `ReplaceLineRange` (oracle: a vector<string> erase+insert) and
// the column-scoped `ReplaceTextRange` (oracle: a splice of the '\n'-joined
// bytes, re-split). The column-scoped form is what the in-line edit path uses,
// so it carries the same corruption risk and needs the same net.

#include "editor/PieceTree.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace {

// Sequential byte reader over the fuzz input.
struct Cursor {
  const std::uint8_t* data;
  std::size_t size;
  std::size_t pos = 0;

  std::uint8_t Byte() { return pos < size ? data[pos++] : 0; }
  std::size_t Bounded(std::size_t bound) { return bound == 0 ? 0 : Byte() % bound; }
  bool Done() const { return pos >= size; }
};

std::string MakeLine(Cursor& cursor) {
  const std::size_t len = cursor.Bounded(10);
  std::string line;
  line.reserve(len);
  for (std::size_t i = 0; i < len; ++i) {
    // Printable, never '\n' (lines hold no embedded newlines).
    line.push_back(static_cast<char>('a' + (cursor.Byte() % 26)));
  }
  return line;
}

void CheckEquivalent(const microide::editor::PieceTree& tree,
                     const std::vector<std::string>& model) {
  assert(tree.LineCount() == model.size());
  for (std::size_t i = 0; i < model.size(); ++i) {
    assert(tree.LineView(i) == model[i]);
    assert(tree.LineLength(i) == model[i].size());
  }
  assert(tree.ToVector() == model);
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (data == nullptr) return 0;
  Cursor cursor{data, size};

  microide::editor::PieceTree tree;
  std::vector<std::string> model;

  // Optional initial document.
  const std::size_t initial = cursor.Bounded(16);
  std::vector<std::string> seed;
  for (std::size_t i = 0; i < initial; ++i) seed.push_back(MakeLine(cursor));
  tree.Reset(seed);
  model = seed;
  CheckEquivalent(tree, model);

  int guard = 0;
  while (!cursor.Done() && guard++ < 512) {
    const std::size_t n = model.size();
    if (n != 0 && (cursor.Byte() & 1u) != 0u) {
      // Column-scoped splice. Oracle: replace the byte span in the joined text.
      const std::size_t start_line = cursor.Bounded(n);
      const std::size_t start_column = cursor.Bounded(model[start_line].size() + 1);
      const std::size_t end_line = start_line + cursor.Bounded(n - start_line);
      const std::size_t end_column = cursor.Bounded(model[end_line].size() + 1);
      std::string text;
      for (std::size_t i = 0, count = cursor.Bounded(6); i < count; ++i) {
        const std::uint8_t byte = cursor.Byte();
        text.push_back((byte % 8u) == 0u ? '\n' : static_cast<char>('a' + (byte % 26u)));
      }

      std::string joined;
      for (std::size_t i = 0; i < model.size(); ++i) {
        if (i != 0) joined.push_back('\n');
        joined.append(model[i]);
      }
      std::size_t start_base = 0;
      for (std::size_t i = 0; i < start_line; ++i) start_base += model[i].size() + 1;
      std::size_t end_base = 0;
      for (std::size_t i = 0; i < end_line; ++i) end_base += model[i].size() + 1;
      const std::size_t from = start_base + start_column;
      const std::size_t raw_to = end_base + end_column;
      const std::size_t to = raw_to > from ? raw_to : from;

      std::string extracted;
      tree.AppendTextRange(start_line, start_column, end_line, end_column, extracted);
      assert(extracted == joined.substr(from, to - from));

      joined.replace(from, to - from, text);
      model.clear();
      std::size_t piece_start = 0;
      for (std::size_t i = 0; i <= joined.size(); ++i) {
        if (i == joined.size() || joined[i] == '\n') {
          model.emplace_back(joined.substr(piece_start, i - piece_start));
          piece_start = i + 1;
        }
      }

      tree.ReplaceTextRange(start_line, start_column, end_line, end_column, text);
      CheckEquivalent(tree, model);
      continue;
    }

    const std::size_t start = cursor.Bounded(n + 1);
    const std::size_t removed = cursor.Bounded((n - (start < n ? start : n)) + 1);
    const std::size_t insert_count = cursor.Bounded(4);
    std::vector<std::string> inserted;
    for (std::size_t i = 0; i < insert_count; ++i) inserted.push_back(MakeLine(cursor));

    tree.ReplaceLineRange(start, removed, inserted);

    std::size_t s = start < n ? start : n;
    std::size_t r = removed < (n - s) ? removed : (n - s);
    model.erase(model.begin() + static_cast<std::ptrdiff_t>(s),
                model.begin() + static_cast<std::ptrdiff_t>(s + r));
    model.insert(model.begin() + static_cast<std::ptrdiff_t>(s), inserted.begin(),
                 inserted.end());

    CheckEquivalent(tree, model);
  }
  return 0;
}
