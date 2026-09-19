// Leading-indent measurement, chunked, against a single whole-line pass.
//
// `MeasureLeadingIndent` and `AdvanceLeadingIndentOverChunk` are shared by three
// call sites (the fold indent source, the indent guides, and the caret's active
// guide column) precisely so a fix cannot land in one and miss the others -- and
// neither had a test naming it.
//
// The header states the contract that matters: `visual` is the GLOBAL visual
// column, not a chunk-local one, "so a tab's stop arithmetic is identical to a
// single pass over the whole line". That equivalence is only exercised when an
// indent crosses a chunk boundary, and the chunk is 256 bytes -- "deep enough
// that no real indent needs a second chunk". So the multi-chunk path is
// unreachable with real data and would stay wrong indefinitely. These drive it
// directly, at every chunk size, with indents long enough to need many chunks.

#include "TestSupport.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

// Deliberately NOT tested here: an out-of-range line index. `LineSpan`'s
// accessors are raw like `std::vector::operator[]` -- both backing
// implementations index without a bounds check -- so a valid index is a
// precondition, and a test that passed a bad one would be asserting on
// undefined behaviour rather than on a contract. (Written, run, and removed:
// it "failed", and the fix would have been a bounds check on a per-frame
// accessor to satisfy a rule nobody stated.)

#include "editor/LineIndentScan.h"
#include "editor/LineSpan.h"

namespace microide::tests {
namespace {

using microide::editor::AdvanceLeadingIndentOverChunk;
using microide::editor::LineSpan;
using microide::editor::MeasureLeadingIndent;

// Oracle: one pass over the whole line, no chunking at all.
std::size_t IndentWidthOracle(std::string_view line, std::size_t tab_size, bool* found_content) {
  std::size_t visual = 0;
  for (const char ch : line) {
    if (ch == ' ') {
      ++visual;
    } else if (ch == '\t') {
      // Advance to the next tab stop.
      visual += tab_size - (visual % tab_size);
    } else {
      if (found_content != nullptr) {
        *found_content = true;
      }
      return visual;
    }
  }
  if (found_content != nullptr) {
    *found_content = false;
  }
  return visual;
}

// Drive AdvanceLeadingIndentOverChunk with a fixed chunk size.
std::size_t IndentWidthInChunks(std::string_view line, std::size_t tab_size,
                                std::size_t chunk_size) {
  std::size_t visual = 0;
  for (std::size_t offset = 0; offset < line.size(); offset += chunk_size) {
    const std::string_view chunk = line.substr(offset, std::min(chunk_size, line.size() - offset));
    if (AdvanceLeadingIndentOverChunk(chunk, tab_size, visual)) {
      return visual;
    }
  }
  return visual;
}

const std::vector<std::string>& IndentLines() {
  static const std::vector<std::string> lines = {
      "",
      "no indent",
      "    four spaces",
      "\tone tab",
      "\t\ttwo tabs",
      " \t space then tab",
      "\t  tab then spaces",
      "   ",                              // whitespace only
      "\t",                               // one tab, no content
      "  \t  \t  mixed",
      std::string(300, ' ') + "past one chunk",
      std::string(300, '\t') + "tabs past one chunk",
      std::string(100, ' ') + std::string(100, '\t') + std::string(100, ' ') + "three runs",
      std::string(600, ' '),              // whitespace only, several chunks
      "\t" + std::string(255, ' ') + "\tboundary tab",   // a tab right after a 256-byte edge
      std::string(255, ' ') + "\ttab exactly at the chunk edge",
  };
  return lines;
}

void TestChunkedIndentScanMatchesASinglePass() {
  std::size_t multi_chunk_lines = 0;
  for (const std::string& line : IndentLines()) {
    for (const std::size_t tab_size : {1u, 2u, 3u, 4u, 8u}) {
      bool oracle_found = false;
      const std::size_t oracle = IndentWidthOracle(line, tab_size, &oracle_found);

      // Every chunk size from 1 up, plus the production 256 and the whole line.
      for (const std::size_t chunk : {std::size_t{1}, std::size_t{2}, std::size_t{3},
                                      std::size_t{5}, std::size_t{7}, std::size_t{64},
                                      std::size_t{256}, line.size() + 1}) {
        if (chunk == 0) continue;
        const std::size_t chunked = IndentWidthInChunks(line, tab_size, chunk);
        Expect(chunked == oracle,
               "tab_size " + std::to_string(tab_size) + ", chunk " + std::to_string(chunk) +
                   ": chunked scan gave " + std::to_string(chunked) + ", one pass " +
                   std::to_string(oracle) + " (line length " + std::to_string(line.size()) + ")");
        if (line.size() > chunk) {
          ++multi_chunk_lines;
        }
      }

      // The LineSpan entry point must agree too, including its found_content flag.
      const std::vector<std::string> one_line = {line};
      bool found = false;
      const std::size_t measured = MeasureLeadingIndent(LineSpan(one_line), 0, tab_size, &found);
      Expect(measured == oracle,
             "MeasureLeadingIndent gave " + std::to_string(measured) + ", one pass " +
                 std::to_string(oracle));
      Expect(found == oracle_found,
             "found_content disagrees with the oracle for \"" +
                 line.substr(0, 12) + (line.size() > 12 ? "..." : "") + "\"");
    }
  }
  // The multi-chunk path is what this exists for, and the production chunk size
  // makes it unreachable with real indents -- so it has to be reached here.
  Expect(multi_chunk_lines > 100,
         "the sweep barely crossed a chunk boundary: " + std::to_string(multi_chunk_lines));
}

}  // namespace

void RegisterLineIndentScanTests(std::vector<TestCase>& tests) {
  AddTest(tests, "LineIndentScan/ChunkedScanMatchesASinglePass",
          TestChunkedIndentScanMatchesASinglePass);
}

}  // namespace microide::tests
