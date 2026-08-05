#pragma once

#include <cstddef>
#include <string_view>

#include "editor/LineSpan.h"

namespace microide::editor {

// Leading-indent measurement, read a bounded chunk at a time.
//
// "How wide is this line's leading whitespace" is answered by its first few
// bytes, and three places asked for the whole line to get it: the fold indent
// source, the indent guides, and the caret's active-guide column. On a piece-tree
// source `lines[index]` materializes a copy of any line that spans pieces -- every
// line an in-line edit has touched -- so a file with no line breaks in it paid a
// multi-megabyte copy per frame for a question about its first byte
// (TD-2026-08-05-133).
//
// The three had grown their own byte-identical scans; they share this one now, so
// a fix or a threshold cannot land in one and miss the others.

// Bytes read at a time. Deep enough that no real indent needs a second chunk, and
// the loop extends rather than truncating, so this is a chunk size and not a cap.
inline constexpr std::size_t kIndentScanChunkBytes = 256;

// Extend an indent measurement over the next `chunk`, carrying the running visual
// column in `visual`. Returns true when the indent run ENDED inside this chunk
// (`visual` is then final); false when the chunk was entirely spaces and tabs and
// the caller must continue with the next one.
//
// `visual` is the global visual column, not a chunk-local one, so a tab's stop
// arithmetic is identical to a single pass over the whole line.
bool AdvanceLeadingIndentOverChunk(std::string_view chunk, std::size_t tab_size,
                                   std::size_t& visual);

// Visual width of line `index`'s leading spaces and tabs. `*found_content`, when
// given, reports whether a non-indent byte was reached -- false means the line is
// blank or whitespace-only, which the fold indent source treats as neither an
// opener nor a dedent.
std::size_t MeasureLeadingIndent(LineSpan lines, std::size_t index, std::size_t tab_size,
                                 bool* found_content = nullptr);

}  // namespace microide::editor
