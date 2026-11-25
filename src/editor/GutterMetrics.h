#pragma once

namespace microide::editor {

inline constexpr float kGutterDiagnosticInset = 2.0f;

inline constexpr float kGutterMarkerInset = 4.0f;

inline constexpr float kGutterMarkerMaxExtent = 12.0f;

inline constexpr float kGutterLineNumberGap = 6.0f;

inline constexpr float kGutterLineNumberInset =
    kGutterMarkerInset + kGutterMarkerMaxExtent + kGutterLineNumberGap;

inline constexpr float kGutterFoldGap = 6.0f;

inline constexpr float kGutterFoldMarkerSize = 8.0f;
inline constexpr float kGutterFoldRightPad = 6.0f;
inline constexpr float kGutterFoldHitWidth = 18.0f;
inline constexpr float kGutterFoldColumnWidth = kGutterFoldMarkerSize + kGutterFoldRightPad;

inline constexpr float kGutterFoldHitLeft(float gutter_x, float gutter_width) {
  return gutter_x + gutter_width - kGutterFoldHitWidth;
}

}
