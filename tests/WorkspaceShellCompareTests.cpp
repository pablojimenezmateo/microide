#include "TestSupport.h"

#include "platform/ProcessLauncher.h"
#include "project/GitRepository.h"
#include "util/PerformanceCounters.h"

#include "support/RetainedRedraw.h"
#include "support/SoftwareCanvas.h"
#include "WorkspaceShellEventHelpers.h"

#include "workspace/render/CompareMergeRender.h"
#include "workspace/render/DiffDividerGeometry.h"
#include "render/TextRenderer.h"
#include "workspace/git/CompareTabReview.h"
#include "workspace/registries/WorkspaceCommandRegistry.h"
#include "workspace/shell/WorkspaceShellTestAccess.h"
#include "render/Theme.h"
#include "editor/PluginDecorationStore.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <limits>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <vector>
#include "WorkspaceShellEventHelpers.h"

namespace microide::tests {
namespace {


using microide::workspace::WorkspaceShell;
using WorkspaceShellTestAccess = microide::workspace::WorkspaceShell::TestAccess;

SDL_FRect CompareDividerRectOf(microide::workspace::WorkspaceShell& shell) {
  const auto layout = WorkspaceShellTestAccess::CurrentLayout(shell);
  return microide::workspace::CompareDividerHitRect(
      layout.editor_surface, WorkspaceShellTestAccess::ActiveCompareSurfaceLayout(shell));
}

std::array<SDL_FRect, 2> MergeDividerRectsOf(microide::workspace::WorkspaceShell& shell) {
  const auto layout = WorkspaceShellTestAccess::CurrentLayout(shell);
  return microide::workspace::MergeDividerHitRects(
      layout.editor_surface, WorkspaceShellTestAccess::ActiveMergeSurfaceLayout(shell));
}

#if MICROIDE_HAS_SDL3_TTF

void EnsureDummySdlVideo() {
  static ScopedEnvVar video_driver("SDL_VIDEODRIVER", "dummy");
  static const bool initialized = SDL_Init(SDL_INIT_VIDEO);
  Expect(initialized, "SDL should initialize with the dummy video driver");
}


#endif

bool SettleComparePicker(WorkspaceShell& shell) {
  return WaitUntil(
      [&shell]() { return !WorkspaceShellTestAccess::ComparePickerLoading(shell); },
      std::chrono::seconds(2), std::chrono::milliseconds(5),
      [&shell]() { WorkspaceShellTestAccess::ConsumeGitSidebarRefresh(shell); });
}

std::optional<microide::editor::EditorBlameOverlay> WaitForActiveCompareBlameOverlay(
    WorkspaceShell& shell,
    std::size_t minimum_line_count = 1) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline) {
    WorkspaceShellTestAccess::ApplyGitBlameResults(shell);
    const auto overlay = WorkspaceShellTestAccess::ActiveCompareBlameOverlay(shell);
    if (overlay.has_value() && overlay->lines.size() >= minimum_line_count) {
      return overlay;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return WorkspaceShellTestAccess::ActiveCompareBlameOverlay(shell);
}

std::optional<microide::editor::EditorBlameOverlay> WaitForActiveMergeBlameOverlay(
    WorkspaceShell& shell,
    std::size_t minimum_line_count = 1) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline) {
    WorkspaceShellTestAccess::ApplyGitBlameResults(shell);
    const auto overlay = WorkspaceShellTestAccess::ActiveMergeBlameOverlay(shell);
    if (overlay.has_value() && overlay->lines.size() >= minimum_line_count) {
      return overlay;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return WorkspaceShellTestAccess::ActiveMergeBlameOverlay(shell);
}

void TestWorkspaceShellIdenticalMergeOutputRewriteKeepsResolution() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "main.cpp";
  const std::filesystem::path base = temp_dir.path() / "base.cpp";
  const std::filesystem::path incoming = temp_dir.path() / "incoming.cpp";
  WriteFile(source, "line 1\ncurrent line\nline 3\n");
  WriteFile(base, "line 1\nbase line\nline 3\n");
  WriteFile(incoming, "line 1\nincoming line\nline 3\n");
  InitializeGitRepo(root);
  CommitAll(root, "Add merge staleness fixture", "merge staleness fixture");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::OpenMergeEditor(shell, base, incoming, source, source),
         "merge editor should open");

  auto& merge = WorkspaceShellTestAccess::ActiveMerge(shell);
  const std::string resolved = "line 1\nresolved line\nline 3\n";
  WriteFile(merge.output_path, resolved);
  merge.persisted_output_baseline = resolved;
  merge.disk_result_tick = microide::util::FileModificationTick(merge.output_path);
  merge.external_result_stale = false;
  merge.marked_resolved = true;

  WriteFile(merge.output_path, resolved);
  std::error_code mtime_error;
  std::filesystem::last_write_time(
      merge.output_path,
      std::filesystem::file_time_type::clock::now() + std::chrono::seconds(2), mtime_error);
  WorkspaceShellTestAccess::InvalidateMergeTabsForPath(shell, merge.output_path);

  const auto& after = WorkspaceShellTestAccess::ActiveMerge(shell);
  Expect(!after.external_result_stale,
         "a byte-identical rewrite of the merge output must not mark the result stale");
  Expect(after.marked_resolved,
         "a byte-identical rewrite must not un-mark a completed resolution");
}

void TestWorkspaceShellRealMergeOutputEditDropsResolution() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "main.cpp";
  const std::filesystem::path base = temp_dir.path() / "base.cpp";
  const std::filesystem::path incoming = temp_dir.path() / "incoming.cpp";
  WriteFile(source, "line 1\ncurrent line\nline 3\n");
  WriteFile(base, "line 1\nbase line\nline 3\n");
  WriteFile(incoming, "line 1\nincoming line\nline 3\n");
  InitializeGitRepo(root);
  CommitAll(root, "Add merge staleness fixture", "merge staleness fixture");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::OpenMergeEditor(shell, base, incoming, source, source),
         "merge editor should open");

  auto& merge = WorkspaceShellTestAccess::ActiveMerge(shell);
  const std::string resolved = "line 1\nresolved line\nline 3\n";
  WriteFile(merge.output_path, resolved);
  merge.persisted_output_baseline = resolved;
  merge.disk_result_tick = microide::util::FileModificationTick(merge.output_path);
  merge.external_result_stale = false;
  merge.marked_resolved = true;

  WriteFile(merge.output_path, "line 1\nsomeone else edited this\nline 3\n");
  std::error_code mtime_error;
  std::filesystem::last_write_time(
      merge.output_path,
      std::filesystem::file_time_type::clock::now() + std::chrono::seconds(2), mtime_error);
  WorkspaceShellTestAccess::InvalidateMergeTabsForPath(shell, merge.output_path);

  const auto& after = WorkspaceShellTestAccess::ActiveMerge(shell);
  Expect(after.external_result_stale, "a real external edit still marks the result stale");
  Expect(!after.marked_resolved, "a real external edit un-marks the resolution");
}

void TestWorkspaceShellWorkingTreeCompareOpensOnFirstChange() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "main.cpp";

  std::string committed;
  for (int line = 0; line < 400; ++line) {
    committed += "line " + std::to_string(line) + "\n";
  }
  WriteFile(source, committed);
  InitializeGitRepo(root);
  CommitAll(root, "Add multi-hunk compare fixture", "multi-hunk compare fixture");

  std::string edited;
  for (int line = 0; line < 400; ++line) {
    if (line == 50 || line == 200 || line == 350) {
      edited += "changed " + std::to_string(line) + "\n";
    } else {
      edited += "line " + std::to_string(line) + "\n";
    }
  }
  WriteFile(source, edited);

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, source, "HEAD", "HEAD"),
         "working-tree comparison should open");

  SoftwareCanvas canvas(1280, 720);
  for (int frame = 0; frame < 4; ++frame) {
    shell.Render(canvas.renderer(), 1280, 720);
  }

  auto& compare = WorkspaceShellTestAccess::ActiveCompare(shell);
  const auto& hunks = compare.model.hunks;
  Expect(hunks.size() >= 3,
         "the fixture must produce several hunks or this test cannot tell first from last");

  const std::size_t first_hunk_row = microide::compare::ComparePresentationRowForModelRow(
      compare.presentation, static_cast<std::size_t>(hunks.front().start_row));
  Expect(compare.selected_row == first_hunk_row,
         "opening a working-tree comparison should select the FIRST change, not the last");
  Expect(compare.right_viewport.cursor_line() ==
             microide::workspace::CompareTabRightLineForModelRow(
                 compare, static_cast<std::size_t>(hunks.front().start_row)),
         "the editable pane's caret should agree with the selected row at open");
}

void TestWorkspaceShellCompareStagesHunksInSequence() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "f.txt";
  WriteFile(source, "}\n\neps\n");
  InitializeGitRepo(root);
  CommitAll(root, "base", "base");
  const std::string edited = "alpha\nalpha\n}\n\neps\nbeta\n";
  WriteFile(source, edited);

  WorkspaceShell shell;
  WorkspaceShellTestAccess::RegisterLifecycleWakeEvents(shell);
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, source, "HEAD", "HEAD"),
         "working-tree comparison should open");
  SoftwareCanvas canvas(1280, 720);
  shell.Render(canvas.renderer(), 1280, 720);
  WorkspaceShellTestAccess::RefreshGitSidebar(shell);
  WorkspaceShellTestAccess::DrainProjectBackgroundExecutor(shell);
  WorkspaceShellTestAccess::ConsumeGitSidebarRefresh(shell);

  const auto stage_selected_hunk = [&]() {
    WorkspaceShellTestAccess::StageCompareHunk(shell);
    WorkspaceShellTestAccess::DrainProjectBackgroundExecutor(shell);
    WorkspaceShellTestAccess::ConsumeGitSidebarRefresh(shell);
    WorkspaceShellTestAccess::DrainProjectBackgroundExecutor(shell);
    WorkspaceShellTestAccess::ConsumeGitSidebarRefresh(shell);
  };
  const auto index_content = [&]() {
    microide::project::GitRepository repo(root, microide::platform::LocalProcessLauncher());
    return repo.Execute({"show", ":f.txt"}).output;
  };

  {
    auto& compare = WorkspaceShellTestAccess::ActiveCompare(shell);
    Expect(compare.model.hunks.size() == 2, "the edit is two hunks: a top insertion and a tail");
    compare.selected_row = microide::compare::ComparePresentationRowForModelRow(
        compare.presentation, static_cast<std::size_t>(compare.model.hunks.front().start_row));
  }
  stage_selected_hunk();
  Expect(index_content() == "alpha\nalpha\n}\n\neps\n",
         ("the top hunk is staged; index=[" + index_content() + "] feedback=[" +
          WorkspaceShellTestAccess::CommandFeedbackText(shell) + "]")
             .c_str());

  {
    auto& compare = WorkspaceShellTestAccess::ActiveCompare(shell);
    Expect(!compare.model.hunks.empty(), "the tail hunk is still there to stage");
    compare.selected_row = microide::compare::ComparePresentationRowForModelRow(
        compare.presentation, static_cast<std::size_t>(compare.model.hunks.back().start_row));
  }
  stage_selected_hunk();
  Expect(index_content() == edited,
         ("staging the second hunk after the first leaves the index equal to the file; index=[" +
          index_content() + "] feedback=[" +
          WorkspaceShellTestAccess::CommandFeedbackText(shell) + "]")
             .c_str());
}

void TestWorkspaceShellCompareUnstagesRightAfterAStage() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "f.txt";
  const std::string base = "one\ntwo\nthree\nfour\nfive\n";
  WriteFile(source, base);
  InitializeGitRepo(root);
  CommitAll(root, "base", "base");
  WriteFile(source, "one\ntwo\nTHREE\nfour\nfive\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::RegisterLifecycleWakeEvents(shell);
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, source, "HEAD", "HEAD"),
         "working-tree comparison should open");
  SoftwareCanvas canvas(1280, 720);
  shell.Render(canvas.renderer(), 1280, 720);
  WorkspaceShellTestAccess::RefreshGitSidebar(shell);
  WorkspaceShellTestAccess::DrainProjectBackgroundExecutor(shell);
  WorkspaceShellTestAccess::ConsumeGitSidebarRefresh(shell);
  const auto index_content = [&]() {
    microide::project::GitRepository repo(root, microide::platform::LocalProcessLauncher());
    return repo.Execute({"show", ":f.txt"}).output;
  };

  {
    auto& compare = WorkspaceShellTestAccess::ActiveCompare(shell);
    Expect(compare.model.hunks.size() == 1, "the edit is one hunk");
    compare.selected_row = microide::compare::ComparePresentationRowForModelRow(
        compare.presentation, static_cast<std::size_t>(compare.model.hunks.front().start_row));
  }
  WorkspaceShellTestAccess::StageCompareHunk(shell);
  WorkspaceShellTestAccess::DrainProjectBackgroundExecutor(shell);
  WorkspaceShellTestAccess::ConsumeGitSidebarRefresh(shell);
  Expect(index_content() == "one\ntwo\nTHREE\nfour\nfive\n",
         ("the hunk is staged; feedback=[" + WorkspaceShellTestAccess::CommandFeedbackText(shell) +
          "]")
             .c_str());
  Expect(WorkspaceShellTestAccess::CommandFeedbackText(shell) == "Patch applied",
         ("a stage that applied says so; feedback=[" +
          WorkspaceShellTestAccess::CommandFeedbackText(shell) + "]")
             .c_str());

  WorkspaceShellTestAccess::RefreshGitSidebar(shell);
  WorkspaceShellTestAccess::UnstageCompareHunk(shell);
  for (int round = 0; round < 3; ++round) {
    WorkspaceShellTestAccess::DrainProjectBackgroundExecutor(shell);
    WorkspaceShellTestAccess::ConsumeGitSidebarRefresh(shell);
  }
  Expect(index_content() == base,
         ("an unstage right after the stage unstages it; index=[" + index_content() +
          "] feedback=[" + WorkspaceShellTestAccess::CommandFeedbackText(shell) + "]")
             .c_str());
}

void TestWorkspaceShellCompareSelectedRangesStageAndRevertFromAWorkingTreeDiff() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "f.txt";
  WriteFile(source, "}\n\neps\n");
  InitializeGitRepo(root);
  CommitAll(root, "base", "base");
  WriteFile(source, "alpha\nalpha\n}\n\neps\nbeta\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::RegisterLifecycleWakeEvents(shell);
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, source, "HEAD", "HEAD"),
         "working-tree comparison should open");
  SoftwareCanvas canvas(1280, 720);
  shell.Render(canvas.renderer(), 1280, 720);
  WorkspaceShellTestAccess::RefreshGitSidebar(shell);
  WorkspaceShellTestAccess::DrainProjectBackgroundExecutor(shell);
  WorkspaceShellTestAccess::ConsumeGitSidebarRefresh(shell);
  const auto settle = [&]() {
    for (int i = 0; i < 2; ++i) {
      WorkspaceShellTestAccess::DrainProjectBackgroundExecutor(shell);
      WorkspaceShellTestAccess::ConsumeGitSidebarRefresh(shell);
    }
  };
  const auto index_content = [&]() {
    microide::project::GitRepository repo(root, microide::platform::LocalProcessLauncher());
    return repo.Execute({"show", ":f.txt"}).output;
  };
  Expect(WorkspaceShellTestAccess::ActiveCompare(shell).right_editable,
         "a working-tree diff's right side is editable (where the review letters type)");

  Expect(SendKeyDown(shell, SDLK_K, SDL_KMOD_CTRL), "Ctrl+K arms the chord");
  Expect(SendKeyDown(shell, SDLK_S, static_cast<SDL_Keymod>(SDL_KMOD_CTRL | SDL_KMOD_ALT)),
         "Ctrl+K Ctrl+Alt+S should be handled");
  settle();
  Expect(index_content() == "alpha\nalpha\n}\n\neps\n",
         "Ctrl+K Ctrl+Alt+S stages the change at the caret and nothing else; index=[" + index_content() +
             "] feedback=[" + WorkspaceShellTestAccess::CommandFeedbackText(shell) + "]");

  {
    auto& compare = WorkspaceShellTestAccess::ActiveCompare(shell);
    Expect(!compare.model.hunks.empty(), "the tail change is still in the diff");
    compare.selected_row = microide::compare::ComparePresentationRowForModelRow(
        compare.presentation, static_cast<std::size_t>(compare.model.hunks.back().start_row));
  }
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "git-revert-selected-ranges"),
         "git-revert-selected-ranges should run in a diff");
  Expect(WorkspaceShellTestAccess::PromptSurfaceVisible(shell),
         "reverting asks first (the discard preview)");
}

void TestWorkspaceShellCompareHunkJumpMovesTheEditableCaret() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "main.cpp";

  std::string committed;
  for (int line = 0; line < 400; ++line) {
    committed += "line " + std::to_string(line) + "\n";
  }
  WriteFile(source, committed);
  InitializeGitRepo(root);
  CommitAll(root, "Add hunk-jump fixture", "hunk jump fixture");

  std::string edited;
  for (int line = 0; line < 400; ++line) {
    if (line == 50 || line == 200 || line == 350) {
      edited += "changed " + std::to_string(line) + "\n";
    } else {
      edited += "line " + std::to_string(line) + "\n";
    }
  }
  WriteFile(source, edited);

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, source, "HEAD", "HEAD"),
         "working-tree comparison should open");

  SoftwareCanvas canvas(1280, 720);
  shell.Render(canvas.renderer(), 1280, 720);

  auto& compare = WorkspaceShellTestAccess::ActiveCompare(shell);
  Expect(compare.model.hunks.size() >= 3, "the fixture should produce several hunks");

  Expect(SendKeyDown(shell, SDLK_RIGHTBRACKET, SDL_KMOD_ALT),
         "Alt+] should navigate to the next hunk");
  Expect(SendKeyDown(shell, SDLK_RIGHTBRACKET, SDL_KMOD_ALT),
         "Alt+] should navigate to the next hunk");
  shell.Render(canvas.renderer(), 1280, 720);

  auto& jumped = WorkspaceShellTestAccess::ActiveCompare(shell);
  const std::size_t selected_model_row =
      microide::compare::ComparePresentationToModelRow(jumped.presentation, jumped.selected_row);
  const std::size_t selected_right_line =
      microide::workspace::CompareTabRightLineForModelRow(jumped, selected_model_row);
  Expect(selected_right_line > 0,
         "the fixture's later hunks are not at line 0, so the jump must leave line 0");
  Expect(jumped.right_viewport.cursor_line() == selected_right_line,
         "a hunk jump should carry the editable pane's caret to the selected change");

  Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "X"),
         "text input should reach the compare current-state pane");
  Expect(jumped.right_viewport.lines()[selected_right_line].rfind("X", 0) == 0,
         "typing after a hunk jump should edit the hunk the reader is looking at");
}

void TestWorkspaceShellCompareHunkJumpFromBetweenChangesGoesToTheNearestOne() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "main.cpp";
  std::string committed;
  std::string edited;
  for (int line = 0; line < 400; ++line) {
    committed += "line " + std::to_string(line) + "\n";
    edited += (line == 50 || line == 200 || line == 350 ? "changed " : "line ") + std::to_string(line) + "\n";
  }
  WriteFile(source, committed);
  InitializeGitRepo(root);
  CommitAll(root, "Add hunk-jump fixture", "hunk jump fixture");
  WriteFile(source, edited);

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, source, "HEAD", "HEAD"),
         "working-tree comparison should open");
  SoftwareCanvas canvas(1280, 720);
  shell.Render(canvas.renderer(), 1280, 720);
  auto& compare = WorkspaceShellTestAccess::ActiveCompare(shell);
  if (compare.model.hunks.size() < 3) {
    Expect(false, "the fixture should produce three hunks");
    return;
  }
  const auto selected_hunk = [&]() { return microide::workspace::CompareTabSelectedHunkIndex(compare); };
  const auto context_row_after = [&](std::size_t after) -> std::optional<std::size_t> {
    for (std::size_t row = 0; row < compare.presentation.rows.size(); ++row) {
      if (compare.presentation.rows[row].kind != microide::compare::ComparePresentationRowKind::Model) {
        continue;
      }
      const std::size_t model = microide::compare::ComparePresentationToModelRow(compare.presentation, row);
      if (model > after && model < compare.model.rows.size() && compare.model.rows[model].hunk < 0) {
        return row;
      }
    }
    return std::nullopt;
  };

  compare.selected_row = 0;
  Expect(selected_hunk() < 0, "the top row is in no change");
  Expect(SendKeyDown(shell, SDLK_RIGHTBRACKET, SDL_KMOD_ALT), "Alt+] runs");
  Expect(selected_hunk() == compare.model.hunks[0].index, "from above the first change, ] goes to the first");

  const auto between = context_row_after(static_cast<std::size_t>(compare.model.hunks[1].start_row));
  Expect(between.has_value(), "there is a context row between the second and third changes");
  if (!between.has_value()) {
    return;
  }
  compare.selected_row = *between;
  Expect(SendKeyDown(shell, SDLK_RIGHTBRACKET, SDL_KMOD_ALT), "Alt+] runs");
  Expect(selected_hunk() == compare.model.hunks[2].index,
         "from between the second and third, ] goes to the third");
  compare.selected_row = *between;
  Expect(SendKeyDown(shell, SDLK_LEFTBRACKET, SDL_KMOD_ALT), "Alt+[ runs");
  Expect(selected_hunk() == compare.model.hunks[1].index,
         "and [ goes back to the second");
}

void TestWorkspaceShellWorkingTreeCompareIsEditableAndSaves() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "main.cpp";
  WriteFile(source, "int alpha() {\n  return 1;\n}\n");

  InitializeGitRepo(root);
  CommitAll(root, "Add compare edit fixture", "compare edit fixture");
  WriteFile(source, "int beta() {\n  return 2;\n}\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, source, "HEAD", "HEAD"),
         "working-tree comparison should open");

  auto& compare = WorkspaceShellTestAccess::ActiveCompare(shell);
  Expect(compare.right_editable,
         "working-tree comparison should expose an editable current-state pane");
  Expect(compare.right_view_active,
         "working-tree comparison should focus the editable current-state pane");

  Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "// note "),
         "text input should edit the compare current-state pane");
  Expect(compare.right_viewport.dirty(),
         "editing the compare current-state pane should mark the tab dirty");
  Expect(WorkspaceShellTestAccess::SaveTab(shell, 0),
         "saving the compare tab should write the current-state buffer");
  Expect(!compare.right_viewport.dirty(),
         "saving the compare tab should clear the dirty state");
  Expect(ReadFile(source).rfind("// note ", 0) == 0,
         "saving the compare tab should persist the edited current-state text");
}

void TestWorkspaceShellCompareEditablePaneTakesMultipleCarets() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "main.cpp";
  WriteFile(source, "alpha\nbravo\ncharlie\ndelta\n");

  InitializeGitRepo(root);
  CommitAll(root, "Add multi-caret compare fixture", "multi-caret compare fixture");
  WriteFile(source, "alpha\nbravo\ncharlie\ndelta\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, source, "HEAD", "HEAD"),
         "working-tree comparison should open");

  auto& compare = WorkspaceShellTestAccess::ActiveCompare(shell);
  Expect(compare.right_editable && compare.right_view_active,
         "the editable pane must be active, or the carets have nowhere to land");
  auto& viewport = compare.right_viewport;

  viewport.MoveCursorTo(0, 0);
  viewport.AddSecondaryCaret(1, 0);
  viewport.AddSecondaryCaret(2, 0);
  Expect(viewport.secondary_caret_range_view().size() == 2,
         "three carets, one per line");

  Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "// "),
         "text input should reach the compare pane with several carets");
  Expect(viewport.lines()[0] == "// alpha" && viewport.lines()[1] == "// bravo" &&
             viewport.lines()[2] == "// charlie" && viewport.lines()[3] == "delta",
         "every caret edits, and the caretless line is untouched");
  Expect(viewport.secondary_caret_range_view().size() == 2,
         "the caret set survives the edit");

  const std::size_t lines_before = viewport.line_count();
  Expect(WorkspaceShellTestAccess::HandleKeyDown(shell, SDLK_RETURN, SDL_KMOD_NONE),
         "Enter should reach the compare pane");
  Expect(viewport.line_count() == lines_before + 3,
         "three carets each split their line, got " + std::to_string(viewport.line_count()) +
             " from " + std::to_string(lines_before));

  WorkspaceShellTestAccess::RenderFrame(shell);
  Expect(viewport.cursor_line() < viewport.line_count(),
         "the primary caret is inside the edited buffer");
  for (const auto& caret : viewport.secondary_caret_range_view()) {
    Expect(caret.position.line < viewport.line_count(),
           "every secondary caret is inside the edited buffer");
  }

  Expect(viewport.Undo(), "undo steps the compare pane's shared history");
  Expect(viewport.line_count() == lines_before,
         "one undo takes back all three splits, got " + std::to_string(viewport.line_count()));
}

void TestWorkspaceShellCompareLeftSideIsRecognizedByIdentity() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "a.txt";
  WriteFile(source, "one\ntwo\n");
  InitializeGitRepo(root);
  CommitAll(root, "base", "base");
  WriteFile(source, "one\nTWO\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, source, "HEAD", "HEAD"),
         "the comparison should open");
  auto& compare = WorkspaceShellTestAccess::ActiveCompare(shell);

  const std::uint64_t built = compare.model_revision;
  WorkspaceShellTestAccess::RefreshActiveCompareDerivedState(shell);
  Expect(compare.model_revision == built, "an unchanged left buffer does not rebuild the model");

  compare.left_content = compare::MakeCompareText(std::string(*compare.left_content));
  WorkspaceShellTestAccess::RefreshActiveCompareDerivedState(shell);
  Expect(compare.model_revision != built, "a new left buffer rebuilds, whatever its bytes");

  compare.left_content = compare::MakeCompareText("zero\n");
  WorkspaceShellTestAccess::RefreshActiveCompareDerivedState(shell);
  bool saw_new_left = false;
  for (const auto& row : compare.model.rows) {
    saw_new_left = saw_new_left || row.left_text == "zero";
  }
  Expect(saw_new_left, "and the rebuilt model is diffed against the new left side");
}

void TestWorkspaceShellExternalChangeRefreshesACompareInAnUnfocusedPane() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "a.txt";
  const std::filesystem::path other = root / "b.txt";
  WriteFile(source, "one\ntwo\n");
  WriteFile(other, "other\n");
  InitializeGitRepo(root);
  CommitAll(root, "base", "base");
  WriteFile(source, "one\nTWO\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, other), "open a file to split");
  const std::size_t compare_group = WorkspaceShellTestAccess::FocusedGroupIndex(shell);
  Expect(WorkspaceShellTestAccess::SplitEditorGroup(
             shell, microide::workspace::EditorSplitOrientation::Vertical),
         "split the editor area");
  const std::size_t other_group = WorkspaceShellTestAccess::FocusedGroupIndex(shell);
  Expect(other_group != compare_group, "the split made a second pane");
  WorkspaceShellTestAccess::FocusEditorGroup(shell, compare_group);
  Expect(WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, source, "HEAD", "HEAD"),
         "the comparison should open in the first pane");
  WorkspaceShellTestAccess::FocusEditorGroup(shell, other_group);

  WriteFile(source, "one\nTHREE\n");
  WorkspaceShellTestAccess::MarkCompareTabsStaleForPath(shell, source);
  WorkspaceShellTestAccess::FlushPendingFileReads(shell);

  const auto* compare = WorkspaceShellTestAccess::GroupActiveCompareOrNull(shell, compare_group);
  Expect(compare != nullptr, "the unfocused pane still shows the compare");
  bool saw_new_text = false;
  for (const auto& row : compare->model.rows) {
    saw_new_text = saw_new_text || row.right_text == "THREE";
  }
  Expect(saw_new_text, "the unfocused pane's compare was rebuilt from the changed file");
}

class ShellThreadSpawnCounter final : public microide::platform::ProcessLauncher {
 public:
  std::vector<std::string> ResolveArgv(std::vector<std::string> argv) const override {
    return local().ResolveArgv(std::move(argv));
  }
  std::filesystem::path ResolveWorkingDirectory(std::filesystem::path cwd) const override {
    return local().ResolveWorkingDirectory(std::move(cwd));
  }
  std::filesystem::path LocalPathFromHost(std::filesystem::path host_path) const override {
    return local().LocalPathFromHost(std::move(host_path));
  }
  microide::platform::SubprocessResult Run(std::vector<std::string> argv,
                                           microide::platform::SubprocessOptions options) const override {
    if (std::this_thread::get_id() == shell_thread_) {
      shell_thread_spawns_.fetch_add(1);
    }
    return local().Run(std::move(argv), std::move(options));
  }
  bool is_local() const override { return true; }
  std::string_view description() const override { return "local (shell spawns counted)"; }
  int shell_thread_spawns() const { return shell_thread_spawns_.load(); }

 private:
  static const microide::platform::ProcessLauncher& local() {
    return microide::platform::LocalProcessLauncher();
  }
  std::thread::id shell_thread_ = std::this_thread::get_id();
  mutable std::atomic<int> shell_thread_spawns_{0};
};

void TestWorkspaceShellExternalChangeReadsNoRefOnTheShellThread() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "a.txt";
  WriteFile(source, "one\ntwo\n");
  InitializeGitRepo(root);
  CommitAll(root, "base", "base");
  const microide::project::GitRepository repo(root, microide::platform::LocalProcessLauncher());
  Expect(repo.Execute({"branch", "side"}).exit_code == 0, "a second branch at the base");
  WriteFile(source, "one\nTWO\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::OpenBranchHeadComparison(shell, source, "side", "side", "HEAD", "HEAD"),
         "the branch comparison opens");
  Expect(WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, source, "HEAD", "HEAD"),
         "the working-tree comparison against HEAD opens");
  ShellThreadSpawnCounter counter;
  const microide::platform::ProcessLauncher* real_launcher =
      WorkspaceShellTestAccess::CurrentProjectState(shell).locality.launcher;
  WorkspaceShellTestAccess::CurrentProjectState(shell).locality.launcher = &counter;

  WriteFile(source, "one\nTHREE\n");
  CommitAll(root, "three", "three");
  WorkspaceShellTestAccess::MarkCompareTabsStaleForPath(shell, source);
  Expect(counter.shell_thread_spawns() == 0,
         "no git ran on the shell thread for the change (" + std::to_string(counter.shell_thread_spawns()) + ")");
  const auto* compare = WorkspaceShellTestAccess::GroupActiveCompareOrNull(
      shell, WorkspaceShellTestAccess::FocusedGroupIndex(shell));
  Expect(compare != nullptr && compare->right_viewport.lines().LineView(1) == "TWO",
         "until the read lands, the diff on screen is the one it had");

  WorkspaceShellTestAccess::FlushPendingFileReads(shell);
  Expect(counter.shell_thread_spawns() == 0, "and none ran when it landed");
  const auto row_texts = [](const microide::workspace::CompareTabState& tab, bool left) {
    std::string out;
    for (const auto& row : tab.model.rows) {
      out += std::string(left ? row.left_text : row.right_text) + "|";
    }
    return out;
  };
  compare = WorkspaceShellTestAccess::GroupActiveCompareOrNull(shell, WorkspaceShellTestAccess::FocusedGroupIndex(shell));
  Expect(compare != nullptr && compare->right_ref == "WORKTREE", "the working-tree diff is in front");
  Expect(compare != nullptr && row_texts(*compare, true).find("THREE") != std::string::npos &&
             row_texts(*compare, false).find("THREE") != std::string::npos && compare->model.hunks.empty(),
         "its HEAD side is the new commit and it shows no change: " +
             (compare != nullptr ? row_texts(*compare, true) : std::string{}));
  WorkspaceShellTestAccess::ActivateTab(shell, 0);
  compare = WorkspaceShellTestAccess::GroupActiveCompareOrNull(shell, WorkspaceShellTestAccess::FocusedGroupIndex(shell));
  Expect(compare != nullptr && compare->right_ref == "HEAD", "the branch diff");
  Expect(compare != nullptr && row_texts(*compare, false).find("THREE") != std::string::npos &&
             row_texts(*compare, true).find("two") != std::string::npos,
         "its HEAD side moved with the commit, its branch side did not: " +
             (compare != nullptr ? row_texts(*compare, true) + " / " + row_texts(*compare, false) : std::string{}));
  WorkspaceShellTestAccess::CurrentProjectState(shell).locality.launcher = real_launcher;
}

void TestWorkspaceShellExternalChangeReadsNoCommittedSideAgain() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "a.txt";
  WriteFile(source, "one\ntwo\n");
  InitializeGitRepo(root);
  CommitAll(root, "base", "base");
  WriteFile(source, "one\nTWO\n");
  const microide::project::GitRepository repo(root, microide::platform::LocalProcessLauncher());
  std::string head = repo.Execute({"rev-parse", "HEAD"}).output;
  head.erase(head.find_last_not_of(" \n\r") + 1);
  Expect(head.size() == 40, "HEAD resolves to a full object id: " + head);

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  for (int tab = 0; tab < 3; ++tab) {
    Expect(WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, source, head, head.substr(0, 7)),
           "the comparison against the commit opens");
  }
  WriteFile(source, "one\nTHREE\n");
  const std::uint64_t reads_before = util::ReadPerformanceCounter(util::PerfCounterId::GitDiffLoads);
  WorkspaceShellTestAccess::MarkCompareTabsStaleForPath(shell, source);
  Expect(util::ReadPerformanceCounter(util::PerfCounterId::GitDiffLoads) == reads_before,
         "the committed side is not read from git again");
  const auto* compare = WorkspaceShellTestAccess::GroupActiveCompareOrNull(
      shell, WorkspaceShellTestAccess::FocusedGroupIndex(shell));
  Expect(compare != nullptr, "the compare is still open");
  bool left_kept = false;
  bool right_new = false;
  for (const auto& row : compare->model.rows) {
    left_kept = left_kept || row.left_text == "two";
    right_new = right_new || row.right_text == "THREE";
  }
  Expect(left_kept && right_new, "the committed side is kept and the worktree side is the changed file");
}

void TestWorkspaceShellWorkingTreeCompareSharesTheEditorTabsBuffer() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "main.cpp";
  WriteFile(source, "int alpha() {\n  return 1;\n}\n");
  InitializeGitRepo(root);
  CommitAll(root, "Add shared-buffer fixture", "shared-buffer fixture");
  WriteFile(source, "int beta() {\n  return 2;\n}\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);

  WorkspaceShellTestAccess::OpenFile(shell, source);
  WorkspaceShellTestAccess::ActiveEditor(shell).MoveCursorTo(0, 0);
  Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "// edited\n"), "edit the editor tab");
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).dirty(), "the editor tab is dirty");

  Expect(WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, source, "HEAD", "HEAD"),
         "the working-tree comparison should open");
  editor::TextViewport& editor = WorkspaceShellTestAccess::FocusedGroupTabEditor(shell, 0);
  auto& compare = WorkspaceShellTestAccess::ActiveCompare(shell);
  Expect(compare.right_editable, "the working-tree side is editable");
  Expect(compare.right_viewport.SharesDocumentWith(editor),
         "the compare's editable side must be a view of the editor tab's buffer");
  Expect(compare.right_viewport.lines().LineView(0) == "// edited",
         "the compare shows the unsaved buffer, not the file on disk");
  bool first_row_is_edit = false;
  for (const auto& row : compare.model.rows) {
    if (row.right_text == "// edited") {
      first_row_is_edit = true;
      break;
    }
  }
  Expect(first_row_is_edit, "the diff model is built from the shared buffer");

  Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "X"), "edit through the compare side");
  Expect(editor.lines().LineView(0) == "X// edited", "the editor tab sees the compare's edit");
  const std::uint64_t model_before = compare.model_revision;
  editor.MoveCursorTo(0, 0);
  editor.InsertText("Y");
  Expect(compare.right_viewport.lines().LineView(0) == "YX// edited",
         "the compare side sees the editor's edit");
  WorkspaceShellTestAccess::RefreshActiveCompareDerivedState(shell);
  Expect(compare.model_revision != model_before, "the diff model follows the shared buffer");

  Expect(WorkspaceShellTestAccess::SaveTab(shell, WorkspaceShellTestAccess::GroupActiveTabIndex(shell, 0)),
         "saving the compare tab should succeed");
  Expect(!editor.dirty() && !compare.right_viewport.dirty(), "one save cleans both views");
  Expect(ReadFile(source).rfind("YX// edited\n", 0) == 0, "the save wrote the shared buffer");
  Expect(!WorkspaceShellTestAccess::HasExternalChangeBanner(shell, source),
         "saving one view of the buffer is not an external change to the other");

  WorkspaceShell reverse;
  WorkspaceShellTestAccess::SetProjectRoot(reverse, root);
  WorkspaceShellTestAccess::SetWindowSize(reverse, 1280, 720);
  Expect(WorkspaceShellTestAccess::OpenWorkingTreeComparison(reverse, source, "HEAD", "HEAD"),
         "the comparison should open first");
  Expect(WorkspaceShellTestAccess::HandleTextInput(reverse, "Z"), "edit the compare side");
  Expect(WorkspaceShellTestAccess::OpenFileInNewTab(reverse, source), "then open the editor tab");
  const editor::TextViewport& late_editor = WorkspaceShellTestAccess::ActiveEditor(reverse);
  Expect(late_editor.lines().LineView(0) == "ZYX// edited" && late_editor.dirty(),
         "the editor tab opens on the compare's dirty buffer");
  bool shares = false;
  for (std::size_t ti = 0; ti < WorkspaceShellTestAccess::GroupTabCount(reverse, 0); ++ti) {
    const editor::TextViewport* view = WorkspaceShellTestAccess::GroupTabViewport(reverse, 0, ti);
    if (view != nullptr && view != &late_editor && view->SharesDocumentWith(late_editor)) {
      shares = true;
    }
  }
  Expect(shares, "the editor tab and the compare's side are views of one document");
}

void TestWorkspaceShellWorkingTreeCompareFollowsAnExternalReload() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "notes.txt";
  WriteFile(source, "first\nsecond\n");
  InitializeGitRepo(root);
  CommitAll(root, "Add reload fixture", "reload fixture");
  WriteFile(source, "first\nsecond\nthird\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::OpenFile(shell, source);
  Expect(WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, source, "HEAD", "HEAD"),
         "the working-tree comparison should open");
  Expect(WorkspaceShellTestAccess::GroupTabCount(shell, 0) == 2, "an editor tab and a compare tab");
  const editor::TextViewport* editor = WorkspaceShellTestAccess::GroupTabViewport(shell, 0, 0);
  const editor::TextViewport* compare = WorkspaceShellTestAccess::GroupTabViewport(shell, 0, 1);
  Expect(editor != nullptr && compare != nullptr && compare->SharesDocumentWith(*editor),
         "the two tabs start as views of one document");

  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  WriteFile(source, "changed on disk\nsecond\nthird\n");
  Expect(WorkspaceShellTestAccess::ReloadProjectIfFilesChanged(shell, true),
         "the forced project check should notice the modified file");
  editor = WorkspaceShellTestAccess::GroupTabViewport(shell, 0, 0);
  compare = WorkspaceShellTestAccess::GroupTabViewport(shell, 0, 1);
  Expect(editor != nullptr && editor->lines().LineView(0) == "changed on disk",
         "the editor tab reloads the changed file");
  Expect(compare != nullptr && compare->lines().LineView(0) == "changed on disk",
         "the compare's editable side shows the changed file too");
  Expect(compare != nullptr && editor != nullptr && compare->SharesDocumentWith(*editor),
         "after the reload the two tabs are still views of ONE document");
}

void TestWorkspaceShellWorkingTreeCompareKeepsUtf8Bom() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "notes.txt";
  WriteFile(source, "\xEF\xBB\xBF" "first\nsecond\n");

  InitializeGitRepo(root);
  CommitAll(root, "Add BOM fixture", "BOM fixture");
  WriteFile(source, "\xEF\xBB\xBF" "first\nsecond\nthird\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, source, "HEAD", "HEAD"),
         "working-tree comparison should open");

  auto& compare = WorkspaceShellTestAccess::ActiveCompare(shell);
  Expect(compare.right_viewport.has_utf8_bom() &&
             compare.right_viewport.lines().LineView(0) == "first",
         "the editable pane strips the mark");
  std::size_t changed_rows = 0;
  for (const auto& row : compare.model.rows) {
    if (row.kind != microide::compare::CompareRowKind::Unchanged) {
      ++changed_rows;
    }
  }
  Expect(changed_rows == 1 && !compare.model.rows.empty() &&
             compare.model.rows[0].kind == microide::compare::CompareRowKind::Unchanged &&
             compare.model.rows[0].left_text == compare.model.rows[0].right_text,
         "only the appended line differs; line 1 is not a phantom change, got " +
             std::to_string(changed_rows) + " changed rows over " +
             std::to_string(compare.model.rows.size()) + ", row 0 left [" +
             std::string(compare.model.rows[0].left_text) + "] right [" +
             std::string(compare.model.rows[0].right_text) + "]");

  Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "> "),
         "text input should edit the compare current-state pane");
  Expect(WorkspaceShellTestAccess::SaveTab(shell, 0), "saving the compare tab should write");
  Expect(ReadFile(source) == "\xEF\xBB\xBF" "first\nsecond\n> third\n",
         "saving the pane writes the mark back ahead of the text");
}

void TestWorkspaceShellCompareEditablePaneIndentsAndOutdents() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "main.cpp";
  WriteFile(source, "int alpha() {\n  return 1;\n}\n");

  InitializeGitRepo(root);
  CommitAll(root, "Add compare indent fixture", "compare indent fixture");
  WriteFile(source, "alpha\nbravo\ncharlie\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, source, "HEAD", "HEAD"),
         "working-tree comparison should open");

  auto& compare = WorkspaceShellTestAccess::ActiveCompare(shell);
  Expect(compare.right_editable && compare.right_view_active,
         "the compare fixture should start on the editable pane");

  compare.right_viewport.MoveCursorTo(0, 0);
  compare.right_viewport.MoveCursorTo(1, 5, true);
  Expect(SendKeyDown(shell, SDLK_TAB, SDL_KMOD_NONE),
         "Tab should be handled on the compare editable pane");
  const std::string_view indented = compare.right_viewport.lines().LineView(0);
  Expect(indented.size() > 5 && indented.ends_with("alpha") &&
             indented.find_first_not_of(" \t") == indented.size() - 5,
         "Tab with a multi-line selection indents the block instead of replacing it");
  Expect(compare.right_viewport.lines().LineView(1).find("bravo") != std::string::npos,
         "the second selected line survives the block indent");

  Expect(SendKeyDown(shell, SDLK_TAB, SDL_KMOD_SHIFT),
         "Shift+Tab should be handled on the compare editable pane");
  Expect(compare.right_viewport.lines().LineView(0) == "alpha",
         "Shift+Tab outdents the block back rather than inserting a tab");
}

void TestWorkspaceShellComparePaintsPluginDecorations() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "main.cpp";
  WriteFile(source, "int alpha() {\n  return 1;\n}\n");
  InitializeGitRepo(root);
  CommitAll(root, "Add decoration fixture", "decoration fixture");
  WriteFile(source, "int beta(int x) {\n  return (x + 1);\n}\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, source, "HEAD", "HEAD"),
         "working-tree comparison should open");

  constexpr SDL_Color kStyleBackground{13, 240, 111, 255};
  constexpr SDL_Color kMarkColor{240, 13, 200, 255};
  SoftwareCanvas canvas(1280, 720);
  const auto canvas_contains = [&canvas](SDL_Color wanted) {
    for (int y = 0; y < 720; ++y) {
      for (int x = 0; x < 1280; ++x) {
        const SDL_Color pixel = canvas.PixelAt(x, y);
        if (pixel.r == wanted.r && pixel.g == wanted.g && pixel.b == wanted.b) {
          return true;
        }
      }
    }
    return false;
  };

  WorkspaceShellTestAccess::RenderFrameWithRenderer(shell, canvas.renderer());
  Expect(!canvas_contains(kStyleBackground) && !canvas_contains(kMarkColor),
         "the probe colors are absent before anything is published");

  auto& project_state = WorkspaceShellTestAccess::CurrentProjectState(shell);
  editor::PluginDecorationData data;
  data.text_styles.push_back(editor::TextStyleDecoration{
      .line = 1,
      .start_column = 0,
      .end_column = 0,
      .background = kStyleBackground,
      .flags = editor::kDecorationWholeLine,
  });
  data.gutter_marks.push_back(editor::GutterMarkDecoration{
      .line = 1,
      .shape = editor::GutterIconShape::Dot,
      .color = kMarkColor,
      .priority = 1,
  });
  Expect(project_state.EnsurePluginPresentation().decorations.ReplaceForOwnerFile(
             "decoration-test", source, std::move(data)),
         "publishing decorations for the compared file should change the store");

  WorkspaceShellTestAccess::RenderFrameWithRenderer(shell, canvas.renderer());
  Expect(canvas_contains(kStyleBackground),
         "a plugin text style paints on the editable compare pane");
  Expect(canvas_contains(kMarkColor),
         "a plugin gutter mark paints in the editable pane's gutter");
}

void TestWorkspaceShellCompareBracketMatchIsMemoized() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "main.cpp";
  WriteFile(source, "int alpha() {\n  return 1;\n}\n");

  InitializeGitRepo(root);
  CommitAll(root, "Add bracket fixture", "bracket fixture");
  WriteFile(source, "int beta(int x) {\n  return (x + 1);\n}\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, source, "HEAD", "HEAD"),
         "working-tree comparison should open");

  auto& compare = WorkspaceShellTestAccess::ActiveCompare(shell);
  Expect(compare.right_view_active, "the editable pane is active");
  Expect(!compare.bracket_match_valid, "the memo starts cold");

  SoftwareCanvas canvas(1280, 720);

  compare.right_viewport.MoveCursorTo(1, 9);
  WorkspaceShellTestAccess::RenderFrameWithRenderer(shell, canvas.renderer());
  Expect(compare.bracket_match_valid, "painting resolves and memoizes the bracket match");
  const auto first_pair = compare.bracket_match_pair;
  const std::uint64_t revision_after_first = compare.bracket_match_content_revision;

  WorkspaceShellTestAccess::RenderFrameWithRenderer(shell, canvas.renderer());
  Expect(compare.bracket_match_content_revision == revision_after_first &&
             compare.bracket_match_caret_line == 1 && compare.bracket_match_caret_column == 9,
         "an unchanged repaint keeps the memo key");
  Expect(compare.bracket_match_pair.has_value() == first_pair.has_value(),
         "an unchanged repaint keeps the memoized answer");

  compare.right_viewport.MoveCursorTo(0, 0);
  WorkspaceShellTestAccess::RenderFrameWithRenderer(shell, canvas.renderer());
  Expect(compare.bracket_match_caret_line == 0 && compare.bracket_match_caret_column == 0,
         "moving the caret re-keys the memo rather than serving the old match");
}

void TestWorkspaceShellCompareAltOOpensTheWorkingFile() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "main.cpp";
  WriteFile(source, "int alpha() {\n  return 1;\n}\n");
  InitializeGitRepo(root);
  CommitAll(root, "Add compare open fixture", "compare open fixture");
  WriteFile(source, "int beta() {\n  return 2;\n}\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, source, "HEAD", "HEAD"),
         "the compare open fixture should open its diff");
  Expect(WorkspaceShellTestAccess::ActiveTabIsCompare(shell), "the diff should be the active tab");

  Expect(SendKeyDown(shell, SDLK_O, SDL_KMOD_ALT), "Alt+O on a diff should be handled");
  Expect(!WorkspaceShellTestAccess::ActiveTabIsCompare(shell), "Alt+O should activate an editor tab");
  const editor::TextViewport* viewport = WorkspaceShellTestAccess::ActiveEditorOrNull(shell);
  Expect(viewport != nullptr && viewport->path() == source,
         "Alt+O should open the diff's working file");
}

void TestWorkspaceShellCompareClickTogglesEditablePaneFocus() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "main.cpp";
  WriteFile(source, "int alpha() {\n  return 1;\n}\n");

  InitializeGitRepo(root);
  CommitAll(root, "Add compare click fixture", "compare click fixture");
  WriteFile(source, "int beta() {\n  return 2;\n}\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, source, "HEAD", "HEAD"),
         "compare click fixture should open");

  auto& compare = WorkspaceShellTestAccess::ActiveCompare(shell);
  const auto surface = WorkspaceShellTestAccess::ActiveCompareSurfaceLayout(shell);
  const float row_y = surface.rows_y + surface.line_height * 0.5f;
  const float left_x = surface.left_x + 24.0f;
  const float right_x = surface.right_x + 24.0f;

  Expect(compare.right_view_active,
         "compare click fixture should start with the editable pane active");
  Expect(SendMouseDown(shell, left_x, row_y, SDL_BUTTON_LEFT),
         "clicking the compare left pane should be handled");
  Expect(!compare.right_view_active,
         "clicking the compare left pane should leave the editable pane inactive");

  Expect(SendMouseDown(shell, right_x, row_y, SDL_BUTTON_LEFT),
         "clicking the compare right pane should be handled");
  Expect(compare.right_view_active,
         "clicking the compare right pane should reactivate the editable pane");
}

void TestWorkspaceShellCompareClickAboveFirstRowIsNotHandled() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "main.cpp";
  WriteFile(source, "int alpha() {\n  return 1;\n}\n");

  InitializeGitRepo(root);
  CommitAll(root, "Add compare band fixture", "compare band fixture");
  WriteFile(source, "int beta() {\n  return 2;\n}\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, source, "HEAD", "HEAD"),
         "compare band fixture should open");

  auto& compare = WorkspaceShellTestAccess::ActiveCompare(shell);
  const auto surface = WorkspaceShellTestAccess::ActiveCompareSurfaceLayout(shell);
  const float left_x = surface.left_x + 24.0f;

  const float row2_y = surface.rows_y + surface.line_height * 2.5f;
  Expect(SendMouseDown(shell, left_x, row2_y, SDL_BUTTON_LEFT),
         "clicking a real compare row should be handled");
  const std::size_t selected_after_real_click = compare.selected_row;

  const float above_y = surface.rows_y - 2.0f;
  Expect(!SendMouseDown(shell, left_x, above_y, SDL_BUTTON_LEFT),
         "clicking just above the first compare row must not be handled as a row hit");
  Expect(compare.selected_row == selected_after_real_click,
         "a click above the first row must not move the compare selection to row 0");
}

void TestWorkspaceShellCompareCollapsedContextButtonsExpandHiddenRows() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "main.cpp";
  auto build_text = [](std::string_view first_change, std::string_view second_change) {
    std::string text;
    for (int i = 0; i < 24; ++i) {
      text += "prefix " + std::to_string(i) + "\n";
    }
    text += std::string(first_change) + "\n";
    for (int i = 0; i < 30; ++i) {
      text += "middle " + std::to_string(i) + "\n";
    }
    text += std::string(second_change) + "\n";
    for (int i = 0; i < 8; ++i) {
      text += "suffix " + std::to_string(i) + "\n";
    }
    return text;
  };
  WriteFile(source, build_text("left a", "left b"));

  InitializeGitRepo(root);
  CommitAll(root, "Add compare context fixture", "compare context fixture");
  WriteFile(source, build_text("right a", "right b"));

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, source, "HEAD", "HEAD"),
         "compare collapsed-context fixture should open");

  std::optional<std::size_t> collapsed_row;
  std::pair<std::size_t, std::size_t> collapsed_run_identity{0, 0};
  const std::size_t initial_row_count =
      WorkspaceShellTestAccess::ActiveComparePresentationRowCount(shell);
  for (std::size_t i = 0; i < initial_row_count; ++i) {
    if (WorkspaceShellTestAccess::ActiveComparePresentationRowKind(shell, i) !=
        microide::compare::ComparePresentationRowKind::CollapsedContext) {
      continue;
    }
    const auto action_rects =
        WorkspaceShellTestAccess::ActiveCompareCollapsedContextActionRects(shell, i);
    if (action_rects.previous_rect.has_value() && action_rects.next_rect.has_value()) {
      collapsed_row = i;
      collapsed_run_identity =
          WorkspaceShellTestAccess::ActiveCompareCollapsedRunIdentity(shell, i);
      break;
    }
  }
  Expect(collapsed_row.has_value(),
         "compare collapsed-context fixture should expose a middle hidden context row");

  auto& compare = WorkspaceShellTestAccess::ActiveCompare(shell);
  compare.scroll_row = std::max(0, static_cast<int>(*collapsed_row) - 2);
  const auto action_rects =
      WorkspaceShellTestAccess::ActiveCompareCollapsedContextActionRects(shell, *collapsed_row);
  Expect(action_rects.previous_rect.has_value(),
         "middle collapsed context row should expose a previous-context expansion button");
  Expect(ExpandCompareCollapsedContext(compare, *collapsed_row,
                                       microide::workspace::CompareCollapsedContextAction::ShowAll),
         "expanding collapsed context from the selected run should succeed");
  Expect(WorkspaceShellTestAccess::ActiveComparePresentationRowCount(shell) > initial_row_count,
         "expanding collapsed context should reveal additional rows");
  std::optional<std::size_t> updated_collapsed_row;
  const std::size_t updated_row_count =
      WorkspaceShellTestAccess::ActiveComparePresentationRowCount(shell);
  for (std::size_t i = 0; i < updated_row_count; ++i) {
    if (WorkspaceShellTestAccess::ActiveComparePresentationRowKind(shell, i) !=
        microide::compare::ComparePresentationRowKind::CollapsedContext) {
      continue;
    }
    if (WorkspaceShellTestAccess::ActiveCompareCollapsedRunIdentity(shell, i) ==
        collapsed_run_identity) {
      updated_collapsed_row = i;
      break;
    }
  }
  Expect(!updated_collapsed_row.has_value(),
         "Show all should fully expand the selected collapsed run");
}

void TestWorkspaceShellCompareCollapsedContextButtonsHoverAsInteractive() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "main.cpp";
  const auto build_text = [](std::string_view first_change, std::string_view second_change) {
    std::string text;
    for (int i = 0; i < 24; ++i) {
      text += "prefix " + std::to_string(i) + "\n";
    }
    text += std::string(first_change) + "\n";
    for (int i = 0; i < 30; ++i) {
      text += "middle " + std::to_string(i) + "\n";
    }
    text += std::string(second_change) + "\n";
    for (int i = 0; i < 8; ++i) {
      text += "suffix " + std::to_string(i) + "\n";
    }
    return text;
  };
  WriteFile(source, build_text("left a", "left b"));

  InitializeGitRepo(root);
  CommitAll(root, "Add compare hover fixture", "compare hover fixture");
  WriteFile(source, build_text("right a", "right b"));

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, source, "HEAD", "HEAD"),
         "compare collapsed-context hover fixture should open");

  std::optional<std::size_t> collapsed_row;
  const std::size_t row_count =
      WorkspaceShellTestAccess::ActiveComparePresentationRowCount(shell);
  for (std::size_t i = 0; i < row_count; ++i) {
    if (WorkspaceShellTestAccess::ActiveComparePresentationRowKind(shell, i) !=
        microide::compare::ComparePresentationRowKind::CollapsedContext) {
      continue;
    }
    const auto action_rects =
        WorkspaceShellTestAccess::ActiveCompareCollapsedContextActionRects(shell, i);
    if (action_rects.previous_rect.has_value() && action_rects.next_rect.has_value()) {
      collapsed_row = i;
      break;
    }
  }
  Expect(collapsed_row.has_value(),
         "compare collapsed-context hover fixture should expose a middle hidden context row");

  auto& compare = WorkspaceShellTestAccess::ActiveCompare(shell);
  compare.scroll_row = std::max(0, static_cast<int>(*collapsed_row) - 2);
  const auto surface = WorkspaceShellTestAccess::ActiveCompareSurfaceLayout(shell);
  const auto action_rects =
      WorkspaceShellTestAccess::ActiveCompareCollapsedContextActionRects(shell, *collapsed_row);
  const float hover_x = action_rects.all_rect.x + action_rects.all_rect.w * 0.5f;
  const float hover_y = action_rects.all_rect.y + action_rects.all_rect.h * 0.5f;
  Expect(WorkspaceShellTestAccess::CursorKindAtIsPointer(shell, hover_x, hover_y),
         "collapsed-context action buttons should advertise a pointer cursor");
  SendMouseMotion(shell, hover_x, hover_y, 0);
  Expect(WorkspaceShellTestAccess::CachedCursorIsPointer(shell),
         "hovering a collapsed-context action button should cache the pointer cursor");

  const microide::workspace::CompareHoverKind kinds[] = {
      microide::workspace::CompareHoverKind::CollapsedContextPreviousAction,
      microide::workspace::CompareHoverKind::CollapsedContextAllAction,
      microide::workspace::CompareHoverKind::CollapsedContextNextAction,
  };
  for (const microide::workspace::CompareHoverKind expected_kind : kinds) {
    const auto rects =
        WorkspaceShellTestAccess::ActiveCompareCollapsedContextActionRects(shell, *collapsed_row);
    const std::optional<SDL_FRect> rect =
        expected_kind == microide::workspace::CompareHoverKind::CollapsedContextPreviousAction
            ? rects.previous_rect
        : expected_kind == microide::workspace::CompareHoverKind::CollapsedContextNextAction
            ? rects.next_rect
            : std::optional<SDL_FRect>(rects.all_rect);
    Expect(rect.has_value() && rect->w > 0.0f,
           "the fixture row should expose all three action buttons");
    const float by = rect->y + rect->h * 0.5f;
    for (const float bx : {rect->x + 1.0f, rect->x + rect->w * 0.5f, rect->x + rect->w - 1.0f}) {
      Expect(WorkspaceShellTestAccess::CursorKindAtIsPointer(shell, bx, by),
             "every collapsed-context action button should advertise a pointer cursor");
      SendMouseMotion(shell, bx, by, 0);
      Expect(WorkspaceShellTestAccess::ActiveCompareHoverKind(shell) == expected_kind,
             "the hover highlight should resolve the same button the cursor points at");
    }
  }

  {
    const auto rects =
        WorkspaceShellTestAccess::ActiveCompareCollapsedContextActionRects(shell, *collapsed_row);
    const float gap_x = rects.all_rect.x - 6.0f;
    const float gap_y = rects.all_rect.y + rects.all_rect.h * 0.5f;
    if (gap_x > surface.left_x && (!rects.previous_rect.has_value() ||
                                   gap_x > rects.previous_rect->x + rects.previous_rect->w)) {
      SendMouseMotion(shell, gap_x, gap_y, 0);
      Expect(!WorkspaceShellTestAccess::ActiveCompareHoverKind(shell).has_value(),
             "the gap between action buttons should not highlight one");
      Expect(!WorkspaceShellTestAccess::CursorKindAtIsPointer(shell, gap_x, gap_y),
             "the cursor may only offer a hand where the hover highlight agrees");
    }
  }

  const float clear_x = surface.right_x + 12.0f;
  SendMouseMotion(shell, clear_x, hover_y, 0);
  Expect(WorkspaceShellTestAccess::CachedCursorIsText(shell),
         "ordinary editable compare content should restore the text cursor");
}

void TestWorkspaceShellCompareCollapsedContextExpansionPersistsAcrossChunks() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "main.cpp";
  const auto build_text = [](std::string_view first_change,
                             std::string_view second_change,
                             std::string_view third_change) {
    std::string text;
    for (int i = 0; i < 24; ++i) {
      text += "prefix " + std::to_string(i) + "\n";
    }
    text += std::string(first_change) + "\n";
    for (int i = 0; i < 30; ++i) {
      text += "middle-a " + std::to_string(i) + "\n";
    }
    text += std::string(second_change) + "\n";
    for (int i = 0; i < 30; ++i) {
      text += "middle-b " + std::to_string(i) + "\n";
    }
    text += std::string(third_change) + "\n";
    for (int i = 0; i < 8; ++i) {
      text += "suffix " + std::to_string(i) + "\n";
    }
    return text;
  };
  WriteFile(source, build_text("left a", "left b", "left c"));

  InitializeGitRepo(root);
  CommitAll(root, "Add compare multi-chunk fixture", "compare multi-chunk fixture");
  WriteFile(source, build_text("right a", "right b", "right c"));

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, source, "HEAD", "HEAD"),
         "compare multi-chunk fixture should open");

  const auto collect_middle_runs = [&](WorkspaceShell& current_shell) {
    std::vector<std::pair<std::size_t, std::size_t>> identities;
    const std::size_t row_count =
        WorkspaceShellTestAccess::ActiveComparePresentationRowCount(current_shell);
    for (std::size_t i = 0; i < row_count; ++i) {
      if (WorkspaceShellTestAccess::ActiveComparePresentationRowKind(current_shell, i) !=
          microide::compare::ComparePresentationRowKind::CollapsedContext) {
        continue;
      }
      const auto action_rects =
          WorkspaceShellTestAccess::ActiveCompareCollapsedContextActionRects(current_shell, i);
      if (action_rects.previous_rect.has_value() && action_rects.next_rect.has_value()) {
        identities.push_back(
            WorkspaceShellTestAccess::ActiveCompareCollapsedRunIdentity(current_shell, i));
      }
    }
    return identities;
  };
  const auto find_row = [&](WorkspaceShell& current_shell,
                            std::pair<std::size_t, std::size_t> identity) {
    const std::size_t row_count =
        WorkspaceShellTestAccess::ActiveComparePresentationRowCount(current_shell);
    for (std::size_t i = 0; i < row_count; ++i) {
      if (WorkspaceShellTestAccess::ActiveComparePresentationRowKind(current_shell, i) !=
          microide::compare::ComparePresentationRowKind::CollapsedContext) {
        continue;
      }
      if (WorkspaceShellTestAccess::ActiveCompareCollapsedRunIdentity(current_shell, i) ==
          identity) {
        return std::optional<std::size_t>(i);
      }
    }
    return std::optional<std::size_t>{};
  };
  const std::vector<std::pair<std::size_t, std::size_t>> middle_runs = collect_middle_runs(shell);
  Expect(middle_runs.size() >= 2,
         "compare multi-chunk fixture should expose at least two independently collapsed middle runs");

  auto& compare = WorkspaceShellTestAccess::ActiveCompare(shell);
  const std::optional<std::size_t> first_row = find_row(shell, middle_runs[0]);
  Expect(first_row.has_value(), "first collapsed middle run should be present before expansion");
  Expect(ExpandCompareCollapsedContext(compare, *first_row,
                                       microide::workspace::CompareCollapsedContextAction::ShowAll),
         "ShowAll should expand the first collapsed middle run");
  const std::size_t after_first_expand =
      WorkspaceShellTestAccess::ActiveComparePresentationRowCount(shell);
  Expect(!find_row(shell, middle_runs[0]).has_value(),
         "the first collapsed middle run should remain expanded");

  const std::optional<std::size_t> second_row = find_row(shell, middle_runs[1]);
  Expect(second_row.has_value(),
         "the second collapsed middle run should remain available after the first expansion");
  Expect(ExpandCompareCollapsedContext(compare, *second_row,
                                       microide::workspace::CompareCollapsedContextAction::ShowAll),
         "ShowAll should expand the second collapsed middle run");
  Expect(WorkspaceShellTestAccess::ActiveComparePresentationRowCount(shell) > after_first_expand,
         "expanding a second collapsed middle run should reveal additional rows");
  Expect(!find_row(shell, middle_runs[0]).has_value(),
         "expanding a second run should not collapse the first one again");
  Expect(!find_row(shell, middle_runs[1]).has_value(),
         "the second collapsed middle run should also remain expanded");
}

void TestWorkspaceShellCompareCollapsedContextExpansionSurvivesTreeRefresh() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "main.cpp";
  const auto build_text = [](std::string_view first_change, std::string_view second_change) {
    std::string text;
    for (int i = 0; i < 24; ++i) {
      text += "prefix " + std::to_string(i) + "\n";
    }
    text += std::string(first_change) + "\n";
    for (int i = 0; i < 30; ++i) {
      text += "middle " + std::to_string(i) + "\n";
    }
    text += std::string(second_change) + "\n";
    for (int i = 0; i < 8; ++i) {
      text += "suffix " + std::to_string(i) + "\n";
    }
    return text;
  };
  const std::string left = build_text("left a", "left b");
  const std::string right = build_text("right a", "right b");
  WriteFile(source, left);

  InitializeGitRepo(root);
  CommitAll(root, "Add compare refresh fixture", "compare refresh fixture");
  WriteFile(source, right);

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, source, "HEAD", "HEAD"),
         "compare refresh fixture should open");

  std::optional<std::size_t> collapsed_row;
  std::pair<std::size_t, std::size_t> collapsed_run_identity{0, 0};
  const std::size_t initial_row_count =
      WorkspaceShellTestAccess::ActiveComparePresentationRowCount(shell);
  for (std::size_t i = 0; i < initial_row_count; ++i) {
    if (WorkspaceShellTestAccess::ActiveComparePresentationRowKind(shell, i) !=
        microide::compare::ComparePresentationRowKind::CollapsedContext) {
      continue;
    }
    const auto action_rects =
        WorkspaceShellTestAccess::ActiveCompareCollapsedContextActionRects(shell, i);
    if (action_rects.previous_rect.has_value() && action_rects.next_rect.has_value()) {
      collapsed_row = i;
      collapsed_run_identity =
          WorkspaceShellTestAccess::ActiveCompareCollapsedRunIdentity(shell, i);
      break;
    }
  }
  Expect(collapsed_row.has_value(),
         "compare refresh fixture should expose a collapsed middle run before expansion");

  auto& compare = WorkspaceShellTestAccess::ActiveCompare(shell);
  compare.scroll_row = std::max(0, static_cast<int>(*collapsed_row) - 2);
  Expect(ExpandCompareCollapsedContext(compare, *collapsed_row,
                                       microide::workspace::CompareCollapsedContextAction::ShowAll),
         "ShowAll should expand the collapsed middle run before refresh");
  const std::size_t expanded_row_count =
      WorkspaceShellTestAccess::ActiveComparePresentationRowCount(shell);
  Expect(expanded_row_count > initial_row_count,
         "expanding the middle collapsed run should reveal additional rows");

  Expect(WorkspaceShellTestAccess::ExecuteTreeRefresh(shell),
         "tree refresh should execute for compare refresh fixture");
  Expect(WorkspaceShellTestAccess::ActiveComparePresentationRowCount(shell) == expanded_row_count,
         "tree refresh should preserve the expanded compare presentation row count");

  bool run_recollapsed = false;
  const std::size_t refreshed_row_count =
      WorkspaceShellTestAccess::ActiveComparePresentationRowCount(shell);
  for (std::size_t i = 0; i < refreshed_row_count; ++i) {
    if (WorkspaceShellTestAccess::ActiveComparePresentationRowKind(shell, i) !=
        microide::compare::ComparePresentationRowKind::CollapsedContext) {
      continue;
    }
    if (WorkspaceShellTestAccess::ActiveCompareCollapsedRunIdentity(shell, i) ==
        collapsed_run_identity) {
      run_recollapsed = true;
      break;
    }
  }
  Expect(!run_recollapsed,
         "tree refresh should preserve expanded compare runs instead of re-collapsing them");
}

void TestWorkspaceShellReadOnlyCompareRightPaneSupportsSelectAllAndCopy() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "history.txt";
  WriteFile(source, "base line\n");

  InitializeGitRepo(root);
  CommitAll(root, "base fixture", "base fixture");
  WriteFile(source, "head line\n");
  CommitAll(root, "head fixture", "head fixture");

  const auto history = microide::project::CollectGitFileHistory(root,
                                                                platform::LocalProcessLauncher(),
                                                                    source).commits;
  Expect(history.size() == 2, "read-only compare fixture should have two commits");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::OpenBranchHeadComparison(shell, source, history[1].hash, "base",
                                                            history[0].hash, "head"),
         "read-only branch comparison should open");

  const auto surface = WorkspaceShellTestAccess::ActiveCompareSurfaceLayout(shell);
  const float row_y = surface.rows_y + surface.line_height * 0.5f;
  const float right_x = surface.right_x + 24.0f;
  Expect(SendMouseDown(shell, right_x, row_y, SDL_BUTTON_LEFT),
         "clicking the read-only compare right pane should be handled");
  Expect(WorkspaceShellTestAccess::ActiveCompare(shell).right_view_active,
         "clicking the read-only compare right pane should make it the active navigable surface");

  std::string clipboard_text;
  WorkspaceShellTestAccess::SetClipboardTextWriter(
      shell, [&](std::string_view text) {
        clipboard_text = std::string(text);
        return true;
      });

  Expect(WorkspaceShellTestAccess::ExecuteSelectAll(shell),
         "Select All should execute on the read-only compare right pane");
  Expect(WorkspaceShellTestAccess::ExecuteCopySelection(shell),
         "Copy Selection should execute on the read-only compare right pane");
  Expect(clipboard_text.find("head line") != std::string::npos,
         "copying from the read-only compare right pane should copy its visible text");
}

void TestWorkspaceShellReadOnlyCompareShortcutCopyUsesNavigableViewport() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "history.txt";
  WriteFile(source, "base line\n");

  InitializeGitRepo(root);
  CommitAll(root, "base fixture", "base fixture");
  WriteFile(source, "head line\n");
  CommitAll(root, "head fixture", "head fixture");

  const auto history = microide::project::CollectGitFileHistory(root,
                                                                platform::LocalProcessLauncher(),
                                                                    source).commits;
  Expect(history.size() == 2, "compare shortcut fixture should have two commits");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::OpenBranchHeadComparison(shell, source, history[1].hash, "base",
                                                            history[0].hash, "head"),
         "read-only branch comparison should open");

  const auto surface = WorkspaceShellTestAccess::ActiveCompareSurfaceLayout(shell);
  const float row_y = surface.rows_y + surface.line_height * 0.5f;
  const float right_x = surface.right_x + 24.0f;
  Expect(SendMouseDown(shell, right_x, row_y, SDL_BUTTON_LEFT),
         "clicking the read-only compare right pane should be handled");

  std::string clipboard_text;
  WorkspaceShellTestAccess::SetClipboardTextWriter(
      shell, [&](std::string_view text) {
        clipboard_text = std::string(text);
        return true;
      });

  Expect(SendKeyDown(shell, SDLK_A, SDL_KMOD_CTRL),
         "Ctrl+A should be handled on the read-only compare pane");
  Expect(SendKeyDown(shell, SDLK_C, SDL_KMOD_CTRL),
         "Ctrl+C should be handled on the read-only compare pane");
  Expect(clipboard_text.find("head line") != std::string::npos,
         "compare shortcut copy should use the active read-only navigable viewport");
}

void TestWorkspaceShellCompareWheelScrollsRows() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "main.cpp";

  std::string base_text;
  std::string working_text;
  for (int i = 0; i < 120; ++i) {
    base_text += "base line " + std::to_string(i) + "\n";
    working_text += ((i % 5 == 0) ? "changed line " + std::to_string(i) + "\n"
                                  : "base line " + std::to_string(i) + "\n");
  }
  WriteFile(source, base_text);

  InitializeGitRepo(root);
  CommitAll(root, "Add compare wheel fixture", "compare wheel fixture");
  WriteFile(source, working_text);

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 420);
  Expect(WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, source, "HEAD", "HEAD"),
         "compare wheel fixture should open");

  auto& compare = WorkspaceShellTestAccess::ActiveCompare(shell);
  const auto surface = WorkspaceShellTestAccess::ActiveCompareSurfaceLayout(shell);
  Expect(workspace::CompareTabPresentationRowCount(compare) >
             static_cast<std::size_t>(surface.visible_rows),
         "compare wheel fixture should overflow the viewport");

  const int before_scroll = compare.scroll_row;
  const float wheel_x = surface.right_x + 24.0f;
  const float wheel_y = surface.rows_y + surface.line_height * 0.5f;
  WorkspaceShellTestAccess::SetFocusPanel(shell);
  Expect(SendMouseWheel(shell, wheel_x, wheel_y, -1),
         "scrolling the compare surface should be handled");
  Expect(compare.scroll_row > before_scroll,
         "scrolling the compare surface should advance the visible row");
  Expect(WorkspaceShellTestAccess::FocusIsPanel(shell),
         "scrolling the compare surface should leave keyboard focus where it was");
}

void TestWorkspaceShellCompareWheelScrollsColumns() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "main.cpp";

  const std::string base_line =
      "abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0123456789\n";
  const std::string working_line =
      "abcdefghijklmnopqrstuvwxyz9876543210abcdefghijklmnopqrstuvwxyz9876543210\n";
  WriteFile(source, base_line);

  InitializeGitRepo(root);
  CommitAll(root, "Add compare horizontal wheel fixture", "compare horizontal wheel fixture");
  WriteFile(source, working_line);

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 640, 420);
  Expect(WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, source, "HEAD", "HEAD"),
         "compare horizontal wheel fixture should open");

  auto& compare = WorkspaceShellTestAccess::ActiveCompare(shell);
  const auto surface = WorkspaceShellTestAccess::ActiveCompareSurfaceLayout(shell);
  Expect(compare.max_visual_columns > surface.visible_columns,
         "compare horizontal wheel fixture should overflow horizontally");

  const std::size_t before_scroll = compare.horizontal_scroll;
  const float wheel_x = surface.right_x + 24.0f;
  const float wheel_y = surface.rows_y + surface.line_height * 0.5f;
  Expect(SendMouseWheel(shell, wheel_x, wheel_y, 0, -1),
         "horizontal scrolling the compare surface should be handled");
  Expect(compare.horizontal_scroll > before_scroll,
         "horizontal scrolling the compare surface should advance the visible column");
}

void TestWorkspaceShellCompareWheelScrollsLeftDominatedColumns() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "main.cpp";

  const std::string base_line =
      "abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0123456789\n";
  const std::string working_line = "short\n";
  WriteFile(source, base_line);

  InitializeGitRepo(root);
  CommitAll(root, "Add compare left-dominated fixture", "compare left-dominated fixture");
  WriteFile(source, working_line);

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 640, 420);
  Expect(WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, source, "HEAD", "HEAD"),
         "compare left-dominated fixture should open");

  auto& compare = WorkspaceShellTestAccess::ActiveCompare(shell);
  const auto surface = WorkspaceShellTestAccess::ActiveCompareSurfaceLayout(shell);
  Expect(compare.max_visual_columns > surface.visible_columns,
         "compare left-dominated fixture should overflow horizontally via the left pane");

  const std::size_t before_scroll = compare.horizontal_scroll;
  const float wheel_x = surface.right_x + 24.0f;
  const float wheel_y = surface.rows_y + surface.line_height * 0.5f;
  Expect(SendMouseWheel(shell, wheel_x, wheel_y, 0, -1),
         "horizontal scrolling a left-dominated compare should be handled");
  Expect(compare.horizontal_scroll > before_scroll,
         "left-pane overflow should still advance the shared horizontal offset");
}

void TestWorkspaceShellCompareHorizontalNavigationInvalidatesEditablePane() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "main.cpp";
  WriteFile(source, "int alpha() {\n  return 1;\n}\n");

  InitializeGitRepo(root);
  CommitAll(root, "Add compare invalidation fixture", "compare invalidation fixture");
  WriteFile(source, "int beta() {\n  return 2;\n}\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, source, "HEAD", "HEAD"),
         "compare invalidation fixture should open");
  (void)shell.ConsumePendingRenderInvalidation();

  const SDL_FRect editable_rect = WorkspaceShellTestAccess::ActiveCompareEditableRect(shell);

  SoftwareCanvas canvas{1280, 720};
  shell.Render(canvas.renderer(), 1280, 720);
  (void)shell.ConsumePendingRenderInvalidation();
  SDL_Event event{};
  event.type = SDL_EVENT_KEY_DOWN;
  event.key.key = SDLK_RIGHT;
  const auto result = shell.HandleEvent(event);
  const auto damage = FrameDamage(shell, canvas, 1280, 720, result.redraw);

  Expect(result.handled, "compare horizontal navigation should be handled");
  Expect(!damage.full && !damage.rects.empty(), "compare horizontal navigation should stay on a partial redraw path");
  bool editable = false;
  for (const SDL_FRect& rect : damage.rects) {
    editable = editable || RectsIntersect(rect, editable_rect);
  }
  Expect(editable, "compare horizontal navigation should repaint the editable pane");
}

void TestWorkspaceShellCompareSelectionStepInvalidatesRowBand() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "main.cpp";
  WriteFile(source, "one\ntwo\nthree\nfour\n");

  InitializeGitRepo(root);
  CommitAll(root, "Add compare row invalidation fixture", "compare row invalidation fixture");
  WriteFile(source, "one\nTWO\nthree\nfour\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, source, "HEAD", "HEAD"),
         "compare row invalidation fixture should open");
  (void)shell.ConsumePendingRenderInvalidation();

  SoftwareCanvas canvas{1280, 720};
  shell.Render(canvas.renderer(), 1280, 720);
  (void)shell.ConsumePendingRenderInvalidation();
  const std::size_t before_row = WorkspaceShellTestAccess::ActiveCompare(shell).selected_row;
  const auto previous_row_rect =
      WorkspaceShellTestAccess::ActiveCompareRowRangeRect(shell, before_row, before_row + 1);
  SDL_Event event{};
  event.type = SDL_EVENT_KEY_DOWN;
  event.key.key = SDLK_DOWN;
  const auto result = shell.HandleEvent(event);
  const auto damage = FrameDamage(shell, canvas, 1280, 720, result.redraw);
  const std::size_t after_row = WorkspaceShellTestAccess::ActiveCompare(shell).selected_row;
  Expect(after_row != before_row, "Down should move the compare selection");
  const auto next_row_rect =
      WorkspaceShellTestAccess::ActiveCompareRowRangeRect(shell, after_row, after_row + 1);
  const auto surface = WorkspaceShellTestAccess::ActiveCompareSurfaceLayout(shell);

  Expect(result.handled, "compare selection step should be handled");
  Expect(!damage.full && !damage.rects.empty(),
         "compare selection step should stay on a partial redraw path");
  Expect(previous_row_rect.has_value() && AnyRectCovers(damage.rects, *previous_row_rect),
         "compare selection step should repaint the previously selected row");
  Expect(next_row_rect.has_value() && AnyRectCovers(damage.rects, *next_row_rect),
         "compare selection step should repaint the newly selected row");
  Expect(MaxRectHeight(damage.rects) <
             static_cast<float>(surface.visible_rows) * surface.line_height,
         "compare selection step should redraw less than the full compare surface height");
}

void TestWorkspaceShellCompareRenderPaintsDiagnosticGutterMarkers() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  EnsureDummySdlVideo();
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "main.cpp";
  WriteFile(source, "int alpha() {\n  return 1;\n}\n");

  InitializeGitRepo(root);
  CommitAll(root, "Add compare diagnostics fixture", "compare diagnostics fixture");
  WriteFile(source, "int beta() {\n  return 2;\n}\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, source, "HEAD", "HEAD"),
         "compare diagnostics fixture should open");
  Expect(WorkspaceShellTestAccess::PublishDiagnostics(
             shell, "diagnostics", source,
             {microide::editor::Diagnostic{
                 .range =
                     microide::editor::SelectionRange{
                         .start = microide::editor::TextPosition{.line = 0, .column = 0},
                         .end = microide::editor::TextPosition{.line = 0, .column = 3},
                     },
                 .severity = microide::editor::DiagnosticSeverity::Error,
                 .message = "compare error",
             }}),
         "compare diagnostics fixture should publish one right-pane diagnostic");

  SoftwareCanvas canvas(1280, 720);
  shell.Render(canvas.renderer(), 1280, 720);
  SDL_Surface* pixels = SDL_RenderReadPixels(canvas.renderer(), nullptr);
  Expect(pixels != nullptr, "compare diagnostics render test should read software pixels");

  const auto surface = WorkspaceShellTestAccess::ActiveCompareSurfaceLayout(shell);
  const auto theme = microide::render::MakeDefaultTheme();
  Uint8 r = 0;
  Uint8 g = 0;
  Uint8 b = 0;
  Uint8 a = 0;
  const int marker_x = static_cast<int>(std::floor(surface.right_x + 3.0f));
  const int marker_y = static_cast<int>(std::floor(surface.rows_y + surface.line_height - 2.0f));
  Expect(SDL_ReadSurfacePixel(pixels, marker_x, marker_y, &r, &g, &b, &a),
         "compare diagnostics render test should read the gutter marker pixel");
  Expect(r == theme.diagnostic_error.r && g == theme.diagnostic_error.g &&
             b == theme.diagnostic_error.b && a == theme.diagnostic_error.a,
         "compare right-pane diagnostics should paint severity markers in the gutter");

  SDL_DestroySurface(pixels);
}

void TestWorkspaceShellCompareRenderReusesVisibleLayoutCache() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  EnsureDummySdlVideo();
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "main.cpp";
  WriteFile(source, "same\tline\nold\tline\n");

  InitializeGitRepo(root);
  CommitAll(root, "Add compare layout cache fixture", "compare layout cache fixture");
  WriteFile(source, "same\tline\nnew\tline\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, source, "HEAD", "HEAD"),
         "compare layout cache fixture should open");
  auto& compare = WorkspaceShellTestAccess::ActiveCompare(shell);
  Expect(compare.visible_layouts.live_count == 0,
         "compare visible-layout cache should start empty before first render");

  SoftwareCanvas canvas(1280, 720);
  shell.Render(canvas.renderer(), 1280, 720);
  const std::size_t warmed_cache_size = compare.visible_layouts.live_count;
  Expect(warmed_cache_size >= 2,
         "first compare render should warm visible layouts for rendered panes");

  shell.Render(canvas.renderer(), 1280, 720);
  Expect(compare.visible_layouts.live_count == warmed_cache_size,
         "stable compare frames should reuse cached visible layouts instead of appending more");

  const std::size_t retained_slots = compare.visible_layouts.layouts.size();
  const std::size_t retained_text_capacity =
      compare.visible_layouts.layouts.front().text.capacity();
  Expect(retained_text_capacity > 0,
         "a warmed layout slot should hold a non-empty text buffer to recycle");

  compare.left_content = compare::MakeCompareText("same\tline\nolder\tline\n");
  WorkspaceShellTestAccess::RefreshActiveCompareDerivedState(shell);
  Expect(compare.visible_layouts.live_count == 0,
         "compare visible-layout cache should drop its live entries when the model changes");
  Expect(compare.visible_layouts.layouts.size() == retained_slots,
         "invalidation must recycle the layout slab, not free it");
  Expect(compare.visible_layouts.layouts.front().text.capacity() == retained_text_capacity,
         "a recycled layout slot must keep the buffer capacity the next build refills");

  shell.Render(canvas.renderer(), 1280, 720);
  Expect(compare.visible_layouts.live_count >= 2,
         "the frame after an invalidation should rewarm the visible layouts");
  Expect(compare.visible_layouts.layouts.size() == retained_slots,
         "rewarming into a recycled slab must not grow it");
}

void TestWorkspaceShellCompareBlameLoadsForWorkingTreePane() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "main.cpp";
  WriteFile(source, "line 1\nline 2\nline 3\nline 4\n");

  InitializeGitRepo(root);
  CommitAll(root, "Add compare blame fixture", "compare blame fixture");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, source, "HEAD", "HEAD"),
         "working-tree comparison should open");

  auto& compare = WorkspaceShellTestAccess::ActiveCompare(shell);
  compare.right_viewport.MoveCursorTo(1, 0);

  const auto overlay = WaitForActiveCompareBlameOverlay(shell, 1);
  Expect(overlay.has_value(),
         "clean working-tree comparison should eventually expose compare blame");
  Expect(overlay->lines.size() == 1,
         "compare blame should annotate the caret line only, like the editor surface");
  Expect(overlay->lines[0].author == "Microide Tests",
         "compare blame should keep the blame author metadata");
  Expect(overlay->lines[0].summary == "Add compare blame fixture",
         "compare blame should keep the blame summary metadata");
}

void TestWorkspaceShellMergeBlameLoadsForResultPane() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "main.cpp";
  const std::filesystem::path base = temp_dir.path() / "base.cpp";
  const std::filesystem::path incoming = temp_dir.path() / "incoming.cpp";
  WriteFile(source, "line 1\ncurrent line\nline 3\nline 4\n");
  WriteFile(base, "line 1\nbase line\nline 3\nline 4\n");
  WriteFile(incoming, "line 1\nincoming line\nline 3\nline 4\n");

  InitializeGitRepo(root);
  CommitAll(root, "Add merge blame fixture", "merge blame fixture");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::OpenMergeEditor(shell, base, incoming, source, source),
         "merge editor should open");

  auto& merge = WorkspaceShellTestAccess::ActiveMerge(shell);
  merge.result_viewport.MoveCursorTo(1, 0);

  const auto overlay = WaitForActiveMergeBlameOverlay(shell, 1);
  Expect(overlay.has_value(),
         "clean merge result pane should eventually expose blame");
  Expect(overlay->lines.size() == 1,
         "merge blame should annotate the caret line only, like the editor surface");
  Expect(overlay->lines[0].author == "Microide Tests",
         "merge blame should keep the blame author metadata");
  Expect(overlay->lines[0].summary == "Add merge blame fixture",
         "merge blame should keep the blame summary metadata");
}

void TestWorkspaceShellMergeSaveRepaintsTheResultPane() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "main.cpp";
  const std::filesystem::path base = temp_dir.path() / "base.cpp";
  const std::filesystem::path incoming = temp_dir.path() / "incoming.cpp";
  WriteFile(source, "line 1\ncurrent line\nline 3\nline 4\n");
  WriteFile(base, "line 1\nbase line\nline 3\nline 4\n");
  WriteFile(incoming, "line 1\nincoming line\nline 3\nline 4\n");
  InitializeGitRepo(root);
  CommitAll(root, "Add merge blame fixture", "merge blame fixture");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::OpenMergeEditor(shell, base, incoming, source, source),
         "merge editor should open");
  auto& merge = WorkspaceShellTestAccess::ActiveMerge(shell);
  merge.result_viewport.MoveCursorTo(1, 0);
  merge.result_viewport.InsertText("edited ");
  (void)WorkspaceShellTestAccess::TakePendingRenderInvalidation(shell);
  SoftwareCanvas canvas{1280, 720};
  shell.Render(canvas.renderer(), 1280, 720);
  (void)WorkspaceShellTestAccess::TakePendingRenderInvalidation(shell);

  Expect(WorkspaceShellTestAccess::SaveTab(shell, 0), "the merge result saves");
  Expect(!merge.result_viewport.dirty(), "and is clean");
  const auto damage =
      FrameDamage(shell, canvas, 1280, 720, WorkspaceShellTestAccess::TakePendingRenderInvalidation(shell));
  const SDL_FRect pane = WorkspaceShellTestAccess::ActiveMergeResultRect(shell);
  bool covered = damage.full;
  for (const SDL_FRect& rect : damage.rects) {
    SDL_FRect overlap{};
    covered = covered || SDL_GetRectIntersectionFloat(&rect, &pane, &overlap);
  }
  Expect(covered, "the save that cleaned the result pane must repaint it (its blame shows only when clean)");
}

void TestWorkspaceShellPathlessComparisonTabIsNamedLikeVsCode() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  std::filesystem::create_directories(root);
  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetClipboardTextReader(
      shell, []() -> std::optional<std::string> { return std::string("from the clipboard\n"); });
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "tab"), "an untitled buffer opens");
  WorkspaceShellTestAccess::ActiveEditor(shell).InsertText("typed\n");
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "compare-clipboard"),
         "the clipboard compares with the untitled buffer");
  const std::size_t active = WorkspaceShellTestAccess::ActiveTabIndex(shell);
  const std::string title = WorkspaceShellTestAccess::TabDisplayTitle(shell, active);
  Expect(title.find("compare:") == std::string::npos && title.find("↔") != std::string::npos &&
             title.find("Clipboard") != std::string::npos,
         "the tab is named by its two sides, without the control title's prefix: " + title);
}

void TestWorkspaceShellAFileComparedWithAnUntitledBufferIsNamedByBothSides() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path readme = root / "README.md";
  WriteFile(readme, "# readme\n");
  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenFile(shell, readme);
  Expect(WorkspaceShellTestAccess::OpenPlainComparison(
             shell,
             microide::workspace::CompareInput{.content = "# readme\n", .label = "README.md", .path = readme},
             microide::workspace::CompareInput{.content = "typed\n", .label = "Untitled", .path = {}, .editable = true}),
         "the comparison opens");
  const std::size_t active = WorkspaceShellTestAccess::ActiveTabIndex(shell);
  const std::string title = WorkspaceShellTestAccess::TabDisplayTitle(shell, active);
  Expect(title.find("README.md") != std::string::npos && title.find("Untitled") != std::string::npos &&
             title.find("compare:") == std::string::npos,
         "the comparison is named by both sides, not as README.md's own tab: " + title);
}

void TestWorkspaceShellCompareTabUsesFilenameOnlyLabelAndTooltip() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "nested" / "compare.txt";
  WriteFile(source, "zero\none\ntwo\n");

  InitializeGitRepo(root);
  CommitAll(root, "Add compare tab fixture", "compare tab fixture");
  WriteFile(source, "zero\none changed\ntwo changed\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  Expect(WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, source, "HEAD", "HEAD"),
         "working-tree comparison should open for compact-tab fixture");

  Expect(WorkspaceShellTestAccess::TabDisplayTitle(shell, 0) == "compare.txt",
         "compare tabs should display only the filename");
  Expect(WorkspaceShellTestAccess::TabTooltipLabel(shell, 0) == "src/nested/compare.txt",
         "compare tab tooltip should expose the full relative path");
  const std::string breadcrumb = WorkspaceShellTestAccess::BreadcrumbLabel(shell);
  Expect(breadcrumb.find("src/nested/compare.txt") != std::string::npos,
         "active compare breadcrumbs should keep the relative path");
  Expect(breadcrumb.find("HEAD -> Working tree") != std::string::npos,
         "active compare breadcrumbs should keep the compare refs");
}

void TestWorkspaceShellMergeTabUsesFilenameOnlyLabelAndTooltip() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path base = root / "base.txt";
  const std::filesystem::path incoming = root / "incoming.txt";
  const std::filesystem::path current = root / "current.txt";
  const std::filesystem::path output = root / "src" / "result.txt";
  WriteFile(base, "top\nbase\nbottom\n");
  WriteFile(incoming, "top\nincoming\nbottom\n");
  WriteFile(current, "top\ncurrent\nbottom\n");
  WriteFile(output, "top\ncurrent\nbottom\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  Expect(WorkspaceShellTestAccess::OpenMergeEditor(shell, base, incoming, current, output),
         "merge editor should open for compact-tab fixture");

  Expect(WorkspaceShellTestAccess::TabDisplayTitle(shell, 0) == "result.txt",
         "merge tabs should display only the output filename");
  Expect(WorkspaceShellTestAccess::TabTooltipLabel(shell, 0) == "src/result.txt",
         "merge tab tooltip should expose the full relative path");
  Expect(WorkspaceShellTestAccess::BreadcrumbLabel(shell).find("src/result.txt") != std::string::npos,
         "active merge breadcrumbs should keep the relative path");
}

void TestWorkspaceShellMergeEditorRefusesBinaryInput() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path base = root / "base.bin";
  const std::filesystem::path incoming = root / "incoming.bin";
  const std::filesystem::path current = root / "current.bin";
  const std::filesystem::path output = root / "result.bin";
  WriteFile(base, "top\nbase\nbottom\n");
  WriteFile(incoming, "top\nincoming\nbottom\n");
  WriteFile(current, std::string("top\ncur\0rent\nbottom\n", 20));
  WriteFile(output, "top\ncurrent\nbottom\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  Expect(!WorkspaceShellTestAccess::OpenMergeEditor(shell, base, incoming, current, output),
         "a binary/NUL merge input must not build a text merge tab");

  WriteFile(current, "top\ncurrent\nbottom\n");
  Expect(WorkspaceShellTestAccess::OpenMergeEditor(shell, base, incoming, current, output),
         "an all-text merge input should still open the merge editor");
}

void TestWorkspaceShellCompareDividerMatchesMarkerWidth() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "main.cpp";
  WriteFile(source, "int alpha() {\n  return 1;\n}\n");

  InitializeGitRepo(root);
  CommitAll(root, "Add compare divider fixture", "compare divider fixture");
  WriteFile(source, "int beta() {\n  return 2;\n}\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, source, "HEAD", "HEAD"),
         "compare divider fixture should open");

  const auto surface = WorkspaceShellTestAccess::ActiveCompareSurfaceLayout(shell);
  Expect(surface.divider_width <= std::ceil(WorkspaceShellTestAccess::TextCharWidth(shell)) + 1.0f,
         "compare divider should stay at roughly one glyph wide");
  Expect(surface.right_x - surface.center_x <= surface.divider_width + 0.5f,
         "compare divider should not reserve extra empty space before the right gutter");
}

void TestWorkspaceShellCompareInANarrowPaneStaysInsideIt() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "main.cpp";
  WriteFile(source, "int alpha() {\n  return 1;\n}\n");
  InitializeGitRepo(root);
  CommitAll(root, "Add narrow compare fixture", "narrow compare fixture");
  WriteFile(source, "int beta() {\n  return 2;\n}\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 824, 600);
  WorkspaceShellTestAccess::OpenFile(shell, source);
  for (int i = 0; i < 3; ++i) {
    Expect(WorkspaceShellTestAccess::SplitEditorGroup(shell, microide::workspace::EditorSplitOrientation::Vertical),
           "the editor splits");
  }
  Expect(WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, source, "HEAD", "HEAD"), "the diff opens");
  Expect(WorkspaceShellTestAccess::ActiveTabIsCompare(shell), "in the focused pane");
  const SDL_FRect pane = WorkspaceShellTestAccess::ActiveTabSurface(shell);
  const auto surface = WorkspaceShellTestAccess::ActiveCompareSurfaceLayout(shell);
  const float right = pane.x + pane.w;
  Expect(pane.w < 300.0f, "the pane is narrow: " + std::to_string(pane.w));
  Expect(surface.center_x >= pane.x && surface.center_x + surface.divider_width <= right + 0.5f,
         "the divider stays inside the pane: divider at " + std::to_string(surface.center_x) + ", pane " +
             std::to_string(pane.x) + ".." + std::to_string(right));
  Expect(surface.right_x + surface.gutter_width + surface.right_width <= right + 0.5f,
         "and so does the right pane: it ends at " +
             std::to_string(surface.right_x + surface.gutter_width + surface.right_width));
}

void TestWorkspaceShellCompareInAPaneNarrowerThanItsDividerStaysInsideIt() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "main.cpp";
  WriteFile(source, "int alpha() {\n  return 1;\n}\n");
  InitializeGitRepo(root);
  CommitAll(root, "Add sliver compare fixture", "sliver compare fixture");
  WriteFile(source, "int beta() {\n  return 2;\n}\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 640, 600);
  WorkspaceShellTestAccess::OpenFile(shell, source);
  while (WorkspaceShellTestAccess::ActiveTabSurface(shell).w > 12.0f &&
         WorkspaceShellTestAccess::SplitEditorGroup(shell, microide::workspace::EditorSplitOrientation::Vertical)) {
  }
  Expect(WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, source, "HEAD", "HEAD"), "the diff opens");
  Expect(WorkspaceShellTestAccess::ActiveTabIsCompare(shell), "in the focused pane");
  const SDL_FRect pane = WorkspaceShellTestAccess::ActiveTabSurface(shell);
  const auto surface = WorkspaceShellTestAccess::ActiveCompareSurfaceLayout(shell);
  const float right = pane.x + pane.w;
  Expect(pane.w < surface.divider_width + 16.0f, "the pane is a sliver: " + std::to_string(pane.w));
  Expect(surface.center_x >= pane.x && surface.center_x + surface.divider_width <= right + 0.5f,
         "the divider stays inside the pane: divider at " + std::to_string(surface.center_x) + "+" +
             std::to_string(surface.divider_width) + ", pane " + std::to_string(pane.x) + ".." +
             std::to_string(right));
  const SDL_FRect grab = microide::workspace::CompareDividerHitRect(pane, surface);
  Expect(grab.x >= pane.x - 0.5f && grab.x + grab.w <= right + 0.5f,
         "and so does its grab rect: " + std::to_string(grab.x) + ".." + std::to_string(grab.x + grab.w));
}

void TestWorkspaceShellMergeInANarrowPaneStaysInsideIt() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "main.cpp";
  const std::filesystem::path base = temp_dir.path() / "base.cpp";
  const std::filesystem::path incoming = temp_dir.path() / "incoming.cpp";
  WriteFile(source, "line 1\ncurrent line\nline 3\n");
  WriteFile(base, "line 1\nbase line\nline 3\n");
  WriteFile(incoming, "line 1\nincoming line\nline 3\n");
  InitializeGitRepo(root);
  CommitAll(root, "Add narrow merge fixture", "narrow merge fixture");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 824, 600);
  WorkspaceShellTestAccess::OpenFile(shell, source);
  for (int i = 0; i < 3; ++i) {
    Expect(WorkspaceShellTestAccess::SplitEditorGroup(shell, microide::workspace::EditorSplitOrientation::Vertical),
           "the editor splits");
  }
  Expect(WorkspaceShellTestAccess::OpenMergeEditor(shell, base, incoming, source, source), "the merge opens");
  const SDL_FRect pane = WorkspaceShellTestAccess::ActiveTabSurface(shell);
  const auto surface = WorkspaceShellTestAccess::ActiveMergeSurfaceLayout(shell);
  const float right = pane.x + pane.w;
  Expect(pane.w < 300.0f, "the pane is narrow: " + std::to_string(pane.w));
  Expect(surface.right_x + surface.gutter_width + surface.right_width <= right + 0.5f,
         "the merge's right pane stays inside its pane: it ends at " +
             std::to_string(surface.right_x + surface.gutter_width + surface.right_width) + ", pane at " +
             std::to_string(right));
}

void TestWorkspaceShellMergeInAPaneNarrowerThanItsDividersStaysInsideIt() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "main.cpp";
  const std::filesystem::path base = temp_dir.path() / "base.cpp";
  const std::filesystem::path incoming = temp_dir.path() / "incoming.cpp";
  WriteFile(source, "line 1\ncurrent line\nline 3\n");
  WriteFile(base, "line 1\nbase line\nline 3\n");
  WriteFile(incoming, "line 1\nincoming line\nline 3\n");
  InitializeGitRepo(root);
  CommitAll(root, "Add sliver merge fixture", "sliver merge fixture");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 640, 600);
  WorkspaceShellTestAccess::OpenFile(shell, source);
  while (WorkspaceShellTestAccess::ActiveTabSurface(shell).w > 12.0f &&
         WorkspaceShellTestAccess::SplitEditorGroup(shell, microide::workspace::EditorSplitOrientation::Vertical)) {
  }
  Expect(WorkspaceShellTestAccess::OpenMergeEditor(shell, base, incoming, source, source), "the merge opens");
  const SDL_FRect pane = WorkspaceShellTestAccess::ActiveTabSurface(shell);
  const auto surface = WorkspaceShellTestAccess::ActiveMergeSurfaceLayout(shell);
  const float right = pane.x + pane.w;
  Expect(pane.w < 48.0f, "the pane is a sliver: " + std::to_string(pane.w));
  Expect(surface.left_x >= pane.x && surface.right_x + surface.gutter_width + surface.right_width <= right + 0.5f,
         "the merge stays inside its pane: " + std::to_string(surface.left_x) + ".." +
             std::to_string(surface.right_x + surface.gutter_width + surface.right_width) + ", pane " +
             std::to_string(pane.x) + ".." + std::to_string(right));
  for (const SDL_FRect& grab : microide::workspace::MergeDividerHitRects(pane, surface)) {
    Expect(grab.x >= pane.x - 0.5f && grab.x + grab.w <= right + 0.5f,
           "and so do its dividers' grab rects: " + std::to_string(grab.x) + ".." +
               std::to_string(grab.x + grab.w));
  }
}

void TestWorkspaceShellCompareRenderKeepsDividerBorderOnUnchangedRows() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  EnsureDummySdlVideo();
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "main.cpp";
  WriteFile(source, "same line\nold line\n");

  InitializeGitRepo(root);
  CommitAll(root, "Add compare divider border fixture", "compare divider border fixture");
  WriteFile(source, "same line\nnew line\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, source, "HEAD", "HEAD"),
         "compare divider border fixture should open");

  SoftwareCanvas canvas(1280, 720);
  shell.Render(canvas.renderer(), 1280, 720);
  SDL_Surface* pixels = SDL_RenderReadPixels(canvas.renderer(), nullptr);
  Expect(pixels != nullptr, "compare divider border render test should read software pixels");

  const auto surface = WorkspaceShellTestAccess::ActiveCompareSurfaceLayout(shell);
  const auto theme = microide::render::MakeDefaultTheme();
  Uint8 r = 0;
  Uint8 g = 0;
  Uint8 b = 0;
  Uint8 a = 0;
  const int border_x = static_cast<int>(std::floor(surface.center_x));
  const int border_y = static_cast<int>(std::floor(surface.rows_y + surface.line_height * 0.5f));
  Expect(SDL_ReadSurfacePixel(pixels, border_x, border_y, &r, &g, &b, &a),
         "compare divider border render test should read the divider border pixel");
  Expect(r == theme.border.r && g == theme.border.g &&
             b == theme.border.b && a == theme.border.a,
         "compare divider should keep a visible left border on unchanged rows");

  SDL_DestroySurface(pixels);
}

void TestWorkspaceShellComparePaneResizeKeepsWiderPaneTextVisible() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "main.cpp";
  WriteFile(source, "abcdefghijklmnopqrstuvwxyz0123456789\n");

  InitializeGitRepo(root);
  CommitAll(root, "Add compare resize fixture", "compare resize fixture");
  WriteFile(source, "abcdefghijklmnopqrstuvwxyz9876543210\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, source, "HEAD", "HEAD"),
         "compare resize fixture should open");

  auto& compare = WorkspaceShellTestAccess::ActiveCompare(shell);
  compare.divider_fraction = 0.75f;
  const auto left_wide = WorkspaceShellTestAccess::ActiveCompareSurfaceLayout(shell);
  Expect(left_wide.left_visible_columns > left_wide.right_visible_columns,
         "widening the left compare pane should preserve more visible text on the left");

  compare.divider_fraction = 0.25f;
  const auto right_wide = WorkspaceShellTestAccess::ActiveCompareSurfaceLayout(shell);
  Expect(right_wide.right_visible_columns > right_wide.left_visible_columns,
         "widening the right compare pane should preserve more visible text on the right");
}

void TestWorkspaceShellCompareAndMergeDividersResetOnDoubleClick() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path compare_source = root / "src" / "compare.cpp";
  const std::filesystem::path base = root / "src" / "base.cpp";
  const std::filesystem::path incoming = root / "src" / "incoming.cpp";
  const std::filesystem::path current = root / "src" / "current.cpp";
  const std::filesystem::path output = root / "src" / "output.cpp";

  WriteFile(compare_source, "abcdefghijklmnopqrstuvwxyz0123456789\n");
  WriteFile(base, "int answer() {\n  return 0;\n}\n");
  WriteFile(incoming, "int answer() {\n  return 1;\n}\n");
  WriteFile(current, "int answer() {\n  return 2;\n}\n");
  WriteFile(output, "int answer() {\n  return 0;\n}\n");

  InitializeGitRepo(root);
  CommitAll(root, "Add divider reset fixture", "divider reset fixture");
  WriteFile(compare_source, "abcdefghijklmnopqrstuvwxyz9876543210\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, compare_source, "HEAD", "HEAD"),
         "divider reset compare fixture should open");

  auto& compare = WorkspaceShellTestAccess::ActiveCompare(shell);
  compare.divider_fraction = 0.8f;
  const SDL_FRect compare_divider = CompareDividerRectOf(shell);
  Expect(SendMouseDown(shell, compare_divider.x + compare_divider.w * 0.5f,
                       compare_divider.y + compare_divider.h * 0.5f, SDL_BUTTON_LEFT, 2),
         "double-clicking the compare divider should be handled");
  Expect(std::abs(compare.divider_fraction -
                  microide::workspace::kWorkspaceDefaultCompareDividerFraction) < 0.001f,
         "double-clicking the compare divider should restore the even split");
  Expect(WorkspaceShellTestAccess::TransientDragTargetIsNone(shell),
         "a divider reset must not leave a drag armed behind it");

  Expect(WorkspaceShellTestAccess::OpenMergeEditor(shell, base, incoming, current, output),
         "divider reset merge fixture should open");
  auto& merge = WorkspaceShellTestAccess::ActiveMerge(shell);
  merge.left_divider_fraction = 0.15f;
  merge.right_divider_fraction = 0.85f;
  const auto merge_dividers = MergeDividerRectsOf(shell);
  Expect(SendMouseDown(shell, merge_dividers[1].x + merge_dividers[1].w * 0.5f,
                       merge_dividers[1].y + merge_dividers[1].h * 0.5f, SDL_BUTTON_LEFT, 2),
         "double-clicking the merge right divider should be handled");
  Expect(std::abs(merge.left_divider_fraction -
                  microide::workspace::kWorkspaceDefaultMergeLeftDividerFraction) < 0.001f &&
             std::abs(merge.right_divider_fraction -
                      microide::workspace::kWorkspaceDefaultMergeRightDividerFraction) < 0.001f,
         "double-clicking a merge divider should restore both to equal thirds");
  Expect(WorkspaceShellTestAccess::TransientDragTargetIsNone(shell),
         "a merge divider reset must not leave a drag armed behind it");

  const auto reset_dividers = MergeDividerRectsOf(shell);
  Expect(SendMouseDown(shell, reset_dividers[0].x + reset_dividers[0].w * 0.5f,
                       reset_dividers[0].y + reset_dividers[0].h * 0.5f, SDL_BUTTON_LEFT),
         "grabbing the merge left divider should be handled");
  Expect(!WorkspaceShellTestAccess::TransientDragTargetIsNone(shell),
         "a single click on the merge divider should arm the resize drag");
  Expect(WorkspaceShellTestAccess::CursorKindAtIsEwResize(shell, 10.0f, 10.0f),
         "a merge divider drag should hold the resize cursor away from the divider");
  SendMouseUp(shell, reset_dividers[0].x, reset_dividers[0].y, SDL_BUTTON_LEFT);
}

void TestWorkspaceShellCompareAndMergePaneMinimaPreserveVisibleColumns() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path compare_source = root / "src" / "compare.cpp";
  const std::filesystem::path base = root / "src" / "base.cpp";
  const std::filesystem::path incoming = root / "src" / "incoming.cpp";
  const std::filesystem::path current = root / "src" / "current.cpp";
  const std::filesystem::path output = root / "src" / "output.cpp";

  WriteFile(compare_source, "abcdefghijklmnopqrstuvwxyz0123456789\n");
  WriteFile(base, "int answer() {\n  return 0;\n}\n");
  WriteFile(incoming, "int answer() {\n  return 1;\n}\n");
  WriteFile(current, "int answer() {\n  return 2;\n}\n");
  WriteFile(output, "int answer() {\n  return 0;\n}\n");

  InitializeGitRepo(root);
  CommitAll(root, "Add pane minima fixture", "pane minima fixture");
  WriteFile(compare_source, "abcdefghijklmnopqrstuvwxyz9876543210\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, compare_source, "HEAD", "HEAD"),
         "pane minima compare fixture should open");

  auto& compare = WorkspaceShellTestAccess::ActiveCompare(shell);
  compare.divider_fraction = 0.0f;
  const auto compare_left_min = WorkspaceShellTestAccess::ActiveCompareSurfaceLayout(shell);
  compare.divider_fraction = 1.0f;
  const auto compare_right_min = WorkspaceShellTestAccess::ActiveCompareSurfaceLayout(shell);
  Expect(compare_left_min.left_visible_columns >= 1 && compare_left_min.right_visible_columns >= 1,
         "compare layout should preserve one visible column per pane at minimum left fraction");
  Expect(compare_right_min.left_visible_columns >= 1 && compare_right_min.right_visible_columns >= 1,
         "compare layout should preserve one visible column per pane at minimum right fraction");

  Expect(WorkspaceShellTestAccess::OpenMergeEditor(shell, base, incoming, current, output),
         "pane minima merge fixture should open");
  auto& merge = WorkspaceShellTestAccess::ActiveMerge(shell);
  merge.left_divider_fraction = 0.0f;
  merge.right_divider_fraction = 1.0f;
  const auto merge_surface = WorkspaceShellTestAccess::ActiveMergeSurfaceLayout(shell);
  Expect(merge_surface.visible_columns >= 1,
         "merge layout should preserve one visible column at minimum divider fractions");
}

void TestWorkspaceShellMergeToolbarLayoutClearsPaneHeaders() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path base = root / "base.txt";
  const std::filesystem::path incoming = root / "incoming.txt";
  const std::filesystem::path current = root / "current.txt";
  const std::filesystem::path output = root / "conflict.txt";
  WriteFile(base, "line 1\nshared\n");
  WriteFile(incoming, "line 1\nincoming change\nline 3\n");
  WriteFile(current, "line 1\ncurrent change\nline 3\n");
  WriteFile(output, "line 1\nshared\nline 3\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1920, 1080);
  Expect(WorkspaceShellTestAccess::OpenMergeEditor(shell, base, incoming, current, output),
         "merge toolbar layout fixture should open");

  const auto surface = WorkspaceShellTestAccess::ActiveMergeSurfaceLayout(shell);
  const auto toolbar = WorkspaceShellTestAccess::MergeToolbarNavigationRects(shell);

  constexpr float kMergeToolbarButtonHeight = 22.0f;
  Expect(surface.header_y >= surface.secondary_button_y + kMergeToolbarButtonHeight + 4.0f,
         "pane headers should sit below the secondary toolbar row");
  Expect(surface.rows_y >= surface.header_y + surface.line_height,
         "merge rows should start below pane headers");
  Expect(toolbar[0].y + toolbar[0].h <= surface.secondary_button_y + 0.5f,
         "primary toolbar should stay on the first row");
  Expect(surface.secondary_button_y + kMergeToolbarButtonHeight <= surface.header_y + 0.5f,
         "secondary toolbar should stay above pane headers");
}

void TestWorkspaceShellCommitPickerDismissesAfterOpeningCompare() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "main.cpp";
  WriteFile(source, "int alpha() {\n  return 1;\n}\n");

  InitializeGitRepo(root);
  CommitAll(root, "Add commit picker fixture", "commit picker fixture");
  WriteFile(source, "int beta() {\n  return 2;\n}\n");
  CommitAll(root, "Edit commit picker fixture", "commit picker fixture");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);

  Expect(WorkspaceShellTestAccess::OpenComparePickerForPath(shell, source),
         "opening the commit picker for a file with history should succeed");
  Expect(WorkspaceShellTestAccess::OverlayVisible(shell),
         "the commit picker overlay should be visible after opening");
  Expect(WorkspaceShellTestAccess::ActiveOverlayMode(shell) ==
             WorkspaceShell::OverlayMode::CommitPicker,
         "the open overlay should be the commit picker");

  Expect(SettleComparePicker(shell), "the async commit picker query should settle");

  Expect(SendKeyDown(shell, SDLK_RETURN, SDL_KMOD_NONE),
         "pressing Enter in the commit picker should be handled");

  Expect(!WorkspaceShellTestAccess::OverlayVisible(shell),
         "selecting a commit should dismiss the picker instead of leaving it over the comparison");
  Expect(WorkspaceShellTestAccess::ActiveTabIsCompare(shell),
         "selecting a commit should open and focus the comparison tab");
}

microide::project::GitCommitEntry MakeFakeCommit(int index) {
  microide::project::GitCommitEntry commit;
  const std::string suffix = std::to_string(index);
  commit.hash = "deadbeef" + suffix;
  commit.short_hash = "dead" + suffix;
  commit.subject = "fake commit " + suffix;
  commit.author = "tester";
  commit.relative_date = "just now";
  return commit;
}

void TestWorkspaceShellComparePickerOpensAsyncAndDropsStaleResult() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source_a = root / "src" / "a.cpp";
  const std::filesystem::path source_b = root / "src" / "b.cpp";

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);

  std::atomic<bool> release{false};
  std::atomic<int> call_count{0};
  WorkspaceShellTestAccess::SetComparePickerFileHistoryProvider(
      shell,
      [&release, &call_count](const std::filesystem::path&, const std::filesystem::path&) {
        const int this_call = ++call_count;
        while (!release.load(std::memory_order_acquire)) {
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        microide::project::GitFileHistoryResult result;
        for (int i = 0; i < this_call; ++i) {
          result.commits.push_back(MakeFakeCommit(i));
        }
        return result;
      });

  Expect(WorkspaceShellTestAccess::OpenComparePickerForPath(shell, source_a),
         "opening the async commit picker should succeed immediately");
  Expect(WorkspaceShellTestAccess::OverlayVisible(shell) &&
             WorkspaceShellTestAccess::ActiveOverlayMode(shell) ==
                 WorkspaceShell::OverlayMode::CommitPicker,
         "the commit picker overlay should be visible while the query is in flight");
  Expect(WorkspaceShellTestAccess::ComparePickerLoading(shell),
         "the picker should report loading before git returns");
  Expect(WorkspaceShellTestAccess::ComparePickerItemCount(shell) == 0,
         "the picker item list should be empty while loading");

  const auto call_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (call_count.load() < 1 && std::chrono::steady_clock::now() < call_deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  Expect(call_count.load() >= 1, "the first async history query should have started");

  Expect(WorkspaceShellTestAccess::OpenComparePickerForPath(shell, source_b),
         "opening a second async commit picker should succeed");
  Expect(WorkspaceShellTestAccess::ComparePickerLoading(shell),
         "the superseding picker should also start in a loading state");

  release.store(true, std::memory_order_release);
  Expect(SettleComparePicker(shell), "both async queries should settle");
  Expect(!WorkspaceShellTestAccess::ComparePickerLoading(shell),
         "the picker should leave the loading state once the current query lands");
  Expect(WorkspaceShellTestAccess::ComparePickerItemCount(shell) == 2,
         "only the superseding (second) result should populate the picker, not the stale first");
}

void TestWorkspaceShellCompareRecomputeGate() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "main.cpp";
  WriteFile(source, "int alpha() {\n  return 1;\n}\n");
  InitializeGitRepo(root);
  CommitAll(root, "Add compare gate fixture", "compare gate fixture");
  WriteFile(source, "int beta() {\n  return 2;\n}\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, source, "HEAD", "HEAD"),
         "working-tree comparison should open");
  auto& compare = WorkspaceShellTestAccess::ActiveCompare(shell);

  const std::uint64_t baseline_revision = compare.model_revision;
  WorkspaceShellTestAccess::RefreshActiveCompareDerivedState(shell);
  WorkspaceShellTestAccess::RefreshActiveCompareDerivedState(shell);
  Expect(compare.model_revision == baseline_revision,
         "no-op refreshes must not rebuild the compare model");

  compare.left_content = compare::MakeCompareText("int gamma() {\n  return 3;\n}\n");
  WorkspaceShellTestAccess::RefreshActiveCompareDerivedState(shell);
  Expect(compare.model_revision == baseline_revision + 1,
         "changed content must rebuild the compare model once");
  const std::uint64_t after_content = compare.model_revision;
  WorkspaceShellTestAccess::RefreshActiveCompareDerivedState(shell);
  Expect(compare.model_revision == after_content,
         "refresh after a rebuild with unchanged content must not rebuild again");

  compare.build_options.ignore_whitespace = !compare.build_options.ignore_whitespace;
  WorkspaceShellTestAccess::RefreshActiveCompareDerivedState(shell);
  Expect(compare.model_revision == after_content + 1,
         "toggling ignore-whitespace must rebuild the compare model");

  const std::uint64_t before_right_edit = compare.model_revision;
  Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "// note "),
         "typing into the editable current-state pane should edit it");
  Expect(compare.model_revision > before_right_edit,
         "editing the right pane must rebuild the compare model");
  const std::uint64_t after_right_edit = compare.model_revision;
  WorkspaceShellTestAccess::RefreshActiveCompareDerivedState(shell);
  WorkspaceShellTestAccess::RefreshActiveCompareDerivedState(shell);
  Expect(compare.model_revision == after_right_edit,
         "no-op refreshes after a right-pane edit must not rebuild the compare model");
}

void TestWorkspaceShellCompareHorizontalScrollRepaintsTheSurface() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  EnsureDummySdlVideo();
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "notes.md";
  const std::string long_line(150, 'w');
  WriteFile(source, long_line + "\nA tiny, dependency-free task scheduler\nsecond row of text\n");
  InitializeGitRepo(root);
  CommitAll(root, "Add compare scroll fixture", "compare scroll fixture");
  WriteFile(source, long_line + "\nA tiny, dependency-free task scheduler\nsecond row, edited\n");

  static constexpr int kWidth = 1280;
  static constexpr int kHeight = 720;
  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, kWidth, kHeight);
  Expect(WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, source, "HEAD", "HEAD"),
         "working-tree comparison should open");
  auto& compare = WorkspaceShellTestAccess::ActiveCompare(shell);
  Expect(compare.right_editable && compare.right_view_active, "the editable pane is active");
  compare.right_viewport.MoveCursorTo(0, long_line.size());
  const auto pin_caret = [&]() { WorkspaceShellTestAccess::SetCaretBlinkEpochMs(shell, SDL_GetTicks()); };
  microide::tests::SoftwareCanvas canvas(kWidth, kHeight);
  pin_caret();
  shell.Render(canvas.renderer(), kWidth, kHeight);
  (void)shell.ConsumePendingRenderInvalidation();
  const std::size_t scroll_before = compare.horizontal_scroll;
  for (int i = 0; i < 4; ++i) {
    const auto diff = PaintAndDiffWithFullRepaint(
        shell, canvas, kWidth, kHeight, DispatchEvents(shell, {TextInputEvent("xxxxxxxx")}),
        pin_caret);
    Expect(diff.identical(), "typing at the line's end should paint what a full repaint "
                             "paints; " + DescribePixelDiff(diff));
  }
  Expect(compare.horizontal_scroll != scroll_before,
         "the fixture must actually scroll the panes sideways");
}


void TestWorkspaceShellCompareInASplitStaysInItsPane() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  EnsureDummySdlVideo();
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "notes.md";
  const std::filesystem::path other = root / "other.md";
  std::string base;
  std::string edited;
  for (int line = 0; line < 240; ++line) {
    base += "line " + std::to_string(line) + "\n";
    edited += (line % 2 == 0 ? "LINE " : "line ") + std::to_string(line) + "\n";
  }
  WriteFile(source, base);
  WriteFile(other, "the other pane's editor\nsecond line\n");
  InitializeGitRepo(root);
  CommitAll(root, "Add split compare fixture", "split compare fixture");
  WriteFile(source, edited);

  static constexpr int kWidth = 1280;
  static constexpr int kHeight = 720;
  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, kWidth, kHeight);
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, other);
  Expect(WorkspaceShellTestAccess::SplitEditorGroup(
             shell, microide::workspace::EditorSplitOrientation::Vertical),
         "the fixture must split the editor area");
  const std::size_t compare_group = WorkspaceShellTestAccess::FocusedGroupIndex(shell);
  const std::size_t editor_group = compare_group == 0 ? 1 : 0;
  const SDL_FRect editor_pane = WorkspaceShellTestAccess::GroupEditorSurfaceRect(shell, editor_group);

  microide::tests::SoftwareCanvas canvas(kWidth, kHeight);
  shell.Render(canvas.renderer(), kWidth, kHeight);
  (void)shell.ConsumePendingRenderInvalidation();
  SDL_FlushRenderer(canvas.renderer());
  SDL_Surface* before = SDL_DuplicateSurface(canvas.surface());
  Expect(before != nullptr, "the fixture keeps the pre-diff frame");

  Expect(WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, source, "HEAD", "HEAD"),
         "working-tree comparison should open");
  Expect(WorkspaceShellTestAccess::FocusedGroupIndex(shell) == compare_group,
         "the comparison opens in the focused pane");
  const SDL_FRect compare_pane = WorkspaceShellTestAccess::GroupEditorSurfaceRect(shell, compare_group);
  const SDL_FRect surface = WorkspaceShellTestAccess::ActiveTabSurface(shell);
  Expect(surface.x == compare_pane.x && surface.y == compare_pane.y && surface.w == compare_pane.w &&
             surface.h == compare_pane.h,
         "the diff's surface is its own pane, not the editor column");

  const auto pin_caret = [&]() { WorkspaceShellTestAccess::SetCaretBlinkEpochMs(shell, SDL_GetTicks()); };
  pin_caret();
  WorkspaceShellTestAccess::ResetRenderCompareSurfaceInvocationCount(shell);
  shell.Render(canvas.renderer(), kWidth, kHeight);
  (void)shell.ConsumePendingRenderInvalidation();
  Expect(WorkspaceShellTestAccess::RenderCompareSurfaceInvocationCount(shell) == 1,
         "the diff paints once, in its pane");
  std::size_t changed = 0;
  for (int y = static_cast<int>(editor_pane.y) + 1; y < static_cast<int>(editor_pane.y + editor_pane.h) - 1;
       y += 2) {
    for (int x = static_cast<int>(editor_pane.x) + 1; x < static_cast<int>(editor_pane.x + editor_pane.w) - 1;
         x += 2) {
      Uint8 ar = 0, ag = 0, ab = 0, aa = 0;
      Expect(SDL_ReadSurfacePixel(before, x, y, &ar, &ag, &ab, &aa), "read the pre-diff frame");
      const SDL_Color b = canvas.PixelAt(x, y);
      changed += (ar != b.r || ag != b.g || ab != b.b) ? 1 : 0;
    }
  }
  SDL_DestroySurface(before);
  Expect(changed == 0, "the other pane still shows its own editor, untouched by the diff (" +
                           std::to_string(changed) + " sampled pixels changed)");

  {
    const float x = editor_pane.x + editor_pane.w * 0.5f;
    const float y = editor_pane.y + editor_pane.h * 0.5f;
    const auto diff = PaintAndDiffWithFullRepaint(
        shell, canvas, kWidth, kHeight,
        DispatchEvents(shell, {MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_DOWN, x, y),
                               MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_UP, x, y)}),
        pin_caret);
    Expect(diff.identical(), "focusing the editor pane paints what a full repaint paints; " +
                                 DescribePixelDiff(diff));
  }
  Expect(WorkspaceShellTestAccess::FocusedGroupIndex(shell) == editor_group,
         "a click into the editor pane focuses it");
  WorkspaceShellTestAccess::ResetRenderCompareSurfaceInvocationCount(shell);
  shell.Render(canvas.renderer(), kWidth, kHeight);
  (void)shell.ConsumePendingRenderInvalidation();
  Expect(WorkspaceShellTestAccess::RenderCompareSurfaceInvocationCount(shell) == 1,
         "a diff in an unfocused pane still paints there (not the welcome page)");

  {
    auto& unfocused_compare = WorkspaceShellTestAccess::ActiveCompareInGroup(shell, compare_group);
    const int scroll_before = unfocused_compare.scroll_row;
    const float x = compare_pane.x + compare_pane.w * 0.25f;
    const float y = compare_pane.y + compare_pane.h * 0.5f;
    const auto diff = PaintAndDiffWithFullRepaint(
        shell, canvas, kWidth, kHeight, DispatchEvents(shell, {MouseWheelEvent(x, y, -3.0f)}),
        pin_caret);
    Expect(diff.identical(), "wheeling the unfocused diff paints what a full repaint paints; " +
                                 DescribePixelDiff(diff));
    Expect(unfocused_compare.scroll_row > scroll_before, "the wheel scrolls the diff under the pointer");
    Expect(WorkspaceShellTestAccess::FocusedGroupIndex(shell) == editor_group,
           "and leaves the focus where it was");
  }

  {
    const float x = compare_pane.x + compare_pane.w * 0.25f;
    const float y = compare_pane.y + compare_pane.h * 0.5f;
    const auto diff = PaintAndDiffWithFullRepaint(
        shell, canvas, kWidth, kHeight,
        DispatchEvents(shell, {MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_DOWN, x, y),
                               MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_UP, x, y)}),
        pin_caret);
    Expect(diff.identical(), "focusing the diff pane paints what a full repaint paints; " +
                                 DescribePixelDiff(diff));
  }
  Expect(WorkspaceShellTestAccess::FocusedGroupIndex(shell) == compare_group,
         "a click into an unfocused diff's pane focuses that pane");
}

void TestWorkspaceShellCompareTypingRepaintsTheHorizontalThumb() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  EnsureDummySdlVideo();
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "notes.md";
  const std::string long_line(150, 'w');
  WriteFile(source, long_line + "\nA tiny, dependency-free task scheduler\nsecond row of text\n");
  InitializeGitRepo(root);
  CommitAll(root, "Add compare thumb fixture", "compare thumb fixture");
  WriteFile(source, long_line + "\nA tiny, dependency-free task scheduler\nsecond row, edited\n");

  static constexpr int kWidth = 1280;
  static constexpr int kHeight = 720;
  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, kWidth, kHeight);
  Expect(WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, source, "HEAD", "HEAD"),
         "working-tree comparison should open");
  auto& compare = WorkspaceShellTestAccess::ActiveCompare(shell);
  Expect(compare.right_editable && compare.right_view_active, "the editable pane is active");
  compare.right_viewport.MoveCursorTo(0, 10);
  const auto pin_caret = [&]() { WorkspaceShellTestAccess::SetCaretBlinkEpochMs(shell, SDL_GetTicks()); };
  microide::tests::SoftwareCanvas canvas(kWidth, kHeight);
  pin_caret();
  shell.Render(canvas.renderer(), kWidth, kHeight);
  (void)shell.ConsumePendingRenderInvalidation();
  const std::size_t scroll_before = compare.horizontal_scroll;
  for (int i = 0; i < 4; ++i) {
    const auto diff = PaintAndDiffWithFullRepaint(
        shell, canvas, kWidth, kHeight, DispatchEvents(shell, {TextInputEvent("xxxxxxxx")}),
        pin_caret);
    Expect(diff.identical(), "typing mid-line should paint what a full repaint paints; " +
                                 DescribePixelDiff(diff));
  }
  Expect(compare.horizontal_scroll == scroll_before,
         "the fixture must NOT scroll, or the surface repaints for that instead");
}


void TestWorkspaceShellComparePaintsOfOneFrameAgree() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  EnsureDummySdlVideo();
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "big.cpp";
  std::string before;
  for (int line = 0; line < 150; ++line) {
    before += "// comment line " + std::to_string(line) + "\nint value_" + std::to_string(line) + " = 1;\n";
  }
  WriteFile(source, before);
  InitializeGitRepo(root);
  CommitAll(root, "Add big compare fixture", "big compare fixture");
  WriteFile(source, before + "// a new last comment\nint tail = 2;\n");

  static constexpr int kWidth = 1280;
  static constexpr int kHeight = 720;
  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, kWidth, kHeight);
  Expect(WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, source, "HEAD", "HEAD"),
         "working-tree comparison should open");
  auto& compare = WorkspaceShellTestAccess::ActiveCompare(shell);
  compare.scroll_row = std::numeric_limits<int>::max() / 2;
  const auto pin_caret = [&]() { WorkspaceShellTestAccess::SetCaretBlinkEpochMs(shell, SDL_GetTicks()); };

  microide::tests::SoftwareCanvas canvas(kWidth, kHeight);
  pin_caret();
  const WorkspaceShell::FrameToken token = shell.PrepareFrameOnce(canvas.renderer(), kWidth, kHeight);
  (void)shell.ConsumePendingRenderInvalidation();
  const SDL_FRect everything{0.0f, 0.0f, static_cast<float>(kWidth), static_cast<float>(kHeight)};
  shell.RenderClip(token, canvas.renderer(), kWidth, kHeight, everything);
  SDL_FlushRenderer(canvas.renderer());
  SDL_Surface* first = SDL_DuplicateSurface(canvas.surface());
  shell.RenderClip(token, canvas.renderer(), kWidth, kHeight);
  SDL_FlushRenderer(canvas.renderer());
  const std::size_t differing = CountPixelDifferences(first, canvas.surface());
  SDL_DestroySurface(first);
  Expect(differing == 0, "a hinted paint and a full paint of the same frame paint the same pixels: " +
                             std::to_string(differing) + " differ");
}


void TestWorkspaceShellCompareSyntaxCatchUpSchedulesTheNextFrame() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  EnsureDummySdlVideo();
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "big.cpp";
  std::string before;
  for (int line = 0; line < 150; ++line) {
    before += "// comment line " + std::to_string(line) + "\nint value_" + std::to_string(line) + " = 1;\n";
  }
  WriteFile(source, before);
  InitializeGitRepo(root);
  CommitAll(root, "Add catch-up fixture", "catch-up fixture");
  WriteFile(source, before + "// a new last comment\nint tail = 2;\n");

  static constexpr int kWidth = 1280;
  static constexpr int kHeight = 720;
  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, kWidth, kHeight);
  Expect(WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, source, "HEAD", "HEAD"),
         "working-tree comparison should open");
  const auto pin_caret = [&]() { WorkspaceShellTestAccess::SetCaretBlinkEpochMs(shell, SDL_GetTicks()); };
  microide::tests::SoftwareCanvas canvas(kWidth, kHeight);
  pin_caret();
  shell.Render(canvas.renderer(), kWidth, kHeight);
  (void)shell.ConsumePendingRenderInvalidation();

  int frames = 0;
  for (; frames < 16; ++frames) {
    shell.OnFramePresented();
    const workspace::RenderInvalidation next = shell.ConsumePendingRenderInvalidation();
    const auto diff = PaintAndDiffWithFullRepaint(shell, canvas, kWidth, kHeight, next, pin_caret);
    Expect(diff.identical(), "frame " + std::to_string(frames) +
                                 " of the catch-up paints what a full repaint paints; " +
                                 DescribePixelDiff(diff));
    if (next.rects.empty() && !next.full) {
      break;
    }
  }
  Expect(frames < 16, "the catch-up stops asking for frames once it has caught up");
}

void TestCompareCollapsedBarButtonsStayInsideTheirRow() {
  render::TextRenderer text_renderer;
  for (const float row_height : {10.0f, 15.0f, 16.0f, 24.0f}) {
    const SDL_FRect row{100.0f, 200.0f, 800.0f, row_height};
    const workspace::CollapsedContextActionRects rects =
        workspace::BuildCollapsedContextActionRects(text_renderer, row, true, true);
    for (const std::optional<SDL_FRect>& button : {rects.previous_rect, std::optional(rects.all_rect), rects.next_rect}) {
      Expect(button.has_value() && button->y >= row.y && button->y + button->h <= row.y + row.h,
             "a " + std::to_string(row_height) + " px row holds its buttons: button y=" +
                 std::to_string(button ? button->y : -1.0f) + " h=" + std::to_string(button ? button->h : -1.0f));
    }
  }
}

}

void TestWorkspaceShellWorkingTreeCompareRejectsBinaryAndUnreadable() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path missing_file = root / "src" / "missing.cpp";
  const std::filesystem::path binary_file = root / "src" / "binary.cpp";
  const std::filesystem::path locked_file = root / "src" / "locked.cpp";
  WriteFile(missing_file, "int alpha() {\n  return 1;\n}\n");
  WriteFile(binary_file, "int beta() {\n  return 2;\n}\n");
  WriteFile(locked_file, "int gamma() {\n  return 3;\n}\n");

  InitializeGitRepo(root);
  CommitAll(root, "Add compare reject fixture", "compare reject fixture");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);

  std::filesystem::remove(missing_file);
  Expect(WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, missing_file, "HEAD", "HEAD"),
         "a missing working-tree file should still open as a whole-file-deleted diff");

  std::string binary_bytes;
  binary_bytes.push_back('\0');
  binary_bytes.append("PNG\r\n\x1a\n binary body ");
  binary_bytes.push_back('\0');
  binary_bytes.append(" trailing");
  WriteFile(binary_file, binary_bytes);
  Expect(!WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, binary_file, "HEAD", "HEAD"),
         "a binary working-tree file must not open as an editable text compare");

  std::error_code ec;
  std::filesystem::permissions(locked_file, std::filesystem::perms::none,
                               std::filesystem::perm_options::replace, ec);
  if (!ec) {
    std::ifstream probe(locked_file, std::ios::binary);
    const bool actually_locked = !probe.good();
    probe.close();
    if (actually_locked) {
      Expect(!WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, locked_file, "HEAD", "HEAD"),
             "an unreadable working-tree file must not open as an empty compare");
    }
    std::filesystem::permissions(locked_file, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace, ec);
  }
}

void TestWorkspaceShellCompareSyntaxReachesDeepCollapsedRows() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "big.cpp";
  std::string base;
  for (int i = 0; i < 300; ++i) {
    base += "int v" + std::to_string(i) + " = " + std::to_string(i) + ";\n";
  }
  WriteFile(source, base);
  InitializeGitRepo(root);
  CommitAll(root, "big base fixture", "big base fixture");
  std::string edited = base;
  edited.replace(edited.find("int v0 = 0;"), std::string("int v0 = 0;").size(), "int v0 = 999;");
  edited.replace(edited.find("int v299 = 299;"), std::string("int v299 = 299;").size(),
                 "int v299 = 42;");
  WriteFile(source, edited);

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, source, "HEAD", "HEAD"),
         "large collapsed-diff comparison should open");

  auto& compare = WorkspaceShellTestAccess::ActiveCompare(shell);
  const std::size_t model_row_count = compare.model.rows.size();
  const std::size_t presentation_row_count =
      WorkspaceShellTestAccess::ActiveComparePresentationRowCount(shell);
  Expect(model_row_count > presentation_row_count + 100,
         "the unchanged middle should collapse so model rows far exceed presentation rows");

  compare.scroll_row = static_cast<int>(presentation_row_count);

  SoftwareCanvas canvas(1280, 720);
  for (int frame = 0; frame < 20; ++frame) {
    shell.Render(canvas.renderer(), 1280, 720);
  }

  auto& tokenized_compare = WorkspaceShellTestAccess::ActiveCompare(shell);
  Expect(tokenized_compare.left_token_window.frontier() == tokenized_compare.left_line_rows.size(),
         "tokenization reaches the deepest visible line behind the collapsed run");
  bool deep_row_has_tokens = false;
  const std::size_t tail_start = tokenized_compare.model.rows.size() > 8
                                     ? tokenized_compare.model.rows.size() - 8
                                     : 0;
  for (std::size_t row = tail_start; row < tokenized_compare.model.rows.size(); ++row) {
    if (!microide::workspace::CompareRowLeftTokens(tokenized_compare, tokenized_compare.model.rows[row]).empty() ||
        !microide::workspace::CompareRowRightOwnTokens(tokenized_compare, tokenized_compare.model.rows[row]).empty()) {
      deep_row_has_tokens = true;
      break;
    }
  }
  Expect(deep_row_has_tokens,
         "a deep content row behind the collapsed run has syntax tokens (is highlighted)");
}

microide::workspace::CompareInput MakeFileCompareInput(const std::filesystem::path& path,
                                                       bool editable) {
  auto input = microide::workspace::ReadFileCompareInput(path, editable);
  Expect(input.has_value(), "plain-compare fixture file should read cleanly");
  return std::move(*input);
}

void TestWorkspaceShellPlainCompareBuildsStickyEditableTab() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path left_path = root / "a.txt";
  const std::filesystem::path right_path = root / "b.txt";
  WriteFile(left_path, "alpha\nshared\n");
  WriteFile(right_path, "beta\nshared\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  Expect(WorkspaceShellTestAccess::OpenPlainComparison(shell,
                                                       MakeFileCompareInput(left_path, false),
                                                       MakeFileCompareInput(right_path, true)),
         "OpenPlainComparison of two files should succeed");
  Expect(WorkspaceShellTestAccess::ActiveTabIsCompare(shell),
         "a plain comparison should open as a compare tab");

  auto& compare = WorkspaceShellTestAccess::ActiveCompare(shell);
  Expect(compare.plain_compare, "the tab should be marked as a plain (non-git) comparison");
  Expect(compare.review_mode == microide::compare::CompareReviewMode::Plain,
         "a plain comparison should use the Plain review mode");
  Expect(compare.left_label == "a.txt" && compare.right_label == "b.txt",
         "plain-compare labels should be the two filenames");
  Expect(compare.left_path == left_path.lexically_normal() &&
             compare.right_path == right_path.lexically_normal(),
         "plain-compare should keep the distinct left/right paths");
  Expect(compare.right_editable,
         "the right pane should be editable when its side is a real file");
  Expect(!compare.model.hunks.empty(), "differing files should produce at least one hunk");

  WorkspaceShellTestAccess::RefreshActiveCompareDerivedState(shell);
  Expect(compare.plain_compare && compare.review_mode == microide::compare::CompareReviewMode::Plain,
         "review mode should stay Plain across a derived-state refresh");
}

void TestWorkspaceShellCompareOpenDoesNotMaterializeEitherSide() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path left_path = root / "left.txt";
  const std::filesystem::path right_path = root / "right.txt";
  std::string left = "#!/usr/bin/env python3\n";
  std::string right = "#!/usr/bin/env python3\n";
  for (int i = 0; i < 4000; ++i) {
    left += "value_" + std::to_string(i) + " = " + std::to_string(i) + "\n";
    right += "value_" + std::to_string(i) + " = " + std::to_string(i % 97) + "\n";
  }
  WriteFile(left_path, left);
  WriteFile(right_path, right);

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  microide::editor::TextBuffer::reset_snapshot_build_count();
  Expect(WorkspaceShellTestAccess::OpenPlainComparison(shell,
                                                       MakeFileCompareInput(left_path, false),
                                                       MakeFileCompareInput(right_path, true)),
         "the large plain comparison should open");
  const std::size_t snapshots = microide::editor::TextBuffer::snapshot_build_count();
  Expect(snapshots == 0,
         "opening a compare tab must not build a whole-document snapshot (built " +
             std::to_string(snapshots) + ")");

  auto& compare = WorkspaceShellTestAccess::ActiveCompare(shell);
  Expect(!compare.model.hunks.empty(), "the fixture must actually differ");
  Expect(compare.left_token_window.StateBefore(0).definition_id != 0,
         "the left side's syntax state must still be detected from its shebang");
  Expect(compare.right_token_window.StateBefore(0).definition_id ==
             compare.left_token_window.StateBefore(0).definition_id,
         "both sides detect the same definition from the same shebang");
}

void TestWorkspaceShellPlainCompareBufferSidesAreReadOnly() {
  WorkspaceShell shell;
  microide::workspace::CompareInput clipboard{
      .content = "one\ntwo\n", .label = "Clipboard", .path = {}, .editable = false};
  microide::workspace::CompareInput buffer{
      .content = "one\nTWO\n", .label = "Untitled", .path = {}, .editable = true};
  Expect(WorkspaceShellTestAccess::OpenPlainComparison(shell, std::move(clipboard),
                                                       std::move(buffer)),
         "OpenPlainComparison of buffer vs clipboard should succeed");
  auto& compare = WorkspaceShellTestAccess::ActiveCompare(shell);
  Expect(compare.plain_compare, "buffer/clipboard comparison should be plain");
  Expect(!compare.right_editable,
         "the right pane must be read-only when its side has no on-disk path");
  Expect(compare.left_label == "Clipboard" && compare.right_label == "Untitled",
         "buffer/clipboard labels should be preserved");
}

void TestWorkspaceShellPlainCompareDedupsSameFilePair() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path left_path = root / "a.txt";
  const std::filesystem::path right_path = root / "b.txt";
  WriteFile(left_path, "alpha\n");
  WriteFile(right_path, "beta\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  Expect(WorkspaceShellTestAccess::OpenPlainComparison(shell,
                                                       MakeFileCompareInput(left_path, false),
                                                       MakeFileCompareInput(right_path, true)),
         "first plain comparison should open");
  const std::size_t tabs_after_first = WorkspaceShellTestAccess::FocusedGroupOpenTabCount(shell);
  Expect(WorkspaceShellTestAccess::OpenPlainComparison(shell,
                                                       MakeFileCompareInput(left_path, false),
                                                       MakeFileCompareInput(right_path, true)),
         "re-opening the same file pair should succeed");
  Expect(WorkspaceShellTestAccess::FocusedGroupOpenTabCount(shell) == tabs_after_first,
         "re-opening the same file pair should activate the existing tab, not add one");
}

void TestWorkspaceShellCompareFilesCommandOpensPlainCompare() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path left_path = temp_dir.path() / "outside_a.txt";
  const std::filesystem::path right_path = temp_dir.path() / "outside_b.txt";
  WriteFile(left_path, "one\n");
  WriteFile(right_path, "TWO\n");

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::ExecuteAction(
             shell, WorkspaceShell::ActionId::CompareFiles,
             {left_path.string(), right_path.string()}),
         "compare-files command should be handled");
  Expect(WorkspaceShellTestAccess::ActiveTabIsCompare(shell),
         "compare-files should open a compare tab");
  auto& compare = WorkspaceShellTestAccess::ActiveCompare(shell);
  Expect(compare.plain_compare && compare.left_label == "outside_a.txt" &&
             compare.right_label == "outside_b.txt",
         "compare-files should build a plain comparison of the two files");
}

void TestWorkspaceShellSelectForCompareThenCompareWithSelected() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path file_a = root / "a.txt";
  const std::filesystem::path file_b = root / "b.txt";
  WriteFile(file_a, "alpha\n");
  WriteFile(file_b, "beta\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);

  Expect(!WorkspaceShellTestAccess::IsActionEnabled(shell,
                                                    WorkspaceShell::ActionId::CompareWithSelected),
         "Compare with Selected should be disabled before anything is selected");

  WorkspaceShellTestAccess::OpenTreeContextMenuForPath(shell, microide::workspace::TreeContextTargetKind::File, file_a);
  Expect(WorkspaceShellTestAccess::ExecuteContextMenuAction(
             shell, WorkspaceShell::ActionId::SelectForCompare),
         "Select for Compare should execute");
  Expect(WorkspaceShellTestAccess::HasCompareSelection(shell),
         "selecting a file should stash a compare selection");

  WorkspaceShellTestAccess::OpenTreeContextMenuForPath(shell, microide::workspace::TreeContextTargetKind::File, file_b);
  Expect(WorkspaceShellTestAccess::ExecuteContextMenuAction(
             shell, WorkspaceShell::ActionId::CompareWithSelected),
         "Compare with Selected should execute");
  Expect(WorkspaceShellTestAccess::ActiveTabIsCompare(shell),
         "Compare with Selected should open a compare tab");
  auto& compare = WorkspaceShellTestAccess::ActiveCompare(shell);
  Expect(compare.plain_compare && compare.left_label == "a.txt" && compare.right_label == "b.txt",
         "the stashed file should be the left side and the current file the right side");
}

void TestWorkspaceShellCompareWithClipboardOpensPlainCompare() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path file_a = root / "a.txt";
  WriteFile(file_a, "alpha\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetClipboardTextReader(
      shell, []() { return std::optional<std::string>("clipboard body\n"); });

  WorkspaceShellTestAccess::OpenTreeContextMenuForPath(shell, microide::workspace::TreeContextTargetKind::File, file_a);
  Expect(WorkspaceShellTestAccess::ExecuteContextMenuAction(
             shell, WorkspaceShell::ActionId::CompareWithClipboard),
         "Compare with Clipboard should execute");
  Expect(WorkspaceShellTestAccess::ActiveTabIsCompare(shell),
         "Compare with Clipboard should open a compare tab");
  auto& compare = WorkspaceShellTestAccess::ActiveCompare(shell);
  Expect(compare.plain_compare && compare.left_label == "Clipboard" &&
             compare.right_label == "a.txt",
         "clipboard should be the left side and the current file the right side");
  Expect(compare.right_editable,
         "the right pane should be editable when its side is a real file");
}

void TestWorkspaceShellCompareDragAutoscrollsAndKeepsGranularity() {
  EnsureDummySdlVideo();

  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "long.txt";
  std::string base;
  std::string head;
  for (int i = 0; i < 300; ++i) {
    base += "alpha bravo " + std::to_string(i) + "\n";
    head += "alpha charlie " + std::to_string(i) + "\n";
  }
  WriteFile(source, base);
  InitializeGitRepo(root);
  CommitAll(root, "base fixture", "base fixture");
  WriteFile(source, head);
  CommitAll(root, "head fixture", "head fixture");

  const auto history = microide::project::CollectGitFileHistory(root,
                                                                platform::LocalProcessLauncher(),
                                                                    source).commits;
  Expect(history.size() == 2, "compare autoscroll fixture should have two commits");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::OpenBranchHeadComparison(shell, source, history[1].hash, "base",
                                                            history[0].hash, "head"),
         "branch comparison should open");
  WorkspaceShellTestAccess::NoteLayoutInputsChanged(shell);

  const auto layout = WorkspaceShellTestAccess::CurrentLayout(shell);
  const auto surface = WorkspaceShellTestAccess::ActiveCompareSurfaceLayout(shell);
  const float row_y = surface.rows_y + surface.line_height * 0.5f;
  const float right_x = surface.right_x + 24.0f;

  Expect(SendMouseDown(shell, right_x, row_y, SDL_BUTTON_LEFT, 1),
         "the first click should be handled");
  Expect(SendMouseDown(shell, right_x, row_y, SDL_BUTTON_LEFT, 2),
         "the double-click should be handled");
  auto& compare = WorkspaceShellTestAccess::ActiveCompare(shell);
  const auto word_selection = compare.right_viewport.selection_range();
  Expect(word_selection.has_value() && word_selection->start.column != word_selection->end.column,
         "a double-click in the compare right pane should select a word, not place a caret");
  Expect(SendMouseUp(shell, right_x, row_y, SDL_BUTTON_LEFT), "release should be handled");

  Expect(SendMouseDown(shell, right_x, row_y, SDL_BUTTON_LEFT, 1),
         "pressing in the right pane should start a selection");
  Expect(SendMouseMotion(shell, right_x, layout.editor_surface.y + layout.editor_surface.h + 200.0f,
                         SDL_BUTTON_LMASK),
         "dragging below the compare surface should be handled");

  const int scroll_after_motion = WorkspaceShellTestAccess::ActiveCompare(shell).scroll_row;
  for (int tick = 0; tick < 5; ++tick) {
    const auto result = WorkspaceShellTestAccess::HandleScheduledWake(shell);
    Expect(result.handled, "an armed compare autoscroll should keep the wake busy");
  }
  Expect(WorkspaceShellTestAccess::ActiveCompare(shell).scroll_row > scroll_after_motion,
         "holding the pointer past the bottom of a diff must keep scrolling on the wake");
  Expect(SendMouseUp(shell, right_x, row_y, SDL_BUTTON_LEFT), "release should be handled");
}

void TestWorkspaceShellMergeDragAutoscrollsAndKeepsGranularity() {
  EnsureDummySdlVideo();

  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path base = root / "base.txt";
  const std::filesystem::path incoming = root / "incoming.txt";
  const std::filesystem::path current = root / "current.txt";
  const std::filesystem::path output = root / "conflict.txt";
  std::string body;
  for (int i = 0; i < 300; ++i) {
    body += "alpha bravo " + std::to_string(i) + "\n";
  }
  WriteFile(base, body);
  WriteFile(incoming, body + "incoming tail\n");
  WriteFile(current, body + "current tail\n");
  WriteFile(output, body);

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::OpenMergeEditor(shell, base, incoming, current, output),
         "merge autoscroll fixture should open");
  WorkspaceShellTestAccess::NoteLayoutInputsChanged(shell);

  const auto layout = WorkspaceShellTestAccess::CurrentLayout(shell);
  const auto interaction = WorkspaceShellTestAccess::ActiveMergeInteractionLayout(shell);
  const SDL_FRect& result_rect = interaction.result.rect;
  const float x = result_rect.x + std::min(24.0f, result_rect.w * 0.25f);
  const float y = interaction.result.text.first_line_y + interaction.result.text.line_height * 0.5f;

  Expect(SendMouseDown(shell, x, y, SDL_BUTTON_LEFT, 1), "first click handled");
  Expect(SendMouseDown(shell, x, y, SDL_BUTTON_LEFT, 2), "double-click handled");
  const auto word_selection =
      WorkspaceShellTestAccess::ActiveMerge(shell).result_viewport.selection_range();
  Expect(word_selection.has_value() && word_selection->start.column != word_selection->end.column,
         "a double-click in the merge result pane should select a word, not place a caret");
  Expect(SendMouseUp(shell, x, y, SDL_BUTTON_LEFT), "release handled");

  Expect(SendMouseDown(shell, x, y, SDL_BUTTON_LEFT, 1), "press handled");
  Expect(SendMouseMotion(shell, x, layout.editor_surface.y + layout.editor_surface.h + 200.0f,
                         SDL_BUTTON_LMASK),
         "dragging below the merge surface should be handled");

  const int scroll_after_motion = WorkspaceShellTestAccess::ActiveMerge(shell).scroll_row;
  for (int tick = 0; tick < 5; ++tick) {
    const auto result = WorkspaceShellTestAccess::HandleScheduledWake(shell);
    Expect(result.handled, "an armed merge autoscroll should keep the wake busy");
  }
  Expect(WorkspaceShellTestAccess::ActiveMerge(shell).scroll_row > scroll_after_motion,
         "holding the pointer past the bottom of a merge must keep scrolling on the wake");
  Expect(WorkspaceShellTestAccess::ActiveMerge(shell).scroll_row ==
             static_cast<int>(
                 WorkspaceShellTestAccess::ActiveMerge(shell).result_viewport.scroll_line()),
         "the merge tab's scroll mirror must track the result viewport");
  Expect(SendMouseUp(shell, x, y, SDL_BUTTON_LEFT), "release handled");
}


void TestWorkspaceShellCompareWordWrapExpandsRowsAndKeepsPanesAligned() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "main.cpp";
  const std::string long_left = "int alpha() { return " + std::string(600, 'a') + "; }";
  const std::string long_right = "int beta() { return " + std::string(900, 'b') + "; }";
  WriteFile(source, long_left + "\nshort\n");

  InitializeGitRepo(root);
  CommitAll(root, "Add compare wrap fixture", "compare wrap fixture");
  WriteFile(source, long_right + "\nshort\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, source, "HEAD", "HEAD"),
         "compare wrap fixture should open");

  auto& compare = WorkspaceShellTestAccess::ActiveCompare(shell);
  const std::size_t presentation_rows =
      WorkspaceShellTestAccess::ActiveComparePresentationRowCount(shell);
  Expect(!WorkspaceShellTestAccess::ActiveCompareWrapActive(shell),
         "wrap is off by default on a compare tab");
  Expect(WorkspaceShellTestAccess::ActiveCompareVisualRowCount(shell) == presentation_rows,
         "with wrap off an on-screen row IS a presentation row");
  Expect(!compare.right_viewport.soft_wrap(),
         "the compare right pane starts unwrapped");

  Expect(WorkspaceShellTestAccess::SetSettingValue(shell, "editor.wrap", "word"),
         "editor.wrap should be settable");
  Expect(compare.right_viewport.soft_wrap(),
         "turning Word Wrap on must reach the compare tab's editable pane");

  const std::size_t wrapped_rows = WorkspaceShellTestAccess::ActiveCompareVisualRowCount(shell);
  Expect(WorkspaceShellTestAccess::ActiveCompareWrapActive(shell),
         "the compare wrap table should be live once the setting is on");
  Expect(wrapped_rows > presentation_rows,
         "wrapping a 900-column line must add on-screen rows");

  std::size_t previous = 0;
  for (std::size_t visual = 0; visual < wrapped_rows; ++visual) {
    const std::size_t presentation =
        WorkspaceShellTestAccess::ActiveComparePresentationRowForVisualRow(shell, visual);
    Expect(presentation >= previous, "on-screen rows must map to presentation rows in order");
    Expect(presentation < presentation_rows, "a mapped presentation row must be in range");
    previous = presentation;
  }
  for (std::size_t presentation = 0; presentation < presentation_rows; ++presentation) {
    const std::size_t first =
        WorkspaceShellTestAccess::ActiveCompareVisualRowForPresentationRow(shell, presentation);
    Expect(WorkspaceShellTestAccess::ActiveComparePresentationRowForVisualRow(shell, first) ==
               presentation,
           "a presentation row's first on-screen row must map back to it");
  }

  const auto surface = WorkspaceShellTestAccess::ActiveCompareSurfaceLayout(shell);
  Expect(!surface.show_horizontal,
         "a wrapped compare surface must not offer a horizontal scrollbar");

  Expect(WorkspaceShellTestAccess::SetSettingValue(shell, "editor.wrap", "off"),
         "editor.wrap should be settable back off");
  Expect(!compare.right_viewport.soft_wrap(), "turning Word Wrap off must reach the compare pane");
  Expect(WorkspaceShellTestAccess::ActiveCompareVisualRowCount(shell) == presentation_rows,
         "turning wrap off must collapse the row table back to the identity");
}

void TestWorkspaceShellMergeWordWrapExpandsRows() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path base = root / "base.txt";
  const std::filesystem::path incoming = root / "incoming.txt";
  const std::filesystem::path current = root / "current.txt";
  const std::filesystem::path output = root / "result.txt";
  const std::string long_line(900, 'x');
  WriteFile(base, "top\nbase\nbottom\n");
  WriteFile(incoming, "top\n" + long_line + "\nbottom\n");
  WriteFile(current, "top\ncurrent\nbottom\n");
  WriteFile(output, "top\ncurrent\nbottom\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::OpenMergeEditor(shell, base, incoming, current, output),
         "merge editor should open for the wrap fixture");

  auto& merge = WorkspaceShellTestAccess::ActiveMerge(shell);
  const std::size_t unwrapped_rows = WorkspaceShellTestAccess::ActiveMergeVisualRowCount(shell);
  Expect(!WorkspaceShellTestAccess::ActiveMergeWrapActive(shell),
         "wrap is off by default on a merge tab");
  Expect(!merge.result_viewport.soft_wrap(), "the merge result pane starts unwrapped");

  Expect(WorkspaceShellTestAccess::SetSettingValue(shell, "editor.wrap", "word"),
         "editor.wrap should be settable");
  Expect(merge.result_viewport.soft_wrap(),
         "turning Word Wrap on must reach the merge tab's result pane");
  Expect(WorkspaceShellTestAccess::ActiveMergeWrapActive(shell),
         "the merge source-pane wrap table should be live once the setting is on");
  Expect(WorkspaceShellTestAccess::ActiveMergeVisualRowCount(shell) > unwrapped_rows,
         "wrapping a 900-column incoming line must add on-screen rows");

  const auto surface = WorkspaceShellTestAccess::ActiveMergeSurfaceLayout(shell);
  Expect(!surface.show_horizontal,
         "a wrapped merge surface must not offer a horizontal scrollbar");
}

void TestWorkspaceShellCompareEditActionRefreshesDiffModel() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "main.cpp";
  WriteFile(source, "alpha\nbravo\ncharlie\n");

  InitializeGitRepo(root);
  CommitAll(root, "Add compare action fixture", "compare action fixture");
  WriteFile(source, "alpha\nbravo\ncharlie\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, source, "HEAD", "HEAD"),
         "compare action fixture should open");

  auto& compare = WorkspaceShellTestAccess::ActiveCompare(shell);
  Expect(compare.right_editable, "the working-tree compare pane should be editable");
  compare.right_viewport.MoveCursorTo(0, 0);

  const auto painted_right_side = [&compare]() {
    std::string joined;
    for (const auto& row : compare.model.rows) {
      if (row.right_line > 0) {
        joined += row.right_text;
        joined += '\n';
      }
    }
    return joined;
  };
  const std::string initial_right_side = painted_right_side();
  Expect(initial_right_side.rfind("alpha\nbravo\ncharlie", 0) == 0,
         "the diff model should start out agreeing with the working-tree buffer");

  Expect(WorkspaceShellTestAccess::ExecuteAction(shell, WorkspaceShell::ActionId::MoveLineDown, {}),
         "MoveLineDown should be dispatched to the compare tab's editable pane");
  Expect(compare.right_viewport.lines().LineView(0) == "bravo",
         "MoveLineDown should have edited the compare pane's buffer");
  Expect(painted_right_side().rfind("bravo\nalpha\ncharlie", 0) == 0,
         "the diff model must be rebuilt after an action-driven edit, not left stale");
}

std::string Quoted(std::string_view text) {
  std::string out = "\"";
  for (char c : text) {
    if (c == '\n') {
      out += "\\n";
    } else {
      out.push_back(c);
    }
  }
  return out + "\"";
}

void TestWorkspaceShellCompareEveryActionKeepsTheDiffModelInSync() {
  const std::vector<WorkspaceShell::ActionId> skipped = {
      WorkspaceShell::ActionId::CloseActiveTab,
      WorkspaceShell::ActionId::CloseOtherTabs,
      WorkspaceShell::ActionId::CloseTabsToRight,
      WorkspaceShell::ActionId::CloseTabsToLeft,
      WorkspaceShell::ActionId::CloseAllTabs,
      WorkspaceShell::ActionId::CloseGroup,
      WorkspaceShell::ActionId::ProjectClose,
      WorkspaceShell::ActionId::Quit,
      WorkspaceShell::ActionId::Open,
      WorkspaceShell::ActionId::ProjectOpen,
  };

  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "main.cpp";
  WriteFile(source, "alpha\nbravo\ncharlie\ndelta\n");
  InitializeGitRepo(root);
  CommitAll(root, "compare action sweep fixture", "compare action sweep");

  std::size_t mutating_actions = 0;
  for (const auto& spec : microide::workspace::WorkspaceCommandSpecs()) {
    if (std::find(skipped.begin(), skipped.end(), spec.id) != skipped.end()) {
      continue;
    }
    WriteFile(source, "alpha\nbravo\ncharlie\ndelta\n");

    WorkspaceShell shell;
    WorkspaceShellTestAccess::SetProjectRoot(shell, root);
    WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
    if (!WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, source, "HEAD", "HEAD")) {
      Expect(false, std::string("the compare fixture should open for ") +
                        std::string(spec.command_name));
      return;
    }
    const auto tab = [&shell]() { return WorkspaceShellTestAccess::ActiveCompareOrNull(shell); };
    Expect(tab() != nullptr && tab()->right_editable, "the working-tree pane is editable");
    tab()->right_viewport.MoveCursorTo(1, 2);
    tab()->right_viewport.MoveCursorTo(2, 3, true);

    const auto pane_text = [&tab]() {
      std::string joined;
      const auto* compare = tab();
      if (compare == nullptr) return joined;
      for (std::size_t i = 0; i < compare->right_viewport.line_count(); ++i) {
        joined += compare->right_viewport.lines().LineView(i);
        joined += '\n';
      }
      return joined;
    };
    const auto painted_text = [&tab]() {
      std::string joined;
      const auto* compare = tab();
      if (compare == nullptr) return joined;
      for (const auto& row : compare->model.rows) {
        if (row.right_line > 0) {
          joined += row.right_text;
          joined += '\n';
        }
      }
      return joined;
    };

    const std::string before = pane_text();
    Expect(painted_text().rfind(before, 0) == 0 || before.rfind(painted_text(), 0) == 0,
           std::string("before ") + std::string(spec.command_name) +
               " the model and the pane already disagree, so this sweep would blame the "
               "wrong verb");

    WorkspaceShellTestAccess::ExecuteAction(shell, spec.id, {});

    if (tab() == nullptr) {
      continue;
    }
    const std::string after = pane_text();
    if (after == before) {
      continue;
    }
    ++mutating_actions;
    const std::string painted = painted_text();
    Expect(painted.rfind(after, 0) == 0,
           std::string("`") + std::string(spec.command_name) +
               "` edited the compare pane's buffer but left the diff model stale: the "
               "pane holds " +
               Quoted(after) + " and the surface would paint " + Quoted(painted));
  }
  Expect(mutating_actions >= 8,
         "the sweep must have found actions that actually edit the compare pane, found "
         "only " +
             std::to_string(mutating_actions));
}

void TestWorkspaceShellCompareMultiCaretIsPainted() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  EnsureDummySdlVideo();
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "main.cpp";
  WriteFile(source, "foo aaa\nfoo bbb\nfoo ccc\n");
  InitializeGitRepo(root);
  CommitAll(root, "Add compare multi-caret fixture", "compare multi-caret fixture");
  WriteFile(source, "foo aaa\nfoo bbb\nfoo ccc\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, source, "HEAD", "HEAD"),
         "compare multi-caret fixture should open");
  auto* compare = WorkspaceShellTestAccess::ActiveCompareOrNull(shell);
  Expect(compare != nullptr && compare->right_editable,
         "the working-tree pane should be editable");

  Expect(WorkspaceShellTestAccess::SetSettingValueTransient(shell, "editor.caret_blink.enabled",
                                                            "false"),
         "the caret blink setting should be settable");

  const auto theme = microide::render::MakeDefaultTheme();
  const auto rows_with_a_caret = [&]() {
    SoftwareCanvas canvas(1280, 720);
    shell.Render(canvas.renderer(), 1280, 720);
    SDL_Surface* pixels = SDL_RenderReadPixels(canvas.renderer(), nullptr);
    Expect(pixels != nullptr, "the compare render should read back software pixels");
    const auto surface = WorkspaceShellTestAccess::ActiveCompareSurfaceLayout(shell);
    int rows = 0;
    for (int row = 0; row < 3; ++row) {
      const int y = static_cast<int>(
          std::floor(surface.rows_y + surface.line_height * (static_cast<float>(row) + 0.5f)));
      bool found = false;
      const int scan_from = static_cast<int>(std::floor(surface.right_x + surface.gutter_width));
      const int scan_to = scan_from + static_cast<int>(std::floor(surface.right_width));
      for (int x = scan_from; x < scan_to && !found; ++x) {
        Uint8 r = 0;
        Uint8 g = 0;
        Uint8 b = 0;
        Uint8 a = 0;
        if (SDL_ReadSurfacePixel(pixels, x, y, &r, &g, &b, &a) && r == theme.cursor.r &&
            g == theme.cursor.g && b == theme.cursor.b && a == theme.cursor.a) {
          found = true;
        }
      }
      if (found) ++rows;
    }
    SDL_DestroySurface(pixels);
    return rows;
  };

  compare->right_viewport.MoveCursorTo(0, 1);
  Expect(rows_with_a_caret() == 1, "one caret paints on one row to start with");

  for (int press = 0; press < 3; ++press) {
    Expect(WorkspaceShellTestAccess::ExecuteAction(
               shell, WorkspaceShell::ActionId::AddCursorAtNextMatch, {}),
           "add-cursor-next-match should dispatch to the compare pane");
  }
  compare = WorkspaceShellTestAccess::ActiveCompareOrNull(shell);
  Expect(compare != nullptr &&
             compare->right_viewport.secondary_caret_range_view().size() == 2,
         "three presses should leave the primary plus two secondary carets");

  Expect(rows_with_a_caret() == 3,
         "every caret must be painted, not just the primary: only " +
             std::to_string(rows_with_a_caret()) + " of 3 rows carried one");

  compare->right_viewport.InsertText("X");
  Expect(std::string(compare->right_viewport.lines().LineView(0)) == "X aaa" &&
             std::string(compare->right_viewport.lines().LineView(1)) == "X bbb" &&
             std::string(compare->right_viewport.lines().LineView(2)) == "X ccc",
         "typing replaces every selected occurrence: " +
             std::string(compare->right_viewport.lines().LineView(0)));
}

void RegisterWorkspaceShellCompareTests(std::vector<TestCase>& tests) {
  AddTest(tests, "WorkspaceShell/CompareCollapsedBarButtonsStayInsideTheirRow",
          TestCompareCollapsedBarButtonsStayInsideTheirRow);
  AddTest(tests, "WorkspaceShell/IdenticalMergeOutputRewriteKeepsResolution",
          TestWorkspaceShellIdenticalMergeOutputRewriteKeepsResolution);
  AddTest(tests, "WorkspaceShell/RealMergeOutputEditDropsResolution",
          TestWorkspaceShellRealMergeOutputEditDropsResolution);
  AddTest(tests, "WorkspaceShell/CompareSyntaxReachesDeepCollapsedRows",
          TestWorkspaceShellCompareSyntaxReachesDeepCollapsedRows);
  AddTest(tests, "WorkspaceShell/CompareDragAutoscrollsAndKeepsGranularity",
          TestWorkspaceShellCompareDragAutoscrollsAndKeepsGranularity);
  AddTest(tests, "WorkspaceShell/MergeDragAutoscrollsAndKeepsGranularity",
          TestWorkspaceShellMergeDragAutoscrollsAndKeepsGranularity);
  AddTest(tests, "WorkspaceShell/CompareLeftSideIsRecognizedByIdentity",
          TestWorkspaceShellCompareLeftSideIsRecognizedByIdentity);
  AddTest(tests, "WorkspaceShell/ExternalChangeRefreshesACompareInAnUnfocusedPane",
          TestWorkspaceShellExternalChangeRefreshesACompareInAnUnfocusedPane);
  AddTest(tests, "WorkspaceShell/ExternalChangeReadsNoCommittedSideAgain",
          TestWorkspaceShellExternalChangeReadsNoCommittedSideAgain);
  AddTest(tests, "WorkspaceShell/ExternalChangeReadsNoRefOnTheShellThread",
          TestWorkspaceShellExternalChangeReadsNoRefOnTheShellThread);
  AddTest(tests, "WorkspaceShell/CompareEditablePaneTakesMultipleCarets",
          TestWorkspaceShellCompareEditablePaneTakesMultipleCarets);
  AddTest(tests, "WorkspaceShell/WorkingTreeCompareIsEditableAndSaves",
          TestWorkspaceShellWorkingTreeCompareIsEditableAndSaves);
  AddTest(tests, "WorkspaceShell/WorkingTreeCompareSharesTheEditorTabsBuffer",
          TestWorkspaceShellWorkingTreeCompareSharesTheEditorTabsBuffer);
  AddTest(tests, "WorkspaceShell/WorkingTreeCompareFollowsAnExternalReload",
          TestWorkspaceShellWorkingTreeCompareFollowsAnExternalReload);
  AddTest(tests, "WorkspaceShell/WorkingTreeCompareKeepsUtf8Bom",
          TestWorkspaceShellWorkingTreeCompareKeepsUtf8Bom);
  AddTest(tests, "WorkspaceShell/CompareWordWrapExpandsRowsAndKeepsPanesAligned",
          TestWorkspaceShellCompareWordWrapExpandsRowsAndKeepsPanesAligned);
  AddTest(tests, "WorkspaceShell/MergeWordWrapExpandsRows",
          TestWorkspaceShellMergeWordWrapExpandsRows);
  AddTest(tests, "WorkspaceShell/CompareMultiCaretIsPainted",
          TestWorkspaceShellCompareMultiCaretIsPainted);
  AddTest(tests, "WorkspaceShell/CompareEveryActionKeepsTheDiffModelInSync",
          TestWorkspaceShellCompareEveryActionKeepsTheDiffModelInSync);
  AddTest(tests, "WorkspaceShell/CompareEditActionRefreshesDiffModel",
          TestWorkspaceShellCompareEditActionRefreshesDiffModel);
  AddTest(tests, "WorkspaceShell/WorkingTreeCompareRejectsBinaryAndUnreadable",
          TestWorkspaceShellWorkingTreeCompareRejectsBinaryAndUnreadable);
  AddTest(tests, "WorkspaceShell/CompareBracketMatchIsMemoized",
          TestWorkspaceShellCompareBracketMatchIsMemoized);
  AddTest(tests, "WorkspaceShell/ComparePaintsPluginDecorations",
          TestWorkspaceShellComparePaintsPluginDecorations);
  AddTest(tests, "WorkspaceShell/CompareEditablePaneIndentsAndOutdents",
          TestWorkspaceShellCompareEditablePaneIndentsAndOutdents);
  AddTest(tests, "WorkspaceShell/CompareAltOOpensTheWorkingFile",
          TestWorkspaceShellCompareAltOOpensTheWorkingFile);
  AddTest(tests, "WorkspaceShell/CompareSelectedRangesStageAndRevertFromAWorkingTreeDiff",
          TestWorkspaceShellCompareSelectedRangesStageAndRevertFromAWorkingTreeDiff);
  AddTest(tests, "WorkspaceShell/CompareClickTogglesEditablePaneFocus",
          TestWorkspaceShellCompareClickTogglesEditablePaneFocus);
  AddTest(tests, "WorkspaceShell/CompareClickAboveFirstRowIsNotHandled",
          TestWorkspaceShellCompareClickAboveFirstRowIsNotHandled);
  AddTest(tests, "WorkspaceShell/CompareCollapsedContextButtonsExpandHiddenRows",
          TestWorkspaceShellCompareCollapsedContextButtonsExpandHiddenRows);
  AddTest(tests, "WorkspaceShell/CompareCollapsedContextButtonsHoverAsInteractive",
          TestWorkspaceShellCompareCollapsedContextButtonsHoverAsInteractive);
  AddTest(tests, "WorkspaceShell/CompareCollapsedContextExpansionPersistsAcrossChunks",
          TestWorkspaceShellCompareCollapsedContextExpansionPersistsAcrossChunks);
  AddTest(tests, "WorkspaceShell/CompareCollapsedContextExpansionSurvivesTreeRefresh",
          TestWorkspaceShellCompareCollapsedContextExpansionSurvivesTreeRefresh);
  AddTest(tests, "WorkspaceShell/ReadOnlyCompareRightPaneSupportsSelectAllAndCopy",
          TestWorkspaceShellReadOnlyCompareRightPaneSupportsSelectAllAndCopy);
  AddTest(tests, "WorkspaceShell/ReadOnlyCompareShortcutCopyUsesNavigableViewport",
          TestWorkspaceShellReadOnlyCompareShortcutCopyUsesNavigableViewport);
  AddTest(tests, "WorkspaceShell/CompareWheelScrollsRows",
          TestWorkspaceShellCompareWheelScrollsRows);
  AddTest(tests, "WorkspaceShell/CompareWheelScrollsColumns",
          TestWorkspaceShellCompareWheelScrollsColumns);
  AddTest(tests, "WorkspaceShell/CompareWheelScrollsLeftDominatedColumns",
          TestWorkspaceShellCompareWheelScrollsLeftDominatedColumns);
  AddTest(tests, "WorkspaceShell/CompareHorizontalNavigationInvalidatesEditablePane",
          TestWorkspaceShellCompareHorizontalNavigationInvalidatesEditablePane);
  AddTest(tests, "WorkspaceShell/CompareSelectionStepInvalidatesRowBand",
          TestWorkspaceShellCompareSelectionStepInvalidatesRowBand);
  AddTest(tests, "WorkspaceShell/CompareRenderPaintsDiagnosticGutterMarkers",
          TestWorkspaceShellCompareRenderPaintsDiagnosticGutterMarkers);
  AddTest(tests, "WorkspaceShell/CompareRenderReusesVisibleLayoutCache",
          TestWorkspaceShellCompareRenderReusesVisibleLayoutCache);
  AddTest(tests, "WorkspaceShell/CompareBlameLoadsForWorkingTreePane",
          TestWorkspaceShellCompareBlameLoadsForWorkingTreePane);
  AddTest(tests, "WorkspaceShell/MergeSaveRepaintsTheResultPane", TestWorkspaceShellMergeSaveRepaintsTheResultPane);
  AddTest(tests, "WorkspaceShell/MergeBlameLoadsForResultPane",
          TestWorkspaceShellMergeBlameLoadsForResultPane);
  AddTest(tests, "WorkspaceShell/PathlessComparisonTabIsNamedLikeVsCode",
          TestWorkspaceShellPathlessComparisonTabIsNamedLikeVsCode);
  AddTest(tests, "WorkspaceShell/AFileComparedWithAnUntitledBufferIsNamedByBothSides",
          TestWorkspaceShellAFileComparedWithAnUntitledBufferIsNamedByBothSides);
  AddTest(tests, "WorkspaceShell/CompareTabUsesFilenameOnlyLabelAndTooltip",
          TestWorkspaceShellCompareTabUsesFilenameOnlyLabelAndTooltip);
  AddTest(tests, "WorkspaceShell/MergeTabUsesFilenameOnlyLabelAndTooltip",
          TestWorkspaceShellMergeTabUsesFilenameOnlyLabelAndTooltip);
  AddTest(tests, "WorkspaceShell/MergeEditorRefusesBinaryInput",
          TestWorkspaceShellMergeEditorRefusesBinaryInput);
  AddTest(tests, "WorkspaceShell/MergeInANarrowPaneStaysInsideIt", TestWorkspaceShellMergeInANarrowPaneStaysInsideIt);
  AddTest(tests, "WorkspaceShell/MergeInAPaneNarrowerThanItsDividersStaysInsideIt",
          TestWorkspaceShellMergeInAPaneNarrowerThanItsDividersStaysInsideIt);
  AddTest(tests, "WorkspaceShell/CompareInANarrowPaneStaysInsideIt", TestWorkspaceShellCompareInANarrowPaneStaysInsideIt);
  AddTest(tests, "WorkspaceShell/CompareInAPaneNarrowerThanItsDividerStaysInsideIt",
          TestWorkspaceShellCompareInAPaneNarrowerThanItsDividerStaysInsideIt);
  AddTest(tests, "WorkspaceShell/CompareDividerMatchesMarkerWidth",
          TestWorkspaceShellCompareDividerMatchesMarkerWidth);
  AddTest(tests, "WorkspaceShell/CompareRenderKeepsDividerBorderOnUnchangedRows",
          TestWorkspaceShellCompareRenderKeepsDividerBorderOnUnchangedRows);
  AddTest(tests, "WorkspaceShell/ComparePaneResizeKeepsWiderPaneTextVisible",
          TestWorkspaceShellComparePaneResizeKeepsWiderPaneTextVisible);
  AddTest(tests, "WorkspaceShell/CompareAndMergeDividersResetOnDoubleClick",
          TestWorkspaceShellCompareAndMergeDividersResetOnDoubleClick);
  AddTest(tests, "WorkspaceShell/CompareAndMergePaneMinimaPreserveVisibleColumns",
          TestWorkspaceShellCompareAndMergePaneMinimaPreserveVisibleColumns);
  AddTest(tests, "WorkspaceShell/MergeToolbarLayoutClearsPaneHeaders",
          TestWorkspaceShellMergeToolbarLayoutClearsPaneHeaders);
  AddTest(tests, "WorkspaceShell/CompareRecomputeGate", TestWorkspaceShellCompareRecomputeGate);
  AddTest(tests, "WorkspaceShell/CommitPickerDismissesAfterOpeningCompare",
          TestWorkspaceShellCommitPickerDismissesAfterOpeningCompare);
  AddTest(tests, "WorkspaceShell/ComparePickerOpensAsyncAndDropsStaleResult",
          TestWorkspaceShellComparePickerOpensAsyncAndDropsStaleResult);
  AddTest(tests, "WorkspaceShell/PlainCompareBuildsStickyEditableTab",
          TestWorkspaceShellPlainCompareBuildsStickyEditableTab);
  AddTest(tests, "WorkspaceShell/CompareOpenDoesNotMaterializeEitherSide",
          TestWorkspaceShellCompareOpenDoesNotMaterializeEitherSide);
  AddTest(tests, "WorkspaceShell/PlainCompareBufferSidesAreReadOnly",
          TestWorkspaceShellPlainCompareBufferSidesAreReadOnly);
  AddTest(tests, "WorkspaceShell/PlainCompareDedupsSameFilePair",
          TestWorkspaceShellPlainCompareDedupsSameFilePair);
  AddTest(tests, "WorkspaceShell/CompareFilesCommandOpensPlainCompare",
          TestWorkspaceShellCompareFilesCommandOpensPlainCompare);
  AddTest(tests, "WorkspaceShell/SelectForCompareThenCompareWithSelected",
          TestWorkspaceShellSelectForCompareThenCompareWithSelected);
  AddTest(tests, "WorkspaceShell/CompareWithClipboardOpensPlainCompare",
          TestWorkspaceShellCompareWithClipboardOpensPlainCompare);
  AddTest(tests, "WorkspaceShell/WorkingTreeCompareOpensOnFirstChange",
          TestWorkspaceShellWorkingTreeCompareOpensOnFirstChange);
  AddTest(tests, "WorkspaceShell/CompareHunkJumpMovesTheEditableCaret",
          TestWorkspaceShellCompareHunkJumpMovesTheEditableCaret);
  AddTest(tests, "WorkspaceShell/CompareHunkJumpFromBetweenChangesGoesToTheNearestOne",
          TestWorkspaceShellCompareHunkJumpFromBetweenChangesGoesToTheNearestOne);
  AddTest(tests, "WorkspaceShell/ComparePaintsOfOneFrameAgree", TestWorkspaceShellComparePaintsOfOneFrameAgree);
  AddTest(tests, "WorkspaceShell/CompareSyntaxCatchUpSchedulesTheNextFrame",
          TestWorkspaceShellCompareSyntaxCatchUpSchedulesTheNextFrame);
  AddTest(tests, "WorkspaceShell/CompareTypingRepaintsTheHorizontalThumb",
          TestWorkspaceShellCompareTypingRepaintsTheHorizontalThumb);
  AddTest(tests, "WorkspaceShell/CompareInASplitStaysInItsPane",
          TestWorkspaceShellCompareInASplitStaysInItsPane);
  AddTest(tests, "WorkspaceShell/CompareHorizontalScrollRepaintsTheSurface",
          TestWorkspaceShellCompareHorizontalScrollRepaintsTheSurface);
  AddTest(tests, "WorkspaceShell/StagesHunksInSequence",
          TestWorkspaceShellCompareStagesHunksInSequence);
  AddTest(tests, "WorkspaceShell/UnstagesRightAfterAStage",
          TestWorkspaceShellCompareUnstagesRightAfterAStage);
}

}
