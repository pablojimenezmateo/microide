#include "TestSupport.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "editor/LineIndentScan.h"
#include "editor/LineSpan.h"

namespace microide::tests {
namespace {

using microide::editor::AdvanceLeadingIndentOverChunk;
using microide::editor::LineSpan;
using microide::editor::MeasureLeadingIndent;

std::size_t IndentWidthOracle(std::string_view line, std::size_t tab_size, bool* found_content) {
  std::size_t visual = 0;
  for (const char ch : line) {
    if (ch == ' ') {
      ++visual;
    } else if (ch == '\t') {
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
      "   ",
      "\t",
      "  \t  \t  mixed",
      std::string(300, ' ') + "past one chunk",
      std::string(300, '\t') + "tabs past one chunk",
      std::string(100, ' ') + std::string(100, '\t') + std::string(100, ' ') + "three runs",
      std::string(600, ' '),
      "\t" + std::string(255, ' ') + "\tboundary tab",
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
  Expect(multi_chunk_lines > 100,
         "the sweep barely crossed a chunk boundary: " + std::to_string(multi_chunk_lines));
}

}

void RegisterLineIndentScanTests(std::vector<TestCase>& tests) {
  AddTest(tests, "LineIndentScan/ChunkedScanMatchesASinglePass",
          TestChunkedIndentScanMatchesASinglePass);
}

}
