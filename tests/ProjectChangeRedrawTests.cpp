// What a project-change batch repaints (TD-2026-10-15-341).
//
// The watcher's wake used to answer every applied batch with a full-window frame.
// Under agents writing a tree that was two full frames a second, almost all of it
// the editor re-rasterizing text nothing had changed. The apply now requests the
// surfaces it touched. Each test here paints ONLY those rects over the previous
// frame -- as the app loop does -- and requires the result to be pixel-identical
// to a full repaint of the same state, so a surface the narrowing forgets is a
// failure here rather than stale pixels on someone's screen. Each also proves the
// change was visible at all, or "identical" would hold for a frame that never
// changed.

#include "TestSupport.h"
#include "TestSupportSdl.h"
#include "WorkspaceShellEventHelpers.h"
#include "support/RetainedRedraw.h"
#include "support/SoftwareCanvas.h"

#include "platform/FileIndexWatcher.h"
#include "platform/Filesystem.h"
#include "workspace/shell/WorkspaceShell.h"
#include "workspace/shell/WorkspaceShellTestAccess.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace microide::tests {
namespace {

using microide::workspace::RenderInvalidation;
using microide::workspace::WorkspaceShell;
using WorkspaceShellTestAccess = microide::workspace::WorkspaceShell::TestAccess;

constexpr int kWidth = 1280;
constexpr int kHeight = 720;

struct SurfaceDeleter {
  void operator()(SDL_Surface* surface) const { SDL_DestroySurface(surface); }
};
using SurfacePtr = std::unique_ptr<SDL_Surface, SurfaceDeleter>;

struct Fixture {
  TemporaryDirectory temp_dir;
  std::filesystem::path root = temp_dir.path() / "project";
  std::filesystem::path open_file = root / "open.cpp";
  WorkspaceShell shell;
  SoftwareCanvas retained{kWidth, kHeight};
  // Every rect the change asked for, across the wake and the frames it led to.
  std::vector<SDL_FRect> painted;
  bool painted_full = false;

  explicit Fixture(bool sidebar_visible) {
    EnsureDummySdlVideoInitialized();
    WriteFile(open_file, "int open() {\n  return 1;\n}\n");
    WriteFile(root / "other.cpp", "int other() { return 2; }\n");
    WriteFile(root / "notes.txt", "notes\n");
    Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
           "the redraw fixture should open its project");
    WorkspaceShellTestAccess::SetWindowSize(shell, kWidth, kHeight);
    WorkspaceShellTestAccess::OpenSingleEditorTab(shell, open_file);
    Expect(WorkspaceShellTestAccess::ActiveEditor(shell).path() == open_file.lexically_normal(),
           "the fixture's open file should be the tab on screen");
    WorkspaceShellTestAccess::SetSidebarVisible(shell, sidebar_visible);
    WorkspaceShellTestAccess::NoteLayoutInputsChanged(shell);
    // The initial index lands asynchronously; let it, then stop the live watcher so
    // the only batches are the ones a test dispatches (see QuiesceFileIndexWatcher).
    Expect(WaitUntil([&]() { return WorkspaceShellTestAccess::ProjectFileIndexVersion(shell) > 0; },
                     std::chrono::seconds(5), std::chrono::milliseconds(5)),
           "the redraw fixture's initial index should land");
    Expect(WorkspaceShellTestAccess::QuiesceFileIndexWatcherForTesting(shell),
           "the redraw fixture should have a live watcher to quiesce");
    while (WorkspaceShellTestAccess::ReloadProjectIfFilesChanged(shell, false)) {
    }
    Paint(RenderInvalidation{.full = true, .rects = {}});
    Settle();
    painted.clear();
    painted_full = false;
  }

  // A blinking caret would make two paints of one state differ by when they ran.
  void PinCaret() { WorkspaceShellTestAccess::SetCaretBlinkEpochMs(shell, SDL_GetTicks()); }

  void Paint(const RenderInvalidation& redraw) {
    PinCaret();
    painted_full = painted_full || redraw.full;
    painted.insert(painted.end(), redraw.rects.begin(), redraw.rects.end());
    PaintInvalidation(shell, retained, kWidth, kHeight, redraw);
  }

  // What a frame queues for the NEXT one (frame prep asks for the status bar when a
  // segment moved) is painted too, as the app loop would.
  void Settle() {
    for (int frame = 0; frame < 4; ++frame) {
      const RenderInvalidation pending = WorkspaceShellTestAccess::TakePendingRenderInvalidation(shell);
      if (!pending.HasAnyRedraw()) {
        return;
      }
      Paint(pending);
    }
  }

  // A copy of what is on screen now. Glyph textures belong to the renderer that
  // made them, so a reference frame has to be drawn on THIS canvas, not a fresh one
  // (a second canvas differs everywhere there is text) -- hence copies.
  SurfacePtr Snapshot() {
    SDL_FlushRenderer(retained.renderer());
    return SurfacePtr(SDL_DuplicateSurface(retained.surface()));
  }

  // Repaints the whole window in place: the ground truth for the current state.
  SurfacePtr FullFrame() {
    PinCaret();
    shell.Render(retained.renderer(), kWidth, kHeight);
    return Snapshot();
  }

  // Deliver `batch` the way the watcher thread does, then run the wake that applies it.
  void ApplyThroughTheWake(platform::IndexUpdateBatch batch) {
    Expect(WorkspaceShellTestAccess::DispatchFileIndexWatcherBatchForTesting(shell, std::move(batch)),
           "the redraw fixture should deliver its batch");
    const std::uint64_t before = WorkspaceShellTestAccess::LastAppliedProjectChangeGeneration(shell);
    const WorkspaceShell::EventResult result = WorkspaceShellTestAccess::HandleScheduledWake(shell);
    Expect(WorkspaceShellTestAccess::LastAppliedProjectChangeGeneration(shell) > before,
           "the wake should have applied the batch, or nothing below is about it");
    Expect(result.handled, "applying a batch is a handled wake");
    Paint(result.redraw);
    Settle();
  }

  platform::IndexUpdateBatch::Change Modified(const std::filesystem::path& absolute) const {
    const std::optional<platform::FileMetadata> metadata = platform::ReadFileMetadata(absolute);
    Expect(metadata.has_value(), "a modified fixture file should exist");
    return platform::IndexUpdateBatch::Change{
        .kind = platform::IndexUpdateBatch::Kind::CreatedOrModified,
        .entry = platform::IndexFileEntry{
            .relative_path = absolute.lexically_relative(root),
            .mtime = metadata->mtime,
            .size = metadata->size,
        },
    };
  }

  void ExpectRetainedMatchesFull(const std::string& what) {
    const SurfacePtr retained_pixels = Snapshot();
    const SurfacePtr full_pixels = FullFrame();
    // Where they differ names the surface the narrowing forgot.
    int x0 = kWidth, y0 = kHeight, x1 = -1, y1 = -1;
    for (int y = 0; y < kHeight; ++y) {
      for (int x = 0; x < kWidth; ++x) {
        Uint8 a[4] = {}, b[4] = {};
        SDL_ReadSurfacePixel(retained_pixels.get(), x, y, &a[0], &a[1], &a[2], &a[3]);
        SDL_ReadSurfacePixel(full_pixels.get(), x, y, &b[0], &b[1], &b[2], &b[3]);
        if (std::equal(a, a + 4, b)) {
          continue;
        }
        x0 = std::min(x0, x);
        y0 = std::min(y0, y);
        x1 = std::max(x1, x);
        y1 = std::max(y1, y);
      }
    }
    Expect(x1 < 0, what + " (stale pixels in x " + std::to_string(x0) + ".." + std::to_string(x1) +
                       ", y " + std::to_string(y0) + ".." + std::to_string(y1) + ")");
  }

  std::string DescribePainted() const {
    std::string text = painted_full ? " [full]" : "";
    for (const SDL_FRect& rect : painted) {
      text += " [" + std::to_string(static_cast<int>(rect.x)) + "," +
              std::to_string(static_cast<int>(rect.y)) + " " +
              std::to_string(static_cast<int>(rect.w)) + "x" +
              std::to_string(static_cast<int>(rect.h)) + "]";
    }
    return text;
  }

  bool PaintedAny(const SDL_FRect& target) const {
    return painted_full || AnyRectIntersects(painted, target);
  }
};

void TestAWriteToAnUnopenedFileRepaintsTheSidebarNotTheEditor() {
  Fixture fixture(/*sidebar_visible=*/true);
  const SurfacePtr before = fixture.Snapshot();

  // A new file at the root: the tree grows a row, the open buffer is untouched.
  const std::filesystem::path created = fixture.root / "agent-notes.md";
  WriteFile(created, "written by an agent\n");
  platform::IndexUpdateBatch batch;
  batch.changes.push_back(fixture.Modified(created));
  fixture.ApplyThroughTheWake(std::move(batch));

  const auto layout = WorkspaceShellTestAccess::CurrentLayout(fixture.shell);
  Expect(!fixture.painted_full, "a write to a file nobody has open must not repaint the window");
  Expect(fixture.PaintedAny(layout.sidebar), "the tree lists files, so the sidebar repaints");
  Expect(!AnyRectIntersects(fixture.painted, layout.editor_surface),
         "the editor shows none of what changed, so it does not re-rasterize");
  fixture.ExpectRetainedMatchesFull(
      "painting only the batch's rects should leave the window identical to a full repaint");

  Expect(CountPixelDifferences(before.get(), fixture.Snapshot().get()) > 0,
         "the new file should have changed the tree on screen (or the comparison proves nothing)");
}

void TestAWriteToTheOpenFileRepaintsItsPane() {
  Fixture fixture(/*sidebar_visible=*/true);
  const SurfacePtr before = fixture.Snapshot();

  WriteFile(fixture.open_file, "int open() {\n  return 42;  // an agent was here\n}\n");
  platform::IndexUpdateBatch batch;
  batch.changes.push_back(fixture.Modified(fixture.open_file));
  fixture.ApplyThroughTheWake(std::move(batch));

  const auto layout = WorkspaceShellTestAccess::CurrentLayout(fixture.shell);
  Expect(!fixture.painted_full, "reloading one open file must not repaint the window");
  Expect(fixture.PaintedAny(layout.editor_surface), "the reloaded buffer's pane repaints");
  fixture.ExpectRetainedMatchesFull(
      "a clean reload's rects should leave the window identical to a full repaint");

  Expect(CountPixelDifferences(before.get(), fixture.Snapshot().get()) > 0,
         "the reload should have changed the editor on screen");
}

void TestAHiddenSidebarRepaintsNothingForAnUnopenedFile() {
  Fixture fixture(/*sidebar_visible=*/false);

  const std::filesystem::path other = fixture.root / "other.cpp";
  WriteFile(other, "int other() { return 3; }\n");
  platform::IndexUpdateBatch batch;
  batch.changes.push_back(fixture.Modified(other));
  fixture.ApplyThroughTheWake(std::move(batch));

  // RequestSidebarRedraw fell back to the whole WINDOW when the sidebar was hidden,
  // so the automatic git refresh every batch triggers was the full frame under
  // another name. The status-bar strip is all it may ask for now: its branch and
  // counts are re-derived at frame prep, which needs some frame to run in.
  const auto layout = WorkspaceShellTestAccess::CurrentLayout(fixture.shell);
  Expect(!fixture.painted_full, "a hidden sidebar must not turn a batch into a full frame;" +
                                    fixture.DescribePainted());
  for (const SDL_FRect& rect : fixture.painted) {
    Expect(rect.y >= layout.status_bar.y && rect.h <= layout.status_bar.h,
           "with nothing on screen listing files, only the status bar may repaint;" +
               fixture.DescribePainted());
  }
  fixture.ExpectRetainedMatchesFull("and nothing on screen was stale");
}

void TestATreeShapeChangeRepaintsTheEditorColumnNotTheWindow() {
  Fixture fixture(/*sidebar_visible=*/true);
  const SurfacePtr before = fixture.Snapshot();

  // A directory appearing is a shape change with no file in it: a "rescan", which
  // re-checks every open buffer. The open file changed too, unannounced.
  std::filesystem::create_directories(fixture.root / "agent-work");
  WriteFile(fixture.open_file, "int open() {\n  return 7;\n}\n");
  platform::IndexUpdateBatch batch;
  batch.tree_structure_changed = true;
  fixture.ApplyThroughTheWake(std::move(batch));

  const auto layout = WorkspaceShellTestAccess::CurrentLayout(fixture.shell);
  Expect(!fixture.painted_full, "a tree rescan must not repaint the window");
  Expect(fixture.PaintedAny(layout.sidebar) && fixture.PaintedAny(layout.editor_surface),
         "a rescan repaints the tree and the editor column, where any buffer may have reloaded");
  fixture.ExpectRetainedMatchesFull(
      "a rescan's rects should leave the window identical to a full repaint");

  Expect(CountPixelDifferences(before.get(), fixture.Snapshot().get()) > 0, "the rescan should have changed the screen");
}

}  // namespace

void RegisterProjectChangeRedrawTests(std::vector<TestCase>& tests) {
#if MICROIDE_HAS_SDL3_TTF
  AddTest(tests, "ProjectChangeRedraw/AnUnopenedFileRepaintsTheSidebarNotTheEditor",
          TestAWriteToAnUnopenedFileRepaintsTheSidebarNotTheEditor);
  AddTest(tests, "ProjectChangeRedraw/AnOpenFileRepaintsItsPane",
          TestAWriteToTheOpenFileRepaintsItsPane);
  AddTest(tests, "ProjectChangeRedraw/AHiddenSidebarRepaintsNothing",
          TestAHiddenSidebarRepaintsNothingForAnUnopenedFile);
  AddTest(tests, "ProjectChangeRedraw/ATreeShapeChangeRepaintsTheEditorColumn",
          TestATreeShapeChangeRepaintsTheEditorColumnNotTheWindow);
#else
  (void)tests;
#endif
}

}  // namespace microide::tests
