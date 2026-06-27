// Equivalence fuzzer: drives a PieceTree and a naive vector<string> oracle with
// the same random ReplaceLineRange operations (decoded from the fuzz input) and
// aborts if any observable -- line count, per-line content/length, or full
// materialization -- ever diverges. This is the load-bearing safety net for the
// piece-tree data structure (Phase 3 of the large-file overhaul).

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
