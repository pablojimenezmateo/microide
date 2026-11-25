#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace microide::editor {

struct SurfaceContent;
struct CodeLensDecoration;
struct GhostTextInset;

enum class RowGapPlacement : std::uint8_t { Below, Above };

struct RowGap {
  std::uint32_t visual_row = 0;
  float height = 0.0f;
  RowGapPlacement placement = RowGapPlacement::Below;
};

struct RowGapContent {
  const SurfaceContent* surface = nullptr;
  const CodeLensDecoration* code_lens = nullptr;
  const GhostTextInset* ghost_text = nullptr;
};

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

  float GapHeightBelow(std::size_t row) const {
    const std::uint32_t target = scroll_line_ + static_cast<std::uint32_t>(row);
    for (const RowGap& gap : gaps_) {
      if (gap.placement == RowGapPlacement::Below && gap.visual_row == target) {
        return gap.height;
      }
    }
    return 0.0f;
  }

  float GapAbove(std::size_t row) const {
    const std::uint32_t target = scroll_line_ + static_cast<std::uint32_t>(row);
    for (const RowGap& gap : gaps_) {
      if (gap.placement == RowGapPlacement::Above && gap.visual_row == target) {
        return gap.height;
      }
    }
    return 0.0f;
  }

  struct HitResult {
    std::size_t row = 0;
    bool in_gap = false;
  };
  HitResult HitTest(float y, std::size_t visible_rows) const;

 private:
  float first_line_y_;
  float line_height_;
  std::uint32_t scroll_line_;
  std::span<const RowGap> gaps_;
};

}
