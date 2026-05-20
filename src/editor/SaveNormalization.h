#pragma once

#include <string>
#include <vector>

namespace microide::editor {

// Strip trailing spaces and tabs from every line in `lines`. Returns true when
// any line was modified.
bool TrimTrailingWhitespace(std::vector<std::string>& lines);

// Ensure exactly one final blank line at the end of the buffer. Idempotent.
// In the line-based representation used by `TextViewport`, "ends with a newline"
// is encoded as an empty trailing element after the last content line. Returns
// true when the buffer was modified.
bool EnsureSingleFinalNewline(std::vector<std::string>& lines);

}  // namespace microide::editor
