#include "TestSupport.h"

#include "editor/GutterIconRegistry.h"
#include "editor/PluginDecorationStore.h"
#include "support/SoftwareCanvas.h"

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace microide::tests {
namespace {

using microide::editor::CodeLensDecoration;
using microide::editor::FileDecorations;
using microide::editor::GutterIconShape;
using microide::editor::GutterMarkDecoration;
using microide::editor::InlineTextDecoration;
using microide::editor::kDecorationUnderline;
using microide::editor::PluginDecorationData;
using microide::editor::PluginDecorationStore;
using microide::editor::TextStyleDecoration;

TextStyleDecoration Style(std::uint32_t line, std::uint32_t start, std::uint32_t end) {
  return TextStyleDecoration{
      .line = line,
      .start_column = start,
      .end_column = end,
      .foreground = SDL_Color{10, 20, 30, 255},
      .flags = kDecorationUnderline,
  };
}

GutterMarkDecoration Mark(std::uint32_t line, GutterIconShape shape, std::uint8_t priority) {
  return GutterMarkDecoration{
      .line = line, .shape = shape, .color = SDL_Color{1, 2, 3, 255}, .priority = priority};
}

void TestMergesOwnersAndSortsByLine() {
  PluginDecorationStore store;
  const std::filesystem::path path = "/tmp/project/main.cpp";

  PluginDecorationData a;
  a.text_styles = {Style(5, 0, 3), Style(2, 1, 4)};
  PluginDecorationData b;
  b.text_styles = {Style(3, 0, 2)};
  Expect(store.ReplaceForOwnerFile("rainbow", path, std::move(a)),
         "first owner publish should change the store");
  Expect(store.ReplaceForOwnerFile("csv", path, std::move(b)),
         "second owner publish should change the store");

  const FileDecorations* file = store.FindByPath(path);
  Expect(file != nullptr, "merged file decorations should be found");
  Expect(file->text_styles.size() == 3, "all three text styles should merge");
  Expect(file->text_styles[0].line == 2 && file->text_styles[1].line == 3 &&
             file->text_styles[2].line == 5,
         "merged text styles should be sorted by line");
}

void TestPerLineSliceAccessors() {
  PluginDecorationStore store;
  const std::filesystem::path path = "/tmp/p/a.cpp";
  PluginDecorationData data;
  data.text_styles = {Style(4, 0, 1), Style(4, 2, 3), Style(9, 0, 1)};
  store.ReplaceForOwnerFile("owner", path, std::move(data));

  const FileDecorations* file = store.FindByPath(path);
  Expect(file != nullptr, "file should be present");
  const auto line4 = file->TextStylesForLine(4);
  Expect(line4.size() == 2, "line 4 should slice both of its styles");
  Expect(line4[0].start_column == 0 && line4[1].start_column == 2,
         "line-4 slice should be ordered by start column");
  Expect(file->TextStylesForLine(9).size() == 1, "line 9 should slice its single style");
  Expect(file->TextStylesForLine(0).empty(), "an empty line should slice to nothing");
  Expect(file->TextStylesForLine(100).empty(), "a past-the-end line should slice to nothing");
}

void TestGutterMarksSortByPriorityWithinLine() {
  PluginDecorationStore store;
  const std::filesystem::path path = "/tmp/p/b.cpp";
  PluginDecorationData data;
  data.gutter_marks = {Mark(7, GutterIconShape::Dot, 1), Mark(7, GutterIconShape::Bookmark, 9),
                       Mark(7, GutterIconShape::Dash, 5)};
  store.ReplaceForOwnerFile("owner", path, std::move(data));

  const FileDecorations* file = store.FindByPath(path);
  Expect(file != nullptr, "file should be present");
  const auto marks = file->GutterMarksForLine(7);
  Expect(marks.size() == 3, "all three gutter marks should be present on the line");
  Expect(marks.front().shape == GutterIconShape::Bookmark,
         "highest-priority mark should sort first so it wins the gutter slot");
}

void TestReplaceIsIdempotentAndClears() {
  PluginDecorationStore store;
  const std::filesystem::path path = "/tmp/p/c.cpp";
  PluginDecorationData data;
  data.text_styles = {Style(1, 0, 2)};
  Expect(store.ReplaceForOwnerFile("owner", path, data), "initial publish should change state");
  // The bool return is the redraw signal: a no-op republish returns false so the
  // host skips a needless repaint, while a real change returns true.
  Expect(!store.ReplaceForOwnerFile("owner", path, data),
         "republishing identical decorations should be a no-op");

  Expect(store.ReplaceForOwnerFile("owner", path, PluginDecorationData{}),
         "publishing an empty set should clear the owner's file and report a change");
  Expect(store.FindByPath(path) == nullptr, "cleared file should no longer be found");
}

void TestClearOwnerAndPathPrefix() {
  PluginDecorationStore store;
  const std::filesystem::path a = "/tmp/proj/src/a.cpp";
  const std::filesystem::path b = "/tmp/proj/src/b.cpp";
  PluginDecorationData da;
  da.inline_texts = {InlineTextDecoration{.line = 0, .text = "blame"}};
  PluginDecorationData db;
  db.code_lenses = {CodeLensDecoration{.line = 0, .text = "2 refs", .command = "refs.show"}};
  store.ReplaceForOwnerFile("git", a, std::move(da));
  store.ReplaceForOwnerFile("lsp", b, std::move(db));

  Expect(store.RetargetPathPrefix("/tmp/proj", "/tmp/moved"),
         "retargeting a covering prefix should change state");
  Expect(store.FindByPath("/tmp/moved/src/a.cpp") != nullptr,
         "decorations should follow a path-prefix retarget");
  Expect(store.FindByPath(a) == nullptr, "the old path should no longer resolve after retarget");

  Expect(store.ClearOwner("git"), "clearing an owner should change state");
  Expect(store.FindByPath("/tmp/moved/src/a.cpp") == nullptr,
         "the git owner's file should clear");
  Expect(store.FindByPath("/tmp/moved/src/b.cpp") != nullptr,
         "an untouched owner's file should remain");

  Expect(store.ClearPathPrefix("/tmp/moved"), "clearing a covering prefix should change state");
  Expect(store.empty(), "store should be empty after clearing the whole prefix");
}

void TestAggregatePerFileCapTruncatesMergedKinds() {
  PluginDecorationStore store;
  const std::filesystem::path path = "/tmp/p/huge.cpp";

  // Three owners each contribute a large, distinct line range. The per-owner publish
  // cap does not bound the multi-owner sum (270k here), so the store's aggregate
  // per-file cap (200k) must truncate the merged view to stay render-bounded.
  constexpr std::uint32_t kPerOwner = 90000;
  for (int owner = 0; owner < 3; ++owner) {
    PluginDecorationData data;
    data.text_styles.reserve(kPerOwner);
    const std::uint32_t base = static_cast<std::uint32_t>(owner) * kPerOwner;
    for (std::uint32_t i = 0; i < kPerOwner; ++i) {
      data.text_styles.push_back(Style(base + i, 0, 1));
    }
    store.ReplaceForOwnerFile("owner" + std::to_string(owner), path, std::move(data));
  }

  const FileDecorations* file = store.FindByPath(path);
  Expect(file != nullptr, "the heavily-decorated file should be present");
  Expect(file->text_styles.size() == 200000,
         "the merged per-file text styles should be capped at the aggregate limit");
  // The merge sorts by line before truncating, so the retained set is the lowest
  // 200k lines (0 .. 199999) -- a deterministic head, never a wrapped/undefined tail.
  Expect(file->text_styles.front().line == 0 && file->text_styles.back().line == 199999,
         "the aggregate cap should keep the lowest-line decorations after sorting");
  // TD-2026-07-17-090: the cap must bound WORK, not just the retained result. The
  // bounded k-way merge reserves at most the cap and stops there, so the retained
  // vector never over-allocates to the full 270k concatenation. (The old
  // concatenate/sort/resize path left capacity at the full contributed total.)
  Expect(file->text_styles.capacity() <= 200000,
         "the merge must not allocate beyond the aggregate cap (peak work bounded)");
}

// A multi-kind, multi-owner merge: every kind must independently honor the cap and
// its peak allocation, and the retained set must stay in each kind's render order.
void TestAggregateCapBoundsEveryKind() {
  PluginDecorationStore store;
  const std::filesystem::path path = "/tmp/p/multi-kind.cpp";
  constexpr std::uint32_t kPerOwner = 90000;
  constexpr std::size_t kCap = 200000;
  for (int owner = 0; owner < 3; ++owner) {
    PluginDecorationData data;
    const std::uint32_t base = static_cast<std::uint32_t>(owner) * kPerOwner;
    for (std::uint32_t i = 0; i < kPerOwner; ++i) {
      const std::uint32_t line = base + i;
      data.text_styles.push_back(Style(line, 0, 1));
      data.gutter_marks.push_back(Mark(line, GutterIconShape::Dot, 0));
      data.inline_texts.push_back(InlineTextDecoration{.line = line, .text = "x"});
      data.code_lenses.push_back(
          CodeLensDecoration{.line = line, .text = "x", .command = "c"});
    }
    store.ReplaceForOwnerFile("owner" + std::to_string(owner), path, std::move(data));
  }

  const FileDecorations* file = store.FindByPath(path);
  Expect(file != nullptr, "the multi-kind file should be present");
  Expect(file->text_styles.size() == kCap && file->gutter_marks.size() == kCap &&
             file->inline_texts.size() == kCap && file->code_lenses.size() == kCap,
         "every decoration kind should be independently capped at the aggregate limit");
  Expect(file->text_styles.capacity() <= kCap && file->gutter_marks.capacity() <= kCap &&
             file->inline_texts.capacity() <= kCap && file->code_lenses.capacity() <= kCap,
         "every kind's peak allocation should be bounded by the cap");
  Expect(file->code_lenses.front().line == 0 && file->code_lenses.back().line == 199999,
         "the retained set should be the lowest-line decorations in render order");
}

// The bookmark icon is a pennant: a full-width body whose bottom has a V-notch
// that opens from nothing at the notch's top to its widest at the bottom edge.
// The notch was drawn inverted -- no hole at the bottom edge and a full-width one
// at the notch's top -- so a blank row cut the body off from a tent-shaped base.
void TestBookmarkGutterIconIsAPennant() {
  SoftwareCanvas canvas(48, 48);
  SDL_SetRenderDrawColor(canvas.renderer(), 0, 0, 0, 255);
  SDL_RenderClear(canvas.renderer());
  constexpr float kLineHeight = 22.0f;
  editor::GutterIconRegistry::Draw(canvas.renderer(), GutterIconShape::Bookmark,
                                   SDL_Color{255, 255, 255, 255}, 4.0f, 4.0f, 40.0f, kLineHeight);
  const SDL_FRect b = editor::GutterIconRegistry::MarkerRect(4.0f, 4.0f, 40.0f, kLineHeight);
  const auto lit = [&](int x, int y) { return canvas.PixelAt(x, y).r > 127; };
  const int center = static_cast<int>(b.x + b.w * 0.5f);
  const int top = static_cast<int>(b.y);
  const int bottom = static_cast<int>(std::ceil(b.y + b.h)) - 1;
  const int x0 = static_cast<int>(b.x);
  const int x1 = static_cast<int>(std::ceil(b.x + b.w)) - 1;
  const auto row_ink = [&](int y) {
    int count = 0;
    for (int x = x0; x <= x1; ++x) count += lit(x, y) ? 1 : 0;
    return count;
  };
  int last_inked = top;
  for (int y = top; y <= bottom; ++y) {
    if (row_ink(y) > 0) last_inked = y;
  }
  Expect(last_inked > top + static_cast<int>(b.h * 0.6f), "the pennant reaches into its notch");
  // No blank row inside the icon: the inverted notch emptied the row at its top.
  for (int y = top; y <= last_inked; ++y) {
    Expect(row_ink(y) > 0, "row " + std::to_string(y - top) + " of the pennant has ink");
  }
  Expect(lit(center, top) && lit(center, top + static_cast<int>(b.h * 0.5f)), "the body is solid down its middle");
  Expect(!lit(center, last_inked), "the notch is open at the bottom");
  Expect(lit(x0, last_inked) || lit(x0 + 1, last_inked), "the left point reaches the bottom");
  Expect(lit(x1, last_inked) || lit(x1 - 1, last_inked), "the right point reaches the bottom");
}


}  // namespace


void RegisterPluginDecorationStoreTests(std::vector<TestCase>& tests) {
  AddTest(tests, "GutterIcon/BookmarkIsAPennant", TestBookmarkGutterIconIsAPennant);
  AddTest(tests, "PluginDecorationStore/MergesOwnersAndSortsByLine",
          TestMergesOwnersAndSortsByLine);
  AddTest(tests, "PluginDecorationStore/PerLineSliceAccessors", TestPerLineSliceAccessors);
  AddTest(tests, "PluginDecorationStore/GutterMarksSortByPriority",
          TestGutterMarksSortByPriorityWithinLine);
  AddTest(tests, "PluginDecorationStore/ReplaceIsIdempotentAndClears",
          TestReplaceIsIdempotentAndClears);
  AddTest(tests, "PluginDecorationStore/ClearOwnerAndPathPrefix", TestClearOwnerAndPathPrefix);
  AddTest(tests, "PluginDecorationStore/AggregatePerFileCapTruncatesMergedKinds",
          TestAggregatePerFileCapTruncatesMergedKinds);
  AddTest(tests, "PluginDecorationStore/AggregateCapBoundsEveryKind",
          TestAggregateCapBoundsEveryKind);
}

}  // namespace microide::tests
