#include "TestSupport.h"

#include "editor/PluginDecorationStore.h"

#include <cstdint>
#include <filesystem>
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

}  // namespace

void RegisterPluginDecorationStoreTests(std::vector<TestCase>& tests) {
  AddTest(tests, "PluginDecorationStore/MergesOwnersAndSortsByLine",
          TestMergesOwnersAndSortsByLine);
  AddTest(tests, "PluginDecorationStore/PerLineSliceAccessors", TestPerLineSliceAccessors);
  AddTest(tests, "PluginDecorationStore/GutterMarksSortByPriority",
          TestGutterMarksSortByPriorityWithinLine);
  AddTest(tests, "PluginDecorationStore/ReplaceIsIdempotentAndClears",
          TestReplaceIsIdempotentAndClears);
  AddTest(tests, "PluginDecorationStore/ClearOwnerAndPathPrefix", TestClearOwnerAndPathPrefix);
}

}  // namespace microide::tests
