#include "editor/LineIndentScan.h"

#include <algorithm>
#include <string>

#include "editor/TextLayout.h"
#include "util/StringUtil.h"

namespace microide::editor {

bool AdvanceLeadingIndentOverChunk(std::string_view chunk, std::size_t tab_size,
                                   std::size_t& visual) {
  // Count the leading spaces a word at a time. Each space used to contribute one
  // loop iteration and one branch; on deeply nested code that was the whole cost
  // of the scan (the 50k-line fixture averages 131 leading whitespace bytes per
  // line).
  const std::size_t spaces = util::LeadingByteRun(chunk, ' ');
  visual += spaces;
  if (spaces == chunk.size()) {
    return false;  // whitespace all the way to the end of the chunk
  }
  if (chunk[spaces] != '\t') {
    return true;  // ordinary space indent, which is almost every line
  }
  // A tab appears: fall back to the exact per-character rule from that point,
  // since a tab advances to the next stop rather than by one column.
  for (std::size_t i = spaces; i < chunk.size(); ++i) {
    const char c = chunk[i];
    if (c != ' ' && c != '\t') {
      return true;
    }
    visual = TextLayout::AdvanceVisualColumn(visual, c, tab_size);
  }
  return false;
}

std::size_t MeasureLeadingIndent(LineSpan lines, std::size_t index, std::size_t tab_size,
                                 bool* found_content) {
  std::size_t visual = 0;
  std::size_t offset = 0;
  std::string scratch;
  while (true) {
    const std::string_view chunk =
        lines.LineWindow(index, offset, kIndentScanChunkBytes, scratch);
    if (chunk.empty()) {
      if (found_content != nullptr) {
        *found_content = false;
      }
      return visual;
    }
    if (AdvanceLeadingIndentOverChunk(chunk, tab_size, visual)) {
      if (found_content != nullptr) {
        *found_content = true;
      }
      return visual;
    }
    offset += chunk.size();
  }
}

}  // namespace microide::editor
