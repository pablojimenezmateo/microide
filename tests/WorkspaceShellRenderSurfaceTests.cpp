#include "TestSupport.h"
#include "support/RetainedRedraw.h"

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

  Expect(merge.scrollbar_marker_cache_valid,
         "painting a scrollable merge tab should populate the overview marker cache");
  Expect(!merge.scrollbar_marker_cache.empty(),
         "a merge tab with conflicts should produce overview markers");

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

    merge.scroll_row = 60;
    merge.selected_hunk = conflict_count > 1 ? conflict_count - 1 : 0;
  }
  WorkspaceShellTestAccess::RenderFrameWithRenderer(shell, canvas.renderer());

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

  {
    auto& merge = WorkspaceShellTestAccess::ActiveMerge(shell);
    merge.horizontal_scroll = 120;
  }
  WorkspaceShellTestAccess::RenderFrameWithRenderer(shell, canvas.renderer());
}

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

void TestMergePaintsOfOneFrameAgree() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const auto lines = [](const std::string& marker) {
    std::string text;
    for (int i = 0; i < 400; ++i) {
      text += "// comment " + std::to_string(i) + "\nint v" + std::to_string(i) + " = " +
              (i == 350 ? marker : std::string("0")) + ";\n";
    }
    return text;
  };
  WriteFile(root / "base.cpp", lines("0"));
  WriteFile(root / "incoming.cpp", lines("1"));
  WriteFile(root / "current.cpp", lines("2"));
  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, kWindowWidth, kWindowHeight);
  Expect(WorkspaceShellTestAccess::OpenMergeEditor(shell, root / "base.cpp", root / "incoming.cpp",
                                                   root / "current.cpp", root / "current.cpp"),
         "merge editor should open");
  WorkspaceShellTestAccess::ActiveMerge(shell).scroll_row = 300;

  SoftwareCanvas canvas(kWindowWidth, kWindowHeight);
  const WorkspaceShell::FrameToken token =
      shell.PrepareFrameOnce(canvas.renderer(), kWindowWidth, kWindowHeight);
  (void)shell.ConsumePendingRenderInvalidation();
  const SDL_FRect everything{0.0f, 0.0f, static_cast<float>(kWindowWidth),
                             static_cast<float>(kWindowHeight)};
  shell.RenderClip(token, canvas.renderer(), kWindowWidth, kWindowHeight, everything);
  SDL_FlushRenderer(canvas.renderer());
  SDL_Surface* first = SDL_DuplicateSurface(canvas.surface());
  shell.RenderClip(token, canvas.renderer(), kWindowWidth, kWindowHeight);
  SDL_FlushRenderer(canvas.renderer());
  const std::size_t differing = CountPixelDifferences(first, canvas.surface());
  SDL_DestroySurface(first);
  Expect(differing == 0, "a hinted paint and a full paint of the same frame paint the same pixels: " +
                             std::to_string(differing) + " differ");
}

void TestMergeColumnsPaintAtOneRowPastTheEnd() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const auto lines = [](const std::string& marker) {
    std::string text;
    for (int i = 0; i < 60; ++i) {
      text += "int v" + std::to_string(i) + " = " + (i == 30 ? marker : std::string("0")) + ";\n";
    }
    return text;
  };
  WriteFile(root / "base.cpp", lines("0"));
  WriteFile(root / "incoming.cpp", lines("1"));
  WriteFile(root / "current.cpp", lines("2"));
  std::size_t mismatched_heights = 0;
  std::string first_mismatch;
  for (int height = 300; height <= 900; height += 2) {
    WorkspaceShell shell;
    WorkspaceShellTestAccess::SetProjectRoot(shell, root);
    WorkspaceShellTestAccess::SetWindowSize(shell, kWindowWidth, height);
    Expect(WorkspaceShellTestAccess::OpenMergeEditor(shell, root / "base.cpp", root / "incoming.cpp",
                                                     root / "current.cpp", root / "current.cpp"),
           "merge editor should open");
    WorkspaceShellTestAccess::ActiveMerge(shell).scroll_row = 1 << 20;

    SoftwareCanvas canvas(kWindowWidth, height);
    const WorkspaceShell::FrameToken token = shell.PrepareFrameOnce(canvas.renderer(), kWindowWidth, height);
    (void)shell.ConsumePendingRenderInvalidation();
    shell.RenderClip(token, canvas.renderer(), kWindowWidth, height);
    SDL_FlushRenderer(canvas.renderer());
    SDL_Surface* first = SDL_DuplicateSurface(canvas.surface());
    shell.RenderClip(token, canvas.renderer(), kWindowWidth, height);
    SDL_FlushRenderer(canvas.renderer());
    const std::size_t differing = CountPixelDifferences(first, canvas.surface());
    SDL_DestroySurface(first);
    if (differing != 0) {
      ++mismatched_heights;
      if (first_mismatch.empty()) {
        first_mismatch = "height " + std::to_string(height) + ": " + std::to_string(differing) + " pixels";
      }
    }
  }
  Expect(mismatched_heights == 0, "two paints of one frame of a merge scrolled past its end agree; " +
                                      std::to_string(mismatched_heights) + " heights differ, first " +
                                      first_mismatch);
}

}

void RegisterWorkspaceShellRenderSurfaceTests(std::vector<TestCase>& tests) {
  AddTest(tests, "WorkspaceShellRenderSurface/MergePaintsOfOneFrameAgree", TestMergePaintsOfOneFrameAgree);
  AddTest(tests, "WorkspaceShellRenderSurface/MergeColumnsPaintAtOneRowPastTheEnd",
          TestMergeColumnsPaintAtOneRowPastTheEnd);
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

}
