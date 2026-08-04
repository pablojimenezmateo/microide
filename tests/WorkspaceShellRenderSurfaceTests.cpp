// Paint real frames with a real renderer.
//
// Every other shell test that "renders" calls WorkspaceShellTestAccess::RenderFrame,
// which passes a null SDL_Renderer. Each render surface opens with
// `if (renderer == nullptr) return;`, so those tests validate layout and view-model
// construction and then stop at the guard. Nothing downstream of it had ever run.
//
// Coverage made the consequence concrete: WorkspaceShellRenderMerge.cpp had 568
// lines and 0 of 11 functions executed across the whole suite, while its compare
// sibling sat at 81% — compare's TU also holds hit-testing and geometry helpers that
// other tests call directly, and merge's TU is paint all the way down. Three
// sanitizers, twelve fuzz targets and an architecture lint cannot find a defect in
// code that never executes, and the lint's structural rules (scratch rows,
// TruncateLabelView, no per-row allocation) are all satisfiable by code that would
// fault on its first real paint.
//
// These tests drive the actual drawing path for the surfaces that had no coverage
// at all, in the states that make their branches diverge: conflicts present and
// absent, scrolled and unscrolled, selected hunk in and out of view, before and
// after a choice is applied.
//
// They paint into tests::SoftwareCanvas, the shared software render target the
// compare/pixel tests already use — which is also why compare's paint path was
// covered and merge's was not. Nothing new was needed here; merge had simply never
// been given the same treatment.

#include "TestSupport.h"

#include "support/SoftwareCanvas.h"

#include "compare/MergeModel.h"
#include "workspace/shell/WorkspaceShellTestAccess.h"

#include <SDL3/SDL.h>

#include <filesystem>
#include <string>
#include <vector>

namespace microide::tests {
namespace {

using microide::workspace::WorkspaceShell;
using WorkspaceShellTestAccess = microide::workspace::WorkspaceShell::TestAccess;

constexpr int kWindowWidth = 1280;
constexpr int kWindowHeight = 720;

// A merge fixture whose three sides diverge on several separated line ranges, so the
// resulting model carries more than one conflict. One conflict would leave the
// multi-marker overview lane and the selected-vs-unselected conflict styling
// unexercised.
struct MergeFixture {
  std::filesystem::path root;
  std::filesystem::path base;
  std::filesystem::path incoming;
  std::filesystem::path current;
};

std::string NumberedLines(const std::string& marker, int count) {
  std::string text;
  for (int i = 0; i < count; ++i) {
    if (i % 9 == 0) {
      text += marker + " divergent line " + std::to_string(i) + "\n";
    } else {
      text += "shared line " + std::to_string(i) + "\n";
    }
  }
  return text;
}

MergeFixture MakeMergeFixture(const std::filesystem::path& dir) {
  MergeFixture fixture;
  fixture.root = dir / "repo";
  fixture.current = fixture.root / "src" / "conflicted.cpp";
  fixture.base = dir / "base.cpp";
  fixture.incoming = dir / "incoming.cpp";

  WriteFile(fixture.base, NumberedLines("base", 120));
  WriteFile(fixture.incoming, NumberedLines("incoming", 120));
  WriteFile(fixture.current, NumberedLines("current", 120));

  InitializeGitRepo(fixture.root);
  CommitAll(fixture.root, "Add merge render fixture", "merge render fixture");
  return fixture;
}

// The merge surface renders only when a merge tab is the active tab, so every case
// here has to go through OpenMergeEditor rather than poking state.
void OpenMergeTab(WorkspaceShell& shell, const MergeFixture& fixture) {
  WorkspaceShellTestAccess::SetProjectRoot(shell, fixture.root);
  WorkspaceShellTestAccess::SetWindowSize(shell, kWindowWidth, kWindowHeight);
  Expect(WorkspaceShellTestAccess::OpenMergeEditor(shell, fixture.base, fixture.incoming,
                                                   fixture.current, fixture.current),
         "merge editor should open for the render fixture");
}

void TestMergeSurfacePaintsWithARealRenderer() {
  TemporaryDirectory temp_dir;
  const MergeFixture fixture = MakeMergeFixture(temp_dir.path());
  SoftwareCanvas canvas(kWindowWidth, kWindowHeight);

  WorkspaceShell shell;
  OpenMergeTab(shell, fixture);

  auto& merge = WorkspaceShellTestAccess::ActiveMerge(shell);
  Expect(!merge.conflicts.empty(),
         "the fixture must actually produce conflicts, or this paints an empty surface "
         "and proves nothing");

  WorkspaceShellTestAccess::RenderFrameWithRenderer(shell, canvas.renderer());

  // The overview-marker cache is built inside RenderMergeScrollbars and only when a
  // vertical scrollbar exists. Asserting it went valid is what distinguishes "the
  // paint path ran" from "the frame early-returned somewhere upstream" — without it
  // this test would still pass if the merge surface were never reached.
  Expect(merge.scrollbar_marker_cache_valid,
         "painting a scrollable merge tab should populate the overview marker cache");
  Expect(!merge.scrollbar_marker_cache.empty(),
         "a merge tab with conflicts should produce overview markers");

  // Pixels, not just state: a merge frame must come out fully opaque like every
  // other painted surface. A transparent pixel means the surface was skipped or
  // drawn with an alpha-less format, which is the failure mode that makes text
  // render as solid blocks.
  const SDL_Color center = canvas.PixelAt(kWindowWidth / 2, kWindowHeight / 2);
  Expect(center.a == 255, "a painted merge frame should be fully opaque");
}

void TestMergeSurfacePaintsWhileScrolledAndAfterChoices() {
  TemporaryDirectory temp_dir;
  const MergeFixture fixture = MakeMergeFixture(temp_dir.path());
  SoftwareCanvas canvas(kWindowWidth, kWindowHeight);

  WorkspaceShell shell;
  OpenMergeTab(shell, fixture);
  WorkspaceShellTestAccess::RenderFrameWithRenderer(shell, canvas.renderer());

  {
    auto& merge = WorkspaceShellTestAccess::ActiveMerge(shell);
    const std::size_t conflict_count = merge.conflicts.size();

    // Scroll deep enough that the first conflict leaves the viewport: the
    // per-row conflict styling and the source-pane button rects take a different
    // branch for conflicts above, inside, and below the visible range.
    merge.scroll_row = 60;
    // Select a conflict that is not the first, so the selected-vs-unselected
    // conflict tone diverges from the default.
    merge.selected_hunk = conflict_count > 1 ? conflict_count - 1 : 0;
  }
  WorkspaceShellTestAccess::RenderFrameWithRenderer(shell, canvas.renderer());

  // Applying a choice bumps model_revision, which must invalidate and rebuild the
  // marker cache on the next paint rather than drawing stale markers.
  WorkspaceShellTestAccess::ApplyMergeChoice(shell, microide::compare::MergeChoice::Incoming);
  WorkspaceShellTestAccess::RenderFrameWithRenderer(shell, canvas.renderer());
  {
    auto& merge = WorkspaceShellTestAccess::ActiveMerge(shell);
    Expect(merge.scrollbar_marker_cache_valid,
           "the marker cache should be rebuilt, not left stale, after a choice is applied");
    Expect(merge.scrollbar_marker_cache_revision == merge.model_revision,
           "the rebuilt marker cache should be keyed to the current model revision");
  }

  WorkspaceShellTestAccess::ApplyMergeChoice(shell, microide::compare::MergeChoice::Current);
  WorkspaceShellTestAccess::RenderFrameWithRenderer(shell, canvas.renderer());

  // Horizontal scroll drives the second scrollbar branch and the row clipping.
  {
    auto& merge = WorkspaceShellTestAccess::ActiveMerge(shell);
    merge.horizontal_scroll = 120;
  }
  WorkspaceShellTestAccess::RenderFrameWithRenderer(shell, canvas.renderer());
}

// A merge tab whose sides agree has zero conflicts. The overview lane, the
// conflict-row styling and the Next/Prev toolbar state all take their empty branch,
// which the conflicted fixtures never reach.
void TestMergeSurfacePaintsWithNoConflicts() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "agreed.cpp";
  const std::filesystem::path base = temp_dir.path() / "base.cpp";
  const std::filesystem::path incoming = temp_dir.path() / "incoming.cpp";
  const std::string identical = "alpha\nbeta\ngamma\n";
  WriteFile(source, identical);
  WriteFile(base, identical);
  WriteFile(incoming, identical);
  InitializeGitRepo(root);
  CommitAll(root, "Add conflict-free merge fixture", "conflict-free merge fixture");

  SoftwareCanvas canvas(kWindowWidth, kWindowHeight);
  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, kWindowWidth, kWindowHeight);
  Expect(WorkspaceShellTestAccess::OpenMergeEditor(shell, base, incoming, source, source),
         "merge editor should open for identical sides");

  Expect(WorkspaceShellTestAccess::ActiveMerge(shell).conflicts.empty(),
         "identical sides should produce no conflicts");
  WorkspaceShellTestAccess::RenderFrameWithRenderer(shell, canvas.renderer());
}

// A window small enough that the merge surface's three panes are squeezed toward
// zero width. Truncation, the sub-glyph-width label paths, and the negative/degenerate
// rect guards only run here.
void TestMergeSurfacePaintsAtDegenerateWindowSizes() {
  TemporaryDirectory temp_dir;
  const MergeFixture fixture = MakeMergeFixture(temp_dir.path());

  for (const auto& [width, height] : std::vector<std::pair<int, int>>{
           {320, 240}, {200, 120}, {1280, 200}, {120, 720}}) {
    SoftwareCanvas canvas(width, height);
    WorkspaceShell shell;
    WorkspaceShellTestAccess::SetProjectRoot(shell, fixture.root);
    WorkspaceShellTestAccess::SetWindowSize(shell, width, height);
    Expect(WorkspaceShellTestAccess::OpenMergeEditor(shell, fixture.base, fixture.incoming,
                                                     fixture.current, fixture.current),
           "merge editor should open at a degenerate window size");
    WorkspaceShellTestAccess::RenderFrameWithRenderer(shell, canvas.renderer());
  }
}

// The compare surface has partial coverage from its geometry helpers but its paint
// path was equally unexecuted. Same treatment.
void TestCompareSurfacePaintsWithARealRenderer() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "compared.cpp";
  WriteFile(source, NumberedLines("original", 120));
  InitializeGitRepo(root);
  CommitAll(root, "Add compare render fixture", "compare render fixture");
  WriteFile(source, NumberedLines("modified", 120));

  SoftwareCanvas canvas(kWindowWidth, kWindowHeight);
  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, kWindowWidth, kWindowHeight);
  Expect(WorkspaceShellTestAccess::OpenComparePickerForPath(shell, source),
         "compare should open against the working tree");

  WorkspaceShellTestAccess::RenderFrameWithRenderer(shell, canvas.renderer());
  WorkspaceShellTestAccess::RenderFrameWithRenderer(shell, canvas.renderer());
}

// The plain editor surface, for the same reason and as the control: if this one
// fails, the harness is wrong rather than the merge surface.
void TestEditorSurfacePaintsWithARealRenderer() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "main.cpp";
  WriteFile(source, NumberedLines("code", 400));

  SoftwareCanvas canvas(kWindowWidth, kWindowHeight);
  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, kWindowWidth, kWindowHeight);
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, source);

  WorkspaceShellTestAccess::RenderFrameWithRenderer(shell, canvas.renderer());
}

}  // namespace

void RegisterWorkspaceShellRenderSurfaceTests(std::vector<TestCase>& tests) {
  AddTest(tests, "WorkspaceShellRenderSurface/MergePaintsWithRealRenderer",
          TestMergeSurfacePaintsWithARealRenderer);
  AddTest(tests, "WorkspaceShellRenderSurface/MergePaintsScrolledAndAfterChoices",
          TestMergeSurfacePaintsWhileScrolledAndAfterChoices);
  AddTest(tests, "WorkspaceShellRenderSurface/MergePaintsWithNoConflicts",
          TestMergeSurfacePaintsWithNoConflicts);
  AddTest(tests, "WorkspaceShellRenderSurface/MergePaintsAtDegenerateWindowSizes",
          TestMergeSurfacePaintsAtDegenerateWindowSizes);
  AddTest(tests, "WorkspaceShellRenderSurface/ComparePaintsWithRealRenderer",
          TestCompareSurfacePaintsWithARealRenderer);
  AddTest(tests, "WorkspaceShellRenderSurface/EditorPaintsWithRealRenderer",
          TestEditorSurfacePaintsWithARealRenderer);
}

}  // namespace microide::tests
