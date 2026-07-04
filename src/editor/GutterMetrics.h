#pragma once

namespace microide::editor {

// Shared horizontal layout for the editor gutter, so the line-number text and the
// overlaid markers (diagnostic bar, breakpoint dot, execution arrow) live in
// distinct columns instead of drawing on top of each other.
//
// Layout, left to right from the gutter's left edge (gutter_x):
//   [diagnostic bar][breakpoint dot / execution arrow][gap][line numbers ...][gap][fold button]
//
// The marker column reserves room for the largest marker footprint
// (kGutterMarkerInset + kGutterMarkerMaxExtent); line numbers begin only after it,
// at kGutterLineNumberInset.

// Thin severity bar hugging the very left edge.
inline constexpr float kGutterDiagnosticInset = 2.0f;

// Left inset of the breakpoint dot / execution arrow.
inline constexpr float kGutterMarkerInset = 4.0f;

// Largest marker diameter (breakpoint dot / execution arrow side), clamped in the
// marker rect helpers. The reserved marker strip spans
// [gutter_x + kGutterMarkerInset, gutter_x + kGutterMarkerInset + kGutterMarkerMaxExtent].
inline constexpr float kGutterMarkerMaxExtent = 12.0f;

// Gap between the marker strip and the start of the line-number digits.
inline constexpr float kGutterLineNumberGap = 6.0f;

// Left inset where line-number digits begin — clear of the marker strip.
inline constexpr float kGutterLineNumberInset =
    kGutterMarkerInset + kGutterMarkerMaxExtent + kGutterLineNumberGap;

// Gap between the widest line-number digits and the fold button column.
inline constexpr float kGutterFoldGap = 6.0f;

// Fold button geometry. The hit zone is intentionally wider than the painted
// button; its extra slop extends left into kGutterFoldGap instead of forcing the
// visible button away from the line-number digits.
inline constexpr float kGutterFoldMarkerSize = 8.0f;
inline constexpr float kGutterFoldRightPad = 6.0f;
inline constexpr float kGutterFoldHitWidth = 18.0f;
inline constexpr float kGutterFoldColumnWidth = kGutterFoldMarkerSize + kGutterFoldRightPad;

inline constexpr float kGutterFoldColumnLeft(float gutter_x, float gutter_width) {
  return gutter_x + gutter_width - kGutterFoldColumnWidth;
}

inline constexpr float kGutterFoldHitLeft(float gutter_x, float gutter_width) {
  return gutter_x + gutter_width - kGutterFoldHitWidth;
}

}  // namespace microide::editor
