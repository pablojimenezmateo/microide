#include "TestSupport.h"

#include "render/PluginDisplayList.h"

#include <limits>
#include <string>

namespace microide::tests {
namespace {

using microide::render::ComputeDisplayListHash;
using microide::render::DisplayOp;
using microide::render::DrawOp;
using microide::render::PluginDisplayList;
using microide::render::ValidateDisplayList;

PluginDisplayList RectList() {
  PluginDisplayList list;
  list.ops.push_back(DisplayOp{.op = DrawOp::Rect,
                               .rect = SDL_FRect{0, 0, 10, 10},
                               .color = SDL_Color{1, 2, 3, 255}});
  list.content_width = 10;
  list.content_height = 10;
  return list;
}

void TestValidListPasses() {
  PluginDisplayList list = RectList();
  list.text_arena = "hello";
  list.ops.push_back(DisplayOp{.op = DrawOp::Text, .data_offset = 0, .data_count = 5});
  std::string error;
  Expect(ValidateDisplayList(list, &error), "a well-formed display list should validate");
  Expect(error.empty(), "no error on a valid list");
}

void TestTextOpOutOfBoundsFails() {
  PluginDisplayList list;
  list.text_arena = "abc";
  list.ops.push_back(DisplayOp{.op = DrawOp::Text, .data_offset = 1, .data_count = 5});
  std::string error;
  Expect(!ValidateDisplayList(list, &error), "a text op past the arena must be rejected");
  Expect(!error.empty(), "rejection should report a reason");
}

// An empty (count == 0) text op with an out-of-range offset must still be
// rejected: replay forms `text_arena.data() + data_offset`, and a past-the-end
// offset is UB (invalid pointer formation) even though the zero-length view is
// never dereferenced. Validation is the sole gate replay trusts.
void TestEmptyTextOpOutOfBoundsOffsetFails() {
  PluginDisplayList list;
  list.text_arena = "abc";
  list.ops.push_back(DisplayOp{.op = DrawOp::Text, .data_offset = 9999, .data_count = 0});
  std::string error;
  Expect(!ValidateDisplayList(list, &error),
         "an empty text op with an out-of-range offset must be rejected");
  Expect(!error.empty(), "rejection should report a reason");
}

// A zero-length text op whose offset is exactly at the arena end is legal (a
// one-past-the-end pointer with zero length is well-defined).
void TestEmptyTextOpAtArenaEndPasses() {
  PluginDisplayList list;
  list.content_width = 1;
  list.content_height = 1;
  list.text_arena = "abc";
  list.ops.push_back(DisplayOp{.op = DrawOp::Text, .data_offset = 3, .data_count = 0});
  std::string error;
  Expect(ValidateDisplayList(list, &error),
         "an empty text op at the arena end should validate");
}

void TestPolylineNeedsTwoPoints() {
  PluginDisplayList list;
  list.point_arena.push_back(SDL_FPoint{0, 0});
  list.ops.push_back(DisplayOp{.op = DrawOp::Polyline, .data_offset = 0, .data_count = 1});
  std::string error;
  Expect(!ValidateDisplayList(list, &error), "a one-point polyline must be rejected");
}

void TestPolylineOutOfBoundsFails() {
  PluginDisplayList list;
  list.point_arena.push_back(SDL_FPoint{0, 0});
  list.point_arena.push_back(SDL_FPoint{1, 1});
  list.ops.push_back(DisplayOp{.op = DrawOp::Polyline, .data_offset = 1, .data_count = 2});
  std::string error;
  Expect(!ValidateDisplayList(list, &error), "a polyline past the point arena must be rejected");
}

void TestImageOpOutOfBoundsFails() {
  PluginDisplayList list;
  list.ops.push_back(DisplayOp{.op = DrawOp::Image, .data_offset = 0});
  std::string error;
  Expect(!ValidateDisplayList(list, &error),
         "an image op with no registered handle must be rejected");
}

void TestUnbalancedClipFails() {
  PluginDisplayList push_only;
  push_only.ops.push_back(DisplayOp{.op = DrawOp::ClipPush, .rect = SDL_FRect{0, 0, 5, 5}});
  std::string error;
  Expect(!ValidateDisplayList(push_only, &error), "an unbalanced clip push must be rejected");

  PluginDisplayList pop_only;
  pop_only.ops.push_back(DisplayOp{.op = DrawOp::ClipPop});
  Expect(!ValidateDisplayList(pop_only, &error), "an unbalanced clip pop must be rejected");
}

void TestBalancedClipPasses() {
  PluginDisplayList list;
  list.ops.push_back(DisplayOp{.op = DrawOp::ClipPush, .rect = SDL_FRect{0, 0, 5, 5}});
  list.ops.push_back(DisplayOp{.op = DrawOp::Rect, .rect = SDL_FRect{0, 0, 2, 2}});
  list.ops.push_back(DisplayOp{.op = DrawOp::ClipPop});
  std::string error;
  Expect(ValidateDisplayList(list, &error), "a balanced clip pair should validate");
}

void TestHashIsStableAndSensitive() {
  const PluginDisplayList a = RectList();
  const PluginDisplayList b = RectList();
  Expect(ComputeDisplayListHash(a) == ComputeDisplayListHash(b),
         "identical lists hash identically");

  PluginDisplayList c = RectList();
  c.ops[0].color.r = 99;
  Expect(ComputeDisplayListHash(a) != ComputeDisplayListHash(c),
         "a color change should change the hash");

  PluginDisplayList d = RectList();
  d.content_height = 11;
  Expect(ComputeDisplayListHash(a) != ComputeDisplayListHash(d),
         "an intrinsic-size change should change the hash");
}

}  // namespace

// Plugin-supplied coordinates flow from Lua without a finiteness check, and replay
// casts them to int (UB on NaN/inf). ValidateDisplayList must reject non-finite
// geometry — the sole gate replay trusts.
void TestNonFiniteRectRejected() {
  PluginDisplayList list;
  list.ops.push_back(DisplayOp{.op = DrawOp::ClipPush,
                               .rect = SDL_FRect{0.0f, 0.0f,
                                                 std::numeric_limits<float>::quiet_NaN(), 10.0f}});
  list.ops.push_back(DisplayOp{.op = DrawOp::ClipPop});
  std::string error;
  Expect(!ValidateDisplayList(list, &error), "a non-finite rectangle must be rejected");
  Expect(!error.empty(), "rejection should report a reason");
}

void TestNonFinitePolylinePointRejected() {
  PluginDisplayList list;
  list.point_arena.push_back(SDL_FPoint{0.0f, 0.0f});
  list.point_arena.push_back(SDL_FPoint{std::numeric_limits<float>::infinity(), 1.0f});
  list.ops.push_back(DisplayOp{.op = DrawOp::Polyline, .data_offset = 0, .data_count = 2});
  std::string error;
  Expect(!ValidateDisplayList(list, &error), "a non-finite polyline point must be rejected");
  Expect(!error.empty(), "rejection should report a reason");
}

// Content dimensions feed the host's scroll extents / intrinsic layout size, so a
// NaN/inf/negative dimension must be rejected even when every op is valid.
void TestNonFiniteContentDimensionRejected() {
  PluginDisplayList list;
  list.content_width = std::numeric_limits<float>::quiet_NaN();
  list.content_height = 10.0f;
  std::string error;
  Expect(!ValidateDisplayList(list, &error), "a NaN content dimension must be rejected");
  Expect(!error.empty(), "rejection should report a reason");

  PluginDisplayList negative;
  negative.content_width = 10.0f;
  negative.content_height = -1.0f;
  Expect(!ValidateDisplayList(negative, &error), "a negative content dimension must be rejected");
}

void RegisterPluginDisplayListTests(std::vector<TestCase>& tests) {
  AddTest(tests, "PluginDisplayList/NonFiniteContentDimensionRejected",
          TestNonFiniteContentDimensionRejected);
  AddTest(tests, "PluginDisplayList/NonFiniteRectRejected", TestNonFiniteRectRejected);
  AddTest(tests, "PluginDisplayList/NonFinitePolylinePointRejected",
          TestNonFinitePolylinePointRejected);
  AddTest(tests, "PluginDisplayList/ValidListPasses", TestValidListPasses);
  AddTest(tests, "PluginDisplayList/TextOpOutOfBoundsFails", TestTextOpOutOfBoundsFails);
  AddTest(tests, "PluginDisplayList/EmptyTextOpOutOfBoundsOffsetFails",
          TestEmptyTextOpOutOfBoundsOffsetFails);
  AddTest(tests, "PluginDisplayList/EmptyTextOpAtArenaEndPasses",
          TestEmptyTextOpAtArenaEndPasses);
  AddTest(tests, "PluginDisplayList/PolylineNeedsTwoPoints", TestPolylineNeedsTwoPoints);
  AddTest(tests, "PluginDisplayList/PolylineOutOfBoundsFails", TestPolylineOutOfBoundsFails);
  AddTest(tests, "PluginDisplayList/ImageOpOutOfBoundsFails", TestImageOpOutOfBoundsFails);
  AddTest(tests, "PluginDisplayList/UnbalancedClipFails", TestUnbalancedClipFails);
  AddTest(tests, "PluginDisplayList/BalancedClipPasses", TestBalancedClipPasses);
  AddTest(tests, "PluginDisplayList/HashIsStableAndSensitive", TestHashIsStableAndSensitive);
}

}  // namespace microide::tests
