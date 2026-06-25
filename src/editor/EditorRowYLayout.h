#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace microide::editor {

// One inert vertical gap inserted *below* a visible visual row. Used to host an
// inline block inset (a plugin surface) without making text rows variable-height.
// Gaps are inert: the caret skips them and a selection cannot enter them.
struct RowGap {
  std::uint32_t visual_row = 0;  // gap is drawn directly below this visual row
  float height = 0.0f;
};

// The single row -> screen-y mapping for the editor viewport. Render, caret,
// selection, and hit-testing all resolve geometry through this one helper so a
// gap never has to be reasoned about in more than one place.
//
// With an empty gap list every result is bit-identical to the legacy formula
// `first_line_y + row * line_height`, so enabling/disabling insets cannot perturb
// the common (no-inset) path.
class EditorRowYLayout {
 public:
  EditorRowYLayout(float first_line_y, float line_height, std::uint32_t scroll_line,
                   std::span<const RowGap> gaps_in_window)
      : first_line_y_(first_line_y),
        line_height_(line_height),
        scroll_line_(scroll_line),
        gaps_(gaps_in_window) {}

  EditorRowYLayout(float first_line_y, float line_height, std::uint32_t scroll_line)
      : EditorRowYLayout(first_line_y, line_height, scroll_line, {}) {}

  float line_height() const { return line_height_; }
  bool has_gaps() const { return !gaps_.empty(); }

  // Top y of the visible row at offset `row` (0 == first visible row), including
  // the heights of all gaps that fall above it within the window.
  float RowTop(std::size_t row) const {
    float y = first_line_y_ + static_cast<float>(row) * line_height_;
    if (gaps_.empty()) {
      return y;
    }
    const std::uint32_t target = scroll_line_ + static_cast<std::uint32_t>(row);
    for (const RowGap& gap : gaps_) {
      if (gap.visual_row < target) {
        y += gap.height;
      } else {
        break;  // gaps_ is sorted by visual_row
      }
    }
    return y;
  }

  // Height of the gap directly below the visible row at offset `row`, or 0 when
  // there is none. The gap's top y is `RowTop(row) + line_height`.
  float GapHeightBelow(std::size_t row) const {
    if (gaps_.empty()) {
      return 0.0f;
    }
    const std::uint32_t target = scroll_line_ + static_cast<std::uint32_t>(row);
    for (const RowGap& gap : gaps_) {
      if (gap.visual_row == target) {
        return gap.height;
      }
      if (gap.visual_row > target) {
        break;
      }
    }
    return 0.0f;
  }

  // The visible row offset whose row band (text + gap below it) contains screen y,
  // clamped to [0, visible_rows-1]. `in_gap` is set when y landed in the inert gap
  // rather than on the text row. Used by click hit-testing.
  struct HitResult {
    std::size_t row = 0;
    bool in_gap = false;
  };
  HitResult HitTest(float y, std::size_t visible_rows) const;

  // Total pixel height of `visible_rows` text rows plus every gap among them.
  float WindowHeight(std::size_t visible_rows) const;

 private:
  float first_line_y_;
  float line_height_;
  std::uint32_t scroll_line_;
  std::span<const RowGap> gaps_;
};

}  // namespace microide::editor
