#include "TestSupport.h"

#include "editor/PluginSurfaceStore.h"

#include <filesystem>
#include <utility>

namespace microide::tests {
namespace {

using microide::editor::PluginSurfaceStore;
using microide::editor::RasterHandle;
using microide::editor::SurfaceAnchor;
using microide::editor::SurfaceContent;
using microide::editor::SurfacePreviewSlot;

SurfaceContent RasterSurface(std::uint64_t hash, SurfacePreviewSlot preview) {
  SurfaceContent content;
  content.body = RasterHandle{.content_hash = hash, .width = 32, .height = 16};
  content.intrinsic_width = 32;
  content.intrinsic_height = 16;
  content.preview = preview;
  return content;
}

void TestReplaceFindAndIdempotency() {
  PluginSurfaceStore store;
  Expect(store.ReplaceForOwnerSurface("plug", "s1", RasterSurface(1, SurfacePreviewSlot::Bottom)),
         "first publish should report a change");
  const SurfaceContent* found = store.Find("plug", "s1");
  Expect(found != nullptr && found->has_body(), "the surface should be findable with a body");

  Expect(!store.ReplaceForOwnerSurface("plug", "s1", RasterSurface(1, SurfacePreviewSlot::Bottom)),
         "an identical republish is a no-op");
  Expect(store.ReplaceForOwnerSurface("plug", "s1", RasterSurface(2, SurfacePreviewSlot::Bottom)),
         "a content-hash change should report a change");
}

void TestClearPaths() {
  PluginSurfaceStore store;
  store.ReplaceForOwnerSurface("a", "s1", RasterSurface(1, SurfacePreviewSlot::None));
  store.ReplaceForOwnerSurface("a", "s2", RasterSurface(2, SurfacePreviewSlot::None));
  store.ReplaceForOwnerSurface("b", "s1", RasterSurface(3, SurfacePreviewSlot::None));

  Expect(store.ClearOwnerSurface("a", "s1"), "clearing one surface should report a change");
  Expect(store.Find("a", "s1") == nullptr, "the cleared surface is gone");
  Expect(store.Find("a", "s2") != nullptr, "the sibling surface remains");

  Expect(store.ClearOwner("a"), "clearing the owner should report a change");
  Expect(store.Find("a", "s2") == nullptr, "the owner's surfaces are gone");
  Expect(store.Find("b", "s1") != nullptr, "another owner is untouched");
  Expect(!store.empty(), "owner b still present");
}

void TestPreviewSurfacesSortedAndToggle() {
  PluginSurfaceStore store;
  store.ReplaceForOwnerSurface("z", "b", RasterSurface(1, SurfacePreviewSlot::Bottom));
  store.ReplaceForOwnerSurface("a", "a", RasterSurface(2, SurfacePreviewSlot::Bottom));
  store.ReplaceForOwnerSurface("a", "z", RasterSurface(3, SurfacePreviewSlot::None));

  const auto previews = store.PreviewSurfaces();
  Expect(previews.size() == 2, "only preview-requesting surfaces are listed");
  Expect(previews[0].owner == "a" && previews[0].surface_id == "a", "sorted by (owner, id)");
  Expect(previews[1].owner == "z", "second entry follows in order");

  Expect(store.SetPreviewSlot("a", "a", SurfacePreviewSlot::None),
         "withdrawing a preview reports a change");
  Expect(store.PreviewSurfaces().size() == 1, "the withdrawn surface drops out of the tab list");
}

void TestAnchoredIndexSortedByLine() {
  PluginSurfaceStore store;
  const std::filesystem::path path = "/proj/main.cpp";
  SurfaceContent a = RasterSurface(1, SurfacePreviewSlot::None);
  a.anchor = SurfaceAnchor{.path = path, .line = 9};
  SurfaceContent b = RasterSurface(2, SurfacePreviewSlot::None);
  b.anchor = SurfaceAnchor{.path = path, .line = 3};
  store.ReplaceForOwnerSurface("p", "late", std::move(a));
  store.ReplaceForOwnerSurface("p", "early", std::move(b));

  const auto anchored = store.AnchoredSurfacesForPath(path);
  Expect(anchored.size() == 2, "both anchored surfaces are indexed by path");
  Expect(anchored[0].line == 3 && anchored[1].line == 9, "anchored surfaces are sorted by line");
  Expect(store.AnchoredSurfacesForPath("/proj/other.cpp").empty(),
         "an unrelated path has no anchored surfaces");
}

void TestRevisionBumps() {
  PluginSurfaceStore store;
  const std::uint64_t before = store.revision();
  store.ReplaceForOwnerSurface("p", "s", RasterSurface(1, SurfacePreviewSlot::None));
  Expect(store.revision() != before, "a publish bumps the revision");
}

// A hostile plugin publishing a fresh surface_id in a loop must not grow the
// store without bound (host-side heap OOM outside the Lua memory cap, plus an
// O(surfaces) RebuildAnchorIndex on every publish → O(N^2) UI-thread hang).
// New ids past the per-owner cap are refused; existing ids keep updating.
void TestPerOwnerSurfaceCountIsBounded() {
  PluginSurfaceStore store;
  bool refused_a_new_id = false;
  for (int i = 0; i < 9000; ++i) {
    const bool changed = store.ReplaceForOwnerSurface(
        "plug", "s" + std::to_string(i), RasterSurface(static_cast<std::uint64_t>(i + 1),
                                                       SurfacePreviewSlot::None));
    if (!changed) {
      refused_a_new_id = true;
    }
  }
  Expect(refused_a_new_id, "a fresh surface_id past the per-owner cap must be refused");
  // Updating an already-stored id still works at the cap.
  Expect(store.Find("plug", "s0") != nullptr, "an early surface stays stored at the cap");
  Expect(store.ReplaceForOwnerSurface("plug", "s0",
                                      RasterSurface(999999, SurfacePreviewSlot::None)),
         "updating an existing surface still works at the cap");
}

}  // namespace

void RegisterPluginSurfaceStoreTests(std::vector<TestCase>& tests) {
  AddTest(tests, "PluginSurfaceStore/ReplaceFindAndIdempotency", TestReplaceFindAndIdempotency);
  AddTest(tests, "PluginSurfaceStore/ClearPaths", TestClearPaths);
  AddTest(tests, "PluginSurfaceStore/PreviewSurfacesSortedAndToggle",
          TestPreviewSurfacesSortedAndToggle);
  AddTest(tests, "PluginSurfaceStore/AnchoredIndexSortedByLine", TestAnchoredIndexSortedByLine);
  AddTest(tests, "PluginSurfaceStore/RevisionBumps", TestRevisionBumps);
  AddTest(tests, "PluginSurfaceStore/PerOwnerSurfaceCountIsBounded",
          TestPerOwnerSurfaceCountIsBounded);
}

}  // namespace microide::tests
