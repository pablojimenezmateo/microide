#pragma once

// Hover-card text wrapping, out of the render TU so it is unit-testable and the
// render path keeps its no-string-materialization lint.
//
// Every hover card -- an LSP/plugin hover, a diagnostic, a debug value, a blame
// summary, signature help -- wraps its text to the card width and a line cap.
// The wrap used to flatten ALL whitespace first, newlines included, so a server's
// hover ("field queue_ / provided by ... / Type: std::vector<Task>") and a
// multi-line compiler diagnostic were run together into one paragraph. Lines are
// kept now: each '\n' starts a new line, a blank line separates paragraphs (runs
// of them collapse to one), and runs of spaces inside a line collapse to one.

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace microide::workspace {

struct HoverTextMeasure {
  // Width of `text` as drawn.
  std::function<float(std::string_view text)> width;
  // The longest prefix of `text` that fits `max_width`, ellipsized if cut.
  std::function<std::string(std::string_view text, float max_width)> truncate;
};

// At most `max_lines` lines, none wider than `max_width`. When the text needs
// more, the last line carries the rest, truncated.
std::vector<std::string> WrapHoverText(std::string_view text, float max_width,
                                       std::size_t max_lines, const HoverTextMeasure& measure);

}  // namespace microide::workspace
