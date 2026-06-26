#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace microide::editor {

struct SurfaceContent;        // editor/PluginSurfaceStore.h (inline surface insets)
struct CodeLensDecoration;    // editor/PluginDecorationStore.h (above-line code lenses)
struct GhostTextInset;        // editor/EditorViewModel.h (below-caret ghost-text rows)

// Where an inert vertical gap sits relative to its anchor visual row.
//  - Below: hosts an inline surface inset directly under the row (Phase E1).
//  - Above: hosts an above-line code-lens strip directly over the row (Phase E2);
//    the anchor row (and everything below it) is pushed down by the gap height.
enum class RowGapPlacement : std::uint8_t { Below, Above };

// One inert vertical gap attached to a visible visual row, hosting an inline
// inset without making text rows variable-height. Gaps are inert: the caret
// skips them and a selection cannot enter them.
struct RowGap {
  std::uint32_t visual_row = 0;  // the gap's anchor row (drawn directly below/above it)
  float height = 0.0f;
  RowGapPlacement placement = RowGapPlacement::Below;
};

// The content a gap hosts, parallel (same index) to the gap list. Holds only
// store-owned pointers so the render pass slices text/geometry without copying.
struct RowGapContent {
  const SurfaceContent* surface = nullptr;        // set for Below surface insets
  const CodeLensDecoration* code_lens = nullptr;  // set for Above code-lens strips
  const GhostTextInset* ghost_text = nullptr;     // set for the Below ghost-text block
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
  // the heights of all gaps that sit above its text band within the window. A
  // Below gap counts for rows strictly past its anchor; an Above gap counts for
  // its own anchor row and everything past it. The gap list is tiny (visible
  // insets only), so a full scan is cheaper than maintaining sort invariants.
  float RowTop(std::size_t row) const {
    float y = first_line_y_ + static_cast<float>(row) * line_height_;
    if (gaps_.empty()) {
      return y;
    }
    const std::uint32_t target = scroll_line_ + static_cast<std::uint32_t>(row);
    for (const RowGap& gap : gaps_) {
      const bool counts = gap.placement == RowGapPlacement::Above ? gap.visual_row <= target
                                                                  : gap.visual_row < target;
      if (counts) {
        y += gap.height;
      }
    }
    return y;
  }

  // Height of the Below gap directly under the visible row at offset `row`, or 0
  // when there is none. The gap's top y is `RowTop(row) + line_height`.
  float GapHeightBelow(std::size_t row) const {
    const std::uint32_t target = scroll_line_ + static_cast<std::uint32_t>(row);
    for (const RowGap& gap : gaps_) {
      if (gap.placement == RowGapPlacement::Below && gap.visual_row == target) {
        return gap.height;
      }
    }
    return 0.0f;
  }

  // Height of the Above gap directly over the visible row at offset `row`, or 0
  // when there is none. The gap occupies `[RowTop(row) - height, RowTop(row))`.
  float GapAbove(std::size_t row) const {
    const std::uint32_t target = scroll_line_ + static_cast<std::uint32_t>(row);
    for (const RowGap& gap : gaps_) {
      if (gap.placement == RowGapPlacement::Above && gap.visual_row == target) {
        return gap.height;
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
