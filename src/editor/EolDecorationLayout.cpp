#include "editor/EolDecorationLayout.h"

#include <algorithm>

namespace microide::editor {
namespace {

// Gap before the first segment and between consecutive segments, in cell widths.
// Matches the inline-blame spacing convention so plugin EOL text sits the same
// distance past code as the built-in git-blame overlay.
constexpr float kEolGapColumns = 8.0f;
constexpr float kEolInterSegmentGapColumns = 2.0f;

// Append one segment for `text` at the running x cursor, advancing it past the
// drawn width plus an inter-segment gap. Returns false (and appends nothing) when
// the segment would overflow `right_limit`, signaling the caller to stop.
bool AppendSegment(const render::TextRenderer& text_renderer,
                   EolDecorationSegment::Kind kind,
                   std::uint32_t index,
                   std::string_view text,
                   float y,
                   float line_height,
                   float right_limit,
                   float inter_gap,
                   float* x_cursor,
                   std::vector<EolDecorationSegment>& out) {
  if (text.empty()) {
    return true;  // skip empty text without consuming horizontal budget
  }
  const float width = text_renderer.MeasureWidth(text);
  if (width <= 0.0f) {
    return true;
  }
  if (*x_cursor + width > right_limit) {
    return false;
  }
  out.push_back(EolDecorationSegment{
      .kind = kind,
      .index = index,
      .rect = SDL_FRect{*x_cursor, y, width, line_height},
  });
  *x_cursor += width + inter_gap;
  return true;
}

}  // namespace

void BuildEolDecorationSegments(const render::TextRenderer& text_renderer,
                                std::span<const InlineTextDecoration> inline_texts,
                                std::span<const CodeLensDecoration> code_lenses,
                                float anchor_x,
                                float y,
                                float line_height,
                                float right_limit,
                                std::vector<EolDecorationSegment>& out) {
  out.clear();
  if (inline_texts.empty() && code_lenses.empty()) {
    return;
  }
  const float char_width = std::max(1.0f, text_renderer.CharWidth());
  const float inter_gap = kEolInterSegmentGapColumns * char_width;
  float x_cursor = anchor_x + kEolGapColumns * char_width;

  for (std::size_t i = 0; i < code_lenses.size(); ++i) {
    if (!AppendSegment(text_renderer, EolDecorationSegment::Kind::CodeLens,
                       static_cast<std::uint32_t>(i), code_lenses[i].text, y, line_height,
                       right_limit, inter_gap, &x_cursor, out)) {
      return;
    }
  }
  for (std::size_t i = 0; i < inline_texts.size(); ++i) {
    if (inline_texts[i].anchor_column != kInlineTextEndOfLine) {
      continue;  // mid-line virtual text deferred (Phase B v1 is EOL-only)
    }
    if (!AppendSegment(text_renderer, EolDecorationSegment::Kind::InlineText,
                       static_cast<std::uint32_t>(i), inline_texts[i].text, y, line_height,
                       right_limit, inter_gap, &x_cursor, out)) {
      return;
    }
  }
}

}  // namespace microide::editor
