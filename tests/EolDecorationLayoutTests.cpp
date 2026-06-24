#include "TestSupport.h"

#include "editor/EolDecorationLayout.h"
#include "render/TextRenderer.h"

#include <SDL3/SDL.h>

#include <cmath>
#include <cstdint>
#include <vector>

namespace microide::tests {
namespace {

using microide::editor::BuildEolDecorationSegments;
using microide::editor::CodeLensDecoration;
using microide::editor::EolDecorationSegment;
using microide::editor::InlineTextDecoration;
using microide::editor::kInlineTextEndOfLine;

// A backend-less TextRenderer measures every glyph as 8px and reports an 8px
// cell, so the geometry below is exact and font-independent. The helper inserts
// an 8-cell gap (64px) before the first segment and a 2-cell gap (16px) between
// segments — those constants mirror EolDecorationLayout.cpp.
constexpr float kCharWidth = 8.0f;
constexpr float kInitialGap = 8.0f * kCharWidth;  // 64px
constexpr float kInterGap = 2.0f * kCharWidth;    // 16px
constexpr float kAnchorX = 100.0f;
constexpr float kRowY = 32.0f;
constexpr float kLineHeight = 16.0f;
constexpr float kWideRightLimit = 100000.0f;

bool NearlyEqual(float a, float b) { return std::fabs(a - b) < 0.01f; }

InlineTextDecoration EolText(std::string text) {
  InlineTextDecoration inl;
  inl.anchor_column = kInlineTextEndOfLine;
  inl.text = std::move(text);
  return inl;
}

CodeLensDecoration Lens(std::string text, std::string command) {
  CodeLensDecoration lens;
  lens.text = std::move(text);
  lens.command = std::move(command);
  return lens;
}

void TestEmptyInputProducesNoSegments() {
  render::TextRenderer text_renderer;
  std::vector<EolDecorationSegment> out;
  out.push_back(EolDecorationSegment{});  // prove the helper clears stale scratch
  BuildEolDecorationSegments(text_renderer, {}, {}, kAnchorX, kRowY, kLineHeight, kWideRightLimit,
                             out);
  Expect(out.empty(), "no decorations should yield no segments and clear the scratch buffer");
}

void TestCodeLensesPrecedeInlineTextLeftToRight() {
  render::TextRenderer text_renderer;
  const std::vector<CodeLensDecoration> lenses{Lens("Run", "run.cmd")};        // 3 glyphs => 24px
  const std::vector<InlineTextDecoration> texts{EolText("blame")};             // 5 glyphs => 40px

  std::vector<EolDecorationSegment> out;
  BuildEolDecorationSegments(text_renderer, texts, lenses, kAnchorX, kRowY, kLineHeight,
                             kWideRightLimit, out);

  Expect(out.size() == 2, "one lens and one inline text should produce two segments");

  // Code lens first, anchored one initial gap past the line's last glyph.
  Expect(out[0].kind == EolDecorationSegment::Kind::CodeLens, "the code lens lays out first");
  Expect(out[0].index == 0, "the lens segment references code_lenses[0]");
  Expect(NearlyEqual(out[0].rect.x, kAnchorX + kInitialGap), "lens starts at anchor + initial gap");
  Expect(NearlyEqual(out[0].rect.w, 24.0f), "lens width is its 3 glyphs at 8px");
  Expect(NearlyEqual(out[0].rect.y, kRowY), "segment y is the row y");
  Expect(NearlyEqual(out[0].rect.h, kLineHeight), "segment height is the line height");

  // Inline text follows the lens after one inter-segment gap.
  const float expected_text_x = kAnchorX + kInitialGap + 24.0f + kInterGap;
  Expect(out[1].kind == EolDecorationSegment::Kind::InlineText, "the inline text lays out second");
  Expect(out[1].index == 0, "the inline-text segment references inline_texts[0]");
  Expect(NearlyEqual(out[1].rect.x, expected_text_x), "inline text follows lens + inter-segment gap");
  Expect(NearlyEqual(out[1].rect.w, 40.0f), "inline text width is its 5 glyphs at 8px");
}

void TestMidLineInlineTextIsExcluded() {
  render::TextRenderer text_renderer;
  InlineTextDecoration mid;
  mid.anchor_column = 4;  // not end-of-line
  mid.text = "hint";
  const std::vector<InlineTextDecoration> texts{mid, EolText("eol")};

  std::vector<EolDecorationSegment> out;
  BuildEolDecorationSegments(text_renderer, texts, {}, kAnchorX, kRowY, kLineHeight, kWideRightLimit,
                             out);

  Expect(out.size() == 1, "only the end-of-line inline text is laid out (mid-line is deferred)");
  Expect(out[0].kind == EolDecorationSegment::Kind::InlineText, "the surviving segment is inline text");
  Expect(out[0].index == 1, "the segment indexes the end-of-line entry, skipping the mid-line one");
}

void TestRightLimitDropsOverflowAndEverythingAfter() {
  render::TextRenderer text_renderer;
  // Three lenses of 3 glyphs (24px) each. Budget the limit so the first fits but
  // the second overflows; the third must not sneak in even though it is narrow.
  const std::vector<CodeLensDecoration> lenses{Lens("aaa", "a"), Lens("bbb", "b"), Lens("ccc", "c")};
  const float first_x = kAnchorX + kInitialGap;
  const float right_limit = first_x + 24.0f + 4.0f;  // room for the first lens only

  std::vector<EolDecorationSegment> out;
  BuildEolDecorationSegments(text_renderer, {}, lenses, kAnchorX, kRowY, kLineHeight, right_limit,
                             out);

  Expect(out.size() == 1, "a segment that overflows the right limit drops it and all after it");
  Expect(out[0].index == 0, "the one fitting segment is the first lens");
}

void TestEmptyTextSkippedWithoutConsumingBudget() {
  render::TextRenderer text_renderer;
  // An empty-text lens between two real ones must not advance the cursor or
  // occupy a slot, so the following lens keeps its natural position.
  const std::vector<CodeLensDecoration> lenses{Lens("Run", "run"), Lens("", "noop"),
                                               Lens("Debug", "debug")};

  std::vector<EolDecorationSegment> out;
  BuildEolDecorationSegments(text_renderer, {}, lenses, kAnchorX, kRowY, kLineHeight, kWideRightLimit,
                             out);

  Expect(out.size() == 2, "the empty-text lens is skipped, leaving two segments");
  Expect(out[0].index == 0 && out[1].index == 2, "indices skip the empty lens at position 1");
  const float expected_second_x = kAnchorX + kInitialGap + 24.0f + kInterGap;
  Expect(NearlyEqual(out[1].rect.x, expected_second_x),
         "the empty lens consumed no horizontal budget");
}

}  // namespace

void RegisterEolDecorationLayoutTests(std::vector<TestCase>& tests) {
  AddTest(tests, "EolDecorationLayout/EmptyInputProducesNoSegments",
          TestEmptyInputProducesNoSegments);
  AddTest(tests, "EolDecorationLayout/CodeLensesPrecedeInlineTextLeftToRight",
          TestCodeLensesPrecedeInlineTextLeftToRight);
  AddTest(tests, "EolDecorationLayout/MidLineInlineTextIsExcluded",
          TestMidLineInlineTextIsExcluded);
  AddTest(tests, "EolDecorationLayout/RightLimitDropsOverflowAndEverythingAfter",
          TestRightLimitDropsOverflowAndEverythingAfter);
  AddTest(tests, "EolDecorationLayout/EmptyTextSkippedWithoutConsumingBudget",
          TestEmptyTextSkippedWithoutConsumingBudget);
}

}  // namespace microide::tests
