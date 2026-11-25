#include "TestSupport.h"
#include "support/ProjectSearchWait.h"
#include "TestSupportSdl.h"

#include "util/PerformanceCounters.h"
#include "util/TextFileIO.h"
#include "workspace/ProjectRootPresence.h"
#include "workspace/ListSelection.h"
#include "workspace/WorkspaceCommandParsing.h"
#include "workspace/TabReorder.h"
#include "workspace/coordinators/SelectionAutoscroll.h"
#include "workspace/registries/WorkspaceCommandRegistry.h"
#include "workspace/shell/WorkspaceShellTestAccess.h"
#include "platform/FileIndexWatcher.h"

#include <unistd.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>
#include <vector>
#include "WorkspaceShellEventHelpers.h"
#include "support/GitSidebarWait.h"
#include "support/RetainedRedraw.h"
#include "support/SoftwareCanvas.h"
#include "EditorSplitTreeInvariants.h"

#include <random>

namespace microide::tests {
namespace {

using microide::workspace::WorkspaceShell;
using WorkspaceShellTestAccess = microide::workspace::WorkspaceShell::TestAccess;

class ScopedProjectAppHomes {
 public:
  ScopedProjectAppHomes(const std::filesystem::path& state_home,
                        const std::filesystem::path& config_home)
      : xdg_state_home_("XDG_STATE_HOME", state_home.string()),
        xdg_config_home_("XDG_CONFIG_HOME", config_home.string()),
        localappdata_("LOCALAPPDATA", state_home.string()),
        appdata_("APPDATA", config_home.string()) {}

 private:
  ScopedEnvVar xdg_state_home_;
  ScopedEnvVar xdg_config_home_;
  ScopedEnvVar localappdata_;
  ScopedEnvVar appdata_;
};

bool ExecuteCommand(WorkspaceShell& shell, std::string_view command) {
  return SendKeyDown(shell, SDLK_P, SDL_KMOD_CTRL | SDL_KMOD_SHIFT) &&
         WorkspaceShellTestAccess::HandleTextInput(shell, command) &&
         SendKeyDown(shell, SDLK_RETURN, SDL_KMOD_NONE);
}

bool RunCommandLine(WorkspaceShell& shell, std::string_view command) {
  return WorkspaceShellTestAccess::ExecuteCommandLine(shell, std::string(command));
}

bool WaitForProjectReload(WorkspaceShell& shell, std::chrono::milliseconds timeout) {
  return WaitUntil(
      [&shell]() { return WorkspaceShellTestAccess::ReloadProjectIfFilesChanged(shell, false); },
      timeout, std::chrono::milliseconds(10));
}

bool WaitForForcedProjectChange(WorkspaceShell& shell, std::chrono::milliseconds timeout) {
  return WaitUntil(
      [&shell]() { return WorkspaceShellTestAccess::ReloadProjectIfFilesChanged(shell, true); },
      timeout, std::chrono::milliseconds(10));
}

bool WaitForFileIndexPath(WorkspaceShell& shell,
                          const std::filesystem::path& relative_path,
                          bool expected_present,
                          std::chrono::milliseconds timeout) {
  return WaitUntil(
      [&shell, &relative_path, expected_present]() {
        return WorkspaceShellTestAccess::FileIndexContainsPath(shell, relative_path) ==
               expected_present;
      },
      timeout, std::chrono::milliseconds(10));
}

platform::IndexUpdateBatch BuildInjectedCreateBatch(const std::filesystem::path& root,
                                                    const std::filesystem::path& relative_path) {
  const std::filesystem::path absolute_path = root / relative_path;
  std::error_code mtime_error;
  const auto mtime = std::filesystem::last_write_time(absolute_path, mtime_error);
  std::error_code size_error;
  const auto size = std::filesystem::file_size(absolute_path, size_error);
  platform::IndexUpdateBatch batch;
  batch.is_initial = false;
  batch.changes.push_back(platform::IndexUpdateBatch::Change{
      .kind = platform::IndexUpdateBatch::Kind::CreatedOrModified,
      .entry = platform::IndexFileEntry{
          .relative_path = relative_path,
          .mtime = mtime_error ? std::filesystem::file_time_type{} : mtime,
          .size = size_error ? 0 : size,
      },
  });
  return batch;
}

platform::IndexUpdateBatch BuildInjectedDeleteBatch(const std::filesystem::path& relative_path) {
  platform::IndexUpdateBatch batch;
  batch.is_initial = false;
  batch.changes.push_back(platform::IndexUpdateBatch::Change{
      .kind = platform::IndexUpdateBatch::Kind::Deleted,
      .entry = platform::IndexFileEntry{
          .relative_path = relative_path,
          .mtime = {},
          .size = 0,
      },
  });
  return batch;
}

void TestWorkspaceShellProjectOpenMenuUsesNativePickerSelection() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "picked-project";
  const std::filesystem::path readme = root / "README.md";
  WriteFile(readme, "hello\n");

  WorkspaceShell shell;
  std::filesystem::path requested_default;
  int launch_count = 0;
  WorkspaceShellTestAccess::SetProjectOpenDialogLauncher(
      shell, [&](WorkspaceShell&, const std::filesystem::path& default_location) {
        ++launch_count;
        requested_default = default_location.lexically_normal();
        return true;
      });

  Expect(WorkspaceShellTestAccess::ExecuteProjectOpenFromMenu(shell),
         "menu open-project should launch the native picker");
  Expect(launch_count == 1, "menu open-project should launch the picker exactly once");
  Expect(!requested_default.empty(),
         "menu open-project should provide a default location to the picker");
  Expect(WorkspaceShellTestAccess::ProjectOpenDialogActive(shell),
         "menu open-project should mark the picker as active while waiting");
  Expect(!WorkspaceShellTestAccess::CommandPaletteOpen(shell),
         "menu open-project should not fall back to the command palette when native launch succeeds");

  WorkspaceShellTestAccess::QueueProjectOpenDialogSelection(shell, root);
  WorkspaceShellTestAccess::ConsumePendingProjectOpenDialogResult(shell);

  Expect(!WorkspaceShellTestAccess::ProjectOpenDialogActive(shell),
         "project picker should clear its active state after selection");
  Expect(WorkspaceShellTestAccess::ProjectCount(shell) == 1,
         "selected project should open as a project tab");
  Expect(WorkspaceShellTestAccess::ProjectRoot(shell) == root.lexically_normal(),
         "selected project should become the active project");
  Expect(WorkspaceShellTestAccess::OpenTabs(shell).size() == 1,
         "selected project should open its startup file");
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).path() == readme.lexically_normal(),
         "selected project should open the README startup file");
}

void TestWorkspaceShellDirtyPromptSurvivesTabShiftWhileOpen() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path a = root / "a.txt";
  const std::filesystem::path b = root / "b.txt";
  const std::filesystem::path c = root / "c.txt";
  WriteFile(a, "a\n");
  WriteFile(b, "b\n");
  WriteFile(c, "c\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenFile(shell, a);
  WorkspaceShellTestAccess::OpenFile(shell, b);
  WorkspaceShellTestAccess::OpenFile(shell, c);
  Expect(WorkspaceShellTestAccess::FocusedGroupOpenTabCount(shell) == 3,
         "fixture should have three open tabs");

  WorkspaceShellTestAccess::FocusedGroupTabEditor(shell, 1).InsertText("!");
  Expect(WorkspaceShellTestAccess::FocusedGroupTabEditor(shell, 1).dirty(),
         "tab b should be dirty");

  WorkspaceShellTestAccess::RequestCloseTab(shell, 1);
  Expect(WorkspaceShellTestAccess::DirtyPromptVisible(shell),
         "closing a dirty tab should open the dirty prompt");

  WorkspaceShellTestAccess::RequestCloseTab(shell, 0);
  Expect(WorkspaceShellTestAccess::FocusedGroupOpenTabCount(shell) == 2,
         "closing the clean tab should leave two tabs while the prompt is up");
  Expect(WorkspaceShellTestAccess::DirtyPromptVisible(shell),
         "the dirty prompt for b should still be up after the unrelated close");

  WorkspaceShellTestAccess::ConfirmDirtyPrompt(shell, 0);

  Expect(WorkspaceShellTestAccess::FocusedGroupOpenTabCount(shell) == 1,
         "confirming should close exactly the targeted dirty tab");
  Expect(WorkspaceShellTestAccess::FocusedGroupTabPath(shell, 0) == c,
         "the surviving tab must be c -- b (the prompt target) was closed, not c");
  Expect(ReadFile(b) == "!b\n",
         "tab b's unsaved edit should have been saved to disk before closing");
}

void TestWorkspaceShellDirtyPromptClosesWithItsTab() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path a = root / "a.txt";
  const std::filesystem::path b = root / "b.txt";
  WriteFile(a, "a\n");
  WriteFile(b, "b\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenFile(shell, a);
  WorkspaceShellTestAccess::OpenFile(shell, b);
  WorkspaceShellTestAccess::FocusedGroupTabEditor(shell, 1).InsertText("!");
  WorkspaceShellTestAccess::RequestCloseTab(shell, 1);
  Expect(WorkspaceShellTestAccess::DirtyPromptVisible(shell), "closing a dirty tab opens the prompt");

  WorkspaceShellTestAccess::CloseTab(shell, 0);
  Expect(WorkspaceShellTestAccess::DirtyPromptVisible(shell),
         "the prompt stays up when a tab it does not ask about closes");

  WorkspaceShellTestAccess::CloseTab(shell, 0);
  Expect(!WorkspaceShellTestAccess::DirtyPromptVisible(shell),
         "the prompt closes with the tab it asks about");
  Expect(WorkspaceShellTestAccess::FocusedGroupOpenTabCount(shell) == 0, "both tabs are gone");
}

void TestWorkspaceShellForcedFileIndexRefreshRunsOffThreadAndPicksUpNewFiles() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "a.txt", "a\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  Expect(WorkspaceShellTestAccess::FileIndexContainsPath(shell, "a.txt"),
         "initial scan should list the existing file");
  Expect(!WorkspaceShellTestAccess::FileIndexContainsPath(shell, "b.txt"),
         "index should not list a file that does not exist yet");

  WriteFile(root / "b.txt", "b\n");
  WorkspaceShellTestAccess::ForceFileIndexRefreshAndDrain(shell);

  Expect(WorkspaceShellTestAccess::FileIndexContainsPath(shell, "b.txt"),
         "off-thread forced rescan should pick up the newly-created file");
  Expect(WorkspaceShellTestAccess::FileIndexContainsPath(shell, "a.txt"),
         "off-thread forced rescan should retain still-present files");
}

void TestWorkspaceShellProjectOpenCommandUsesNativePickerAtActiveProjectRoot() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "current-project";
  const std::filesystem::path readme = root / "README.md";
  WriteFile(readme, "root\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);

  std::filesystem::path requested_default;
  int launch_count = 0;
  WorkspaceShellTestAccess::SetProjectOpenDialogLauncher(
      shell, [&](WorkspaceShell&, const std::filesystem::path& default_location) {
        ++launch_count;
        requested_default = default_location.lexically_normal();
        return true;
      });

  Expect(WorkspaceShellTestAccess::ExecuteProjectOpenFromCommand(shell),
         "command open-project without a path should launch the native picker");
  Expect(launch_count == 1, "command open-project should launch the picker exactly once");
  Expect(requested_default == root.lexically_normal(),
         "command open-project should seed the picker from the active project root");

  WorkspaceShellTestAccess::QueueProjectOpenDialogCancel(shell);
  WorkspaceShellTestAccess::ConsumePendingProjectOpenDialogResult(shell);

  Expect(WorkspaceShellTestAccess::ProjectRoot(shell) == root.lexically_normal(),
         "cancelled project picker should leave the active project unchanged");
}

void TestWorkspaceShellProjectOpenMenuFallsBackToTypedPathWhenNativePickerFails() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectOpenDialogLauncher(
      shell, [](WorkspaceShell&, const std::filesystem::path&) { return false; });

  Expect(WorkspaceShellTestAccess::ExecuteProjectOpenFromMenu(shell),
         "menu open-project should stay handled when native launch fails");
  Expect(!WorkspaceShellTestAccess::ProjectOpenDialogActive(shell),
         "failed picker launch should not leave the picker marked active");
  Expect(WorkspaceShellTestAccess::CommandPaletteOpen(shell),
         "menu open-project should fall back to the prefilled command palette");
  Expect(WorkspaceShellTestAccess::CommandPaletteQuery(shell) == "project-open ",
         "menu fallback should prefill the typed open-project command");
}

void TestWorkspaceShellValueCommandsFromTheMenuSeedTheCommandPalette() {
  struct Case {
    WorkspaceShell::ActionId id;
    std::string_view seed;
  };
  const std::array<Case, 3> cases{{
      {WorkspaceShell::ActionId::TabSize, "tab-size "},
      {WorkspaceShell::ActionId::IndentWidth, "indent-width "},
      {WorkspaceShell::ActionId::UiScale, "ui-scale "},
  }};
  for (const Case& test_case : cases) {
    WorkspaceShell shell;
    Expect(WorkspaceShellTestAccess::ExecuteActionFromMenu(shell, test_case.id),
           "a palette row must stay handled when it carries no argument");
    Expect(WorkspaceShellTestAccess::CommandPaletteOpen(shell),
           "the row should fall back to the prefilled command palette");
    Expect(WorkspaceShellTestAccess::CommandPaletteQuery(shell) == test_case.seed,
           "the palette should be prefilled with the verb and a space");
  }
}

void TestWorkspaceShellTreeExpandRepaintsSidebarWhenCursorChanges() {
  EnsureDummySdlVideoInitialized();
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "alpha" / "one.txt", "1\n");
  WriteFile(root / "alpha" / "two.txt", "2\n");
  WriteFile(root / "readme.txt", "r\n");

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "project fixture should open");
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::ShowTreeSidebar(shell);
  WorkspaceShellTestAccess::NoteLayoutInputsChanged(shell);

  const auto& entries = WorkspaceShellTestAccess::TreeEntries(shell);
  const auto alpha = std::find_if(entries.begin(), entries.end(), [&](const project::TreeEntry& e) {
    return e.path == (root / "alpha").lexically_normal();
  });
  Expect(alpha != entries.end() && alpha->is_directory && !alpha->expanded,
         "the fixture lists a collapsed directory");
  const std::size_t alpha_index = static_cast<std::size_t>(alpha - entries.begin());
  const std::size_t rows_before = entries.size();
  const auto row = WorkspaceShellTestAccess::TreeSidebarRowRect(shell, alpha_index);
  Expect(row.has_value(), "the directory row is on screen");

  (void)WorkspaceShellTestAccess::ConsumePendingRedraw(shell);
  WorkspaceShellTestAccess::ForceCursorReassert(shell);
  const auto send = [&](Uint32 type) {
    SDL_Event event{};
    event.type = type;
    event.button.button = SDL_BUTTON_LEFT;
    event.button.x = row->x + row->w * 0.5f;
    event.button.y = row->y + row->h * 0.5f;
    event.button.clicks = 1;
    return shell.HandleEvent(event);
  };
  (void)send(SDL_EVENT_MOUSE_BUTTON_DOWN);
  Expect(WorkspaceShellTestAccess::TreeEntries(shell).size() == rows_before,
         "the press alone expands nothing: it may be the start of a drag");
  const WorkspaceShell::EventResult result = send(SDL_EVENT_MOUSE_BUTTON_UP);

  Expect(WorkspaceShellTestAccess::TreeEntries(shell).size() > rows_before,
         "the click expanded the directory");
  const SDL_FRect sidebar = WorkspaceShellTestAccess::CurrentLayout(shell).sidebar;
  Expect(result.handled && (result.redraw.full || result.redraw.frame || AnyRectCovers(result.redraw.rects, sidebar)),
         "expanding a directory repaints the sidebar, cursor change or not");
}

void TestWorkspaceShellTreeRowDragAndDrop() {
  EnsureDummySdlVideoInitialized();
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "alpha.txt", "a\n");
  WriteFile(root / "beta.txt", "b\n");
  WriteFile(root / "zdest" / "keep.txt", "k\n");

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false), "project fixture should open");
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, root / "beta.txt");
  WorkspaceShellTestAccess::ShowTreeSidebar(shell);
  WorkspaceShellTestAccess::NoteLayoutInputsChanged(shell);
  const auto row_of = [&](const std::filesystem::path& path) -> std::optional<SDL_FRect> {
    const auto& entries = WorkspaceShellTestAccess::TreeEntries(shell);
    for (std::size_t i = 0; i < entries.size(); ++i) {
      if (entries[i].path == path.lexically_normal()) {
        return WorkspaceShellTestAccess::TreeSidebarRowRect(shell, i);
      }
    }
    return std::nullopt;
  };
  const auto drag = [&](const SDL_FRect& from, float to_x, float to_y, bool ctrl = false) {
    const float x = from.x + from.w * 0.5f;
    const float y = from.y + from.h * 0.5f;
    (void)SendMouseDown(shell, x, y, SDL_BUTTON_LEFT);
    (void)SendMouseMotion(shell, x + 10.0f, y + 2.0f, SDL_BUTTON_LMASK);
    (void)SendMouseMotion(shell, to_x, to_y, SDL_BUTTON_LMASK);
    if (ctrl) {
      (void)shell.HandleEvent(KeyDownEvent(SDLK_LCTRL, SDL_KMOD_LCTRL));
    }
    (void)SendMouseUp(shell, to_x, to_y, SDL_BUTTON_LEFT);
    if (ctrl) {
      (void)shell.HandleEvent(KeyUpEvent(SDLK_LCTRL));
    }
  };
  const auto pane = [&]() {
    const auto rects = WorkspaceShellTestAccess::EditorGroupRectsFor(shell, WorkspaceShellTestAccess::CurrentLayout(shell));
    return rects.groups[0].editor_surface;
  };

  const auto alpha_row = row_of(root / "alpha.txt");
  Expect(alpha_row.has_value(), "alpha.txt is on screen");
  const float ax = alpha_row->x + alpha_row->w * 0.5f;
  const float ay = alpha_row->y + alpha_row->h * 0.5f;
  (void)SendMouseDown(shell, ax, ay, SDL_BUTTON_LEFT);
  (void)SendMouseMotion(shell, ax + 2.0f, ay + 1.0f, SDL_BUTTON_LMASK);
  (void)SendMouseUp(shell, ax + 2.0f, ay + 1.0f, SDL_BUTTON_LEFT);
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).path() == (root / "alpha.txt").lexically_normal(),
         "a click with a small wobble opens the row");

  WorkspaceShellTestAccess::ActivateTab(shell, 0);
  const SDL_FRect body = pane();
  drag(*row_of(root / "alpha.txt"), body.x + body.w - 8.0f, body.y + body.h * 0.5f);
  Expect(WorkspaceShellTestAccess::EditorGroupCount(shell) == 2, "a drop on a pane's right edge carves a new pane");
  Expect(WorkspaceShellTestAccess::GroupTabPaths(shell, 1) ==
             std::vector<std::filesystem::path>{(root / "alpha.txt").lexically_normal()},
         "the new pane, on the right, holds the dropped file");

  WorkspaceShellTestAccess::ShowTreeSidebar(shell);
  WorkspaceShellTestAccess::RefreshProjectFiles(shell);
  const auto dest_row = row_of(root / "zdest");
  Expect(dest_row.has_value(), "zdest is on screen");
  drag(*row_of(root / "beta.txt"), dest_row->x + dest_row->w * 0.5f, dest_row->y + dest_row->h * 0.5f,
       true);
  Expect(std::filesystem::exists(root / "beta.txt") && ReadFile(root / "zdest" / "beta.txt") == "b\n",
         "a Ctrl drop on a folder copies the file there");
  WorkspaceShellTestAccess::RefreshProjectFiles(shell);
  drag(*row_of(root / "alpha.txt"), dest_row->x + dest_row->w * 0.5f, dest_row->y + dest_row->h * 0.5f);
  Expect(!std::filesystem::exists(root / "alpha.txt") && ReadFile(root / "zdest" / "alpha.txt") == "a\n",
         "a drop on a folder moves the file there");
  Expect(WorkspaceShellTestAccess::GroupTabPaths(shell, 1) ==
             std::vector<std::filesystem::path>{(root / "zdest" / "alpha.txt").lexically_normal()},
         "and its open tab follows it");

  WorkspaceShellTestAccess::RefreshProjectFiles(shell);
  const auto keep_parent = row_of(root / "zdest");
  const auto beta_row = row_of(root / "beta.txt");
  (void)SendMouseDown(shell, beta_row->x + 10.0f, beta_row->y + 4.0f, SDL_BUTTON_LEFT);
  (void)SendMouseMotion(shell, keep_parent->x + 20.0f, keep_parent->y + 4.0f, SDL_BUTTON_LMASK);
  Expect(SendKeyDown(shell, SDLK_ESCAPE, SDL_KMOD_NONE), "Escape cancels the drag");
  (void)SendMouseUp(shell, keep_parent->x + 20.0f, keep_parent->y + 4.0f, SDL_BUTTON_LEFT);
  Expect(std::filesystem::exists(root / "beta.txt") && !std::filesystem::exists(root / "zdest" / "beta copy.txt"),
         "a cancelled drag moves nothing");
}

void TestWorkspaceShellTreeMoveRepaintsEveryPaneShowingTheFile() {
  EnsureDummySdlVideoInitialized();
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "a.txt", "a\n");
  WriteFile(root / "dest" / "keep.txt", "k\n");
  constexpr int kWidth = 1280;
  constexpr int kHeight = 720;
  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false), "project fixture should open");
  WorkspaceShellTestAccess::SetWindowSize(shell, kWidth, kHeight);
  Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, root / "dest" / "keep.txt"), "keep.txt opens");
  Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, root / "a.txt"), "a.txt opens");
  Expect(SendKeyDown(shell, SDLK_BACKSLASH, SDL_KMOD_CTRL), "Ctrl+\\ splits a.txt into a second pane");
  WorkspaceShellTestAccess::FocusEditorGroup(shell, 0);
  WorkspaceShellTestAccess::ActivateTab(shell, 0);
  WorkspaceShellTestAccess::ShowTreeSidebar(shell);
  WorkspaceShellTestAccess::SetFocusSidebar(shell);
  WorkspaceShellTestAccess::NoteLayoutInputsChanged(shell);
  SoftwareCanvas canvas(kWidth, kHeight);
  const auto pin = [&]() { WorkspaceShellTestAccess::SetCaretBlinkEpochMs(shell, SDL_GetTicks()); };
  pin();
  shell.Render(canvas.renderer(), kWidth, kHeight);
  (void)shell.ConsumePendingRenderInvalidation();

  const auto row_of = [&](const std::filesystem::path& path) {
    const auto& entries = WorkspaceShellTestAccess::TreeEntries(shell);
    for (std::size_t i = 0; i < entries.size(); ++i) {
      if (entries[i].path == path.lexically_normal()) {
        return WorkspaceShellTestAccess::TreeSidebarRowRect(shell, i).value_or(SDL_FRect{});
      }
    }
    return SDL_FRect{};
  };
  const SDL_FRect from = row_of(root / "a.txt");
  const SDL_FRect to = row_of(root / "dest");
  Expect(from.w > 0.0f && to.w > 0.0f, "both rows are on screen");
  (void)DispatchEvents(shell, {MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_DOWN, from.x + 20.0f, from.y + 4.0f),
                               MouseMotionEvent(from.x + 30.0f, from.y + 10.0f),
                               MouseMotionEvent(to.x + 20.0f, to.y + 4.0f)});
  shell.Render(canvas.renderer(), kWidth, kHeight);
  (void)shell.ConsumePendingRenderInvalidation();
  const microide::workspace::RenderInvalidation redraw =
      DispatchEvents(shell, {MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_UP, to.x + 20.0f, to.y + 4.0f)});
  Expect(std::filesystem::exists(root / "dest" / "a.txt"), "the drop moved the file");
  const auto diff = PaintAndDiffWithFullRepaint(shell, canvas, kWidth, kHeight, redraw, pin);
  Expect(diff.count == 0, "the move repaints every pane that shows the file: " + DescribePixelDiff(diff));
}

void TestWorkspaceShellBareArgumentCommandsOfferTheirChoices() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "left.txt", "one\ntwo\n");
  WriteFile(root / "right.txt", "one\nTWO\n");
  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::ShowOutputChannelWithLines(shell, "qa-channel", 2);
  using ActionId = WorkspaceShell::ActionId;
  using OverlayMode = WorkspaceShell::OverlayMode;

  Expect(WorkspaceShellTestAccess::ExecuteBareMenuAction(shell, ActionId::ShowOutput), "Show Output Channel… runs");
  Expect(WorkspaceShellTestAccess::ActiveOverlayMode(shell) == OverlayMode::QuickPick &&
             WorkspaceShellTestAccess::OverlayVisible(shell),
         "bare, it offers a pick of the channels, not a typed argument");
  const auto& pick = WorkspaceShellTestAccess::QuickPick(shell);
  Expect(std::any_of(pick.items.begin(), pick.items.end(),
                     [](const auto& item) { return item.value == "show-output qa-channel"; }),
         "with a row per channel that runs show-output for it");
  Expect(SendKeyDown(shell, SDLK_ESCAPE, SDL_KMOD_NONE), "Escape closes the pick");

  Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, root / "left.txt"), "left.txt opens");
  Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, root / "right.txt"), "right.txt opens");
  Expect(SendKeyDown(shell, SDLK_BACKSLASH, SDL_KMOD_CTRL), "Ctrl+\\ splits");
  WorkspaceShellTestAccess::FocusEditorGroup(shell, 0);
  Expect(WorkspaceShellTestAccess::ExecuteBareMenuAction(shell, ActionId::TabToGroup), "Move Tab to Group runs");
  const auto& groups = WorkspaceShellTestAccess::QuickPick(shell);
  Expect(WorkspaceShellTestAccess::OverlayVisible(shell) && groups.items.size() == 1 &&
             groups.items.front().value == "tab-to-group 2",
         "bare, it offers the other pane");
  Expect(SendKeyDown(shell, SDLK_RETURN, SDL_KMOD_NONE), "Enter chooses it");
  Expect(WorkspaceShellTestAccess::GroupTabPaths(shell, 1).size() == 2, "the active tab moved to group 2");

  WorkspaceShellTestAccess::FocusEditorGroup(shell, 1);
  WorkspaceShellTestAccess::OpenFile(shell, root / "left.txt");
  Expect(WorkspaceShellTestAccess::ExecuteBareMenuAction(shell, ActionId::CompareFiles), "Compare Files… runs");
  Expect(WorkspaceShellTestAccess::ActiveOverlayMode(shell) == OverlayMode::FileFinder &&
             WorkspaceShellTestAccess::FileFinderCompareTitle(shell) == "Compare left.txt With…",
         "over an open file it opens the file finder, titled for that file");
  Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "right.txt"), "typing narrows the finder");
  Expect(SendKeyDown(shell, SDLK_RETURN, SDL_KMOD_NONE), "Enter picks right.txt");
  Expect(WorkspaceShellTestAccess::ActiveTabIsCompare(shell), "and compares it with left.txt");

  WorkspaceShellTestAccess::OpenFile(shell, root / "left.txt");
  WorkspaceShellTestAccess::ActiveEditor(shell).MoveCursorTo(1, 0);
  Expect(WorkspaceShellTestAccess::ExecuteBareMenuAction(shell, ActionId::BreakpointSet), "Set Breakpoint runs");
  Expect(WorkspaceShellTestAccess::PromptSurfaceVisible(shell) &&
             WorkspaceShellTestAccess::PromptSurfaceInput(shell) ==
                 microide::workspace::QuoteCommandArg((root / "left.txt").lexically_normal().string()) + " 2 ",
         "bare, its prompt holds the active file and the caret's line: " +
             WorkspaceShellTestAccess::PromptSurfaceInput(shell));
}

void TestWorkspaceShellTreeWheelRepaintsSidebarWhenCursorChanges() {
  EnsureDummySdlVideoInitialized();
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  for (int i = 0; i < 80; ++i) {
    WriteFile(root / ("file_" + std::to_string(100 + i) + ".txt"), "x\n");
  }

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "project fixture should open");
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::ShowTreeSidebar(shell);
  WorkspaceShellTestAccess::NoteLayoutInputsChanged(shell);
  const SDL_FRect sidebar = WorkspaceShellTestAccess::CurrentLayout(shell).sidebar;
  const int scroll_before = WorkspaceShellTestAccess::SidebarScrollRow(shell);

  (void)WorkspaceShellTestAccess::ConsumePendingRedraw(shell);
  WorkspaceShellTestAccess::ForceCursorReassert(shell);
  SDL_Event event{};
  event.type = SDL_EVENT_MOUSE_WHEEL;
  event.wheel.y = -3.0f;
  event.wheel.mouse_x = sidebar.x + sidebar.w * 0.5f;
  event.wheel.mouse_y = sidebar.y + sidebar.h * 0.5f;
  event.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
  const WorkspaceShell::EventResult result = shell.HandleEvent(event);

  Expect(WorkspaceShellTestAccess::SidebarScrollRow(shell) > scroll_before,
         "the wheel scrolled the tree");
  Expect(result.handled && (result.redraw.full || result.redraw.frame || AnyRectCovers(result.redraw.rects, sidebar)),
         "scrolling the tree asks for its frame, cursor change or not");
}

void TestWorkspaceShellTreeRepaintsWhenUnrelatedDamageIsQueued() {
  EnsureDummySdlVideoInitialized();
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  for (int i = 0; i < 80; ++i) {
    WriteFile(root / ("file_" + std::to_string(100 + i) + ".txt"), "x\n");
  }

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "project fixture should open");
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::ShowTreeSidebar(shell);
  WorkspaceShellTestAccess::NoteLayoutInputsChanged(shell);
  const auto layout = WorkspaceShellTestAccess::CurrentLayout(shell);
  const SDL_FRect sidebar = layout.sidebar;
  const SDL_FRect unrelated{layout.status_bar.x, layout.status_bar.y, 4.0f, 4.0f};

  {
    const int scroll_before = WorkspaceShellTestAccess::SidebarScrollRow(shell);
    (void)WorkspaceShellTestAccess::ConsumePendingRedraw(shell);
    WorkspaceShellTestAccess::QueueUnrelatedDamage(shell, unrelated);
    SDL_Event event{};
    event.type = SDL_EVENT_MOUSE_WHEEL;
    event.wheel.y = -3.0f;
    event.wheel.mouse_x = sidebar.x + sidebar.w * 0.5f;
    event.wheel.mouse_y = sidebar.y + sidebar.h * 0.5f;
    event.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
    const WorkspaceShell::EventResult result = shell.HandleEvent(event);
    Expect(WorkspaceShellTestAccess::SidebarScrollRow(shell) > scroll_before,
           "the wheel scrolled the tree");
    Expect(result.handled && (result.redraw.full || result.redraw.frame || AnyRectCovers(result.redraw.rects, sidebar)),
           "a wheel over the tree asks for its repaint though other damage was queued");
  }
  {
    WorkspaceShellTestAccess::SetFocusSidebar(shell);
    (void)WorkspaceShellTestAccess::ConsumePendingRedraw(shell);
    WorkspaceShellTestAccess::QueueUnrelatedDamage(shell, unrelated);
    const WorkspaceShell::EventResult result = SendKeyDownResult(shell, SDLK_DOWN, SDL_KMOD_NONE);
    Expect(result.handled && (result.redraw.full || result.redraw.frame || AnyRectCovers(result.redraw.rects, sidebar)),
           "a key the tree handles asks for its repaint though other damage was queued");
  }
}

void TestWorkspaceShellProjectOpenMaterializesTreeGitBadgesAfterFirstPaint() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "src" / "main.cpp";
  WriteFile(source, "int value() {\n  return 1;\n}\n");

  InitializeGitRepo(root);
  CommitAll(root, "Add startup git sidebar fixture", "startup git sidebar fixture");
  WriteFile(source, "int value() {\n  return 2;\n}\n");

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "project fixture should open");
  Expect(WorkspaceShellTestAccess::GitSidebarEntries(shell).empty(),
         "opening a project should not eagerly collect git sidebar entries");
  const auto initial_src_entry = std::find_if(
      WorkspaceShellTestAccess::TreeEntries(shell).begin(),
      WorkspaceShellTestAccess::TreeEntries(shell).end(),
      [&](const project::TreeEntry& entry) { return entry.path == (root / "src").lexically_normal(); });
  Expect(initial_src_entry != WorkspaceShellTestAccess::TreeEntries(shell).end() &&
             initial_src_entry->git_status == project::GitFileStatus::Clean,
         "opening a project should not synchronously collect tree git badges");

  WorkspaceShellTestAccess::OnFramePresented(shell);
  Expect(WorkspaceShellTestAccess::GitSidebarRefreshing(shell),
         "first paint should dispatch async tree git badge refresh");
  (void)SettleGitSidebarRefresh(shell);
  Expect(!WorkspaceShellTestAccess::GitSidebarRefreshing(shell),
         "tree git badge refresh should complete after first paint");
  const auto refreshed_src_entry = std::find_if(
      WorkspaceShellTestAccess::TreeEntries(shell).begin(),
      WorkspaceShellTestAccess::TreeEntries(shell).end(),
      [&](const project::TreeEntry& entry) { return entry.path == (root / "src").lexically_normal(); });
  Expect(refreshed_src_entry != WorkspaceShellTestAccess::TreeEntries(shell).end() &&
             refreshed_src_entry->git_status == project::GitFileStatus::Modified,
         "first-paint tree git badge refresh should apply modified badges without opening git sidebar");

  WorkspaceShellTestAccess::ShowGitSidebar(shell);
  Expect(WorkspaceShellTestAccess::GitSidebarRefreshing(shell),
         "showing git sidebar should enter the refreshing state immediately");
  (void)SettleGitSidebarRefresh(shell);
  Expect(!WorkspaceShellTestAccess::GitSidebarRefreshing(shell),
         "git sidebar should leave refreshing state once async status arrives");
  const auto& entries = WorkspaceShellTestAccess::GitSidebarEntries(shell);
  Expect(!entries.empty(),
         "showing git sidebar should render entries on wake after refresh dispatch");
  const bool found_modified_source = std::any_of(
      entries.begin(), entries.end(), [&](const WorkspaceShell::GitSidebarEntry& entry) {
        return entry.section == WorkspaceShell::GitSidebarEntry::Section::Changed &&
               entry.path == source.lexically_normal();
      });
  Expect(found_modified_source,
         "on-demand git sidebar refresh should include the modified file");
}

void TestWorkspaceShellAutomaticGitRefreshKeepsTreeBadgesClean() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "src" / "main.cpp";
  WriteFile(source, "int value() {\n  return 1;\n}\n");

  InitializeGitRepo(root);
  CommitAll(root, "Add automatic git refresh fixture", "automatic git refresh fixture");
  WriteFile(source, "int value() {\n  return 2;\n}\n");

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "automatic git refresh fixture should open");
  WorkspaceShellTestAccess::RequestAutomaticGitSidebarRefresh(shell);
  (void)SettleGitSidebarRefresh(shell);
  Expect(!WorkspaceShellTestAccess::GitSidebarRefreshing(shell),
         "automatic git refresh should complete");
  Expect(!WorkspaceShellTestAccess::GitSidebarEntries(shell).empty(),
         "automatic git refresh should still publish working-tree entries");
  const auto src_entry = std::find_if(
      WorkspaceShellTestAccess::TreeEntries(shell).begin(),
      WorkspaceShellTestAccess::TreeEntries(shell).end(),
      [&](const project::TreeEntry& entry) { return entry.path == (root / "src").lexically_normal(); });
  Expect(src_entry != WorkspaceShellTestAccess::TreeEntries(shell).end() &&
             src_entry->git_status == project::GitFileStatus::Clean,
         "automatic git refresh should not materialize tree badges before git sidebar is shown");
}

void TestWorkspaceShellStatusBarShowsSourceControlState() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "src" / "main.cpp";
  WriteFile(source, "int value() {\n  return 1;\n}\n");

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "status bar source-control fixture should open");
  WorkspaceShellTestAccess::SetStatusBarGitSnapshot(shell, true, "main", true);
  WorkspaceShellTestAccess::RefreshStatusBar(shell);

  const std::string project_segment = WorkspaceShellTestAccess::StatusBarSegmentText(
      shell, microide::workspace::StatusBarSegmentId::Project);
  const std::string project_tooltip = WorkspaceShellTestAccess::StatusBarSegmentTooltip(
      shell, microide::workspace::StatusBarSegmentId::Project);
  const std::string branch_segment = WorkspaceShellTestAccess::StatusBarSegmentText(
      shell, microide::workspace::StatusBarSegmentId::Branch);
  const bool branch_visible = WorkspaceShellTestAccess::StatusBarSegmentVisible(
      shell, microide::workspace::StatusBarSegmentId::Branch);

  Expect(project_segment.find("main") != std::string::npos,
         "status bar source-control segment should prioritize the branch label");
  Expect(project_segment.find("[dirty]") != std::string::npos,
         "status bar project segment should expose dirty working-tree state");
  Expect(project_tooltip.find("Open Source Control") != std::string::npos,
         "status bar source-control segment should expose click destination intent");
  Expect(!branch_visible && branch_segment.empty(),
         "status bar branch segment should stay hidden when branch is shown in the primary segment");
}

void TestWorkspaceShellTerminalWakeRefreshesStatusBarAfterCommit() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "src" / "main.cpp";
  WriteFile(source, "int value() {\n  return 1;\n}\n");

  InitializeGitRepo(root);
  CommitAll(root, "Add git metadata refresh fixture", "git metadata refresh fixture");
  WriteFile(source, "int value() {\n  return 2;\n}\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::RegisterLifecycleWakeEvents(shell);
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "git metadata refresh fixture should open");
  WorkspaceShellTestAccess::RefreshGitSidebar(shell);
  (void)SettleGitSidebarRefresh(shell);
  WorkspaceShellTestAccess::RefreshStatusBar(shell);
  Expect(WorkspaceShellTestAccess::StatusBarSegmentText(
             shell, microide::workspace::StatusBarSegmentId::Project)
                 .find("[dirty]") != std::string::npos,
         "status bar should start dirty before the external commit");

  CommitAll(root, "Commit tracked change", "git metadata refresh post-open commit");
  WorkspaceShellTestAccess::ConsumeTerminalSessionUpdates(shell);

  const auto clean_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < clean_deadline) {
    WorkspaceShellTestAccess::ConsumeGitSidebarRefresh(shell);
    WorkspaceShellTestAccess::RefreshStatusBar(shell);
    if (WorkspaceShellTestAccess::StatusBarSegmentText(
            shell, microide::workspace::StatusBarSegmentId::Project)
            .find("[clean]") != std::string::npos) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  const std::string project_segment = WorkspaceShellTestAccess::StatusBarSegmentText(
      shell, microide::workspace::StatusBarSegmentId::Project);
  Expect(project_segment.find("[clean]") != std::string::npos,
         "terminal wake refresh should update the status bar after a commit");
}

void TestWorkspaceShellTerminalWakeClearsTreeGitBadgesAfterCommit() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "src" / "main.cpp";
  WriteFile(source, "int value() {\n  return 1;\n}\n");

  InitializeGitRepo(root);
  CommitAll(root, "Add terminal wake tree badge fixture", "terminal wake tree badge fixture");
  WriteFile(source, "int value() {\n  return 2;\n}\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::RegisterLifecycleWakeEvents(shell);
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "terminal wake tree badge fixture should open");

  WorkspaceShellTestAccess::OnFramePresented(shell);
  (void)SettleGitSidebarRefresh(shell);

  const auto dirty_src_entry = std::find_if(
      WorkspaceShellTestAccess::TreeEntries(shell).begin(),
      WorkspaceShellTestAccess::TreeEntries(shell).end(),
      [&](const project::TreeEntry& entry) { return entry.path == (root / "src").lexically_normal(); });
  Expect(dirty_src_entry != WorkspaceShellTestAccess::TreeEntries(shell).end() &&
             dirty_src_entry->git_status == project::GitFileStatus::Modified,
         "first tree git badge refresh should materialize dirty badges before the external commit");

  CommitAll(root, "Commit tracked change", "terminal wake tree badge post-open commit");
  WorkspaceShellTestAccess::ConsumeTerminalSessionUpdates(shell);

  const auto clean_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < clean_deadline) {
    WorkspaceShellTestAccess::ConsumeGitSidebarRefresh(shell);
    const auto src_entry = std::find_if(
        WorkspaceShellTestAccess::TreeEntries(shell).begin(),
        WorkspaceShellTestAccess::TreeEntries(shell).end(),
        [&](const project::TreeEntry& entry) { return entry.path == (root / "src").lexically_normal(); });
    if (src_entry != WorkspaceShellTestAccess::TreeEntries(shell).end() &&
        src_entry->git_status == project::GitFileStatus::Clean) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  const auto clean_src_entry = std::find_if(
      WorkspaceShellTestAccess::TreeEntries(shell).begin(),
      WorkspaceShellTestAccess::TreeEntries(shell).end(),
      [&](const project::TreeEntry& entry) { return entry.path == (root / "src").lexically_normal(); });
  Expect(clean_src_entry != WorkspaceShellTestAccess::TreeEntries(shell).end() &&
             clean_src_entry->git_status == project::GitFileStatus::Clean,
         "terminal wake refresh should clear stale tree git badges after a commit");
}

void TestWorkspaceShellProjectOpenDirectoryTreeRefreshDoesNotBlockOnGitStatuses() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  std::vector<std::filesystem::path> files;
  files.reserve(300);
  for (int i = 0; i < 300; ++i) {
    const std::filesystem::path path = root / "src" / ("file_" + std::to_string(i) + ".cpp");
    WriteFile(path, "int value() {\n  return 1;\n}\n");
    files.push_back(path);
  }

  InitializeGitRepo(root);
  CommitAll(root, "Add non-blocking directory refresh fixture", "non-blocking directory refresh");
  for (int i = 0; i < 120; ++i) {
    WriteFile(files[static_cast<std::size_t>(i)], "int value() {\n  return 2;\n}\n");
  }

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "non-blocking directory refresh fixture should open");
  WorkspaceShellTestAccess::OnFramePresented(shell);

  const auto refresh_start = std::chrono::steady_clock::now();
  const TimingWindow refresh_start_timing;
  WorkspaceShellTestAccess::RefreshProjectFiles(shell);
  const auto refresh_elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                            refresh_start)
          .count();
  Expect(refresh_start_timing.Within(std::chrono::milliseconds(refresh_elapsed), std::chrono::milliseconds(250)),
         "refreshing project files should rebuild the tree without synchronously collecting git "
         "statuses");

  WorkspaceShellTestAccess::SetFileIndexHasPendingChanges(shell, true);
  const auto reload_start = std::chrono::steady_clock::now();
  const TimingWindow reload_start_timing;
  Expect(WorkspaceShellTestAccess::ReloadProjectIfFilesChanged(shell, false),
         "file-index reload should refresh the directory tree");
  const auto reload_elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                            reload_start)
          .count();
  Expect(reload_start_timing.Within(std::chrono::milliseconds(reload_elapsed), std::chrono::milliseconds(250)),
         "file-index reload should not synchronously collect git statuses on the main thread");
}

void TestWorkspaceShellTerminalWakeDoesNotForceProjectScan() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  for (int i = 0; i < 1500; ++i) {
    WriteFile(root / "src" / ("file_" + std::to_string(i) + ".cpp"),
              "int value() {\n  return 1;\n}\n");
  }

  InitializeGitRepo(root);
  CommitAll(root, "Add terminal wake fixture", "terminal wake fixture");
  WriteFile(root / "src" / "file_0.cpp", "int value() {\n  return 2;\n}\n");

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "terminal wake non-blocking fixture should open");

  const auto start = std::chrono::steady_clock::now();
  const TimingWindow start_timing;
  WorkspaceShellTestAccess::ConsumeTerminalSessionUpdates(shell);
  const auto elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start)
          .count();
  Expect(start_timing.Within(std::chrono::milliseconds(elapsed), std::chrono::milliseconds(250)),
         "terminal wake should not synchronously arm the project watcher or scan the tree");
  Expect(WorkspaceShellTestAccess::GitSidebarRefreshing(shell),
         "terminal wake should request source-control refresh asynchronously");
}

void TestWorkspaceShellGitSidebarRefreshDispatchIsNonBlocking() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  std::vector<std::filesystem::path> files;
  files.reserve(300);
  for (int i = 0; i < 300; ++i) {
    const std::filesystem::path path = root / "src" / ("file_" + std::to_string(i) + ".cpp");
    WriteFile(path, "int value() {\n  return 1;\n}\n");
    files.push_back(path);
  }

  InitializeGitRepo(root);
  CommitAll(root, "Add async git status fixture", "async git status fixture");
  for (int i = 0; i < 120; ++i) {
    WriteFile(files[static_cast<std::size_t>(i)], "int value() {\n  return 2;\n}\n");
  }

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "non-blocking git refresh fixture should open");

  const auto start = std::chrono::steady_clock::now();
  const TimingWindow start_timing;
  WorkspaceShellTestAccess::ShowGitSidebar(shell);
  const auto elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start)
          .count();
  Expect(start_timing.Within(std::chrono::milliseconds(elapsed), std::chrono::milliseconds(250)),
         "activating the git sidebar should dispatch status refresh without blocking the main thread");
  Expect(WorkspaceShellTestAccess::GitSidebarRefreshing(shell),
         "activating git sidebar should transition immediately into refreshing state");
}

void TestWorkspaceShellProjectSwitchDiscardsStaleGitSidebarRefreshResult() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path old_root = temp_dir.path() / "old-project";
  const std::filesystem::path new_root = temp_dir.path() / "new-project";
  const std::filesystem::path old_file = old_root / "src" / "old_only.cpp";
  WriteFile(old_file, "int value() {\n  return 1;\n}\n");
  WriteFile(new_root / "src" / "new_only.cpp", "int fresh() {\n  return 3;\n}\n");

  InitializeGitRepo(old_root);
  CommitAll(old_root, "Add old project baseline", "old project baseline");
  WriteFile(old_file, "int value() {\n  return 2;\n}\n");

  InitializeGitRepo(new_root);
  CommitAll(new_root, "Add new project baseline", "new project baseline");

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, old_root, false, false),
         "old project should open for stale-refresh discard fixture");
  WorkspaceShellTestAccess::ShowGitSidebar(shell);
  Expect(WorkspaceShellTestAccess::GitSidebarRefreshing(shell),
         "old project git sidebar should enter refreshing state");

  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, new_root, false, false),
         "switching to a new project should succeed");
  WorkspaceShellTestAccess::ShowGitSidebar(shell);
  Expect(WorkspaceShellTestAccess::GitSidebarRefreshing(shell),
         "new project git sidebar should also enter refreshing state");

  (void)SettleGitSidebarRefresh(shell);
  Expect(!WorkspaceShellTestAccess::GitSidebarRefreshing(shell),
         "new project git sidebar refresh should settle after project switch");

  const auto& entries = WorkspaceShellTestAccess::GitSidebarEntries(shell);
  const bool contains_old_entry =
      std::any_of(entries.begin(), entries.end(),
                  [&](const WorkspaceShell::GitSidebarEntry& entry) {
                    return entry.path == old_file.lexically_normal();
                  });
  Expect(!contains_old_entry,
         "stale git refresh results from the previous project should be discarded after switch");
}

void TestWorkspaceShellUnknownCommandKeepsPromptOpenWithFeedback() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::ResetProjectScopedState(shell, true);

  Expect(SendKeyDown(shell, SDLK_P, SDL_KMOD_CTRL | SDL_KMOD_SHIFT),
         "Ctrl+Shift+P should open the command palette");
  Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "bogus-command"),
         "text input should populate the palette query");
  Expect(SendKeyDown(shell, SDLK_RETURN, SDL_KMOD_NONE),
         "Enter should run the unmatched query as a command line");

  Expect(WorkspaceShellTestAccess::CommandFeedbackText(shell) == "Unknown command: bogus-command",
         "unknown commands should report an explicit executor error");
}

void TestWorkspaceShellCommandReportsMissingProjectInsteadOfSilentNoOp() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::ResetProjectScopedState(shell, true);

  Expect(!RunCommandLine(shell, "search"),
         "a project-dependent command should fail without an active project");
  Expect(WorkspaceShellTestAccess::CommandFeedbackText(shell) == "No active project",
         "project-dependent command failures should report the missing project");
}

void TestWorkspaceShellOpenCommandRequiresPath() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "README.md", "hello\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);

  WorkspaceShellTestAccess::SetOpenFileDialogLauncher(
      shell, [](WorkspaceShell&, const std::filesystem::path&) {
        Expect(false, "a typed `open` must not launch the native file picker");
        return false;
      });

  Expect(!RunCommandLine(shell, "open"),
         "open without a path should fail");
  Expect(WorkspaceShellTestAccess::CommandFeedbackText(shell) == "open requires a path",
         "open without a path should report the missing path explicitly");
}

void TestWorkspaceShellNativeDialogsAreOffInTests() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "README.md", "hello\n");
  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  Expect(WorkspaceShellTestAccess::LaunchNativeFilePicker(shell) ==
             "Native dialogs are disabled in tests",
         "the test process refuses the real dialog before SDL is asked");
  Expect(!WorkspaceShellTestAccess::OpenFileDialogActive(shell), "and nothing is left open");
}

void TestWorkspaceShellProjectNextAndPrevCommandsCycleProjects() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root_a = temp_dir.path() / "alpha-project";
  const std::filesystem::path root_b = temp_dir.path() / "beta-project";
  const std::filesystem::path root_c = temp_dir.path() / "gamma-project";
  WriteFile(root_a / "README.md", "alpha\n");
  WriteFile(root_b / "README.md", "beta\n");
  WriteFile(root_c / "README.md", "gamma\n");

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root_a, false, false),
         "first project should open");
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root_b, false, false),
         "second project should open");
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root_c, false, false),
         "third project should open");

  Expect(ExecuteCommand(shell, "project-prev"),
         "the palette should run the project-prev command line");
  Expect(WorkspaceShellTestAccess::ProjectRoot(shell) == root_b.lexically_normal(),
         "project-prev should activate the previous project tab");

  Expect(ExecuteCommand(shell, "project-next"),
         "the palette should run the project-next command line");
  Expect(WorkspaceShellTestAccess::ProjectRoot(shell) == root_c.lexically_normal(),
         "project-next should activate the next project tab");
}

void TestWorkspaceShellAProjectLeftUnderAPromptGetsItsFocusBack() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root_a = temp_dir.path() / "alpha-project";
  const std::filesystem::path root_b = temp_dir.path() / "beta-project";
  WriteFile(root_a / "README.md", "alpha\n");
  WriteFile(root_b / "README.md", "beta\n");

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root_a, false, false), "first project should open");
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root_b, false, false), "second project should open");
  Expect(RunCommandLine(shell, "project-prev"), "project-prev should run");
  Expect(WorkspaceShellTestAccess::ProjectRoot(shell) == root_a.lexically_normal(), "alpha is in front");
  WorkspaceShellTestAccess::OpenFile(shell, root_a / "README.md");
  WorkspaceShellTestAccess::FocusedGroupTabEditor(shell, 0).InsertText("!");
  WorkspaceShellTestAccess::SetFocusEditor(shell);
  WorkspaceShellTestAccess::RequestCloseTab(shell, 0);
  Expect(WorkspaceShellTestAccess::DirtyPromptVisible(shell), "closing the dirty tab asks first");

  Expect(RunCommandLine(shell, "project-next"), "project-next should run under the prompt");
  if (WorkspaceShellTestAccess::DirtyPromptVisible(shell)) {
    WorkspaceShellTestAccess::ConfirmDirtyPrompt(shell, 2);
  }
  Expect(!WorkspaceShellTestAccess::DirtyPromptVisible(shell), "the prompt is gone");
  Expect(RunCommandLine(shell, "project-prev"), "project-prev should run");
  Expect(WorkspaceShellTestAccess::ProjectRoot(shell) == root_a.lexically_normal(), "alpha is back in front");
  Expect(!WorkspaceShellTestAccess::DirtyPromptVisible(shell), "no prompt is up on alpha");
  Expect(WorkspaceShellTestAccess::FocusIsEditor(shell),
         "alpha's focus must be back on the editor it was on, not on a prompt that is gone");
}

void TestWorkspaceShellProjectSwitchPreservesProjectScopedCommandState() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root_a = temp_dir.path() / "alpha-project";
  const std::filesystem::path root_b = temp_dir.path() / "beta-project";
  WriteFile(root_a / "README.md", "alpha\n");
  WriteFile(root_b / "README.md", "beta\n");

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root_a, false, false),
         "first project should open");
  Expect(ExecuteCommand(shell, "soft-tabs off"),
         "first project should accept a project-scoped command");
  Expect(!WorkspaceShellTestAccess::SoftTabsEnabled(shell),
         "first project should persist its editor preferences after the command");

  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root_b, false, false),
         "second project should open");
  Expect(WorkspaceShellTestAccess::SoftTabsEnabled(shell),
         "second project should start from default editor preferences (spaces)");
  Expect(ExecuteCommand(shell, "soft-tabs on"),
         "second project should accept its own project-scoped command");
  Expect(WorkspaceShellTestAccess::SoftTabsEnabled(shell),
         "second project should keep its own editor preferences");

  Expect(WorkspaceShellTestAccess::SwitchProject(shell, 0, false),
         "switching back to the first project should succeed");
  Expect(!WorkspaceShellTestAccess::SoftTabsEnabled(shell),
         "switching back should restore the first project's editor preferences");

  Expect(WorkspaceShellTestAccess::SwitchProject(shell, 1, false),
         "switching to the second project should succeed");
  Expect(WorkspaceShellTestAccess::SoftTabsEnabled(shell),
         "switching forward should restore the second project's editor preferences");
}

void TestWorkspaceShellProjectSwitchReloadsCleanBuffersChangedInTheBackground() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root_a = temp_dir.path() / "alpha-project";
  const std::filesystem::path root_b = temp_dir.path() / "beta-project";
  const std::filesystem::path notes = root_a / "notes.txt";
  WriteFile(notes, "alpha\n");
  WriteFile(root_b / "README.md", "beta\n");

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root_a, false, false),
         "first project should open");
  WorkspaceShellTestAccess::OpenFile(shell, notes);
  Expect(WorkspaceShellTestAccess::FocusedGroupTabEditor(shell, 0).SerializeDocumentText() ==
             "alpha\n",
         "the first project should show the file");

  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root_b, false, false),
         "second project should open");
  WriteFile(notes, "alpha, rewritten while its project was in the background\n");

  Expect(WorkspaceShellTestAccess::SwitchProject(shell, 0, false),
         "switching back to the first project should succeed");
  const editor::TextViewport& view = WorkspaceShellTestAccess::FocusedGroupTabEditor(shell, 0);
  Expect(!view.dirty(), "the buffer should still be clean");
  Expect(view.SerializeDocumentText() ==
             "alpha, rewritten while its project was in the background\n",
         "a clean buffer must show what its file holds after its project comes back to front");
}

void TestWorkspaceShellProjectSwitchPreservesSearchSidebarSurfaceState() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root_a = temp_dir.path() / "alpha-project";
  const std::filesystem::path root_b = temp_dir.path() / "beta-project";
  WriteFile(root_a / "README.md", "alpha\n");
  WriteFile(root_b / "README.md", "beta\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);

  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root_a, false, false),
         "first project should open");
  Expect(ExecuteCommand(shell, "sidebar-width 420"),
         "first project should accept a sidebar width command");
  WorkspaceShellTestAccess::ShowSearchSidebar(shell, "alpha query", false);
  Expect(WorkspaceShellTestAccess::SidebarMode(shell) == WorkspaceShell::SidebarMode::Search,
         "first project should show the search sidebar");
  Expect(WorkspaceShellTestAccess::SidebarViewId(shell) == "search",
         "first project should keep the search sidebar view id");
  Expect(std::fabs(WorkspaceShellTestAccess::SidebarWidth(shell) - 420.0f) < 0.001f,
         "first project should keep its sidebar width");
  Expect(WorkspaceShellTestAccess::ProjectSearchQuery(shell) == "alpha query",
         "first project should keep its project-search query");

  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root_b, false, false),
         "second project should open");
  Expect(ExecuteCommand(shell, "sidebar-width 320"),
         "second project should accept its own sidebar width command");
  WorkspaceShellTestAccess::ShowGitSidebar(shell);
  Expect(WorkspaceShellTestAccess::SidebarMode(shell) == WorkspaceShell::SidebarMode::Git,
         "second project should show its own sidebar mode");
  Expect(WorkspaceShellTestAccess::SidebarViewId(shell) == "git",
         "second project should keep the git sidebar view id");
  Expect(std::fabs(WorkspaceShellTestAccess::SidebarWidth(shell) - 320.0f) < 0.001f,
         "second project should keep its sidebar width");

  Expect(WorkspaceShellTestAccess::SwitchProject(shell, 0, false),
         "switching back to the first project should succeed");
  Expect(WorkspaceShellTestAccess::SidebarMode(shell) == WorkspaceShell::SidebarMode::Search,
         "switching back should restore the first project's search sidebar");
  Expect(WorkspaceShellTestAccess::SidebarViewId(shell) == "search",
         "switching back should restore the first project's sidebar view id");
  Expect(std::fabs(WorkspaceShellTestAccess::SidebarWidth(shell) - 420.0f) < 0.001f,
         "switching back should restore the first project's sidebar width");
  Expect(WorkspaceShellTestAccess::ProjectSearchQuery(shell) == "alpha query",
         "switching back should restore the first project's search query");

  Expect(WorkspaceShellTestAccess::SwitchProject(shell, 1, false),
         "switching forward to the second project should succeed");
  Expect(WorkspaceShellTestAccess::SidebarMode(shell) == WorkspaceShell::SidebarMode::Git,
         "switching forward should restore the second project's sidebar mode");
  Expect(WorkspaceShellTestAccess::SidebarViewId(shell) == "git",
         "switching forward should restore the second project's sidebar view id");
  Expect(std::fabs(WorkspaceShellTestAccess::SidebarWidth(shell) - 320.0f) < 0.001f,
         "switching forward should restore the second project's sidebar width");
}

void TestWorkspaceShellSettingsScrollbarsReleaseAlike() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "settings-scrollbar-project";
  WriteFile(root / "README.md", "hello\n");

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "the project should open");

  const std::array<std::pair<microide::workspace::DragTarget, const char*>, 3> bars = {{
      {microide::workspace::DragTarget::SettingsScrollbar, "settings row list"},
      {microide::workspace::DragTarget::SettingsCategoryScrollbar, "settings category rail"},
      {microide::workspace::DragTarget::SettingsPickerScrollbar, "settings picker dropdown"},
  }};
  for (const auto& [target, label] : bars) {
    WorkspaceShellTestAccess::SetTransientDragTarget(shell, target);
    WorkspaceShellTestAccess::SetTransientDragScrollbarOffset(shell, 17.5f);

    SDL_Event release{};
    release.type = SDL_EVENT_MOUSE_BUTTON_UP;
    release.button.button = SDL_BUTTON_LEFT;
    Expect(WorkspaceShellTestAccess::HandleMouseButtonUp(shell, release),
           std::string("releasing the ") + label + " scrollbar should be consumed");
    Expect(WorkspaceShellTestAccess::TransientDragTargetIsNone(shell),
           std::string("releasing the ") + label + " scrollbar should clear the drag target");
    Expect(WorkspaceShellTestAccess::TransientDragScrollbarOffset(shell) == 0.0f,
           std::string("releasing the ") + label + " scrollbar should drop the grab offset");
  }
}

void TestWorkspaceShellProjectSwitchClearsTransientInteractionState() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root_a = temp_dir.path() / "alpha-project";
  const std::filesystem::path root_b = temp_dir.path() / "beta-project";
  WriteFile(root_a / "README.md", "alpha\n");
  WriteFile(root_b / "README.md", "beta\n");

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root_a, false, false),
         "first project should open");
  WorkspaceShellTestAccess::SetTransientDragTargetSidebarDivider(shell);
  WorkspaceShellTestAccess::SetTransientMouseSelecting(shell, true);
  WorkspaceShellTestAccess::SetTransientTextDragging(shell);
  WorkspaceShellTestAccess::SetTransientBoxSelecting(shell);

  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root_b, false, false),
         "second project should open");
  Expect(WorkspaceShellTestAccess::TransientDragTargetIsNone(shell),
         "opening a different project should clear the transient drag target");
  Expect(!WorkspaceShellTestAccess::TransientMouseSelecting(shell),
         "opening a different project should clear transient selection tracking");
  Expect(WorkspaceShellTestAccess::TransientTextDragIsIdle(shell),
         "opening a different project should abandon a live text drag");
  Expect(!WorkspaceShellTestAccess::TransientBoxSelecting(shell),
         "opening a different project should abandon a live box selection");

  WorkspaceShellTestAccess::SetTransientDragTargetBottomPanelScrollbar(shell);
  WorkspaceShellTestAccess::SetTransientMouseSelecting(shell, true);
  WorkspaceShellTestAccess::SetTransientTextDragging(shell);
  WorkspaceShellTestAccess::SetTransientBoxSelecting(shell);

  Expect(WorkspaceShellTestAccess::SwitchProject(shell, 0, false),
         "switching back to the first project should succeed");
  Expect(WorkspaceShellTestAccess::TransientDragTargetIsNone(shell),
         "switching back should not restore stale drag state from the previous project");
  Expect(!WorkspaceShellTestAccess::TransientMouseSelecting(shell),
         "switching back should not restore stale transient selection state");
  Expect(WorkspaceShellTestAccess::TransientTextDragIsIdle(shell),
         "switching back should not leave a text drag armed against the old document");
  Expect(!WorkspaceShellTestAccess::TransientBoxSelecting(shell),
         "switching back should not leave a box selection armed against the old document");
}

void TestWorkspaceShellProjectOpenShowsDefaultTerminalPanel() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "README.md", "project\n");
  const std::filesystem::path xdg_state_home = temp_dir.path() / "xdg-state";
  const std::filesystem::path xdg_config_home = temp_dir.path() / "xdg-config";
  ScopedProjectAppHomes scoped_app_homes(xdg_state_home, xdg_config_home);

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "auto-terminal visibility fixture should open the project");
  Expect(WorkspaceShellTestAccess::PanelContent(shell) == WorkspaceShell::PanelContentKind::Terminal,
         "opening a project should surface the terminal panel");
  Expect(WorkspaceShellTestAccess::TerminalLaunchLabels(shell).size() == 1,
         "opening a project should prepare one default terminal tab");
}

void TestWorkspaceShellTermCommandRequestsBottomPanelRedraw() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "README.md", "project\n");

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "term redraw fixture should open the project");
  (void)shell.ConsumePendingRenderInvalidation();
  const std::size_t tab_count_before = WorkspaceShellTestAccess::TerminalLaunchLabels(shell).size();

  Expect(ExecuteCommand(shell, "term"),
         "term command should execute");
  (void)shell.ConsumePendingRenderInvalidation();
  Expect(WorkspaceShellTestAccess::PanelContent(shell) == WorkspaceShell::PanelContentKind::Terminal,
         "term command should keep the terminal panel visible");
  Expect(WorkspaceShellTestAccess::TerminalLaunchLabels(shell).size() == tab_count_before + 1,
         "term command should open exactly one additional terminal tab");
}

void TestWorkspaceShellProjectOpenSchedulesNoIdleWatcherTick() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "README.md", "project\n");

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "idle watcher tick fixture should open the project");
  Expect(WorkspaceShellTestAccess::SetSettingValue(shell, "editor.caret_blink.enabled", "false"),
         "idle watcher tick fixture should be able to disable the caret blink");

  for (int attempt = 0; attempt < 20; ++attempt) {
    if (!WorkspaceShellTestAccess::ReloadProjectIfFilesChanged(shell, false)) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  const auto next_delay = shell.NextAnimationDelayMs();
  Expect(!next_delay.has_value() || *next_delay > 100,
         "an idle open project should schedule no short project-watcher wake");
}

void TestWorkspaceShellSidebarWidthCommandParsesTypedRequests() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "README.md", "hello\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);

  Expect(ExecuteCommand(shell, "sidebar-width 420"),
         "sidebar-width command should execute with a numeric width");
  Expect(std::fabs(WorkspaceShellTestAccess::SidebarWidth(shell) - 420.0f) < 0.001f,
         "sidebar-width command should apply the parsed width");

  Expect(ExecuteCommand(shell, "sidebar-width wide"),
         "sidebar-width command should still route through the command line");
  Expect(WorkspaceShellTestAccess::CommandFeedbackText(shell) ==
             "sidebar-width requires a numeric width",
         "invalid sidebar-width input should report the parser failure");
}

void TestWorkspaceShellMergeCommandResolvesRelativePaths() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path base = root / "base.cpp";
  const std::filesystem::path incoming = root / "incoming.cpp";
  const std::filesystem::path current = root / "current.cpp";
  WriteFile(base, "int value() { return 0; }\n");
  WriteFile(incoming, "int value() { return 1; }\n");
  WriteFile(current, "int value() { return 2; }\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);

  Expect(ExecuteCommand(shell, "merge base.cpp incoming.cpp current.cpp"),
         "merge command should execute for project-relative paths");
  Expect(WorkspaceShellTestAccess::OpenTabs(shell).size() == 1,
         "merge command should open a merge tab");

  auto& merge = WorkspaceShellTestAccess::ActiveMerge(shell);
  Expect(merge.base_path == base.lexically_normal(),
         "merge command should resolve the base path relative to the active project");
  Expect(merge.incoming_path == incoming.lexically_normal(),
         "merge command should resolve the incoming path relative to the active project");
  Expect(merge.current_path == current.lexically_normal(),
         "merge command should resolve the current path relative to the active project");
  Expect(merge.output_path == current.lexically_normal(),
         "merge command should default the output path to the current file");
}

void TestWorkspaceShellTabMoveCommandSupportsRelativeOffsets() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path alpha = root / "alpha.cpp";
  const std::filesystem::path beta = root / "beta.cpp";
  const std::filesystem::path gamma = root / "gamma.cpp";
  WriteFile(alpha, "int alpha() { return 1; }\n");
  WriteFile(beta, "int beta() { return 2; }\n");
  WriteFile(gamma, "int gamma() { return 3; }\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, alpha), "first tab should open");
  Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, beta), "second tab should open");
  Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, gamma), "third tab should open");

  Expect(ExecuteCommand(shell, "tabmove -2"),
         "tabmove should execute with a relative offset");

  const auto& tabs = WorkspaceShellTestAccess::OpenTabs(shell);
  Expect(tabs.size() == 3, "tabmove should keep the same tab count");
  Expect(tabs[0].path == gamma.lexically_normal() && tabs[1].path == alpha.lexically_normal() &&
             tabs[2].path == beta.lexically_normal(),
         "relative tabmove should reorder the active tab into the requested slot");
  Expect(WorkspaceShellTestAccess::ActiveTabIndex(shell) == 0,
         "relative tabmove should keep the moved tab active");
}

void TestWorkspaceShellTabMoveCommandSupportsRelativeForwardOffset() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path alpha = root / "alpha.cpp";
  const std::filesystem::path beta = root / "beta.cpp";
  const std::filesystem::path gamma = root / "gamma.cpp";
  WriteFile(alpha, "int alpha() { return 1; }\n");
  WriteFile(beta, "int beta() { return 2; }\n");
  WriteFile(gamma, "int gamma() { return 3; }\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, alpha), "first tab should open");
  Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, beta), "second tab should open");
  Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, gamma), "third tab should open");
  WorkspaceShellTestAccess::ActivateTab(shell, 0);

  Expect(ExecuteCommand(shell, "tabmove +2"),
         "tabmove should accept a leading '+' relative-forward offset");

  const auto& tabs = WorkspaceShellTestAccess::OpenTabs(shell);
  Expect(tabs.size() == 3, "tabmove should keep the same tab count");
  Expect(tabs[0].path == beta.lexically_normal() && tabs[1].path == gamma.lexically_normal() &&
             tabs[2].path == alpha.lexically_normal(),
         "relative-forward tabmove should move the active tab forward two slots");
  Expect(WorkspaceShellTestAccess::ActiveTabIndex(shell) == 2,
         "relative-forward tabmove should keep the moved tab active");
}

void TestWorkspaceShellWheelScrollsPaneUnderPointer() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path alpha = root / "alpha.txt";
  const std::filesystem::path beta = root / "beta.txt";
  std::string many_lines;
  for (int i = 0; i < 400; ++i) many_lines += "content line\n";
  WriteFile(alpha, many_lines);
  WriteFile(beta, many_lines);

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, alpha), "alpha should open in group 0");
  Expect(RunCommandLine(shell, "split-right beta.txt"), "split-right beta should succeed");
  Expect(WorkspaceShellTestAccess::EditorGroupCount(shell) == 2, "a second group should exist");
  Expect(WorkspaceShellTestAccess::FocusedGroupIndex(shell) == 1, "focus should be on group 1");

  Expect(WorkspaceShellTestAccess::GroupActiveViewport(shell, 0).scroll_line() == 0 &&
             WorkspaceShellTestAccess::GroupActiveViewport(shell, 1).scroll_line() == 0,
         "both panes should start unscrolled");

  const SDL_FRect inactive = WorkspaceShellTestAccess::InactiveEditorPaneRect(shell);
  const float wheel_x = inactive.x + inactive.w * 0.5f;
  const float wheel_y = inactive.y + inactive.h * 0.5f;
  Expect(SendMouseWheel(shell, wheel_x, wheel_y, -3),
         "wheel over the inactive pane should be handled");
  Expect(WorkspaceShellTestAccess::GroupActiveViewport(shell, 0).scroll_line() > 0,
         "the pane under the pointer (inactive group 0) should have scrolled");
  Expect(WorkspaceShellTestAccess::GroupActiveViewport(shell, 1).scroll_line() == 0,
         "the active pane (group 1) must NOT have scrolled");
}

void TestWorkspaceShellSplitDoesNotClobberAnotherGroupsDirtyTab() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path alpha = root / "alpha.txt";
  const std::filesystem::path beta = root / "beta.txt";
  const std::filesystem::path gamma = root / "gamma.txt";
  WriteFile(alpha, "alpha\n");
  WriteFile(beta, "beta\n");
  WriteFile(gamma, "gamma\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, alpha), "alpha should open in group 0");

  Expect(RunCommandLine(shell, "split-right beta.txt"), "split-right beta should succeed");
  Expect(WorkspaceShellTestAccess::EditorGroupCount(shell) == 2, "a second group should exist");
  Expect(WorkspaceShellTestAccess::FocusedGroupIndex(shell) == 1, "focus should be on the new group");

  Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "EDIT "), "typing should dirty beta");
  Expect(WorkspaceShellTestAccess::GroupActiveViewport(shell, 1).dirty(), "beta should be dirty");

  Expect(WorkspaceShellTestAccess::FocusOtherEditorGroup(shell),
         "focus should wrap from the last pane back to group 0");
  Expect(WorkspaceShellTestAccess::FocusedGroupIndex(shell) == 0,
         "group 0 should be focused before the second split");
  Expect(RunCommandLine(shell, "split-right gamma.txt"), "split-right gamma should succeed");

  Expect(WorkspaceShellTestAccess::EditorGroupCount(shell) == 3,
         "splitting again should add a third pane rather than reuse an existing one");
  Expect(WorkspaceShellTestAccess::GroupTabPaths(shell,
                                                 WorkspaceShellTestAccess::FocusedGroupIndex(shell))
                 .front() == gamma.lexically_normal(),
         "gamma should be showing in the pane the split created");
  bool beta_survives = false;
  for (std::size_t gi = 0; gi < WorkspaceShellTestAccess::EditorGroupCount(shell); ++gi) {
    const auto paths = WorkspaceShellTestAccess::GroupTabPaths(shell, gi);
    if (std::find(paths.begin(), paths.end(), beta.lexically_normal()) != paths.end()) {
      beta_survives = paths.size() == 1 && WorkspaceShellTestAccess::GroupActiveViewport(shell, gi).dirty();
    }
  }
  Expect(beta_survives, "beta's dirty tab must survive a split elsewhere, untouched");
}

void TestWorkspaceShellSecondViewOfAnOpenFileSharesItsBuffer() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path alpha = root / "alpha.txt";
  const std::filesystem::path beta = root / "beta.txt";
  WriteFile(alpha, "alpha\n");
  WriteFile(beta, "beta\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, alpha), "alpha should open in group 0");
  Expect(RunCommandLine(shell, "split-right beta.txt"), "split-right beta should succeed");
  Expect(WorkspaceShellTestAccess::FocusedGroupIndex(shell) == 1, "focus should be on the new group");

  Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, alpha), "alpha should open in group 1");
  Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "X"), "typing into group 1's alpha");
  Expect(WorkspaceShellTestAccess::GroupActiveViewport(shell, 1).lines().LineView(0) == "Xalpha",
         "group 1's view should carry the edit");
  Expect(WorkspaceShellTestAccess::GroupActiveViewport(shell, 0).lines().LineView(0) == "Xalpha",
         "group 0's view of the same file must show the edit");
  Expect(WorkspaceShellTestAccess::GroupActiveViewport(shell, 0).dirty(),
         "both views are dirty together");

  Expect(WorkspaceShellTestAccess::FocusOtherEditorGroup(shell), "focus should wrap to group 0");
  Expect(WorkspaceShellTestAccess::FocusedGroupIndex(shell) == 0, "group 0 should be focused");
  Expect(RunCommandLine(shell, "save"), "save should succeed");
  Expect(ReadFile(alpha) == "Xalpha\n", "the save should write the shared buffer");
  Expect(!WorkspaceShellTestAccess::GroupActiveViewport(shell, 1).dirty(),
         "group 1's view is clean once the buffer is saved");

  Expect(RunCommandLine(shell, "split-right alpha.txt"), "split-right alpha should succeed");
  Expect(WorkspaceShellTestAccess::EditorGroupCount(shell) == 3, "a third pane should exist");
  const std::size_t split_group = WorkspaceShellTestAccess::FocusedGroupIndex(shell);
  Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "Y"), "typing into the split's alpha");
  const std::string_view split_line =
      WorkspaceShellTestAccess::GroupActiveViewport(shell, split_group).lines().LineView(0);
  Expect(split_line.find('Y') != std::string_view::npos, "the split's view should carry the edit");
  for (std::size_t gi = 0; gi < WorkspaceShellTestAccess::EditorGroupCount(shell); ++gi) {
    const auto paths = WorkspaceShellTestAccess::GroupTabPaths(shell, gi);
    if (std::find(paths.begin(), paths.end(), alpha.lexically_normal()) == paths.end()) {
      continue;
    }
    Expect(WorkspaceShellTestAccess::GroupActiveViewport(shell, gi).lines().LineView(0) ==
               split_line,
           "every pane showing alpha must show the same buffer");
  }
}

void TestWorkspaceShellRandomTabAndGroupOperationsKeepInvariants() {
  using microide::workspace::EditorGroupDirection;
  using microide::workspace::EditorSplitOrientation;
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::vector<std::string> names = {"alpha.txt", "beta.txt", "gamma.txt", "delta.txt"};
  for (const std::string& name : names) {
    WriteFile(root / name, name + "\n");
  }
  WriteFile(root / "base.txt", "a\nb\nc\n");
  WriteFile(root / "incoming.txt", "a\nB\nc\n");
  WriteFile(root / "current.txt", "a\nb\nC\n");
  const std::filesystem::path other_root = temp_dir.path() / "other";
  WriteFile(other_root / "omega.txt", "omega\n");

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "the first project should open as a project tab");
  WorkspaceShellTestAccess::SetClipboardTextReader(shell, []() { return std::string("clip"); });
  WorkspaceShellTestAccess::SetClipboardTextWriter(shell, [](std::string_view) { return true; });

  for (std::uint32_t seed = 20260906; seed < 20260906 + 5; ++seed) {
  std::mt19937 rng(seed);
  const auto pick = [&](std::size_t n) { return n == 0 ? 0 : static_cast<std::size_t>(rng() % n); };
  std::size_t max_groups_seen = 0;
  std::size_t max_projects_seen = 0;
  std::size_t shared_view_steps = 0;
  std::size_t prompts_answered = 0;
  int last_prompt_action = -1;
  std::size_t saves_refused = 0;
  std::string last_prompt_message;

  const auto active_root = [&]() -> std::filesystem::path {
    const auto roots = WorkspaceShellTestAccess::ProjectRoots(shell);
    const std::size_t active = WorkspaceShellTestAccess::ActiveProjectIndex(shell);
    return active < roots.size() ? roots[active] : root;
  };
  const auto project_files = [&]() {
    std::vector<std::string> files;
    for (const auto& entry : std::filesystem::directory_iterator(active_root())) {
      const std::string name = entry.path().filename().string();
      if (entry.is_regular_file() && name.ends_with(".txt") && name != "base.txt" &&
          name != "incoming.txt" && name != "current.txt" && name != "merged.txt") {
        files.push_back(name);
      }
    }
    std::sort(files.begin(), files.end());
    return files;
  };
  std::size_t fresh_counter = 0;
  const auto check = [&](const std::string& context) {
    const std::size_t groups = WorkspaceShellTestAccess::EditorGroupCount(shell);
    max_groups_seen = std::max(max_groups_seen, groups);
    max_projects_seen = std::max(max_projects_seen, WorkspaceShellTestAccess::ProjectCount(shell));
    Expect(!WorkspaceShellTestAccess::PromptSurfaceVisible(shell),
           "no prompt surface survives a step: " + context);
    const auto& tree = WorkspaceShellTestAccess::EditorSplit(shell);
    ExpectSplitTreeWellFormed(tree, context);
    Expect(tree.leaf_count() == groups, "one leaf per group: " + context);
    Expect(WorkspaceShellTestAccess::FocusedGroupIndex(shell) < groups, "focus is a group: " + context);
    Expect(!WorkspaceShellTestAccess::DirtyPromptVisible(shell),
           "no prompt survives a step: " + context + " (answered " +
               std::to_string(last_prompt_action) + " to '" + last_prompt_message +
               "', now '" + WorkspaceShellTestAccess::DirtyPromptMessage(shell) + "')");
    struct View {
      std::filesystem::path path;
      const editor::TextViewport* viewport;
    };
    std::vector<View> views;
    for (std::size_t gi = 0; gi < groups; ++gi) {
      const std::size_t tabs = WorkspaceShellTestAccess::GroupTabCount(shell, gi);
      Expect(groups == 1 || tabs > 0, "no empty pane beside another: " + context);
      Expect(tabs == 0 || WorkspaceShellTestAccess::GroupActiveTabIndex(shell, gi) < tabs,
             "the active tab index is a tab: " + context);
      const auto paths = WorkspaceShellTestAccess::GroupTabPaths(shell, gi);
      for (std::size_t ti = 0; ti < tabs; ++ti) {
        const editor::TextViewport* viewport = WorkspaceShellTestAccess::GroupTabViewport(shell, gi, ti);
        if (viewport == nullptr) {
          continue;
        }
        Expect(viewport->path().lexically_normal() == paths[ti].lexically_normal(),
               "a tab's path is its viewport's: " + context);
        views.push_back(View{paths[ti].lexically_normal(), viewport});
      }
    }
    for (std::size_t a = 0; a < views.size(); ++a) {
      for (std::size_t b = a + 1; b < views.size(); ++b) {
        const bool same_path = views[a].path == views[b].path;
        Expect(views[a].viewport->SharesDocumentWith(*views[b].viewport) == same_path,
               std::string(same_path ? "two tabs on one file share its document: "
                                     : "tabs on different files do not share a document: ") +
                   context);
        if (same_path) {
          ++shared_view_steps;
          Expect(views[a].viewport->dirty() == views[b].viewport->dirty() &&
                     views[a].viewport->line_count() == views[b].viewport->line_count(),
                 "views of one document agree on its state: " + context);
        }
      }
    }
  };
  const auto answer_prompt = [&]() {
    if (WorkspaceShellTestAccess::DirtyPromptVisible(shell)) {
      ++prompts_answered;
      last_prompt_action = static_cast<int>(pick(3));
      last_prompt_message = WorkspaceShellTestAccess::DirtyPromptMessage(shell);
      WorkspaceShellTestAccess::ConfirmDirtyPrompt(shell, last_prompt_action);
      if (last_prompt_action == 0 && WorkspaceShellTestAccess::DirtyPromptVisible(shell)) {
        ++saves_refused;
        last_prompt_action = 1 + static_cast<int>(pick(2));
        WorkspaceShellTestAccess::ConfirmDirtyPrompt(shell, last_prompt_action);
      }
    }
  };

  check("fresh");
  for (int step = 0; step < 600; ++step) {
    const std::size_t groups = WorkspaceShellTestAccess::EditorGroupCount(shell);
    const std::size_t focused = WorkspaceShellTestAccess::FocusedGroupIndex(shell);
    const std::size_t focused_tabs = WorkspaceShellTestAccess::GroupTabCount(shell, focused);
    const std::vector<std::string> files = project_files();
    const std::string file = files.empty() ? names[0] : files[pick(files.size())];
    const std::size_t op = pick(25);
    std::string context = "seed " + std::to_string(seed) + " step " + std::to_string(step) +
                          " op " + std::to_string(op);
    switch (op) {
      case 0:
      case 1:
        WorkspaceShellTestAccess::OpenFileInNewTab(shell, active_root() / file);
        context += " open " + file;
        break;
      case 2:
        RunCommandLine(shell, (pick(2) == 0 ? "split-right " : "split-down ") + file);
        context += " split " + file;
        break;
      case 3:
        WorkspaceShellTestAccess::SplitEditorGroup(
            shell, pick(2) == 0 ? EditorSplitOrientation::Vertical : EditorSplitOrientation::Horizontal);
        context += " split clone";
        break;
      case 4:
        WorkspaceShellTestAccess::FocusOtherEditorGroup(shell);
        context += " focus other";
        break;
      case 5:
        WorkspaceShellTestAccess::HandleTextInput(shell, "x");
        context += " type";
        break;
      case 6:
        RunCommandLine(shell, pick(2) == 0 ? "undo" : "redo");
        context += " undo/redo";
        break;
      case 7:
        RunCommandLine(shell, "save");
        context += " save";
        break;
      case 8:
        if (focused_tabs > 0) {
          WorkspaceShellTestAccess::RequestCloseTab(shell, pick(focused_tabs));
          answer_prompt();
        }
        context += " close tab";
        break;
      case 9:
        RunCommandLine(shell, "close-group");
        answer_prompt();
        context += " close-group";
        break;
      case 10: {
        const EditorGroupDirection dirs[] = {EditorGroupDirection::Left, EditorGroupDirection::Right,
                                             EditorGroupDirection::Up, EditorGroupDirection::Down};
        WorkspaceShellTestAccess::MoveEditorGroupInDirection(shell, dirs[pick(4)]);
        context += " move group";
        break;
      }
      case 11: {
        const std::size_t from = pick(groups);
        const std::size_t from_tabs = WorkspaceShellTestAccess::GroupTabCount(shell, from);
        const std::size_t to = pick(groups);
        if (from_tabs > 0) {
          WorkspaceShellTestAccess::MoveTabToGroup(
              shell, from, pick(from_tabs), to,
              pick(WorkspaceShellTestAccess::GroupTabCount(shell, to) + 1));
        }
        context += " move tab";
        break;
      }
      case 12: {
        const std::size_t from = pick(groups);
        const std::size_t from_tabs = WorkspaceShellTestAccess::GroupTabCount(shell, from);
        if (from_tabs > 0) {
          WorkspaceShellTestAccess::MoveTabToNewGroup(
              shell, from, pick(from_tabs), pick(groups),
              pick(2) == 0 ? EditorSplitOrientation::Vertical : EditorSplitOrientation::Horizontal,
              pick(2) == 0);
        }
        context += " move tab to new group";
        break;
      }
      case 13:
        RunCommandLine(shell, pick(2) == 0 ? "tabswitch +1" : "tabswitch -1");
        context += " tabswitch";
        break;
      case 14:
        RunCommandLine(shell, "tabmove " + std::to_string(pick(focused_tabs + 1)));
        context += " tabmove";
        break;
      case 15:
        RunCommandLine(shell, "compare-files " + file + " " +
                                  (files.empty() ? names[0] : files[pick(files.size())]));
        context += " compare-files";
        break;
      case 17: {
        WriteFile(active_root() / file, "external " + std::to_string(step) + "\n" + file + "\n");
        WorkspaceShellTestAccess::ReloadProjectIfFilesChanged(shell, true);
        answer_prompt();
        context += " external change " + file;
        break;
      }
      case 18: {
        const std::string target = "renamed_" + std::to_string(step) + ".txt";
        WorkspaceShellTestAccess::PrepareRenamePrompt(shell, active_root() / file, target);
        WorkspaceShellTestAccess::ConfirmPromptSurface(shell);
        answer_prompt();
        if (WorkspaceShellTestAccess::PromptSurfaceVisible(shell)) {
          WorkspaceShellTestAccess::ConfirmPromptSurface(shell, 1);
        }
        context += " rename " + file + " -> " + target;
        break;
      }
      case 19:
        if (files.size() > 2) {
          WorkspaceShellTestAccess::PrepareDeletePrompt(shell, active_root() / file);
          WorkspaceShellTestAccess::ConfirmPromptSurface(shell);
          answer_prompt();
          if (WorkspaceShellTestAccess::PromptSurfaceVisible(shell)) {
            WorkspaceShellTestAccess::ConfirmPromptSurface(shell, 1);
          }
        }
        context += " delete " + file;
        break;
      case 20:
        RunCommandLine(shell, "reopen");
        context += " reopen";
        break;
      case 21:
        RunCommandLine(shell, "tab fresh_" + std::to_string(fresh_counter++) + ".txt");
        context += " new buffer";
        break;
      case 22:
        if (WorkspaceShellTestAccess::ProjectCount(shell) < 2) {
          const auto roots = WorkspaceShellTestAccess::ProjectRoots(shell);
          for (const std::filesystem::path& candidate : {root, other_root}) {
            if (std::find(roots.begin(), roots.end(), candidate.lexically_normal()) == roots.end()) {
              WorkspaceShellTestAccess::OpenProjectTab(shell, candidate, false, false);
              break;
            }
          }
        }
        context += " open second project";
        break;
      case 23:
        RunCommandLine(shell, pick(2) == 0 ? "project-next" : "project-prev");
        context += " project switch";
        break;
      case 24:
        if (WorkspaceShellTestAccess::ProjectCount(shell) > 1) {
          WorkspaceShellTestAccess::RequestCloseProject(
              shell, WorkspaceShellTestAccess::ActiveProjectIndex(shell));
          answer_prompt();
        }
        context += " close project";
        break;
      case 16:
        RunCommandLine(shell, "merge base.txt incoming.txt current.txt merged.txt");
        context += " merge";
        break;
    }
    check(context);
  }
  Expect(max_groups_seen >= 3, "the sequence reached a three-pane layout");
  Expect(shared_view_steps > 0, "the sequence held two views of one file");
  Expect(prompts_answered > 0, "the sequence answered a dirty prompt");
  Expect(max_projects_seen >= 2, "the sequence opened a second project");
  (void)saves_refused;
  while (WorkspaceShellTestAccess::ProjectCount(shell) > 1) {
    WorkspaceShellTestAccess::CloseProject(shell, WorkspaceShellTestAccess::ProjectCount(shell) - 1);
  }
  WorkspaceShellTestAccess::SwitchProject(shell, 0);
  while (WorkspaceShellTestAccess::EditorGroupCount(shell) > 1) {
    RunCommandLine(shell, "close-group");
    answer_prompt();
    if (WorkspaceShellTestAccess::DirtyPromptVisible(shell)) {
      WorkspaceShellTestAccess::ConfirmDirtyPrompt(shell, 1);
    }
  }
  while (WorkspaceShellTestAccess::GroupTabCount(shell, 0) > 0) {
    WorkspaceShellTestAccess::CloseTab(shell, 0);
  }
  }
}

void TestWorkspaceShellBreadcrumbIsPerPane() {
  using microide::workspace::EditorSplitOrientation;
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path alpha = root / "alpha.txt";
  const std::filesystem::path beta = root / "beta.txt";
  WriteFile(alpha, "a\n");
  WriteFile(beta, "b\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1600, 900);
  WorkspaceShellTestAccess::OpenFile(shell, alpha);
  Expect(WorkspaceShellTestAccess::SplitEditorGroup(shell, EditorSplitOrientation::Vertical),
         "split right");
  WorkspaceShellTestAccess::OpenFile(shell, beta);
  Expect(WorkspaceShellTestAccess::FocusedGroupIndex(shell) == 1, "the split pane is focused");

  const std::string left = WorkspaceShellTestAccess::BreadcrumbLabelForGroup(shell, 0);
  const std::string right = WorkspaceShellTestAccess::BreadcrumbLabelForGroup(shell, 1);
  Expect(left.find("alpha.txt") != std::string::npos,
         "the left pane's breadcrumb names the file that pane is showing");
  Expect(right.find("beta.txt") != std::string::npos,
         "the right pane's breadcrumb names its own file, not the other pane's");
  Expect(WorkspaceShellTestAccess::BreadcrumbLabel(shell) == right,
         "the unqualified breadcrumb still follows the focused pane");
  Expect(WorkspaceShellTestAccess::BreadcrumbLabelForGroup(shell, 99) == right,
         "an out-of-range pane index resolves to the focused pane");
}

void TestWorkspaceShellFileFinderOpensIntoTheFocusedPane() {
  using microide::workspace::EditorSplitOrientation;
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "alpha.txt", "a\n");
  WriteFile(root / "beta.txt", "b\n");
  WriteFile(root / "gamma.txt", "g\n");

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "file finder split fixture should open the project");
  Expect(WaitForFileIndexPath(shell, std::filesystem::path("gamma.txt"), true,
                              std::chrono::milliseconds(5000)),
         "the file index must contain gamma.txt before the finder is asked to match it");
  WorkspaceShellTestAccess::SetWindowSize(shell, 1600, 900);
  WorkspaceShellTestAccess::OpenFile(shell, root / "alpha.txt");
  Expect(WorkspaceShellTestAccess::SplitEditorGroup(shell, EditorSplitOrientation::Vertical),
         "split right");
  WorkspaceShellTestAccess::OpenFile(shell, root / "beta.txt");
  Expect(WorkspaceShellTestAccess::FocusedGroupIndex(shell) == 1, "the split pane is focused");

  Expect(WorkspaceShellTestAccess::ExecuteFilesFromShortcut(shell), "Ctrl+P opens the file finder");
  WorkspaceShellTestAccess::SetFileFinderQuery(shell, "gamma");
  Expect(WorkspaceShellTestAccess::FileFinderResultCount(shell) >= 1, "gamma.txt should match");
  Expect(SendKeyDown(shell, SDLK_RETURN, SDL_KMOD_NONE), "Enter opens the selected file");

  const auto right = WorkspaceShellTestAccess::GroupTabPaths(shell, 1);
  const auto left = WorkspaceShellTestAccess::GroupTabPaths(shell, 0);
  const std::filesystem::path gamma = (root / "gamma.txt").lexically_normal();
  Expect(std::find(right.begin(), right.end(), gamma) != right.end(),
         "the file finder must open into the focused pane");
  Expect(std::find(left.begin(), left.end(), gamma) == left.end(),
         "the unfocused pane must not receive the opened file");
}

void TestWorkspaceShellSplitPaneRevealsItsNewTab() {
  using microide::workspace::EditorSplitOrientation;
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  std::vector<std::filesystem::path> files;
  for (int i = 0; i < 8; ++i) {
    files.push_back(root / ("a-source-file-" + std::to_string(i) + ".txt"));
    WriteFile(files.back(), "x\n");
  }

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1600, 900);
  WorkspaceShellTestAccess::OpenFile(shell, files[0]);
  Expect(WorkspaceShellTestAccess::SplitEditorGroup(shell, EditorSplitOrientation::Vertical),
         "split right");
  const std::size_t focused = WorkspaceShellTestAccess::FocusedGroupIndex(shell);

  for (std::size_t i = 1; i < files.size(); ++i) {
    WorkspaceShellTestAccess::OpenFile(shell, files[i]);
    const std::size_t active = WorkspaceShellTestAccess::GroupActiveTabIndex(shell, focused);
    const SDL_FRect rect = WorkspaceShellTestAccess::GroupEditorTabRect(shell, focused, active);
    Expect(rect.w > 0.0f,
           "the tab just opened into the split pane must be visible in that pane's strip");
  }
}

void TestWorkspaceShellSplitStopsAtTheGroupCap() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path alpha = root / "alpha.txt";
  WriteFile(alpha, "alpha\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, alpha), "alpha should open");

  while (WorkspaceShellTestAccess::EditorGroupCount(shell) < microide::workspace::kMaxEditorGroups) {
    Expect(WorkspaceShellTestAccess::SplitEditorGroup(
               shell, microide::workspace::EditorSplitOrientation::Vertical),
           "splitting below the cap should succeed");
  }
  Expect(!WorkspaceShellTestAccess::SplitEditorGroup(
             shell, microide::workspace::EditorSplitOrientation::Vertical),
         "splitting at the cap should fail");
  Expect(WorkspaceShellTestAccess::EditorGroupCount(shell) == microide::workspace::kMaxEditorGroups,
         "the group count should stay at the cap");
  Expect(WorkspaceShellTestAccess::EditorSplit(shell).leaf_count() ==
             WorkspaceShellTestAccess::EditorGroupCount(shell),
         "the split tree's leaves should stay in step with the editor groups");

  WorkspaceShellTestAccess::SetWindowSize(shell, 1600, 900);
  using microide::workspace::ActionId;
  for (const ActionId action : {ActionId::Search, ActionId::ReplaceInBuffer, ActionId::Jump, ActionId::Reopen}) {
    Expect(WorkspaceShellTestAccess::IsActionEnabled(shell, action),
           "an action on the active editor stays available at the group cap (action " +
               std::to_string(static_cast<int>(action)) + ")");
  }
  for (const ActionId action : {ActionId::SplitEditorRight, ActionId::SplitEditorDown}) {
    Expect(!WorkspaceShellTestAccess::IsActionEnabled(shell, action), "the splits are what the cap disables");
  }
  Expect(SendKeyDown(shell, SDLK_F, SDL_KMOD_CTRL), "Ctrl+F is handled with the editor area full");
  Expect(WorkspaceShellTestAccess::OverlayVisible(shell) &&
             WorkspaceShellTestAccess::ActiveOverlayMode(shell) == WorkspaceShell::OverlayMode::BufferSearch,
         "and opens the find widget");
}

void TestWorkspaceShellGotoAndJumpCommandsUseTypedNavigationRequests() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "main.cpp";
  WriteFile(source, "line1\nline2\nline3\nline4\n\xc3\xa9=1\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenFile(shell, source);

  Expect(ExecuteCommand(shell, "goto 3:2"), "goto should execute with an explicit line and column");
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).cursor_line() == 2 &&
             WorkspaceShellTestAccess::ActiveEditor(shell).cursor_column() == 1,
         "goto should move the cursor to the parsed absolute location");

  Expect(ExecuteCommand(shell, "jump -1:1"),
         "jump should execute with a relative line delta and column");
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).cursor_line() == 1 &&
             WorkspaceShellTestAccess::ActiveEditor(shell).cursor_column() == 0,
         "jump should move the cursor relative to the current line");
  Expect(ExecuteCommand(shell, "goto 5:3"), "goto with a column on a multibyte line executes");
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).cursor_line() == 4 &&
             WorkspaceShellTestAccess::ActiveEditor(shell).cursor_column() == 3,
         "the column counts characters, so a two-byte letter before it is one column");
}

void TestWorkspaceShellGotoRejectsNonPositiveLine() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "main.cpp";
  WriteFile(source, "line1\nline2\nline3\nline4\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenFile(shell, source);

  Expect(ExecuteCommand(shell, "goto 2"), "goto 2 should move to line index 1");
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).cursor_line() == 1,
         "caret is at line index 1 after goto 2");

  ExecuteCommand(shell, "goto -1");
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).cursor_line() == 1,
         "goto -1 must be rejected and leave the caret where it was, not jump to EOF");

  ExecuteCommand(shell, "goto 0");
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).cursor_line() == 1,
         "goto 0 must be rejected and leave the caret unchanged");

  Expect(ExecuteCommand(shell, "jump -1"), "jump -1 relative should still execute");
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).cursor_line() == 0,
         "jump -1 moves the caret one line up (relative), unlike absolute goto");
}

void TestWorkspaceShellGlobalCommandsApplyTypedRequests() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::EnsureTerminalTab(shell);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  (void)shell.ConsumePendingRenderInvalidation();

  Expect(SendKeyDown(shell, SDLK_P, SDL_KMOD_CTRL | SDL_KMOD_SHIFT),
         "ui-scale should open the command palette");
  Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "ui-scale 125%"),
         "ui-scale should populate the palette query");
  const auto ui_scale_submit =
      SendKeyDownResult(shell, SDLK_RETURN, SDL_KMOD_NONE);
  Expect(ui_scale_submit.handled,
         "ui-scale should execute with a parsed numeric scale");
  Expect(std::fabs(shell.UiScale() - 1.25f) < 0.001f,
         "ui-scale should apply the parsed scale");
  Expect(ui_scale_submit.redraw.full || ui_scale_submit.redraw.frame || !ui_scale_submit.redraw.rects.empty(),
         "ui-scale command should request an immediate redraw");

  Expect(ExecuteCommand(shell, "soft-tabs on"),
         "soft-tabs should execute with a typed boolean request");
  Expect(WorkspaceShellTestAccess::SoftTabsEnabled(shell),
         "soft-tabs should enable soft tabs for editor preferences");

  Expect(ExecuteCommand(shell, "wrap on"),
         "wrap should execute with a typed boolean request");
  Expect(WorkspaceShellTestAccess::SoftWrapEnabled(shell),
         "wrap should enable soft wrap for editor preferences");

  Expect(ExecuteCommand(shell, "wrap"),
         "wrap should toggle when invoked without explicit args");
  Expect(!WorkspaceShellTestAccess::SoftWrapEnabled(shell),
         "wrap without args should invert the current soft-wrap setting");

  Expect(ExecuteCommand(shell, "soft-tabs"),
         "soft-tabs should toggle when invoked without explicit args");
  Expect(!WorkspaceShellTestAccess::SoftTabsEnabled(shell),
         "soft-tabs without args should invert the current soft-tabs setting");
  Expect(ExecuteCommand(shell, "soft-tabs"), "a second bare soft-tabs toggles back");
  Expect(WorkspaceShellTestAccess::SoftTabsEnabled(shell),
         "soft-tabs without args should invert it again");

  Expect(ExecuteCommand(shell, "focus panel"),
         "focus should execute with a typed focus target");
  Expect(WorkspaceShellTestAccess::FocusIsPanel(shell),
         "focus panel should move focus to the bottom panel when available");
}

void TestWorkspaceShellCommandPaletteTabCompletion() {
  WorkspaceShell shell;

  Expect(SendKeyDown(shell, SDLK_P, SDL_KMOD_CTRL | SDL_KMOD_SHIFT),
         "Ctrl+Shift+P should open the command palette before completion");
  Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "soft"),
         "text input should populate the palette query before completion");
  Expect(SendKeyDown(shell, SDLK_TAB, SDL_KMOD_NONE),
         "tab should trigger command completion");
  Expect(WorkspaceShellTestAccess::CommandPaletteQuery(shell) == "soft-tabs ",
         "tab completion should expand the unique built-in command name");
  Expect(WorkspaceShellTestAccess::CommandFeedbackText(shell) == "Completed soft-tabs",
         "tab completion should report the completed command name");

  Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "on"),
         "completion fixture should allow finishing the completed command");
  Expect(SendKeyDown(shell, SDLK_RETURN, SDL_KMOD_NONE),
         "enter should execute the completed command line");
  Expect(!WorkspaceShellTestAccess::CommandPaletteOpen(shell),
         "successful command execution should close the palette");
  Expect(WorkspaceShellTestAccess::SoftTabsEnabled(shell),
         "the completed 'soft-tabs on' command line should apply");

  Expect(SendKeyDown(shell, SDLK_P, SDL_KMOD_CTRL | SDL_KMOD_SHIFT),
         "Ctrl+Shift+P should reopen the command palette for a second completion");
  Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "wr"),
         "completion fixture should allow typing a second command prefix");
  Expect(SendKeyDown(shell, SDLK_TAB, SDL_KMOD_NONE),
         "tab should complete the wrap command");
  Expect(WorkspaceShellTestAccess::CommandPaletteQuery(shell) == "wrap ",
         "tab completion should expand the wrap command name");
  Expect(WorkspaceShellTestAccess::CommandFeedbackText(shell) == "Completed wrap",
         "tab completion should report the wrap command completion");
}

void TestWorkspaceShellOverlayWheelScrollsWithoutMovingSelection() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(SendKeyDown(shell, SDLK_P, SDL_KMOD_CTRL | SDL_KMOD_SHIFT),
         "Ctrl+Shift+P should open the command palette");
  Expect(WorkspaceShellTestAccess::CommandPaletteMatchCount(shell) > 20,
         "the unfiltered palette should list more commands than fit on one page");
  Expect(WorkspaceShellTestAccess::OverlayScrollRow(shell) == 0,
         "a freshly opened palette should start unscrolled");
  const std::size_t selected_before = WorkspaceShellTestAccess::OverlaySelectedIndex(shell);

  const auto layout = microide::workspace::ComputeLayout(1280.0f, 720.0f, true, false, 288.0f,
                                                          184.0f);
  const float wheel_x = layout.editor_area.x + layout.editor_area.w * 0.5f;
  const float wheel_y = layout.editor_area.y + layout.editor_area.h * 0.5f;

  Expect(SendMouseWheel(shell, wheel_x, wheel_y, -2),
         "wheel over an open palette should be handled");
  Expect(WorkspaceShellTestAccess::OverlayScrollRow(shell) > 0,
         "wheeling down should scroll the palette list");
  Expect(WorkspaceShellTestAccess::OverlaySelectedIndex(shell) == selected_before,
         "wheeling must leave the palette selection untouched");

  const int scrolled = WorkspaceShellTestAccess::OverlayScrollRow(shell);
  Expect(SendMouseWheel(shell, wheel_x, wheel_y, 1),
         "wheel up over an open palette should be handled");
  Expect(WorkspaceShellTestAccess::OverlayScrollRow(shell) < scrolled,
         "wheeling up should scroll the palette list back");
  Expect(WorkspaceShellTestAccess::OverlaySelectedIndex(shell) == selected_before,
         "wheeling up must also leave the selection untouched");

  Expect(SendKeyDown(shell, SDLK_DOWN, SDL_KMOD_NONE),
         "Down should navigate the palette list");
  Expect(WorkspaceShellTestAccess::OverlaySelectedIndex(shell) == selected_before + 1,
         "Down should advance the palette selection by one row");
}

void TestWorkspaceShellCommandPaletteHomeEndEditsQuery() {
  WorkspaceShell shell;
  Expect(SendKeyDown(shell, SDLK_P, SDL_KMOD_CTRL | SDL_KMOD_SHIFT),
         "Ctrl+Shift+P should open the command palette");
  Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "wrap"),
         "typing should populate the palette query");
  Expect(SendKeyDown(shell, SDLK_HOME, SDL_KMOD_NONE),
         "Home should be consumed by the palette query field");
  Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "x"),
         "typing after Home should insert at the caret");
  Expect(WorkspaceShellTestAccess::CommandPaletteQuery(shell) == "xwrap",
         "Home should move the caret to the start of the query, not jump the list");
  Expect(SendKeyDown(shell, SDLK_END, SDL_KMOD_NONE),
         "End should be consumed by the palette query field");
  Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "y"),
         "typing after End should append");
  Expect(WorkspaceShellTestAccess::CommandPaletteQuery(shell) == "xwrapy",
         "End should move the caret to the end of the query");
}

void TestWorkspaceShellCommandPaletteRunsCommandLineVsFuzzyPick() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "README.md", "hello\n");
  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);

  Expect(WorkspaceShellTestAccess::SoftTabsEnabled(shell),
         "soft tabs should start enabled (the default indents with spaces)");
  Expect(ExecuteCommand(shell, "soft-tabs off"),
         "an argument-bearing query should run as a command line");
  Expect(!WorkspaceShellTestAccess::CommandPaletteOpen(shell),
         "running a command line should dismiss the palette");
  Expect(!WorkspaceShellTestAccess::SoftTabsEnabled(shell),
         "the soft-tabs command line should apply its argument");

  Expect(SendKeyDown(shell, SDLK_P, SDL_KMOD_CTRL | SDL_KMOD_SHIFT),
         "Ctrl+Shift+P should open the palette");
  Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "settings"),
         "typing a label fragment should filter the palette");
  Expect(WorkspaceShellTestAccess::CommandPaletteMatchCount(shell) > 0,
         "a label fragment should keep at least one fuzzy match (Settings)");
}

void TestWorkspaceShellCtrlNOpensUntitledTab() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "README.md", "hello\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);

  Expect(SendKeyDown(shell, SDLK_N, SDL_KMOD_CTRL),
         "Ctrl+N should be handled globally");
  Expect(WorkspaceShellTestAccess::OpenTabs(shell).size() == 1,
         "Ctrl+N should open a single untitled editor tab");
  const auto& tab = WorkspaceShellTestAccess::OpenTabs(shell).front();
  Expect(tab.path.empty() && tab.title == "Untitled-1",
         "Ctrl+N should create an untitled editor tab, numbered: " + tab.title);
  Expect(WorkspaceShellTestAccess::ActiveTabIndex(shell) == 0,
         "Ctrl+N should activate the newly opened untitled tab");

  Expect(SendKeyDown(shell, SDLK_N, SDL_KMOD_CTRL), "a second Ctrl+N should be handled");
  Expect(WorkspaceShellTestAccess::OpenTabs(shell).size() == 2 &&
             WorkspaceShellTestAccess::OpenTabs(shell)[1].title == "Untitled-2",
         "a second untitled tab should be Untitled-2");
  WorkspaceShellTestAccess::CloseTab(shell, 0);
  Expect(SendKeyDown(shell, SDLK_N, SDL_KMOD_CTRL), "a third Ctrl+N should be handled");
  const auto& tabs = WorkspaceShellTestAccess::OpenTabs(shell);
  Expect(tabs.size() == 2 && tabs[0].title == "Untitled-2" && tabs[1].title == "Untitled-1",
         "a new untitled tab should take the smallest free number");
}

void TestWorkspaceShellFilesShortcutEscapeRestoresSidebarFocus() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "README.md", "hello\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);

  Expect(WorkspaceShellTestAccess::ExecuteFilesFromShortcut(shell),
         "files shortcut should open the overlay");
  Expect(WorkspaceShellTestAccess::OverlayVisible(shell),
         "files shortcut should mark the overlay visible");
  Expect(WorkspaceShellTestAccess::OverlayModeIsFileFinder(shell),
         "files shortcut should open the file-finder overlay");
  Expect(WorkspaceShellTestAccess::FocusIsOverlay(shell),
         "files shortcut should focus the overlay");

  Expect(SendKeyDown(shell, SDLK_ESCAPE, SDL_KMOD_NONE),
         "Escape should close the overlay when it is visible");
  Expect(!WorkspaceShellTestAccess::OverlayVisible(shell),
         "Escape should dismiss the visible overlay");
  Expect(WorkspaceShellTestAccess::FocusIsSidebar(shell),
         "closing the overlay with a visible sidebar should restore sidebar focus");
}

void TestWorkspaceShellVsCodeAlignedShortcutsDispatch() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "README.md";
  WriteFile(source, "hello\nworld\n");

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "fixture project should open");
  WorkspaceShellTestAccess::OpenFile(shell, source);
  Expect(WorkspaceShellTestAccess::FocusIsEditor(shell),
         "opening a file should focus the editor");

  Expect(SendKeyDown(shell, SDLK_G, SDL_KMOD_CTRL), "Ctrl+G should be handled");
  Expect(WorkspaceShellTestAccess::PromptSurfaceVisible(shell),
         "Ctrl+G should open the Go to Line modal");
  Expect(SendKeyDown(shell, SDLK_ESCAPE, SDL_KMOD_NONE),
         "Escape should dismiss the Go to Line modal");
  Expect(!WorkspaceShellTestAccess::PromptSurfaceVisible(shell),
         "Escape should close the modal");

  Expect(SendKeyDown(shell, SDLK_P, SDL_KMOD_CTRL), "Ctrl+P should be handled");
  Expect(WorkspaceShellTestAccess::OverlayModeIsFileFinder(shell),
         "Ctrl+P should open the file-finder overlay");
  Expect(SendKeyDown(shell, SDLK_ESCAPE, SDL_KMOD_NONE),
         "Escape should dismiss the file finder");

  const bool sidebar_before = WorkspaceShellTestAccess::SidebarVisible(shell);
  Expect(SendKeyDown(shell, SDLK_B, SDL_KMOD_CTRL), "Ctrl+B should be handled");
  Expect(WorkspaceShellTestAccess::SidebarVisible(shell) != sidebar_before,
         "Ctrl+B should toggle sidebar visibility");

  const bool sidebar_state = WorkspaceShellTestAccess::SidebarVisible(shell);
  Expect(!WorkspaceShellTestAccess::OverlayVisible(shell),
         "no overlay should be open before pressing F6");
  SendKeyDown(shell, SDLK_F6, SDL_KMOD_NONE);
  Expect(!WorkspaceShellTestAccess::OverlayVisible(shell),
         "F6 should no longer open the file finder");
  SendKeyDown(shell, SDLK_F8, SDL_KMOD_NONE);
  Expect(WorkspaceShellTestAccess::SidebarVisible(shell) == sidebar_state,
         "F8 should no longer toggle the sidebar");
}

void TestWorkspaceShellFilesShortcutEscapeRestoresEditorFocusOnWelcome() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::ResetProjectScopedState(shell, true);

  Expect(WorkspaceShellTestAccess::ExecuteFilesFromShortcut(shell),
         "files shortcut should still open the overlay on the welcome surface");
  Expect(WorkspaceShellTestAccess::OverlayVisible(shell),
         "welcome files shortcut should mark the overlay visible");
  Expect(WorkspaceShellTestAccess::FocusIsOverlay(shell),
         "welcome files shortcut should focus the overlay");

  Expect(SendKeyDown(shell, SDLK_ESCAPE, SDL_KMOD_NONE),
         "Escape should close the welcome overlay");
  Expect(!WorkspaceShellTestAccess::OverlayVisible(shell),
         "Escape should dismiss the welcome overlay");
  Expect(WorkspaceShellTestAccess::FocusIsEditor(shell),
         "closing the welcome overlay should restore editor focus when no sidebar is visible");
}

void TestWorkspaceShellWelcomeTabUsesLeftEdgeHitArea() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "README.md", "welcome\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);

  const auto layout = WorkspaceShellTestAccess::CurrentLayout(shell);
  Expect(SendMouseDown(shell, layout.tab_strip.x + 1.0f, layout.tab_strip.y + 10.0f,
                       SDL_BUTTON_LEFT),
         "clicking the left edge of the welcome tab should be handled");
  Expect(WorkspaceShellTestAccess::FocusIsEditor(shell),
         "clicking the left edge of the welcome tab should focus the editor surface");
}

void TestWorkspaceShellFilesShortcutOpensMatchedFileAfterDeferredIndexCacheBuild() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path readme = root / "README.md";
  const std::filesystem::path target = root / "src" / "target-match.cpp";
  WriteFile(readme, "hello\n");
  WriteFile(target, "int target() {\n  return 42;\n}\n");

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "project fixture should open");
  Expect(WaitForFileIndexPath(shell, std::filesystem::path("src/target-match.cpp"), true,
                              std::chrono::milliseconds(5000)),
         "file index should contain the target before the finder is asked to match it");
  Expect(WorkspaceShellTestAccess::ExecuteFilesFromShortcut(shell),
         "files shortcut should open the file finder overlay");
  Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "target-match"),
         "typing in the file finder should be handled");
  const bool finder_open = WorkspaceShellTestAccess::OverlayVisible(shell) &&
                           WorkspaceShellTestAccess::OverlayModeIsFileFinder(shell);
  const std::size_t match_count = WorkspaceShellTestAccess::FileFinderResultCount(shell);
  const std::optional<std::filesystem::path> selected =
      WorkspaceShellTestAccess::FileFinderSelectedPath(shell);
  const std::string finder_state =
      std::string(" (finder_open=") + (finder_open ? "yes" : "no") +
      ", matches=" + std::to_string(match_count) +
      ", selected=" + (selected.has_value() ? selected->generic_string() : "<none>") + ")";
  Expect(finder_open, "the file finder must still be the active overlay when Enter arrives" +
                          finder_state);
  Expect(match_count > 0, "typing the target's name must match it in the finder" + finder_state);

  Expect(SendKeyDown(shell, SDLK_RETURN, SDL_KMOD_NONE),
         "pressing enter in the file finder should open the selected match");
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).path() == target.lexically_normal(),
         "file finder should still open matches after deferred index cache build" + finder_state +
             " opened=" + WorkspaceShellTestAccess::ActiveEditor(shell).path().generic_string());
}

void TestWorkspaceShellProjectOpenFromWelcomeInvalidatesCachedLayout() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "README.md", "hello\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::RenderFrame(shell);
  WorkspaceShellTestAccess::ResetPrepareFrameLayoutComputeCount(shell);

  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "project should open from welcome state");
  const auto redraw = shell.ConsumePendingRenderInvalidation();
  Expect(redraw.full || redraw.frame || !redraw.rects.empty(),
         "opening a project from welcome should invalidate cached layout and request redraw");
}

void TestWorkspaceShellResolvedKeybindingsAreCachedUntilInputsChange() {
  WorkspaceShell shell;

  const auto& first = WorkspaceShellTestAccess::ResolvedKeybindings(shell);
  Expect(!first.empty(), "resolved keybindings should include built-ins");
  const microide::workspace::ResolvedKeybinding* first_data = first.data();

  const auto& second = WorkspaceShellTestAccess::ResolvedKeybindings(shell);
  Expect(second.data() == first_data,
         "resolved keybindings should reuse the cached vector when inputs are unchanged");

  const bool has_save_before =
      std::any_of(first.begin(), first.end(),
                  [](const microide::workspace::ResolvedKeybinding& binding) {
        return binding.id == "save";
      });
  Expect(has_save_before, "resolved keybindings should include save before disabling it");

  WorkspaceShellTestAccess::SetDisabledKeybindingIds(shell, {"save"});
  const auto& disabled = WorkspaceShellTestAccess::ResolvedKeybindings(shell);
  const bool has_save_after =
      std::any_of(disabled.begin(), disabled.end(),
                  [](const microide::workspace::ResolvedKeybinding& binding) {
        return binding.id == "save";
      });
  Expect(!has_save_after,
         "resolved keybindings should rebuild when disabled keybinding ids change");

  const microide::workspace::ResolvedKeybinding* disabled_data = disabled.data();
  const auto& disabled_again = WorkspaceShellTestAccess::ResolvedKeybindings(shell);
  Expect(disabled_again.data() == disabled_data,
         "rebuilt resolved keybindings should be reused on subsequent stable calls");
}

void TestWorkspaceShellReopeningCleanTabDoesNotReloadUnrelatedTabs() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path alpha = root / "alpha.txt";
  const std::filesystem::path beta = root / "beta.txt";
  WriteFile(alpha, "alpha\n");
  WriteFile(beta, "beta\n");

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "project should open for clean-tab reload regression");
  Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, alpha),
         "alpha should open for clean-tab reload regression");
  Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, beta),
         "beta should open for clean-tab reload regression");

  util::ResetPerformanceCounters();
  WorkspaceShellTestAccess::OpenFile(shell, alpha);

  Expect(util::ReadPerformanceCounter(util::PerfCounterId::EditorContentRevisionBumps) == 0,
         "reopening an unchanged already-open clean tab should not reload it at all");
  Expect(util::ReadPerformanceCounter(util::PerfCounterId::EditorLineWidthTableBuilds) == 0,
         "reopening an unchanged already-open clean tab should not rebuild any width table");
}

void TestWorkspaceShellReopeningCleanTabPicksUpExternalEdits() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path alpha = root / "alpha.txt";
  WriteFile(alpha, "alpha\n");

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "project should open for external-edit reload regression");
  Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, alpha),
         "alpha should open for external-edit reload regression");

  WriteFile(alpha, "alpha changed on disk\n");

  WorkspaceShellTestAccess::OpenFile(shell, alpha);

  const editor::TextViewport& viewport = WorkspaceShellTestAccess::ActiveEditor(shell);
  Expect(viewport.lines().size() >= 1 && viewport.lines().LineView(0) == "alpha changed on disk",
         "reopening a clean tab whose file changed on disk should load the new content");
}

void TestWorkspaceShellOverlayOutsideClickRestoresPrimaryFocus() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "README.md", "hello\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);

  Expect(WorkspaceShellTestAccess::ExecuteFilesFromShortcut(shell),
         "files shortcut should open the overlay before the outside click test");
  const auto layout = microide::workspace::ComputeLayout(1280.0f, 720.0f, true, false, 288.0f, 184.0f);
  const float click_x = layout.editor_area.x + 12.0f;
  const float click_y = layout.editor_area.y + 12.0f;
  Expect(SendMouseDown(shell, click_x, click_y, SDL_BUTTON_LEFT),
         "clicking outside the overlay should be handled");
  Expect(!WorkspaceShellTestAccess::OverlayVisible(shell),
         "outside clicks should dismiss the overlay");
  Expect(WorkspaceShellTestAccess::FocusIsSidebar(shell),
         "outside-click overlay dismissal should restore the primary sidebar focus");
}

void TestWorkspaceShellTreeCollapseAllowsOpenDescendantsAndReselectReveal() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path readme = root / "README.md";
  const std::filesystem::path source_dir = root / "src";
  const std::filesystem::path source = source_dir / "main.cpp";
  std::filesystem::create_directories(source_dir);
  WriteFile(readme, "readme\n");
  WriteFile(source, "int main() {}\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenFile(shell, readme);
  WorkspaceShellTestAccess::OpenFile(shell, source);

  const auto tree_contains_path = [&](const std::filesystem::path& path) {
    const auto& entries = WorkspaceShellTestAccess::TreeEntries(shell);
    return std::any_of(entries.begin(), entries.end(),
                       [&](const auto& entry) { return entry.path == path.lexically_normal(); });
  };

  Expect(tree_contains_path(source),
         "opening a nested file should reveal it in the tree initially");
  Expect(WorkspaceShellTestAccess::SelectTreePath(shell, source_dir),
         "tree collapse fixture should be able to select the parent directory");

  WorkspaceShellTestAccess::CollapseTreeSelection(shell);

  Expect(WorkspaceShellTestAccess::SelectedTreePath(shell) == source_dir.lexically_normal(),
         "collapsing an open-file ancestor should leave the directory selected");
  Expect(!tree_contains_path(source),
         "collapsing an open-file ancestor should hide the descendant rows");

  WorkspaceShellTestAccess::ActivateTab(shell, 0);
  Expect(!tree_contains_path(source),
         "selecting another tab should not force unrelated collapsed directories open");

  WorkspaceShellTestAccess::ActivateTab(shell, 1);

  Expect(!tree_contains_path(source),
         "reselecting the open file tab should preserve collapsed ancestors in the tree");
}

void TestWorkspaceShellWheelDoesNotStealKeyboardFocus() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  for (int i = 0; i < 40; ++i) {
    WriteFile(root / ("file" + std::to_string(i) + ".txt"), "line\n");
  }

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 320);
  WorkspaceShellTestAccess::OpenFile(shell, root / "file0.txt");
  WorkspaceShellTestAccess::SetFocusEditor(shell);
  Expect(WorkspaceShellTestAccess::FocusIsEditor(shell),
         "the fixture should start with the editor focused");

  const auto layout = microide::workspace::ComputeLayout(
      1280.0f, 320.0f, true, false, 288.0f, 184.0f);
  Expect(SendMouseWheel(shell, layout.sidebar.x + layout.sidebar.w * 0.5f,
                        layout.sidebar.y + 72.0f, -4),
         "scrolling the sidebar should be handled");
  Expect(WorkspaceShellTestAccess::SidebarScrollRow(shell) > 0,
         "the sidebar should have scrolled");
  Expect(WorkspaceShellTestAccess::FocusIsEditor(shell),
         "scrolling the sidebar must leave keyboard focus in the editor");
}

void TestWorkspaceShellWheelStepMatchesEditorAcrossSurfaces() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  for (int i = 0; i < 80; ++i) {
    WriteFile(root / ("file" + std::to_string(i) + ".txt"), "line\n");
  }

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 320);

  const auto layout = microide::workspace::ComputeLayout(
      1280.0f, 320.0f, true, false, 288.0f, 184.0f);
  Expect(SendMouseWheel(shell, layout.sidebar.x + layout.sidebar.w * 0.5f,
                        layout.sidebar.y + 72.0f, -1),
         "one wheel tick over the sidebar should be handled");
  Expect(WorkspaceShellTestAccess::SidebarScrollRow(shell) ==
             microide::workspace::kWheelScrollRows,
         "one wheel tick should advance the sidebar by the shared row step");
}

void TestWorkspaceShellCtrlOOpensNativeFilePicker() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path picked = root / "picked.cpp";
  WriteFile(picked, "int main() {}\n");

  EnsureDummySdlVideoInitialized();
  WorkspaceShell shell;
  WorkspaceShellTestAccess::RegisterLifecycleWakeEvents(shell);
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  std::filesystem::path requested_default;
  int launch_count = 0;
  WorkspaceShellTestAccess::SetOpenFileDialogLauncher(
      shell, [&](WorkspaceShell&, const std::filesystem::path& default_location) {
        ++launch_count;
        requested_default = default_location.lexically_normal();
        return true;
      });

  Expect(SendKeyDown(shell, SDLK_O, SDL_KMOD_CTRL), "Ctrl+O should be bound");
  Expect(launch_count == 1, "Ctrl+O should launch the native file picker exactly once");
  Expect(requested_default == root.lexically_normal(),
         "the picker should open at the project root");
  Expect(WorkspaceShellTestAccess::OpenFileDialogActive(shell),
         "the file picker should be marked active while waiting for a selection");
  Expect(WorkspaceShellTestAccess::OpenTabs(shell).empty(),
         "launching the picker should not open a tab on its own");

  WorkspaceShellTestAccess::QueueOpenFileDialogSelection(shell, picked);
  const Uint32 wake = WorkspaceShellTestAccess::DialogWakeEventType(shell);
  Expect(wake != 0, "the shell should have registered the dialog wake event");
  SDL_Event event{};
  event.type = wake;
  (void)shell.HandleEvent(event);

  Expect(!WorkspaceShellTestAccess::OpenFileDialogActive(shell),
         "the file picker should clear its active state after a selection");
  Expect(WorkspaceShellTestAccess::OpenTabs(shell).size() == 1,
         "the picked file should open as an editor tab");
}

void TestWorkspaceShellTreeSidebarSupportsPageAndHomeEndKeys() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  for (int i = 0; i < 40; ++i) {
    WriteFile(root / ("file" + std::to_string(i) + ".txt"), "line\n");
  }

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::SetFocusSidebar(shell);

  const std::filesystem::path first = WorkspaceShellTestAccess::SelectedTreePath(shell);
  Expect(first == root.lexically_normal(), "the tree should start on the root row");

  Expect(SendKeyDown(shell, SDLK_PAGEDOWN, SDL_KMOD_NONE),
         "PageDown should be handled by the tree sidebar");
  const std::filesystem::path after_page = WorkspaceShellTestAccess::SelectedTreePath(shell);
  Expect(after_page != first, "PageDown should move the tree selection off the first row");

  Expect(SendKeyDown(shell, SDLK_END, SDL_KMOD_NONE),
         "End should be handled by the tree sidebar");
  const std::filesystem::path last = WorkspaceShellTestAccess::SelectedTreePath(shell);
  Expect(last != after_page, "End should jump past a single page");

  Expect(SendKeyDown(shell, SDLK_HOME, SDL_KMOD_NONE),
         "Home should be handled by the tree sidebar");
  Expect(WorkspaceShellTestAccess::SelectedTreePath(shell) == first,
         "Home should return the tree selection to the first row");

  Expect(SendKeyDown(shell, SDLK_PAGEUP, SDL_KMOD_NONE),
         "PageUp should be handled by the tree sidebar");
  Expect(WorkspaceShellTestAccess::SelectedTreePath(shell) == first,
         "PageUp at the top should clamp rather than wrap");
}

void TestWorkspaceShellF6CyclesEveryVisibleSurface() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "main.cpp", "int main() {}\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::SetDebugPaneVisible(shell, true);
  WorkspaceShellTestAccess::SetFocusEditor(shell);

  Expect(SendKeyDown(shell, SDLK_F6, SDL_KMOD_NONE), "F6 should be handled");
  Expect(WorkspaceShellTestAccess::FocusIsDebugPane(shell),
         "F6 from the editor should reach the visible debug pane");
  Expect(SendKeyDown(shell, SDLK_F6, SDL_KMOD_NONE), "F6 should keep cycling");
  Expect(WorkspaceShellTestAccess::FocusIsSidebar(shell),
         "F6 past the last surface should wrap to the first");
  Expect(SendKeyDown(shell, SDLK_F6, SDL_KMOD_NONE), "F6 should keep cycling");
  Expect(WorkspaceShellTestAccess::FocusIsEditor(shell),
         "F6 should return to the editor after a full cycle");

  Expect(SendKeyDown(shell, SDLK_F6, SDL_KMOD_SHIFT),
         "Shift+F6 should be handled");
  Expect(WorkspaceShellTestAccess::FocusIsSidebar(shell),
         "Shift+F6 should step backwards through the ring");
  Expect(SendKeyDown(shell, SDLK_F6, SDL_KMOD_SHIFT),
         "Shift+F6 should keep cycling backwards");
  Expect(WorkspaceShellTestAccess::FocusIsDebugPane(shell),
         "Shift+F6 should wrap backwards onto the debug pane");
}

void TestWorkspaceShellCtrlTabStepsRecentEditors() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path alpha = root / "alpha.cpp";
  const std::filesystem::path beta = root / "beta.cpp";
  const std::filesystem::path gamma = root / "gamma.cpp";
  WriteFile(alpha, "int alpha() { return 1; }\n");
  WriteFile(beta, "int beta() { return 2; }\n");
  WriteFile(gamma, "int gamma() { return 3; }\n");

  SoftwareCanvas canvas(1280, 720);
  WorkspaceShell shell;
  const auto render = [&] { WorkspaceShellTestAccess::RenderFrameWithRenderer(shell, canvas.renderer()); };
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, alpha), "first tab should open");
  render();
  Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, beta), "second tab should open");
  Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, gamma), "third tab should open");
  render();
  WorkspaceShellTestAccess::ActivateTab(shell, 1);
  render();
  WorkspaceShellTestAccess::SetFocusEditor(shell);

  Expect(SendKeyDown(shell, SDLK_TAB, SDL_KMOD_CTRL), "Ctrl+Tab should be handled");
  Expect(WorkspaceShellTestAccess::OverlayVisible(shell) &&
             WorkspaceShellTestAccess::ActiveOverlayMode(shell) == WorkspaceShell::OverlayMode::QuickPick,
         "Ctrl+Tab should show the recent-editors pick while Ctrl is held");
  Expect(WorkspaceShellTestAccess::ActiveTabIndex(shell) == 1,
         "the pick should not switch editors until Ctrl is released");
  Expect(shell.HandleEvent(KeyUpEvent(SDLK_LCTRL)).handled, "releasing Ctrl should be handled");
  Expect(!WorkspaceShellTestAccess::OverlayVisible(shell), "releasing Ctrl should close the pick");
  Expect(WorkspaceShellTestAccess::ActiveTabIndex(shell) == 2,
         "a Ctrl+Tab tap should open the previously used editor (gamma)");
  render();

  Expect(SendKeyDown(shell, SDLK_TAB, SDL_KMOD_CTRL), "Ctrl+Tab should be handled");
  Expect(shell.HandleEvent(KeyUpEvent(SDLK_LCTRL)).handled, "releasing Ctrl should be handled");
  Expect(WorkspaceShellTestAccess::ActiveTabIndex(shell) == 1,
         "a second tap should return to the editor the first one left (beta)");
  render();

  Expect(SendKeyDown(shell, SDLK_TAB, SDL_KMOD_CTRL), "Ctrl+Tab should be handled");
  Expect(SendKeyDown(shell, SDLK_TAB, SDL_KMOD_CTRL), "a second Tab under Ctrl should be handled");
  Expect(WorkspaceShellTestAccess::ActiveTabIndex(shell) == 1,
         "stepping should not switch editors while Ctrl is held");
  Expect(shell.HandleEvent(KeyUpEvent(SDLK_LCTRL)).handled, "releasing Ctrl should be handled");
  Expect(WorkspaceShellTestAccess::ActiveTabIndex(shell) == 0,
         "Ctrl+Tab, Tab should open the least recent of three (alpha)");
  render();

  Expect(SendKeyDown(shell, SDLK_TAB, SDL_KMOD_CTRL), "Ctrl+Tab should be handled");
  Expect(SendKeyDown(shell, SDLK_ESCAPE, SDL_KMOD_CTRL), "Escape should be handled");
  Expect(!WorkspaceShellTestAccess::OverlayVisible(shell), "Escape under Ctrl should close the pick");
  (void)shell.HandleEvent(KeyUpEvent(SDLK_LCTRL));
  Expect(WorkspaceShellTestAccess::ActiveTabIndex(shell) == 0,
         "Escape should leave the active editor as it was");
}

void TestWorkspaceShellDoubleClickResetsResizeDividers() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "main.cpp", "int main() {}\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::SetSidebarWidth(shell, 420.0f);
  WorkspaceShellTestAccess::SetBottomPanelHeight(shell, 320.0f);
  WorkspaceShellTestAccess::EnsureTerminalTab(shell);

  const SDL_FRect sidebar_handle = microide::workspace::SidebarResizeHitRect(
      WorkspaceShellTestAccess::CurrentLayout(shell));
  Expect(SendMouseDown(shell, sidebar_handle.x + sidebar_handle.w * 0.5f,
                       sidebar_handle.y + sidebar_handle.h * 0.5f, SDL_BUTTON_LEFT, 2),
         "double-clicking the sidebar divider should be handled");
  Expect(WorkspaceShellTestAccess::SidebarWidth(shell) ==
             microide::workspace::kWorkspaceDefaultSidebarWidth,
         "double-clicking the sidebar divider should restore the default width");

  const SDL_FRect panel_handle = microide::workspace::BottomPanelResizeHandleRect(
      WorkspaceShellTestAccess::CurrentLayout(shell));
  Expect(SendMouseDown(shell, panel_handle.x + panel_handle.w * 0.5f,
                       panel_handle.y + panel_handle.h * 0.5f, SDL_BUTTON_LEFT, 2),
         "double-clicking the bottom panel divider should be handled");
  Expect(WorkspaceShellTestAccess::BottomPanelHeight(shell) ==
             microide::workspace::kWorkspaceDefaultBottomPanelHeight,
         "double-clicking the bottom panel divider should restore the default height");
}

void TestWorkspaceShellTreeScrollDoesNotSnapToSelectionDuringRender() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  for (int i = 0; i < 40; ++i) {
    WriteFile(root / ("file" + std::to_string(i) + ".txt"), "line\n");
  }

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 320);

  const auto layout = microide::workspace::ComputeLayout(
      1280.0f, 320.0f, true, false, 288.0f, 184.0f);
  const float wheel_x = layout.sidebar.x + layout.sidebar.w * 0.5f;
  const float wheel_y = layout.sidebar.y + 72.0f;

  Expect(WorkspaceShellTestAccess::SelectedTreePath(shell) == root.lexically_normal(),
         "tree scroll fixture should start with the root selected");
  Expect(SendMouseWheel(shell, wheel_x, wheel_y, -8),
         "scrolling the tree sidebar should be handled");
  Expect(WorkspaceShellTestAccess::SidebarScrollRow(shell) > 0,
         "tree scrolling should move the sidebar away from the selected root row");

  WorkspaceShellTestAccess::RenderFrame(shell);

  Expect(WorkspaceShellTestAccess::SidebarScrollRow(shell) > 0,
         "rendering should not snap tree scrolling back to keep the selected row visible");
}

void TestWorkspaceShellTabSwitchSelectsActiveTreePath() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path first = root / "alpha.cpp";
  const std::filesystem::path second = root / "beta.cpp";
  WriteFile(first, "int alpha() { return 1; }\n");
  WriteFile(second, "int beta() { return 2; }\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenFile(shell, first);
  WorkspaceShellTestAccess::OpenFile(shell, second);

  Expect(WorkspaceShellTestAccess::SelectedTreePath(shell) == second.lexically_normal(),
         "opening the second tab should select its path in the project tree");

  WorkspaceShellTestAccess::ActivateTab(shell, 0);
  Expect(WorkspaceShellTestAccess::SelectedTreePath(shell) == first.lexically_normal(),
         "activating the first tab should select its path in the project tree");

  WorkspaceShellTestAccess::ActivateTab(shell, 1);
  Expect(WorkspaceShellTestAccess::SelectedTreePath(shell) == second.lexically_normal(),
         "activating the second tab should select its path in the project tree");
}

void TestWorkspaceShellTreeCollapseButtonCollapsesAllOpenDirectories() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path nested_dir = root / "src" / "nested";
  const std::filesystem::path source = nested_dir / "main.cpp";
  std::filesystem::create_directories(nested_dir);
  WriteFile(source, "int main() {}\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::OpenFile(shell, source);

  const auto tree_contains_path = [&](const std::filesystem::path& path) {
    const auto& entries = WorkspaceShellTestAccess::TreeEntries(shell);
    return std::any_of(entries.begin(), entries.end(),
                       [&](const auto& entry) { return entry.path == path.lexically_normal(); });
  };

  Expect(tree_contains_path(source),
         "opening a nested file should expand its ancestors before collapsing all");
  const SDL_FRect button_rect = WorkspaceShellTestAccess::TreeSidebarCollapseButtonRect(shell);
  Expect(SendMouseDown(
             shell, button_rect.x + button_rect.w * 0.5f, button_rect.y + button_rect.h * 0.5f,
             SDL_BUTTON_LEFT),
         "clicking the collapse button should be handled");
  Expect(!tree_contains_path(source),
         "clicking the collapse button should hide descendants under expanded directories");
  Expect(WorkspaceShellTestAccess::SelectedTreePath(shell) == (root / "src").lexically_normal(),
         "collapsing all should keep selection on the nearest still-visible ancestor");
}

void TestWorkspaceShellTreeHeaderCompactsBeforeButtonsOverlap() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path nested_dir = root / "src" / "nested";
  const std::filesystem::path source = nested_dir / "main.cpp";
  std::filesystem::create_directories(nested_dir);
  WriteFile(source, "int main() {}\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 520, 720);
  WorkspaceShellTestAccess::OpenFile(shell, source);

  const SDL_FRect mode_rect = WorkspaceShellTestAccess::SidebarModeButtonRect(shell);
  const SDL_FRect collapse_rect = WorkspaceShellTestAccess::TreeSidebarCollapseButtonRect(shell);
  const SDL_FRect refresh_rect = WorkspaceShellTestAccess::TreeSidebarRefreshButtonRect(shell);

  Expect(mode_rect.w > 0.0f && collapse_rect.w > 0.0f && refresh_rect.w > 0.0f,
         "compact tree-header fixture should still expose all header controls");
  Expect(mode_rect.y + mode_rect.h <= collapse_rect.y,
         "project sidebar actions should render on a dedicated row below the selector");
  Expect(collapse_rect.x + collapse_rect.w <= refresh_rect.x,
         "collapse and refresh controls should remain non-overlapping in compact header mode");
  Expect(std::abs(collapse_rect.y - refresh_rect.y) < 0.1f,
         "project sidebar action buttons should share the same action-row baseline");

  WorkspaceShellTestAccess::ShowGitSidebar(shell);
  const std::array<SDL_FRect, 3> git_action_rects =
      WorkspaceShellTestAccess::GitSidebarTopActionRects(shell);
  Expect(std::abs(git_action_rects[0].y - collapse_rect.y) < 0.1f,
         "project and source-control sidebars should align action rows for cohesion");

  Expect(SendMouseDown(
             shell, collapse_rect.x + collapse_rect.w * 0.5f, collapse_rect.y + collapse_rect.h * 0.5f,
             SDL_BUTTON_LEFT),
         "compact collapse button should remain clickable after header compaction");
}

void TestWorkspaceShellTabSizeSettingAppliesImmediately() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "main.cpp";
  WriteFile(source, "int main() { return 0; }\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenFile(shell, source);

  Expect(WorkspaceShellTestAccess::EditorTabSize(shell) == 4,
         "tab-size settings fixture should start from the default tab size");
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).tab_size() == 4,
         "active editor should start with the default tab size");
  Expect(WorkspaceShellTestAccess::SetSettingValue(shell, "editor.tab_size", "2"),
         "setting editor.tab_size should succeed through the settings path");
  Expect(WorkspaceShellTestAccess::EditorTabSize(shell) == 2,
         "editor tab-size preference should update immediately after setting change");
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).tab_size() == 2,
         "active editor viewport should apply the new tab-size preference immediately");

}

void TestWorkspaceShellTabSizeSettingStaysVisibleAfterRestart() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "main.cpp";
  WriteFile(source, "int main() { return 0; }\n");

  const std::filesystem::path xdg_state_home = temp_dir.path() / "xdg-state";
  const std::filesystem::path xdg_config_home = temp_dir.path() / "xdg-config";
  ScopedProjectAppHomes scoped_app_homes(xdg_state_home, xdg_config_home);

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenFile(shell, source);
  Expect(WorkspaceShellTestAccess::SetSettingValue(shell, "editor.tab_size", "2"),
         "setting editor.tab_size should succeed before restart");

  WorkspaceShellTestAccess::FlushPendingStateWrites(shell);

  WorkspaceShell reloaded_shell;
  WorkspaceShellTestAccess::SetProjectRoot(reloaded_shell, root);
  Expect(WorkspaceShellTestAccess::RestoreConfigState(reloaded_shell),
         "restarted shell should restore project config state");
  const auto stored_tab_size =
      WorkspaceShellTestAccess::ProjectStoredSettingValue(reloaded_shell, "editor.tab_size");
  Expect(stored_tab_size.has_value() && *stored_tab_size == "2",
         "restored project settings should retain editor.tab_size for settings overlay display");
}

void TestWorkspaceShellFontSizeSettingAppliesImmediately() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "main.cpp";
  WriteFile(source, "int main() { return 0; }\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenFile(shell, source);

  Expect(WorkspaceShellTestAccess::EditorFontSize(shell) == 13,
         "font-size settings fixture should start from the default font size");
  Expect(WorkspaceShellTestAccess::SetSettingValue(shell, "editor.font_size", "20"),
         "setting editor.font_size should succeed through the settings path");
  Expect(WorkspaceShellTestAccess::EditorFontSize(shell) == 20,
         "editor font-size preference should update immediately after the setting change");
  Expect(WorkspaceShellTestAccess::SetSettingValue(shell, "editor.font_size", "100"),
         "setting an out-of-range editor.font_size should still succeed");
  Expect(WorkspaceShellTestAccess::EditorFontSize(shell) == 32,
         "editor font-size preference should clamp to the maximum supported size");
}

void TestWorkspaceShellFontSizeIsProjectScopedAndPersists() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root_a = temp_dir.path() / "project-a";
  const std::filesystem::path source_a = root_a / "main.cpp";
  WriteFile(source_a, "int main() { return 0; }\n");
  const std::filesystem::path root_b = temp_dir.path() / "project-b";
  const std::filesystem::path source_b = root_b / "main.cpp";
  WriteFile(source_b, "int main() { return 1; }\n");

  const std::filesystem::path xdg_state_home = temp_dir.path() / "xdg-state";
  const std::filesystem::path xdg_config_home = temp_dir.path() / "xdg-config";
  ScopedProjectAppHomes scoped_app_homes(xdg_state_home, xdg_config_home);

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root_a);
  WorkspaceShellTestAccess::OpenFile(shell, source_a);
  Expect(WorkspaceShellTestAccess::SetSettingValue(shell, "editor.font_size", "20"),
         "setting editor.font_size should succeed before restart");

  WorkspaceShellTestAccess::FlushPendingStateWrites(shell);
  WorkspaceShell reloaded_shell;
  WorkspaceShellTestAccess::SetProjectRoot(reloaded_shell, root_a);
  Expect(WorkspaceShellTestAccess::RestoreConfigState(reloaded_shell),
         "restarted shell should restore project config state");
  Expect(WorkspaceShellTestAccess::EditorFontSize(reloaded_shell) == 20,
         "restored project should reapply its persisted editor font size");
  const auto stored_font_size =
      WorkspaceShellTestAccess::ProjectStoredSettingValue(reloaded_shell, "editor.font_size");
  Expect(stored_font_size.has_value() && *stored_font_size == "20",
         "restored project settings should retain editor.font_size for settings overlay display");

  WorkspaceShell other_shell;
  WorkspaceShellTestAccess::SetProjectRoot(other_shell, root_b);
  WorkspaceShellTestAccess::OpenFile(other_shell, source_b);
  Expect(WorkspaceShellTestAccess::EditorFontSize(other_shell) == 13,
         "a different project should keep the default font size; the setting is project-scoped");
}

void TestWorkspaceShellCommandTabSizeStaysVisibleAfterRestart() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "main.cpp";
  WriteFile(source, "int main() { return 0; }\n");

  const std::filesystem::path xdg_state_home = temp_dir.path() / "xdg-state";
  const std::filesystem::path xdg_config_home = temp_dir.path() / "xdg-config";
  ScopedProjectAppHomes scoped_app_homes(xdg_state_home, xdg_config_home);

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenFile(shell, source);
  Expect(ExecuteCommand(shell, "tab-size 5"),
         "tab-size command should update editor preferences and persist project config");

  WorkspaceShellTestAccess::FlushPendingStateWrites(shell);

  WorkspaceShell reloaded_shell;
  WorkspaceShellTestAccess::SetProjectRoot(reloaded_shell, root);
  Expect(WorkspaceShellTestAccess::RestoreConfigState(reloaded_shell),
         "restarted shell should restore project config state after tab-size command");
  const auto stored_tab_size =
      WorkspaceShellTestAccess::ProjectStoredSettingValue(reloaded_shell, "editor.tab_size");
  Expect(stored_tab_size.has_value() && *stored_tab_size == "5",
         "restored settings list should mirror canonical tab size after command-driven updates");
}

void TestWorkspaceShellAutoCloseToggleUpdatesViewportContract() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "main.cpp";
  WriteFile(source, "int main() { return 0; }\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenFile(shell, source);

  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).language_contract_view().auto_close_enabled,
         "auto-close should start enabled from the default setting");
  Expect(ExecuteCommand(shell, "toggle-editor-auto-close"),
         "toggle-editor-auto-close should execute from the command line");
  Expect(!WorkspaceShellTestAccess::ActiveEditor(shell).language_contract_view().auto_close_enabled,
         "toggling auto-close off should update the active viewport contract immediately");
  const auto stored_disabled = WorkspaceShellTestAccess::ProjectStoredSettingValue(
      shell, "editor.brackets.auto_close.enabled");
  Expect(stored_disabled.has_value() && *stored_disabled == "false",
         "toggle action should persist editor.brackets.auto_close.enabled=false");

  Expect(ExecuteCommand(shell, "toggle-editor-auto-close"),
         "toggling auto-close again should re-enable it");
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).language_contract_view().auto_close_enabled,
         "second toggle should re-enable auto-close in the active viewport contract");
}

void TestWorkspaceShellAutoCloseToggleUpdatesAllTabContracts() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path first = root / "first.cpp";
  const std::filesystem::path second = root / "second.cpp";
  WriteFile(first, "int a() { return 0; }\n");
  WriteFile(second, "int b() { return 1; }\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenFile(shell, first);
  WorkspaceShellTestAccess::OpenFile(shell, second);

  Expect(WorkspaceShellTestAccess::FocusedGroupTabEditor(shell, 0)
             .language_contract_view()
             .auto_close_enabled,
         "background tab starts with auto-close enabled");

  Expect(ExecuteCommand(shell, "toggle-editor-auto-close"),
         "toggle-editor-auto-close should execute");

  Expect(!WorkspaceShellTestAccess::ActiveEditor(shell).language_contract_view().auto_close_enabled,
         "active tab contract reflects the auto-close toggle");
  Expect(!WorkspaceShellTestAccess::FocusedGroupTabEditor(shell, 0)
              .language_contract_view()
              .auto_close_enabled,
         "background tab contract is also refreshed by the all-tabs contract rebuild");
}

void TestWorkspaceShellLanguageOverrideSettingAppliesLive() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path first = root / "first.cpp";
  const std::filesystem::path second = root / "second.cpp";
  WriteFile(first, "int a;\n");
  WriteFile(second, "int b;\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenFile(shell, first);
  WorkspaceShellTestAccess::OpenFile(shell, second);
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).language_contract_view().line_comment == "//",
         "a C++ tab starts with the language's own comment marker");

  Expect(WorkspaceShellTestAccess::SetSettingValue(shell, "editor.comments.user_line", "#"),
         "the comment override should be writable");
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).language_contract_view().line_comment == "#",
         "the active tab takes the comment override at once");
  Expect(WorkspaceShellTestAccess::FocusedGroupTabEditor(shell, 0).language_contract_view().line_comment ==
             "#",
         "and so does a background tab");

  Expect(WorkspaceShellTestAccess::SetSettingValue(shell, "editor.brackets.user_pairs", "<|>"),
         "the extra-pairs override should be writable");
  const auto& pairs = WorkspaceShellTestAccess::ActiveEditor(shell).language_contract_view().auto_close_pairs;
  Expect(std::any_of(pairs.begin(), pairs.end(),
                     [](const auto& pair) { return pair.open == "<" && pair.close == ">"; }),
         "an extra pair auto-closes at once");

  Expect(WorkspaceShellTestAccess::SetSettingValue(shell, "editor.comments.user_line", ""),
         "clearing the comment override should be accepted");
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).language_contract_view().line_comment == "//",
         "clearing the override restores the language's marker");
}

void TestWorkspaceShellSameLanguageTabsShareOneContractView() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path first = root / "first.cpp";
  const std::filesystem::path second = root / "second.cpp";
  const std::filesystem::path other = root / "script.py";
  WriteFile(first, "int a() { return 0; }\n");
  WriteFile(second, "int b() { return 1; }\n");
  WriteFile(other, "value = 1\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenFile(shell, first);
  WorkspaceShellTestAccess::OpenFile(shell, second);
  WorkspaceShellTestAccess::OpenFile(shell, other);
  WorkspaceShellTestAccess::ApplyEditorPreferencesToAllTabs(shell);

  const auto& cpp_first = WorkspaceShellTestAccess::FocusedGroupTabEditor(shell, 0)
                              .language_contract_view();
  const auto& cpp_second = WorkspaceShellTestAccess::FocusedGroupTabEditor(shell, 1)
                               .language_contract_view();
  const auto& python = WorkspaceShellTestAccess::FocusedGroupTabEditor(shell, 2)
                           .language_contract_view();
  Expect(&cpp_first == &cpp_second,
         "two .cpp tabs must reference the same shared contract view, not two copies");
  Expect(&cpp_first != &python, "a different language must resolve to a different view");
  Expect(!cpp_first.auto_close_pairs.empty(), "the shared C++ view still carries its pairs");

  Expect(cpp_first.auto_close_enabled, "auto-close starts enabled");
  Expect(ExecuteCommand(shell, "toggle-editor-auto-close"),
         "toggle-editor-auto-close should execute");
  const auto& cpp_first_after = WorkspaceShellTestAccess::FocusedGroupTabEditor(shell, 0)
                                    .language_contract_view();
  const auto& cpp_second_after = WorkspaceShellTestAccess::FocusedGroupTabEditor(shell, 1)
                                     .language_contract_view();
  Expect(!cpp_first_after.auto_close_enabled, "the toggle reaches the first tab");
  Expect(!cpp_second_after.auto_close_enabled, "the toggle reaches the second tab");
  Expect(&cpp_first_after == &cpp_second_after,
         "both .cpp tabs still share one view after the toggle rebuild");
}

void TestWorkspaceShellTabSizeChangeAppliesToAllTabsWithoutContractRebuild() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path first = root / "first.cpp";
  const std::filesystem::path second = root / "second.cpp";
  WriteFile(first, "int a() { return 0; }\n");
  WriteFile(second, "int b() { return 1; }\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenFile(shell, first);
  WorkspaceShellTestAccess::OpenFile(shell, second);

  Expect(ExecuteCommand(shell, "tab-size 7"), "tab-size command should update the preference");

  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).tab_size() == 7,
         "active tab picks up the new tab size");
  Expect(WorkspaceShellTestAccess::FocusedGroupTabEditor(shell, 0).tab_size() == 7,
         "background tab also picks up the new tab size via the runtime-setter pass");
}

void TestWorkspaceShellTabKeyIndentsMultiLineSelection() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "main.cpp";
  WriteFile(source, "alpha\nbeta\ngamma\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::OpenFile(shell, source);

  auto& viewport = WorkspaceShellTestAccess::ActiveEditor(shell);
  viewport.MoveCursorTo(0, 0);
  viewport.MoveCursorTo(2, 5, true);
  Expect(viewport.has_selection() && viewport.selection_range().has_value() &&
             viewport.selection_range()->start.line != viewport.selection_range()->end.line,
         "fixture should have a multi-line selection across three lines");

  Expect(SendKeyDown(shell, SDLK_TAB, SDL_KMOD_NONE),
         "Tab should be handled when a multi-line selection is active");
  const auto& after_indent = viewport.lines().Snapshot();
  Expect(after_indent.size() >= 3, "indent should preserve line count");
  Expect(after_indent[0].rfind("\t", 0) == 0 || after_indent[0].rfind("    ", 0) == 0,
         "first selected line should gain one indent unit on Tab");
  Expect(after_indent[1].rfind("\t", 0) == 0 || after_indent[1].rfind("    ", 0) == 0,
         "second selected line should gain one indent unit on Tab");
  Expect(after_indent[2].rfind("\t", 0) == 0 || after_indent[2].rfind("    ", 0) == 0,
         "third selected line should gain one indent unit on Tab");

  Expect(SendKeyDown(shell, SDLK_TAB, SDL_KMOD_SHIFT),
         "Shift+Tab should be handled when a multi-line selection is active");
  const auto& after_outdent = viewport.lines().Snapshot();
  Expect(after_outdent[0] == "alpha" && after_outdent[1] == "beta" &&
             after_outdent[2] == "gamma",
         "Shift+Tab should outdent the previously indented selection back to original");
}

void TestWorkspaceShellTabKeyOnSingleLineInsertsTabCharacter() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "main.cpp";
  WriteFile(source, "alpha\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::OpenFile(shell, source);

  auto& viewport = WorkspaceShellTestAccess::ActiveEditor(shell);
  viewport.MoveCursorTo(0, 0);
  Expect(SendKeyDown(shell, SDLK_TAB, SDL_KMOD_NONE),
         "Tab on no-selection single-line should be handled by InsertTab");
  const std::string& first = viewport.lines().front();
  Expect(first.rfind("\t", 0) == 0 || first.rfind("    ", 0) == 0 ||
             first.rfind("  ", 0) == 0,
         "InsertTab should prepend an indent unit at the caret");
}

void TestWorkspaceShellAddCursorAtNextMatchWalksForwardEachPress() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const auto file = root / "words.txt";
  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WriteFile(file, "foo foo foo foo\nbar foo\n");
  WorkspaceShellTestAccess::OpenFile(shell, file);
  auto& viewport = WorkspaceShellTestAccess::ActiveEditor(shell);
  viewport.MoveCursorTo(0, 1);

  Expect(ExecuteCommand(shell, "add-cursor-next-match"), "the first press dispatches");
  Expect(viewport.selection_range().has_value() && viewport.secondary_caret_range_view().empty(),
         "the first press selects the word under the caret");
  Expect(ExecuteCommand(shell, "add-cursor-next-match"), "the second press dispatches");
  Expect(viewport.secondary_caret_range_view().size() == 1, "the second press adds the next occurrence");
  Expect(ExecuteCommand(shell, "add-cursor-next-match"), "the third press dispatches");
  Expect(viewport.secondary_caret_range_view().size() == 2,
         "the third press adds the occurrence after the one the second press added");
  Expect(ExecuteCommand(shell, "add-cursor-next-match") && ExecuteCommand(shell, "add-cursor-next-match"),
         "two more presses dispatch");
  Expect(viewport.secondary_caret_range_view().size() == 4,
         "presses keep walking, wrapping onto the next line's occurrence");
  Expect(ExecuteCommand(shell, "add-cursor-next-match"), "a press with nothing left dispatches");
  Expect(viewport.secondary_caret_range_view().size() == 4,
         "once every occurrence is a caret a press adds nothing");

  viewport.InsertText("X");
  Expect(viewport.lines()[0] == "X X X X" && viewport.lines()[1] == "bar X",
         "typing replaces every occurrence the presses selected");
}

void TestWorkspaceShellAddCursorAtNextMatchCountsEveryOccurrence() {
  struct Case {
    const char* name;
    const char* content;
    std::size_t caret_line;
    std::size_t caret_column;
    const char* needle;
    std::size_t occurrences;
  };
  const std::vector<Case> cases = {
      {"one-per-line", "foo\nbar\nfoo\nfoo\n", 0, 1, "foo", 3},
      {"several-on-one-line", "foo foo foo\nfoo\n", 0, 0, "foo", 4},
      {"touching", "foofoofoo\n", 0, 0, "foofoofoo", 1},
      {"only-one", "alpha bravo\n", 0, 2, "alpha", 1},
      {"needle-is-the-whole-line", "x\ny\nx\n", 0, 0, "x", 2},
      {"caret-starts-on-the-last-one", "foo bar foo\n", 0, 9, "foo", 2},
      {"blank-lines-between", "foo\n\n\nfoo\n\nfoo\n", 3, 1, "foo", 3},
  };

  for (const Case& test_case : cases) {
    TemporaryDirectory temp_dir;
    const std::filesystem::path root = temp_dir.path() / "project";
    const auto file = root / "words.txt";
    WorkspaceShell shell;
    WorkspaceShellTestAccess::SetProjectRoot(shell, root);
    WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
    WriteFile(file, test_case.content);
    WorkspaceShellTestAccess::OpenFile(shell, file);
    auto& viewport = WorkspaceShellTestAccess::ActiveEditor(shell);
    viewport.MoveCursorTo(test_case.caret_line, test_case.caret_column);

    const std::string where = std::string(test_case.name) + ": ";
    const std::string needle = test_case.needle;

    for (std::size_t press = 1; press <= test_case.occurrences + 2; ++press) {
      Expect(ExecuteCommand(shell, "add-cursor-next-match"),
             where + "press " + std::to_string(press) + " should dispatch");

      std::vector<microide::editor::SelectionRange> selections;
      if (const auto primary = viewport.selection_range()) {
        selections.push_back(*primary);
      }
      for (const auto& caret : viewport.secondary_caret_range_view()) {
        if (!caret.selection_anchor.has_value()) continue;
        microide::editor::SelectionRange range{*caret.selection_anchor, caret.position};
        if (range.end.line < range.start.line ||
            (range.end.line == range.start.line && range.end.column < range.start.column)) {
          std::swap(range.start, range.end);
        }
        selections.push_back(range);
      }

      const std::size_t expected = std::min(press, test_case.occurrences);
      Expect(selections.size() == expected,
             where + "after " + std::to_string(press) + " press(es) there should be " +
                 std::to_string(expected) + " selection(s) for " +
                 std::to_string(test_case.occurrences) + " occurrence(s), got " +
                 std::to_string(selections.size()));
      if (selections.size() != expected) break;

      for (const auto& range : selections) {
        Expect(range.start.line == range.end.line,
               where + "a Ctrl+D selection never spans lines");
        const std::string_view line = viewport.lines().LineView(range.start.line);
        Expect(range.end.column <= line.size() &&
                   line.substr(range.start.column, range.end.column - range.start.column) == needle,
               where + "every selection spans exactly the needle \"" + needle + "\", got \"" +
                   std::string(line.substr(range.start.column,
                                           range.end.column - range.start.column)) +
                   "\" at line " + std::to_string(range.start.line) + " column " +
                   std::to_string(range.start.column));
      }
      std::sort(selections.begin(), selections.end(),
                [](const auto& a, const auto& b) {
                  return a.start.line < b.start.line ||
                         (a.start.line == b.start.line && a.start.column < b.start.column);
                });
      for (std::size_t i = 1; i < selections.size(); ++i) {
        const bool disjoint =
            selections[i - 1].end.line < selections[i].start.line ||
            (selections[i - 1].end.line == selections[i].start.line &&
             selections[i - 1].end.column <= selections[i].start.column);
        Expect(disjoint, where + "two selections overlap at line " +
                             std::to_string(selections[i].start.line) + " column " +
                             std::to_string(selections[i].start.column));
      }
    }
  }
}

void TestWorkspaceShellTextDragDoesNotArmWithSeveralCarets() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const auto file = root / "a.txt";
  WriteFile(file, "alpha bravo\ncharlie delta\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, file);
  WorkspaceShellTestAccess::RenderFrame(shell);

  auto& viewport = WorkspaceShellTestAccess::ActiveEditor(shell);
  const auto metrics = WorkspaceShellTestAccess::ActiveEditorMetrics(shell);
  const float char_width = WorkspaceShellTestAccess::TextCharWidth(shell);
  const float y = metrics.first_line_y + metrics.line_height * 0.5f;
  const float press_x = metrics.text_x + char_width * 2.5f;

  viewport.MoveCursorTo(0, 0);
  viewport.MoveCursorTo(0, 5, true);
  Expect(SendMouseDown(shell, press_x, y, SDL_BUTTON_LEFT), "the press is handled");
  Expect(WorkspaceShellTestAccess::InteractionState_(shell).text_drag !=
             microide::workspace::InteractionState::TextDragState::None,
         "a press inside a single selection arms the drag -- without this the negative "
         "case below would pass for the wrong reason");

  WorkspaceShellTestAccess::InteractionState_(shell).text_drag =
      microide::workspace::InteractionState::TextDragState::None;
  viewport.MoveCursorTo(0, 0);
  viewport.MoveCursorTo(0, 5, true);
  viewport.AddSecondaryCaretWithRange(microide::editor::SelectionRange{
      microide::editor::TextPosition{1, 0}, microide::editor::TextPosition{1, 7}});
  Expect(viewport.has_multiple_carets(), "two selections are in play");

  Expect(SendMouseDown(shell, press_x, y, SDL_BUTTON_LEFT), "the press is handled");
  Expect(WorkspaceShellTestAccess::InteractionState_(shell).text_drag ==
             microide::workspace::InteractionState::TextDragState::None,
         "a press inside a selection must NOT arm a drag while several carets exist: the "
         "gesture carries one source range and would move one selection and strand the rest");
}

void TestWorkspaceShellTextDragRefusesAStaleSourceRange() {
  const auto arm_and_release =
      [](const std::function<void(WorkspaceShell&, const std::filesystem::path&)>& disturb) {
    TemporaryDirectory temp_dir;
    const std::filesystem::path root = temp_dir.path() / "project";
    const auto file = root / "a.txt";
    const auto other = root / "b.txt";
    WriteFile(file, "alpha\nbravo\ncharlie\n");
    WriteFile(other, "one\ntwo\nthree\n");
    WorkspaceShell shell;
    WorkspaceShellTestAccess::SetProjectRoot(shell, root);
    WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
    WorkspaceShellTestAccess::OpenFile(shell, file);

    auto* viewport = WorkspaceShellTestAccess::ActiveEditorOrNull(shell);
    Expect(viewport != nullptr, "the fixture opens an editor tab");
    viewport->MoveCursorTo(0, 0);
    viewport->MoveCursorTo(0, 5, true);
    WorkspaceShellTestAccess::ArmTextDrag(
        shell,
        microide::editor::SelectionRange{microide::editor::TextPosition{0, 0},
                                         microide::editor::TextPosition{0, 5}},
        microide::editor::TextPosition{2, 0});

    disturb(shell, root);
    WorkspaceShellTestAccess::FinishTextDrag(shell, false);

    std::string out;
    if (const auto* buffer = WorkspaceShellTestAccess::EditorViewportForPath(shell, file);
        buffer != nullptr) {
      for (std::size_t i = 0; i < buffer->line_count(); ++i) {
        out += buffer->lines().LineView(i);
        out += '\n';
      }
    }
    return out;
  };

  const std::string moved = arm_and_release([](WorkspaceShell&, const std::filesystem::path&) {});
  Expect(moved.find("alpha") != std::string::npos && moved.rfind("alpha", 0) != 0,
         "the undisturbed drag should move `alpha` off line 0: " + moved);

  const std::string after_edit = arm_and_release([](WorkspaceShell& shell,
                                                    const std::filesystem::path&) {
    auto* viewport = WorkspaceShellTestAccess::ActiveEditorOrNull(shell);
    viewport->MoveCursorTo(0, 0);
    viewport->InsertText("XX\n");
  });
  Expect(after_edit.rfind("XX\nalpha\nbravo\ncharlie\n", 0) == 0,
         "a drag whose buffer changed under it must not move anything: " + after_edit);

  const std::string after_tab_switch = arm_and_release(
      [](WorkspaceShell& shell, const std::filesystem::path& root) {
        WorkspaceShellTestAccess::OpenFile(shell, root / "b.txt");
      });
  Expect(after_tab_switch.rfind("alpha\nbravo\ncharlie\n", 0) == 0,
         "a drag released over a different tab must not move anything: " + after_tab_switch);
}

void TestWorkspaceShellEditorScrollbarUsesVisualRows() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const auto file = root / "wide.txt";
  std::string content;
  for (int i = 0; i < 12; ++i) {
    content += std::string(300, static_cast<char>('a' + i));
    content += '\n';
  }
  WriteFile(file, content);

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::OpenFile(shell, file);
  auto* viewport = WorkspaceShellTestAccess::ActiveEditorOrNull(shell);
  Expect(viewport != nullptr, "the fixture opens an editor tab");
  viewport->SetSoftWrap(true);

  const SDL_FRect pane = microide::workspace::MakeRect(0.0f, 0.0f, 400.0f, 300.0f);
  const auto layout = WorkspaceShellTestAccess::ActiveEditorScrollLayout(shell, pane);
  Expect(layout.has_value(), "the pane has a scroll layout");
  Expect(viewport->visual_line_count() > viewport->line_count() * 2,
         "the fixture must genuinely wrap, got " +
             std::to_string(viewport->visual_line_count()) + " rows for " +
             std::to_string(viewport->line_count()) + " lines");

  const std::size_t visible_rows = static_cast<std::size_t>(std::max(1, layout->visible_rows));
  const std::size_t expected_max = viewport->visual_line_count() > visible_rows
                                       ? viewport->visual_line_count() - visible_rows
                                       : 0;
  Expect(static_cast<std::size_t>(layout->max_vertical_scroll) == expected_max,
         "the scrollbar's travel should span the wrapped rows: max " +
             std::to_string(layout->max_vertical_scroll) + ", expected " +
             std::to_string(expected_max));

  viewport->SetScrollLine(expected_max);
  const auto at_bottom = WorkspaceShellTestAccess::ActiveEditorScrollLayout(shell, pane);
  Expect(at_bottom.has_value() &&
             static_cast<std::size_t>(at_bottom->vertical_scroll) == expected_max,
         "a scroll to the last visual row must not be clamped back to the line count: " +
             std::to_string(at_bottom.has_value() ? at_bottom->vertical_scroll : 0));

  viewport->SetScrollLine(0);
  namespace autoscroll = microide::workspace::selection_autoscroll;
  autoscroll::Band band;
  band.rect = pane;
  band.first_line_y = pane.y;
  band.line_height = 14.0f;
  band.visible_rows = visible_rows;
  auto& interaction = WorkspaceShellTestAccess::InteractionState_(shell);
  for (int step = 0; step < 2000; ++step) {
    autoscroll::Arm(interaction, workspace::InteractionState::SelectionAutoscrollOwner::Text, band, pane.x + 10.0f,
                    pane.y + pane.h + 200.0f);
    autoscroll::Step(interaction, *viewport);
  }
  Expect(viewport->scroll_line() > viewport->line_count(),
         "autoscroll should walk past the logical line count into the wrapped rows, "
         "reached " +
             std::to_string(viewport->scroll_line()) + " of " +
             std::to_string(viewport->visual_line_count()) + " visual rows");
}

void TestWorkspaceShellEditorGestureEndsWhenItsBufferLeavesTheFront() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const auto first = root / "a.txt";
  const auto second = root / "b.txt";
  WriteFile(first, "aaaa\nbbbb\ncccc\n");
  WriteFile(second, "wwww\nxxxx\nyyyy\n");
  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::OpenFile(shell, first);

  WorkspaceShellTestAccess::SetTransientMouseSelecting(shell, true);
  WorkspaceShellTestAccess::SetTransientBoxSelecting(shell);
  WorkspaceShellTestAccess::SetEditorGestureViewportToActive(shell);
  Expect(WorkspaceShellTestAccess::TransientBoxSelecting(shell), "the gesture is armed");

  WorkspaceShellTestAccess::OpenFile(shell, second);
  const auto* second_viewport = WorkspaceShellTestAccess::EditorViewportForPath(shell, second);
  Expect(second_viewport != nullptr && second_viewport->line_count() >= 3,
         "the second file is open");

  SendMouseMotion(shell, 600.0f, 200.0f, SDL_BUTTON_LMASK);
  Expect(!WorkspaceShellTestAccess::TransientBoxSelecting(shell),
         "the box gesture must end rather than build a rectangle in a file it was "
         "never pointed at");
  Expect(WorkspaceShellTestAccess::TransientTextDragIsIdle(shell),
         "and the same check ends a text drag");
  const auto* still_second = WorkspaceShellTestAccess::EditorViewportForPath(shell, second);
  Expect(still_second != nullptr && !still_second->has_multiple_carets(),
         "no box carets were placed in the second file");
}

void TestWorkspaceShellColumnSelectReanchorsAfterAForeignCaretMove() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const auto file = root / "grid.txt";
  WriteFile(file, "aaaa\nbbbb\ncccc\ndddd\neeee\nffff\ngggg\n");
  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::OpenFile(shell, file);
  auto* viewport = WorkspaceShellTestAccess::ActiveEditorOrNull(shell);
  Expect(viewport != nullptr, "the fixture opens an editor tab");

  const auto step_down = [&]() {
    Expect(WorkspaceShellTestAccess::ExecuteAction(
               shell, WorkspaceShell::ActionId::ColumnSelectDown, {}),
           "the column-select step should dispatch");
  };

  viewport->MoveCursorTo(0, 2);
  step_down();
  step_down();
  viewport = WorkspaceShellTestAccess::ActiveEditorOrNull(shell);
  Expect(viewport->column_selection().active, "two steps leave the gesture armed");
  Expect(viewport->secondary_caret_range_view().size() == 2,
         "a box over lines 0-2 is the primary plus two secondaries, got " +
             std::to_string(viewport->secondary_caret_range_view().size()));

  viewport->MoveCursorTo(5, 2);
  Expect(!viewport->column_selection().active,
         "a caret move outside the gesture must end it, or the next step extends a "
         "box anchored where the caret no longer is");

  step_down();
  viewport = WorkspaceShellTestAccess::ActiveEditorOrNull(shell);
  Expect(viewport->secondary_caret_range_view().size() == 1,
         "the next step re-anchors at the new caret: lines 5-6 is one secondary, got " +
             std::to_string(viewport->secondary_caret_range_view().size()));
  for (const auto& caret : viewport->secondary_caret_range_view()) {
    Expect(caret.position.line >= 5,
           "no caret may be left up at the old anchor; found one on line " +
               std::to_string(caret.position.line));
  }

  step_down();
  viewport = WorkspaceShellTestAccess::ActiveEditorOrNull(shell);
  Expect(viewport->column_selection().active, "the gesture survives its own step");
  Expect(viewport->secondary_caret_range_view().size() == 2,
         "and it grew by one more line (5-7, the file's last), got " +
             std::to_string(viewport->secondary_caret_range_view().size()));
}

void TestWorkspaceShellNoActionEditsABackgroundBuffer() {
  const std::vector<WorkspaceShell::ActionId> skipped = {
      WorkspaceShell::ActionId::CloseActiveTab,   WorkspaceShell::ActionId::CloseOtherTabs,
      WorkspaceShell::ActionId::CloseTabsToRight, WorkspaceShell::ActionId::CloseTabsToLeft,
      WorkspaceShell::ActionId::CloseAllTabs,     WorkspaceShell::ActionId::CloseGroup,
      WorkspaceShell::ActionId::ProjectClose,     WorkspaceShell::ActionId::Quit,
      WorkspaceShell::ActionId::Open,             WorkspaceShell::ActionId::ProjectOpen,
  };
  const std::string background_text = "keep0();\nkeep1();\nkeep2();\nkeep3();\n";

  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const auto background = root / "background.cpp";
  const auto foreground = root / "foreground.cpp";
  WriteFile(background, background_text);
  WriteFile(foreground, "front0();\nfront1();\nfront2();\nfront3();\n");
  InitializeGitRepo(root);
  CommitAll(root, "background sweep fixture", "background sweep");

  for (const bool compare_in_front : {false, true}) {
    std::size_t mutating_actions = 0;
    for (const auto& spec : microide::workspace::WorkspaceCommandSpecs()) {
      if (std::find(skipped.begin(), skipped.end(), spec.id) != skipped.end()) {
        continue;
      }
      WriteFile(background, background_text);
      WriteFile(foreground, "front0();\nfront1();\nfront2();\nfront3();\n");
      WorkspaceShell shell;
      WorkspaceShellTestAccess::SetProjectRoot(shell, root);
      WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);

      WorkspaceShellTestAccess::OpenFile(shell, background);
      if (compare_in_front) {
        if (!WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, foreground, "HEAD",
                                                                 "HEAD")) {
          Expect(false, "the compare tab should open");
          return;
        }
      } else {
        WorkspaceShellTestAccess::OpenFile(shell, foreground);
      }

      const auto background_now = [&]() {
        std::string out;
        if (const auto* buffer =
                WorkspaceShellTestAccess::EditorViewportForPath(shell, background);
            buffer != nullptr) {
          for (std::size_t i = 0; i < buffer->line_count(); ++i) {
            out += buffer->lines().LineView(i);
            out += '\n';
          }
        }
        return out;
      };
      const std::string before_background = background_now();
      Expect(before_background.rfind(background_text, 0) == 0,
             "the background buffer starts as written");

      if (auto* front = WorkspaceShellTestAccess::ActiveEditorOrNull(shell); front != nullptr) {
        front->MoveCursorTo(1, 1);
        front->MoveCursorTo(2, 3, true);
      } else if (auto* compare = WorkspaceShellTestAccess::ActiveCompareOrNull(shell);
                 compare != nullptr) {
        compare->right_viewport.MoveCursorTo(1, 1);
        compare->right_viewport.MoveCursorTo(2, 3, true);
      }

      WorkspaceShellTestAccess::ExecuteAction(shell, spec.id, {});

      const std::string after_background = background_now();
      if (after_background.empty()) {
        continue;
      }
      Expect(after_background == before_background,
             std::string("`") + std::string(spec.command_name) +
                 "` edited a buffer that was not in front (" +
                 (compare_in_front ? "compare" : "editor") + " tab focused): " +
                 after_background);
      if (after_background != before_background) {
        return;
      }
      ++mutating_actions;
    }
    Expect(mutating_actions > 100,
           "the sweep must have run essentially the whole registry, ran " +
               std::to_string(mutating_actions));
  }
}

void TestWorkspaceShellNoActionNoticesSoftWrap() {
  const std::vector<WorkspaceShell::ActionId> skipped = {
      WorkspaceShell::ActionId::CloseActiveTab,   WorkspaceShell::ActionId::CloseOtherTabs,
      WorkspaceShell::ActionId::CloseTabsToRight, WorkspaceShell::ActionId::CloseTabsToLeft,
      WorkspaceShell::ActionId::CloseAllTabs,     WorkspaceShell::ActionId::CloseGroup,
      WorkspaceShell::ActionId::ProjectClose,     WorkspaceShell::ActionId::Quit,
      WorkspaceShell::ActionId::Open,             WorkspaceShell::ActionId::ProjectOpen,
      WorkspaceShell::ActionId::Wrap,
  };
  const std::string content =
      "  aaa(" + std::string(200, 'a') + ");\n"
      "  bbb(" + std::string(200, 'b') + ");\n"
      "  ccc(" + std::string(200, 'c') + ");\n"
      "  ddd();\n";

  std::size_t compared_actions = 0;
  std::size_t wrapped_checks = 0;
  for (const auto& spec : microide::workspace::WorkspaceCommandSpecs()) {
    if (std::find(skipped.begin(), skipped.end(), spec.id) != skipped.end()) {
      continue;
    }
    std::array<std::string, 2> results;
    bool usable = true;
    for (int wrapped = 0; wrapped < 2 && usable; ++wrapped) {
      TemporaryDirectory temp_dir;
      const std::filesystem::path root = temp_dir.path() / "project";
      const auto file = root / "code.cpp";
      WriteFile(file, content);
      WorkspaceShell shell;
      WorkspaceShellTestAccess::SetProjectRoot(shell, root);
      WorkspaceShellTestAccess::SetWindowSize(shell, 640, 480);
      Expect(WorkspaceShellTestAccess::SetSettingValueTransient(
                 shell, "editor.wrap", wrapped != 0 ? "word" : "off"),
             "the wrap setting should be settable");
      WorkspaceShellTestAccess::OpenFile(shell, file);
      auto* viewport = WorkspaceShellTestAccess::ActiveEditorOrNull(shell);
      if (viewport == nullptr) {
        usable = false;
        break;
      }
      if (wrapped != 0) {
        if (viewport->visual_line_count() <= viewport->line_count()) {
          usable = false;
          break;
        }
        ++wrapped_checks;
      }
      viewport->MoveCursorTo(1, 4);
      viewport->MoveCursorTo(2, 6, true);
      WorkspaceShellTestAccess::ExecuteAction(shell, spec.id, {});
      viewport = WorkspaceShellTestAccess::ActiveEditorOrNull(shell);
      if (viewport == nullptr) {
        usable = false;
        break;
      }
      std::string& out = results[static_cast<std::size_t>(wrapped)];
      for (std::size_t i = 0; i < viewport->line_count(); ++i) {
        out += viewport->lines().LineView(i);
        out += '\n';
      }
    }
    if (!usable) {
      continue;
    }
    ++compared_actions;
    Expect(results[0] == results[1],
           std::string("`") + std::string(spec.command_name) +
               "` produced different text with soft wrap on. Wrap is a VIEW "
               "setting; no action may notice it.\n  unwrapped: " + results[0] +
               "\n  wrapped:   " + results[1]);
    if (results[0] != results[1]) {
      return;
    }
  }
  Expect(compared_actions > 100 && wrapped_checks > 100,
         "the sweep must have compared essentially the whole registry against a "
         "genuinely wrapped pane, compared " +
             std::to_string(compared_actions) + " with " + std::to_string(wrapped_checks) +
             " wrapped passes");
}

void TestWorkspaceShellCollapsedFoldExpandingVerbsAreExactlyTheDocumentedSet() {
  const std::vector<WorkspaceShell::ActionId> skipped = {
      WorkspaceShell::ActionId::CloseActiveTab,   WorkspaceShell::ActionId::CloseOtherTabs,
      WorkspaceShell::ActionId::CloseTabsToRight, WorkspaceShell::ActionId::CloseTabsToLeft,
      WorkspaceShell::ActionId::CloseAllTabs,     WorkspaceShell::ActionId::CloseGroup,
      WorkspaceShell::ActionId::ProjectClose,     WorkspaceShell::ActionId::Quit,
      WorkspaceShell::ActionId::Open,             WorkspaceShell::ActionId::ProjectOpen,
      WorkspaceShell::ActionId::Fold,             WorkspaceShell::ActionId::Unfold,
      WorkspaceShell::ActionId::ToggleFoldAtCursor, WorkspaceShell::ActionId::FoldAll,
      WorkspaceShell::ActionId::UnfoldAll,
  };
  const std::vector<std::string> expected_expanding = {
      "copy-line-down", "copy-line-up", "cut", "delete-line", "move-line-down", "move-line-up",
  };
  const std::string content = "head();\nvoid f() {\n  body1();\n  body2();\n}\ntail();\n";

  std::vector<std::string> observed_expanding;
  std::size_t compared = 0;
  for (const auto& spec : microide::workspace::WorkspaceCommandSpecs()) {
    if (std::find(skipped.begin(), skipped.end(), spec.id) != skipped.end()) {
      continue;
    }
    std::array<std::string, 2> results;
    bool usable = true;
    for (int folded = 0; folded < 2 && usable; ++folded) {
      TemporaryDirectory temp_dir;
      const std::filesystem::path root = temp_dir.path() / "project";
      const auto file = root / "code.cpp";
      WriteFile(file, content);
      WorkspaceShell shell;
      WorkspaceShellTestAccess::SetProjectRoot(shell, root);
      WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
      WorkspaceShellTestAccess::OpenFile(shell, file);
      auto* viewport = WorkspaceShellTestAccess::ActiveEditorOrNull(shell);
      if (viewport == nullptr) {
        usable = false;
        break;
      }
      if (folded != 0) {
        auto* model = WorkspaceShellTestAccess::EnsureActiveFoldingModelFresh(shell);
        if (model == nullptr || !model->Collapse(1)) {
          Expect(false, "the fixture's brace fold should collapse");
          return;
        }
      }
      viewport->MoveCursorTo(1, 0);
      WorkspaceShellTestAccess::ExecuteAction(shell, spec.id, {});
      viewport = WorkspaceShellTestAccess::ActiveEditorOrNull(shell);
      if (viewport == nullptr) {
        usable = false;
        break;
      }
      std::string& out = results[static_cast<std::size_t>(folded)];
      for (std::size_t i = 0; i < viewport->line_count(); ++i) {
        out += viewport->lines().LineView(i);
        out += '\n';
      }
    }
    if (!usable) {
      continue;
    }
    ++compared;
    if (results[0] != results[1]) {
      observed_expanding.emplace_back(spec.command_name);
    }
  }
  std::sort(observed_expanding.begin(), observed_expanding.end());

  const auto join = [](const std::vector<std::string>& names) {
    std::string out;
    for (const auto& name : names) {
      if (!out.empty()) out += ", ";
      out += name;
    }
    return out.empty() ? std::string("<none>") : out;
  };
  Expect(observed_expanding == expected_expanding,
         "the verbs that treat a collapsed fold as one line should be exactly the set "
         "ResolveLineRanges documents.\n  expected: " +
             join(expected_expanding) + "\n  observed: " + join(observed_expanding));
  Expect(compared > 100 && !observed_expanding.empty(),
         "the sweep must have compared the whole registry and seen the fold matter, "
         "compared " +
             std::to_string(compared) + " with " + std::to_string(observed_expanding.size()) +
             " expanding");
}

void TestWorkspaceShellNoActionEditsAnUnfocusedSplitGroup() {
  const std::vector<WorkspaceShell::ActionId> skipped = {
      WorkspaceShell::ActionId::CloseActiveTab,   WorkspaceShell::ActionId::CloseOtherTabs,
      WorkspaceShell::ActionId::CloseTabsToRight, WorkspaceShell::ActionId::CloseTabsToLeft,
      WorkspaceShell::ActionId::CloseAllTabs,     WorkspaceShell::ActionId::CloseGroup,
      WorkspaceShell::ActionId::ProjectClose,     WorkspaceShell::ActionId::Quit,
      WorkspaceShell::ActionId::Open,             WorkspaceShell::ActionId::ProjectOpen,
  };
  const std::string other_text = "other0();\nother1();\nother2();\n";

  std::size_t swept = 0;
  for (const auto& spec : microide::workspace::WorkspaceCommandSpecs()) {
    if (std::find(skipped.begin(), skipped.end(), spec.id) != skipped.end()) {
      continue;
    }
    TemporaryDirectory temp_dir;
    const std::filesystem::path root = temp_dir.path() / "project";
    const auto other = root / "other.cpp";
    const auto focused = root / "focused.cpp";
    WriteFile(other, other_text);
    WriteFile(focused, "focus0();\nfocus1();\nfocus2();\n");

    WorkspaceShell shell;
    WorkspaceShellTestAccess::SetProjectRoot(shell, root);
    WorkspaceShellTestAccess::SetWindowSize(shell, 1600, 900);
    WorkspaceShellTestAccess::OpenFile(shell, other);
    if (!WorkspaceShellTestAccess::SplitEditorGroup(
            shell, microide::workspace::EditorSplitOrientation::Vertical)) {
      Expect(false, "the fixture should split the editor area");
      return;
    }
    WorkspaceShellTestAccess::OpenFile(shell, focused);
    if (WorkspaceShellTestAccess::EditorGroupCount(shell) < 2) {
      Expect(false, "the fixture should have two groups");
      return;
    }

    const auto other_now = [&]() {
      std::string out;
      if (const auto* buffer = WorkspaceShellTestAccess::EditorViewportForPath(shell, other);
          buffer != nullptr) {
        for (std::size_t i = 0; i < buffer->line_count(); ++i) {
          out += buffer->lines().LineView(i);
          out += '\n';
        }
      }
      return out;
    };
    const std::string before = other_now();
    Expect(before.rfind(other_text, 0) == 0,
           "the unfocused group's buffer starts as written, got: " + before);

    if (auto* front = WorkspaceShellTestAccess::ActiveEditorOrNull(shell); front != nullptr) {
      front->MoveCursorTo(1, 1);
      front->MoveCursorTo(2, 3, true);
    }

    WorkspaceShellTestAccess::ExecuteAction(shell, spec.id, {});

    const std::string after = other_now();
    if (after.empty()) {
      continue;
    }
    ++swept;
    Expect(after == before,
           std::string("`") + std::string(spec.command_name) +
               "` edited the buffer in the UNFOCUSED split group: " + after);
    if (after != before) {
      return;
    }
  }
  Expect(swept > 100,
         "the sweep must have run essentially the whole registry against a real "
         "split, ran " +
             std::to_string(swept));
}

void TestWorkspaceShellAddCursorChordsAreBound() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const auto file = root / "grid.txt";
  WriteFile(file, "aaaa\nbbbb\ncccc\ndddd\n");
  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::OpenFile(shell, file);
  auto* viewport = WorkspaceShellTestAccess::ActiveEditorOrNull(shell);
  Expect(viewport != nullptr, "the fixture opens an editor tab");
  viewport->MoveCursorTo(0, 2);

  const auto ctrl_alt = static_cast<SDL_Keymod>(SDL_KMOD_CTRL | SDL_KMOD_ALT);
  Expect(SendKeyDown(shell, SDLK_DOWN, ctrl_alt), "Ctrl+Alt+Down is bound");
  Expect(SendKeyDown(shell, SDLK_DOWN, ctrl_alt), "and again");
  viewport = WorkspaceShellTestAccess::ActiveEditorOrNull(shell);
  Expect(viewport->secondary_caret_range_view().size() == 2,
         "two presses leave three carets, got " +
             std::to_string(viewport->secondary_caret_range_view().size() + 1));
  Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "X"), "typing is handled");
  viewport = WorkspaceShellTestAccess::ActiveEditorOrNull(shell);
  Expect(std::string(viewport->lines().LineView(0)) == "aaXaa" &&
             std::string(viewport->lines().LineView(1)) == "bbXbb" &&
             std::string(viewport->lines().LineView(2)) == "ccXcc" &&
             std::string(viewport->lines().LineView(3)) == "dddd",
         std::string("every caret in the column typed: ") +
             std::string(viewport->lines().LineView(0)) + "/" +
             std::string(viewport->lines().LineView(2)));

  Expect(SendKeyDown(shell, SDLK_UP, ctrl_alt), "Ctrl+Alt+Up is bound");

  viewport = WorkspaceShellTestAccess::ActiveEditorOrNull(shell);
  viewport->ClearSecondaryCarets();
  viewport->MoveCursorTo(0, 0);
  viewport->MoveCursorTo(2, 1, true);
  Expect(SendKeyDown(shell, SDLK_I, static_cast<SDL_Keymod>(SDL_KMOD_SHIFT | SDL_KMOD_ALT)),
         "Shift+Alt+I is bound");
  viewport = WorkspaceShellTestAccess::ActiveEditorOrNull(shell);
  Expect(viewport->secondary_caret_range_view().size() == 2,
         "three lines touched, three carets, got " +
             std::to_string(viewport->secondary_caret_range_view().size() + 1));
  Expect(WorkspaceShellTestAccess::HandleTextInput(shell, ";"), "typing is handled");
  viewport = WorkspaceShellTestAccess::ActiveEditorOrNull(shell);
  Expect(std::string(viewport->lines().LineView(0)).back() == ';' &&
             std::string(viewport->lines().LineView(1)).back() == ';' &&
             std::string(viewport->lines().LineView(2)).back() == ';',
         "each line gained a character at its END");
}

void TestWorkspaceShellForeignEditEndsTheSnippetSession() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const auto file = root / "code.cpp";
  WriteFile(file, "aaa();\nbbb();\n");
  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::OpenFile(shell, file);
  auto* viewport = WorkspaceShellTestAccess::ActiveEditorOrNull(shell);
  Expect(viewport != nullptr, "the fixture opens an editor tab");
  viewport->MoveCursorTo(1, 5);
  Expect(WorkspaceShellTestAccess::PerfExpandSnippetAtCaret(shell, "for(${1:i};;)$0"),
         "the snippet should expand");
  Expect(WorkspaceShellTestAccess::SnippetSessionActive(shell),
         "the session is live right after expansion");
  const std::string expanded = std::string(viewport->lines().LineView(1));
  Expect(expanded.find("for(i;;)") != std::string::npos,
         "the placeholder text is on line 1: " + expanded);

  Expect(WorkspaceShellTestAccess::ExecuteAction(shell, WorkspaceShell::ActionId::MoveLineUp, {}),
         "move-line-up should dispatch");
  viewport = WorkspaceShellTestAccess::ActiveEditorOrNull(shell);
  Expect(std::string(viewport->lines().LineView(0)) == expanded,
         "the snippet's line moved to line 0: " + std::string(viewport->lines().LineView(0)));
  Expect(!WorkspaceShellTestAccess::SnippetSessionActive(shell),
         "an edit the engine did not make must end the session rather than leave it "
         "pointing at moved text");

  const std::size_t caret_line = viewport->cursor_line();
  Expect(SendKeyDown(shell, SDLK_TAB, SDL_KMOD_NONE), "Tab is handled");
  viewport = WorkspaceShellTestAccess::ActiveEditorOrNull(shell);
  Expect(viewport->cursor_line() == caret_line,
         "Tab must not jump to a stale placeholder; it stayed on line " +
             std::to_string(caret_line) + " but landed on " +
             std::to_string(viewport->cursor_line()));
  Expect(!viewport->selection_range().has_value(),
         "and it selected nothing, where a stale jump would have selected the text "
         "that inherited the placeholder's coordinates");
}

void TestWorkspaceShellTypingInAPlaceholderKeepsTheSnippetSession() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const auto file = root / "code.cpp";
  WriteFile(file, "aaa();\n");
  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::OpenFile(shell, file);
  auto* viewport = WorkspaceShellTestAccess::ActiveEditorOrNull(shell);
  viewport->MoveCursorTo(0, 6);
  Expect(WorkspaceShellTestAccess::PerfExpandSnippetAtCaret(shell, "${1:name} = ${1:name};$0"),
         "the mirrored snippet should expand");
  Expect(WorkspaceShellTestAccess::SnippetSessionActive(shell), "the session is live");

  Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "z"), "typing is handled");
  Expect(WorkspaceShellTestAccess::SnippetSessionActive(shell),
         "typing inside the placeholder is the engine's own edit and must keep the "
         "session alive");
  viewport = WorkspaceShellTestAccess::ActiveEditorOrNull(shell);
  const std::string line = std::string(viewport->lines().LineView(0));
  Expect(line.find("z = z;") != std::string::npos,
         "and the mirror still follows it: " + line);
}

void TestWorkspaceShellABackgroundBufferEditKeepsItsBreakpointsOnTheirLine() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path a = root / "a.py";
  const std::filesystem::path b = root / "b.py";
  WriteFile(a, "alpha\nbeta\ngamma\ndelta\n");
  WriteFile(b, "other\n");
  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenFile(shell, a);
  WorkspaceShellTestAccess::OpenFileInNewTab(shell, b);
  WorkspaceShellTestAccess::ToggleBreakpointForTest(shell, a, 2);
  auto& store = WorkspaceShellTestAccess::BreakpointStore(shell);
  Expect(store.HasBreakpoint(a, 2), "the breakpoint is on gamma");
  Expect(WorkspaceShellTestAccess::ApplyLspWorkspaceEdit(
             shell, {microide::workspace::CodeActionEdit{.path = a, .range = {{0, 0}, {0, 0}}, .new_text = "import x\n"}}),
         "the workspace edit applies to the background buffer");
  Expect(store.HasBreakpoint(a, 3) && !store.HasBreakpoint(a, 2),
         "the breakpoint moved with gamma to line 3");
  WorkspaceShellTestAccess::ActivateTab(shell, 0);
  WorkspaceShellTestAccess::RenderFrame(shell);
  Expect(store.HasBreakpoint(a, 3), "still on gamma after the file comes to the front");
}

void TestWorkspaceShellEveryActionKeepsBreakpointsOnTheirLine() {
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
      WorkspaceShell::ActionId::BreakpointToggle,
      WorkspaceShell::ActionId::BreakpointSet,
      WorkspaceShell::ActionId::BreakpointRemove,
      WorkspaceShell::ActionId::BreakpointClear,
      WorkspaceShell::ActionId::DebugBreakpointToggleEnabled,
      WorkspaceShell::ActionId::DebugBreakpointRemove,
  };
  const std::string marked = "marked_statement();";

  std::size_t mutating_actions = 0;
  for (const auto& spec : microide::workspace::WorkspaceCommandSpecs()) {
    if (std::find(skipped.begin(), skipped.end(), spec.id) != skipped.end()) {
      continue;
    }
    TemporaryDirectory temp_dir;
    const std::filesystem::path root = temp_dir.path() / "project";
    const auto file = root / "code.cpp";
    WriteFile(file, "aaa();\nbbb();\nccc();\n" + marked + "\n");
    WorkspaceShell shell;
    WorkspaceShellTestAccess::SetProjectRoot(shell, root);
    WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
    WorkspaceShellTestAccess::OpenFile(shell, file);
    auto& breakpoints = WorkspaceShellTestAccess::BreakpointStore(shell);

    const auto editor = [&shell]() { return WorkspaceShellTestAccess::ActiveEditorOrNull(shell); };
    const auto line_at = [&editor](std::size_t line) {
      const auto* viewport = editor();
      return viewport != nullptr && line < viewport->line_count()
                 ? std::string(viewport->lines().LineView(line))
                 : std::string("<past end>");
    };
    const auto line_count = [&editor]() {
      const auto* viewport = editor();
      return viewport != nullptr ? viewport->line_count() : std::size_t{0};
    };
    Expect(editor() != nullptr, "the fixture opens an editor tab");
    std::size_t marked_line = line_count();
    for (std::size_t i = 0; i < line_count(); ++i) {
      if (line_at(i) == marked) {
        marked_line = i;
        break;
      }
    }
    Expect(marked_line < line_count(), "the fixture contains the marked line");
    breakpoints.Set(file, marked_line);
    Expect(breakpoints.HasBreakpoint(file, marked_line), "the breakpoint is set");

    editor()->MoveCursorTo(0, 0);
    editor()->MoveCursorTo(0, 2, true);

    const auto joined = [&]() {
      std::string out;
      for (std::size_t i = 0; i < line_count(); ++i) {
        out += line_at(i);
        out += '\n';
      }
      return out;
    };
    const std::string before = joined();

    WorkspaceShellTestAccess::ExecuteAction(shell, spec.id, {});

    if (editor() == nullptr) {
      continue;
    }
    const std::string after = joined();
    if (after == before) {
      continue;
    }
    ++mutating_actions;

    std::size_t moved_to = line_count();
    for (std::size_t i = 0; i < line_count(); ++i) {
      if (line_at(i) == marked) {
        moved_to = i;
        break;
      }
    }
    if (moved_to >= line_count()) {
      continue;
    }
    Expect(breakpoints.HasBreakpoint(file, moved_to),
           std::string("`") + std::string(spec.command_name) + "` moved \"" + marked +
               "\" to line " + std::to_string(moved_to) +
               " and left the breakpoint behind: it is on line(s) " +
               [&] {
                 std::string lines;
                 if (const auto* list = breakpoints.FindByPath(file)) {
                   for (const auto& breakpoint : *list) {
                     lines += std::to_string(breakpoint.line) + " (\"" +
                              line_at(breakpoint.line) + "\") ";
                   }
                 }
                 return lines.empty() ? std::string("<none>") : lines;
               }());
  }
  Expect(mutating_actions >= 8,
         "the sweep must have found actions that actually edit the buffer, found only " +
             std::to_string(mutating_actions));
}

void TestWorkspaceShellEmptySelectionCopyPastesAsAWholeLine() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const auto file = root / "lines.txt";
  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WriteFile(file, "one\ntwo\n");
  WorkspaceShellTestAccess::OpenFile(shell, file);
  auto clipboard = std::make_shared<std::string>();
  WorkspaceShellTestAccess::SetClipboardTextReader(
      shell, [clipboard]() -> std::optional<std::string> { return *clipboard; });
  WorkspaceShellTestAccess::SetClipboardTextWriter(shell, [clipboard](std::string_view text) {
    clipboard->assign(text);
    return true;
  });
  auto& viewport = WorkspaceShellTestAccess::ActiveEditor(shell);

  viewport.MoveCursorTo(0, 1);
  Expect(RunCommandLine(shell, "copy"), "copy with no selection dispatches");
  Expect(*clipboard == "one\n", "it copies the whole line, newline included");
  viewport.MoveCursorTo(1, 1);
  Expect(RunCommandLine(shell, "paste"), "paste dispatches");
  Expect(viewport.lines().Snapshot() == std::vector<std::string>{"one", "one", "two", ""},
         "the line lands above the caret's line, not inside it");
  Expect(viewport.cursor_line() == 2 && viewport.cursor_column() == 1,
         "the caret stays where it was in its line, one line down (VS Code), at " +
             std::to_string(viewport.cursor_line()) + ":" + std::to_string(viewport.cursor_column()));

  viewport.MoveCursorTo(0, 0);
  viewport.SelectWordAtCursor();
  Expect(RunCommandLine(shell, "copy"), "a selection copy dispatches");
  Expect(*clipboard == "one", "the selection is what was copied");
  viewport.MoveCursorTo(2, 1);
  Expect(RunCommandLine(shell, "paste"), "paste dispatches");
  Expect(viewport.lines()[2] == "tonewo", "a selection copy pastes at the caret");

  viewport.MoveCursorTo(0, 2);
  Expect(RunCommandLine(shell, "cut"), "cut with no selection dispatches");
  Expect(*clipboard == "one\n" && viewport.lines()[0] == "one",
         "cut takes the whole line off the buffer and onto the clipboard");
  viewport.MoveCursorTo(1, 3);
  Expect(RunCommandLine(shell, "paste"), "paste dispatches");
  Expect(viewport.lines().Snapshot() == std::vector<std::string>{"one", "one", "tonewo", ""},
         "the cut line comes back above the caret's line");

  *clipboard = "zzz\n";
  viewport.MoveCursorTo(0, 1);
  Expect(RunCommandLine(shell, "paste"), "paste dispatches");
  Expect(viewport.lines()[0] == "ozzz" && viewport.lines()[1] == "ne",
         "text the editor did not line-copy pastes at the caret");
}

void TestWorkspaceShellAddCursorAtAllMatchesFromFindWidgetFocusesTheEditor() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const auto file = root / "words.txt";
  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WriteFile(file, "foo x foo\nfoo\n");
  WorkspaceShellTestAccess::OpenFile(shell, file);
  auto& viewport = WorkspaceShellTestAccess::ActiveEditor(shell);

  Expect(RunCommandLine(shell, "search foo"), "the find widget opens on a query");
  Expect(WorkspaceShellTestAccess::OverlayVisible(shell) &&
             WorkspaceShellTestAccess::BufferSearchSurfaceFocused(shell),
         "the widget has focus, as after Ctrl+F");
  Expect(RunCommandLine(shell, "add-cursor-all-matches"), "the chord dispatches from the widget");
  Expect(viewport.secondary_caret_range_view().size() == 2, "every occurrence is a caret");
  Expect(WorkspaceShellTestAccess::FocusIsEditor(shell), "and the editor holds focus");
  Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "bar"), "typing is accepted");
  Expect(viewport.lines()[0] == "bar x bar" && viewport.lines()[1] == "bar",
         "the keystroke replaces every occurrence, not the search query");
}

void TestWorkspaceShellPasteSpreadsOneLinePerCaret() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const auto file = root / "x.txt";
  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WriteFile(file, "a\nb\n");
  WorkspaceShellTestAccess::OpenFile(shell, file);
  WorkspaceShellTestAccess::SetClipboardTextReader(
      shell, []() -> std::optional<std::string> { return std::string("1\n2\n"); });
  auto& viewport = WorkspaceShellTestAccess::ActiveEditor(shell);
  viewport.MoveCursorTo(0, 0);
  viewport.SetSecondaryCarets({editor::TextPosition{1, 0}});
  Expect(viewport.has_multiple_carets(), "two carets");
  Expect(RunCommandLine(shell, "paste"), "paste dispatches");
  Expect(viewport.lines().Snapshot() == std::vector<std::string>{"1a", "2b", ""},
         "each caret receives its own line of the clipboard");

  WorkspaceShellTestAccess::SetClipboardTextReader(
      shell, []() -> std::optional<std::string> { return std::string("Z"); });
  viewport.MoveCursorTo(0, 0);
  viewport.SetSecondaryCarets({editor::TextPosition{1, 0}});
  Expect(RunCommandLine(shell, "paste"), "paste dispatches");
  Expect(viewport.lines().Snapshot() == std::vector<std::string>{"Z1a", "Z2b", ""},
         "a single-line payload is inserted at every caret");
}

void TestWorkspaceShellSaveAsAndBuffersForPathsThatDoNotExistYet() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  std::filesystem::create_directories(root);
  WriteFile(root / "exists.txt", "keep\n");
  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  const auto content = [](const std::filesystem::path& path) {
    std::string text = ReadFile(path);
    while (!text.empty() && text.back() == '\n') text.pop_back();
    return text;
  };

  Expect(RunCommandLine(shell, "tab"), "an untitled tab opens");
  auto& untitled = WorkspaceShellTestAccess::ActiveEditor(shell);
  untitled.InsertText("hello");
  Expect(!RunCommandLine(shell, "save"),
         "save with no path on an untitled buffer is refused from the command line");
  Expect(RunCommandLine(shell, "save notes/new.txt"), "save <path> names the buffer");
  Expect(content(root / "notes" / "new.txt") == "hello", "and writes it, creating the directory");
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).path() == (root / "notes" / "new.txt") &&
             !WorkspaceShellTestAccess::ActiveEditor(shell).dirty(),
         "the tab is bound to the path and clean");
  Expect(!RunCommandLine(shell, "save exists.txt"), "Save As onto an existing file is refused");
  Expect(ReadFile(root / "exists.txt") == "keep\n", "and the existing file is untouched");

  Expect(RunCommandLine(shell, "tab later.txt"), "a path that does not exist yet opens");
  auto& later = WorkspaceShellTestAccess::ActiveEditor(shell);
  Expect(later.path() == root / "later.txt" && !std::filesystem::exists(root / "later.txt"),
         "as an empty buffer bound to the path, nothing on disk yet");
  Expect(RunCommandLine(shell, "save"), "save creates it");
  Expect(std::filesystem::exists(root / "later.txt") && content(root / "later.txt").empty(),
         "empty");
  later.InsertText("x");
  Expect(RunCommandLine(shell, "save") && content(root / "later.txt") == "x", "and keeps saving");

  Expect(RunCommandLine(shell, "open deep/dir/n.txt"), "open does the same");
  auto& deep = WorkspaceShellTestAccess::ActiveEditor(shell);
  Expect(deep.path() == root / "deep" / "dir" / "n.txt", "bound to the missing path");
  deep.InsertText("z");
  Expect(RunCommandLine(shell, "save") && content(root / "deep" / "dir" / "n.txt") == "z",
         "created with its directories on save");
  Expect(RunCommandLine(shell, "open exists.txt") &&
             WorkspaceShellTestAccess::ActiveEditor(shell).lines()[0] == "keep",
         "an existing path still opens normally");
}

void TestWorkspaceShellShapingCapabilityTogglesGateExecutorCommandsAndIndentTab() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const auto comment_file = root / "comment.cpp";
  const auto lines_file = root / "lines.cpp";
  const auto sort_file = root / "sort.cpp";

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);

  WriteFile(comment_file, "hello\n");
  Expect(WorkspaceShellTestAccess::SetSettingValue(shell, "editor.shaping.toggle_comment.enabled", "false"),
         "fixture should disable toggle-comment shaping");
  WorkspaceShellTestAccess::OpenFile(shell, comment_file);
  auto& comment_vp = WorkspaceShellTestAccess::ActiveEditor(shell);
  comment_vp.MoveCursorTo(0, 0);
  Expect(ExecuteCommand(shell, "toggle-line-comment"),
         "toggle-line-comment should still dispatch when the capability toggle is off");
  Expect(comment_vp.lines()[0] == "hello",
         "toggle-line-comment must not mutate buffer while editor.shaping.toggle_comment.enabled=false");

  Expect(WorkspaceShellTestAccess::SetSettingValue(shell, "editor.shaping.toggle_comment.enabled", "true"),
         "fixture should re-enable toggle-comment shaping");
  Expect(ExecuteCommand(shell, "toggle-line-comment"),
         "toggle-line-comment should run once shaping is enabled again");
  Expect(comment_vp.lines()[0].rfind("//", 0) == 0,
         "toggle-line-comment should insert the language line marker when enabled");

  WriteFile(lines_file, "alpha\nbeta\n");
  WorkspaceShellTestAccess::OpenFile(shell, lines_file);
  auto& line_vp = WorkspaceShellTestAccess::ActiveEditor(shell);
  line_vp.MoveCursorTo(1, 0);
  Expect(WorkspaceShellTestAccess::SetSettingValue(shell, "editor.shaping.line_ops.enabled", "false"),
         "fixture should disable line-operation shaping");
  Expect(ExecuteCommand(shell, "move-line-up"),
         "move-line-up should still dispatch when shaping line ops are disabled");
  Expect(line_vp.lines()[0] == "alpha" && line_vp.lines()[1] == "beta",
         "move-line-up must noop while editor.shaping.line_ops.enabled=false");

  line_vp.MoveCursorTo(0, 0);
  line_vp.MoveCursorTo(1, 4, true);
  Expect(SendKeyDown(shell, SDLK_TAB, SDL_KMOD_NONE),
         "Tab should be handled for a multi-line selection");
  Expect(line_vp.lines()[0] == "alpha" && line_vp.lines()[1] == "beta",
         "multi-line Tab indent must noop while editor.shaping.line_ops.enabled=false");

  Expect(WorkspaceShellTestAccess::SetSettingValue(shell, "editor.shaping.line_ops.enabled", "true"),
         "fixture should re-enable line-operation shaping");
  Expect(SendKeyDown(shell, SDLK_TAB, SDL_KMOD_NONE),
         "Tab should indent after line ops are re-enabled");
  const auto& indented = line_vp.lines().Snapshot();
  Expect(indented.size() >= 2, "indent should keep two lines");
  Expect((indented[0].rfind("\t", 0) == 0) || (indented[0].rfind("    ", 0) == 0) ||
             (indented[0].rfind("  ", 0) == 0),
         "first line should gain a leading indent unit");
  Expect((indented[1].rfind("\t", 0) == 0) || (indented[1].rfind("    ", 0) == 0) ||
             (indented[1].rfind("  ", 0) == 0),
         "second line should gain a leading indent unit");

  WriteFile(sort_file, "banana\napple\n");
  WorkspaceShellTestAccess::OpenFile(shell, sort_file);
  auto& sort_vp = WorkspaceShellTestAccess::ActiveEditor(shell);
  sort_vp.MoveCursorTo(0, 0);
  sort_vp.MoveCursorTo(1, 5, true);
  Expect(WorkspaceShellTestAccess::SetSettingValue(shell, "editor.shaping.sort_lines.enabled", "false"),
         "fixture should disable sort shaping");
  Expect(ExecuteCommand(shell, "sort-lines-ascending"),
         "sort-lines-ascending should still dispatch while disabled");
  Expect(sort_vp.lines()[0] == "banana" && sort_vp.lines()[1] == "apple",
         "sort-lines-ascending must noop while editor.shaping.sort_lines.enabled=false");

  Expect(WorkspaceShellTestAccess::SetSettingValue(shell, "editor.shaping.sort_lines.enabled", "true"),
         "fixture should re-enable sort shaping");
  sort_vp.MoveCursorTo(0, 0);
  sort_vp.MoveCursorTo(1, 5, true);
  Expect(ExecuteCommand(shell, "sort-lines-ascending"),
         "sort-lines-ascending should mutate once enabled");
  Expect(sort_vp.lines()[0] == "apple" && sort_vp.lines()[1] == "banana",
         "enabled sort-lines-ascending should lexicographically order the selection");
}

void TestWorkspaceShellCodeActionMenuIsCentered() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "main.cpp";
  WriteFile(source, "int main() { return 0; }\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenFile(shell, source);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);

  const SDL_FRect editor_area = WorkspaceShellTestAccess::EditorAreaRect(shell);
  const SDL_FRect menu = WorkspaceShellTestAccess::CodeActionMenuRectForItemCount(shell, 3);
  const float menu_center = menu.x + menu.w * 0.5f;
  const float area_center = editor_area.x + editor_area.w * 0.5f;
  Expect(std::fabs(menu_center - area_center) <= 1.0f,
         "code-action menu should be horizontally centered in the editor area");
  Expect(menu.w < editor_area.w,
         "code-action menu should be a compact modal, narrower than the editor area");

  const SDL_FRect taller = WorkspaceShellTestAccess::CodeActionMenuRectForItemCount(shell, 8);
  Expect(taller.h > menu.h, "a longer action list should produce a taller menu");
}

void TestWorkspaceShellSettingsOverlayRightClickDoesNotOpenEditorContextMenu() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "main.cpp";
  WriteFile(source, "int main() { return 0; }\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenFile(shell, source);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::OpenSettingsOverlay(shell);

  Expect(WorkspaceShellTestAccess::SettingsOverlayVisible(shell),
         "settings overlay fixture should open the overlay");
  const SDL_FRect overlay_rect = WorkspaceShellTestAccess::SettingsOverlayRect(shell);
  Expect(SendMouseDown(shell, overlay_rect.x + overlay_rect.w * 0.5f,
                       overlay_rect.y + overlay_rect.h * 0.5f, SDL_BUTTON_RIGHT),
         "right click inside settings overlay should be handled");
  Expect(!WorkspaceShellTestAccess::EditorContextMenuOpen(shell),
         "right click inside settings overlay should not leak into editor context menu");
}


void TestWorkspaceShellCtrlArrowsScrollAndCtrlLSelectsLines() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "long.txt";
  std::string content;
  for (int i = 0; i < 200; ++i) {
    content += (i == 0 ? "" : "\n") + std::string("line ") + std::to_string(i);
  }
  WriteFile(source, content);

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, source);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::RenderFrame(shell);
  WorkspaceShellTestAccess::SetFocusEditor(shell);
  auto& viewport = WorkspaceShellTestAccess::ActiveEditor(shell);
  viewport.SetViewportSize(40, 120);
  viewport.MoveCursorTo(3, 2);
  Expect(viewport.scroll_line() == 0, "the fixture starts scrolled to the top");

  Expect(SendKeyDown(shell, SDLK_DOWN, SDL_KMOD_CTRL), "Ctrl+Down is handled by the editor");
  Expect(viewport.scroll_line() == 1, "Ctrl+Down scrolls the view down one line");
  Expect(viewport.cursor_line() == 3 && viewport.cursor_column() == 2,
         "Ctrl+Down leaves the caret where it was");
  Expect(SendKeyDown(shell, SDLK_UP, SDL_KMOD_CTRL), "Ctrl+Up is handled by the editor");
  Expect(viewport.scroll_line() == 0, "Ctrl+Up scrolls the view back up");
  Expect(SendKeyDown(shell, SDLK_UP, SDL_KMOD_CTRL), "Ctrl+Up at the top is still handled");
  Expect(viewport.scroll_line() == 0 && viewport.cursor_line() == 3,
         "Ctrl+Up at the top neither scrolls nor moves the caret");

  Expect(SendKeyDown(shell, SDLK_L, SDL_KMOD_CTRL), "Ctrl+L is handled by the editor");
  auto selection = viewport.selection_range();
  Expect(selection.has_value() && selection->start.line == 3 && selection->start.column == 0 &&
             selection->end.line == 4 && selection->end.column == 0,
         "Ctrl+L selects the caret's whole line, caret at the start of the next line");
  Expect(SendKeyDown(shell, SDLK_L, SDL_KMOD_CTRL), "a second Ctrl+L is handled");
  selection = viewport.selection_range();
  Expect(selection.has_value() && selection->start.line == 3 && selection->start.column == 0 &&
             selection->end.line == 5 && selection->end.column == 0,
         "a second Ctrl+L grows the selection by one more line");

  const std::size_t last = viewport.line_count() - 1;
  viewport.MoveCursorTo(last, 0);
  Expect(SendKeyDown(shell, SDLK_L, SDL_KMOD_CTRL), "Ctrl+L on the last line is handled");
  selection = viewport.selection_range();
  Expect(selection.has_value() && selection->start.line == last &&
             selection->end.line == last &&
             selection->end.column == viewport.lines().LineLength(last),
         "Ctrl+L on the last line selects to its end");
}


void TestWorkspaceShellEscapeCancelsTheSelection() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "main.txt";
  WriteFile(source, "hello world\nsecond line\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, source);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::RenderFrame(shell);
  WorkspaceShellTestAccess::SetFocusEditor(shell);
  auto& viewport = WorkspaceShellTestAccess::ActiveEditor(shell);

  viewport.MoveCursorTo(0, 0);
  viewport.MoveCursorTo(0, 5, true);
  Expect(viewport.has_selection(), "the fixture selected \"hello\"");
  Expect(SendKeyDown(shell, SDLK_ESCAPE, SDL_KMOD_NONE), "Esc with a selection is handled");
  Expect(!viewport.has_selection(), "Esc cancels the selection");
  Expect(viewport.cursor_line() == 0 && viewport.cursor_column() == 5,
         "Esc leaves the caret at the selection's active end");
  Expect(!SendKeyDown(shell, SDLK_ESCAPE, SDL_KMOD_NONE),
         "Esc with nothing to cancel is not consumed by the editor");

  viewport.MoveCursorTo(0, 0);
  viewport.MoveCursorTo(0, 5, true);
  viewport.AddSecondaryCaret(1, 0);
  Expect(viewport.has_multiple_carets() && viewport.has_selection(),
         "the fixture has a selection plus a secondary caret");
  Expect(SendKeyDown(shell, SDLK_ESCAPE, SDL_KMOD_NONE), "the first Esc is handled");
  Expect(!viewport.has_multiple_carets() && viewport.has_selection(),
         "the first Esc removes the secondary caret and keeps the selection");
  Expect(SendKeyDown(shell, SDLK_ESCAPE, SDL_KMOD_NONE), "the second Esc is handled");
  Expect(!viewport.has_selection(), "the second Esc cancels the selection");
}


void TestWorkspaceShellVsCodeParityBindings() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "main.cpp";
  WriteFile(source, "int main() {\nreturn 0;\n}\n");

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "fixture project should open");
  WorkspaceShellTestAccess::OpenFile(shell, source);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::SetFocusEditor(shell);
  auto& viewport = WorkspaceShellTestAccess::ActiveEditor(shell);

  WorkspaceShellTestAccess::SetSettingValue(shell, "debug.enabled", "true");
  viewport.MoveCursorTo(1, 0);
  Expect(SendKeyDown(shell, SDLK_F9, SDL_KMOD_NONE), "F9 is handled");
  Expect(WorkspaceShellTestAccess::BreakpointStore(shell).HasBreakpoint(source, 1),
         "F9 sets a breakpoint on the caret's line");
  Expect(SendKeyDown(shell, SDLK_F9, SDL_KMOD_NONE), "a second F9 is handled");
  Expect(!WorkspaceShellTestAccess::BreakpointStore(shell).HasBreakpoint(source, 1),
         "a second F9 removes it again");

  viewport.MoveCursorTo(1, 0);
  Expect(SendKeyDown(shell, SDLK_RIGHTBRACKET, SDL_KMOD_CTRL), "Ctrl+] is handled");
  Expect(std::string(viewport.lines().LineView(1)).starts_with(" ") ||
             std::string(viewport.lines().LineView(1)).starts_with("\t"),
         "Ctrl+] indents the caret's line");
  Expect(SendKeyDown(shell, SDLK_LEFTBRACKET, SDL_KMOD_CTRL), "Ctrl+[ is handled");
  Expect(std::string(viewport.lines().LineView(1)) == "return 0;",
         "Ctrl+[ outdents it back");

  const bool wrap_before = viewport.soft_wrap();
  Expect(SendKeyDown(shell, SDLK_Z, SDL_KMOD_ALT), "Alt+Z is handled");
  Expect(viewport.soft_wrap() != wrap_before, "Alt+Z toggles word wrap");
  Expect(SendKeyDown(shell, SDLK_Z, SDL_KMOD_ALT), "a second Alt+Z is handled");
  Expect(viewport.soft_wrap() == wrap_before, "a second Alt+Z toggles it back");

  Expect(SendKeyDown(shell, SDLK_G, SDL_KMOD_CTRL | SDL_KMOD_SHIFT), "Ctrl+Shift+G is handled");
  Expect(WorkspaceShellTestAccess::SidebarMode(shell) == WorkspaceShell::SidebarMode::Git,
         "Ctrl+Shift+G shows source control");
  Expect(SendKeyDown(shell, SDLK_E, SDL_KMOD_CTRL | SDL_KMOD_SHIFT), "Ctrl+Shift+E is handled");
  Expect(WorkspaceShellTestAccess::SidebarMode(shell) == WorkspaceShell::SidebarMode::Tree,
         "Ctrl+Shift+E shows the project tree");
}


void TestWorkspaceShellAltF8StepsDiagnostics() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "main.py";
  WriteFile(source, "a = 1\nb = 2\nc = 3\nd = 4\ne = 5\n");

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "fixture project should open");
  WorkspaceShellTestAccess::OpenFile(shell, source);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::SetFocusEditor(shell);
  auto& viewport = WorkspaceShellTestAccess::ActiveEditor(shell);
  using microide::editor::Diagnostic;
  using microide::editor::DiagnosticSeverity;
  using microide::editor::SelectionRange;
  using microide::editor::TextPosition;
  Expect(WorkspaceShellTestAccess::PublishDiagnostics(
             shell, "lsp", source,
             {Diagnostic{SelectionRange{TextPosition{4, 0}, TextPosition{4, 1}},
                         DiagnosticSeverity::Warning, "fourth"},
              Diagnostic{SelectionRange{TextPosition{0, 2}, TextPosition{0, 3}},
                         DiagnosticSeverity::Error, "first"},
              Diagnostic{SelectionRange{TextPosition{2, 0}, TextPosition{2, 1}},
                         DiagnosticSeverity::Error, "third"}}),
         "the fixture publishes three diagnostics");

  viewport.MoveCursorTo(1, 0);
  Expect(SendKeyDown(shell, SDLK_F8, SDL_KMOD_ALT), "Alt+F8 is handled");
  Expect(viewport.cursor_line() == 2 && viewport.cursor_column() == 0,
         "Alt+F8 jumps to the first diagnostic after the caret (line 2)");
  Expect(SendKeyDown(shell, SDLK_F8, SDL_KMOD_ALT), "a second Alt+F8 is handled");
  Expect(viewport.cursor_line() == 4, "a second Alt+F8 reaches line 4");
  Expect(SendKeyDown(shell, SDLK_F8, SDL_KMOD_ALT), "a third Alt+F8 is handled");
  Expect(viewport.cursor_line() == 0 && viewport.cursor_column() == 2,
         "Alt+F8 wraps from the last diagnostic to the first");
  Expect(SendKeyDown(shell, SDLK_F8, SDL_KMOD_SHIFT | SDL_KMOD_ALT), "Shift+Alt+F8 is handled");
  Expect(viewport.cursor_line() == 4, "Shift+Alt+F8 wraps from the first diagnostic to the last");
  Expect(SendKeyDown(shell, SDLK_F8, SDL_KMOD_SHIFT | SDL_KMOD_ALT),
         "a second Shift+Alt+F8 is handled");
  Expect(viewport.cursor_line() == 2, "Shift+Alt+F8 steps back to line 2");
}

void TestWorkspaceShellSettingsOverlayTrapsKeyboardInput() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "main.cpp";
  WriteFile(source, "int main() { return 0; }\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenFile(shell, source);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);

  const std::size_t initial_line_count = WorkspaceShellTestAccess::ActiveEditor(shell).line_count();
  const std::size_t initial_cursor_line = WorkspaceShellTestAccess::ActiveEditor(shell).cursor_line();
  const std::size_t initial_cursor_column =
      WorkspaceShellTestAccess::ActiveEditor(shell).cursor_column();

  WorkspaceShellTestAccess::OpenSettingsOverlay(shell);
  Expect(WorkspaceShellTestAccess::SettingsOverlayVisible(shell),
         "settings overlay fixture should open the overlay");

  Expect(SendKeyDown(shell, SDLK_RETURN, SDL_KMOD_NONE),
         "Enter should be consumed by the settings overlay");
  Expect(SendKeyDown(shell, SDLK_BACKSPACE, SDL_KMOD_NONE),
         "Backspace should be consumed by the settings overlay");
  Expect(SendKeyDown(shell, SDLK_DOWN, SDL_KMOD_NONE),
         "Arrow keys should be consumed by the settings overlay");

  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).line_count() == initial_line_count,
         "keys typed while settings is open must not change the editor buffer");
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).cursor_line() == initial_cursor_line &&
             WorkspaceShellTestAccess::ActiveEditor(shell).cursor_column() == initial_cursor_column,
         "keys typed while settings is open must not move the editor cursor");

  Expect(SendKeyDown(shell, SDLK_ESCAPE, SDL_KMOD_NONE),
         "Escape should be consumed by the settings overlay");
  Expect(!WorkspaceShellTestAccess::SettingsOverlayVisible(shell),
         "Escape should close the settings overlay");
}

void TestWorkspaceShellSettingsPickerRowsBelowTheCardStillPick() {
  workspace::SettingsOverlayViewModel vm;
  vm.rect = SDL_FRect{100.0f, 100.0f, 600.0f, 300.0f};
  vm.value_picker.visible = true;
  vm.value_picker.rect = SDL_FRect{500.0f, 380.0f, 220.0f, 120.0f};
  Expect(workspace::SettingsOverlayContains(vm, 600.0f, 450.0f), "a point on the dropdown below the card is on the surface");
  Expect(workspace::SettingsOverlayContains(vm, 150.0f, 150.0f), "a point on the card is on the surface");
  Expect(!workspace::SettingsOverlayContains(vm, 50.0f, 450.0f), "a point beside both is outside");
  vm.value_picker.visible = false;
  Expect(!workspace::SettingsOverlayContains(vm, 600.0f, 450.0f), "a hidden dropdown claims nothing");

  TemporaryDirectory temp;
  const std::filesystem::path root = temp.path() / "proj";
  std::filesystem::create_directories(root);
  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::OpenSettingsOverlay(shell);
  WorkspaceShellTestAccess::BeginSettingValueEdit(shell, "editor.font_family");
  Expect(WorkspaceShellTestAccess::SettingsEditingFonts(shell), "the font row's edit opens the picker");
  const workspace::SettingsOverlayViewModel live = WorkspaceShellTestAccess::BuildSettingsOverlayViewModel(shell);
  Expect(live.value_picker.visible && !live.value_picker.items.empty(), "the picker lists its rows");
  const workspace::SettingsPickerItemViewModel& choose = live.value_picker.items.back();
  Expect(choose.is_choose_file, "the last row is Choose file…");
  Expect(SendMouseDown(shell, choose.rect.x + choose.rect.w * 0.5f, choose.rect.y + choose.rect.h * 0.5f,
                       SDL_BUTTON_LEFT),
         "a click on Choose file… is handled");
  Expect(WorkspaceShellTestAccess::SettingsOverlayVisible(shell),
         "the click picks (the native picker, disabled in tests) instead of closing Settings");
  Expect(!WorkspaceShellTestAccess::SettingsEditingFonts(shell), "and the picker edit is over");
}

void TestWorkspaceShellSettingsOverlayWheelScrolls() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::OpenSettingsOverlay(shell);
  Expect(WorkspaceShellTestAccess::SettingsOverlayVisible(shell),
         "settings overlay fixture should open the overlay");
  WorkspaceShellTestAccess::SetSettingsOverlayCategory(shell, 0);
  const SDL_FRect rect = WorkspaceShellTestAccess::SettingsOverlayRect(shell);
  const float wheel_x = rect.x + rect.w * 0.7f;
  const float wheel_y = rect.y + rect.h * 0.6f;

  Expect(WorkspaceShellTestAccess::SettingsOverlayScrollRow(shell) == 0,
         "settings overlay should start unscrolled");
  Expect(SendMouseWheel(shell, wheel_x, wheel_y, -4),
         "wheel over the settings overlay should be handled");
  const int scrolled = WorkspaceShellTestAccess::SettingsOverlayScrollRow(shell);
  Expect(scrolled > 0, "scrolling down should advance the settings scroll row");
  Expect(SendMouseWheel(shell, wheel_x, wheel_y, 4),
         "wheel up over the settings overlay should be handled");
  Expect(WorkspaceShellTestAccess::SettingsOverlayScrollRow(shell) < scrolled,
         "scrolling up should reduce the settings scroll row");
}

void TestWorkspaceShellSettingsClickOnTheHeaderMissesTheRowScrolledBeneathIt() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::OpenSettingsOverlay(shell);
  WorkspaceShellTestAccess::SetSettingsOverlayCategory(shell, 0);
  WorkspaceShellTestAccess::SetSettingsOverlaySelectedRow(shell, 0);
  WorkspaceShellTestAccess::SetSettingsOverlayScrollRow(shell, 2);
  const workspace::SettingsOverlayViewModel vm = WorkspaceShellTestAccess::BuildSettingsOverlayViewModel(shell);
  Expect(vm.scroll_row == 2, "the Editor category scrolls two rows");
  const SDL_FRect header = vm.section_header_rect;
  const workspace::SettingsRowViewModel* hidden = nullptr;
  for (const workspace::SettingsRowViewModel& row : vm.rows) {
    const float mid_y = header.y + header.h * 0.5f;
    if (row.row_in_category != 0 && row.row_rect.y <= mid_y && row.row_rect.y + row.row_rect.h > mid_y) {
      hidden = &row;
    }
  }
  Expect(hidden != nullptr, "a scrolled-away row lies under the section header");
  if (hidden == nullptr) {
    return;
  }
  Expect(!workspace::SettingsRowShownIn(vm.right_pane_rect, hidden->row_rect), "and it is not drawn");
  Expect(SendMouseDown(shell, header.x + header.w * 0.5f, header.y + header.h * 0.5f, SDL_BUTTON_LEFT),
         "a click on the header is handled");
  Expect(WorkspaceShellTestAccess::SettingsOverlaySelectedRowIndex(shell) == 0,
         "the click on the header selects no row");
}

void TestWorkspaceShellDebugPaneKeyboardRevealsAndPages() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::FocusDebugPaneWatch(shell);
  for (int i = 0; i < 60; ++i) {
    WorkspaceShellTestAccess::AddDebugWatchExpressionForTest(shell, "expr_" + std::to_string(i));
  }
  const std::size_t rows = WorkspaceShellTestAccess::DebugWatchRowCount(shell);
  const int visible = WorkspaceShellTestAccess::DebugPaneVisibleRows(shell);
  Expect(visible > 0 && rows > static_cast<std::size_t>(visible) + 1,
         "the watch fixture should overflow the pane");
  Expect(WorkspaceShellTestAccess::DebugPaneWatchScrollRow(shell) == 0,
         "the pane should start unscrolled");

  for (int i = 0; i < visible; ++i) {
    Expect(SendKeyDown(shell, SDLK_DOWN, SDL_KMOD_NONE), "Down should be consumed by the pane");
  }
  const std::size_t selected = WorkspaceShellTestAccess::DebugWatchSelectedRow(shell);
  Expect(selected == static_cast<std::size_t>(visible),
         "Down should have advanced one row past the visible window");
  const int scroll = WorkspaceShellTestAccess::DebugPaneWatchScrollRow(shell);
  Expect(scroll > 0, "moving past the last visible row should scroll the pane");
  Expect(static_cast<int>(selected) >= scroll &&
             static_cast<int>(selected) < scroll + visible,
         "the selection must stay inside the visible window");

  Expect(SendKeyDown(shell, SDLK_END, SDL_KMOD_NONE), "End should be consumed by the pane");
  Expect(WorkspaceShellTestAccess::DebugWatchSelectedRow(shell) == rows - 1,
         "End should select the last row");
  const int end_scroll = WorkspaceShellTestAccess::DebugPaneWatchScrollRow(shell);
  Expect(static_cast<int>(rows) - 1 >= end_scroll &&
             static_cast<int>(rows) - 1 < end_scroll + visible,
         "End should reveal the last row too");

  Expect(SendKeyDown(shell, SDLK_PAGEUP, SDL_KMOD_NONE), "PageUp should be consumed by the pane");
  Expect(WorkspaceShellTestAccess::DebugWatchSelectedRow(shell) ==
             rows - 1 - static_cast<std::size_t>(microide::workspace::kListPageStep),
         "PageUp should move by the shared page step");

  Expect(SendKeyDown(shell, SDLK_HOME, SDL_KMOD_NONE), "Home should be consumed by the pane");
  Expect(WorkspaceShellTestAccess::DebugWatchSelectedRow(shell) == 0,
         "Home should select the first row");
  Expect(WorkspaceShellTestAccess::DebugPaneWatchScrollRow(shell) == 0,
         "Home should scroll the pane back to the top");
}

void TestWorkspaceShellDebugCallStackKeyboardNavigates() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "main.cpp";
  std::string body;
  for (int i = 0; i < 200; ++i) {
    body += "int line_" + std::to_string(i) + "() { return " + std::to_string(i) + "; }\n";
  }
  WriteFile(source, body);

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::FocusDebugPaneCallStack(shell);

  auto& exec = WorkspaceShellTestAccess::DebugExecution(shell);
  exec.stopped = true;
  for (int i = 0; i < 80; ++i) {
    microide::workspace::DebugStackFrameView frame;
    frame.id = i;
    frame.SetSource(source.string());
    frame.line = static_cast<std::size_t>(i);
    exec.frames.push_back(frame);
  }
  const int visible = WorkspaceShellTestAccess::DebugPaneVisibleRows(shell);
  Expect(visible > 0 && exec.PanelRowCount() > static_cast<std::size_t>(visible),
         "the call stack fixture should overflow the pane");
  Expect(exec.focused_frame_index == 0, "the top frame starts focused");

  Expect(SendKeyDown(shell, SDLK_DOWN, SDL_KMOD_NONE), "Down should be consumed by the call stack");
  Expect(WorkspaceShellTestAccess::DebugExecution(shell).focused_frame_index == 1,
         "Down should focus the next frame, as clicking it would");
  Expect(WorkspaceShellTestAccess::FocusIsDebugPane(shell),
         "arrowing the call stack must keep focus in the pane, unlike a click");

  Expect(SendKeyDown(shell, SDLK_END, SDL_KMOD_NONE), "End should be consumed by the call stack");
  const std::size_t last = WorkspaceShellTestAccess::DebugExecution(shell).frames.size() - 1;
  Expect(WorkspaceShellTestAccess::DebugExecution(shell).focused_frame_index == last,
         "End should focus the deepest frame");
  const int scroll = WorkspaceShellTestAccess::DebugPaneCallStackScrollRow(shell);
  Expect(scroll > 0, "focusing the deepest frame should scroll the pane to it");
  Expect(static_cast<int>(last) >= scroll && static_cast<int>(last) < scroll + visible,
         "the focused frame must stay inside the visible window");

  Expect(SendKeyDown(shell, SDLK_HOME, SDL_KMOD_NONE), "Home should be consumed by the call stack");
  Expect(WorkspaceShellTestAccess::DebugExecution(shell).focused_frame_index == 0,
         "Home should focus the top frame again");
  Expect(WorkspaceShellTestAccess::DebugPaneCallStackScrollRow(shell) == 0,
         "Home should scroll the pane back to the top");
}

void TestWorkspaceShellDebugPaneKeysDoNotReachTheEditor() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "main.cpp";
  WriteFile(source, "alpha beta gamma\nsecond line\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::OpenFile(shell, source);
  auto& editor = WorkspaceShellTestAccess::ActiveEditor(shell);
  editor.MoveCursorTo(0, 16);
  for (const microide::workspace::DebugPaneMode mode : {microide::workspace::DebugPaneMode::CallStack, microide::workspace::DebugPaneMode::Variables, microide::workspace::DebugPaneMode::Watch,
                                   microide::workspace::DebugPaneMode::Breakpoints}) {
    WorkspaceShellTestAccess::SetDebugPaneMode(shell, mode);
    WorkspaceShellTestAccess::SetFocusDebugPane(shell);
    for (const auto& [key, mod] : {std::pair{SDLK_LEFT, SDL_KMOD_CTRL}, std::pair{SDLK_RIGHT, SDL_KMOD_SHIFT},
                                   std::pair{SDLK_BACKSPACE, SDL_KMOD_NONE}, std::pair{SDLK_DELETE, SDL_KMOD_NONE},
                                   std::pair{SDLK_RETURN, SDL_KMOD_SHIFT}}) {
      (void)SendKeyDown(shell, key, mod);
      Expect(WorkspaceShellTestAccess::FocusIsDebugPane(shell), "the key leaves focus in the pane");
      Expect(editor.cursor_line() == 0 && editor.cursor_column() == 16 && !editor.has_selection() &&
                 !editor.dirty(),
             "mode " + std::to_string(static_cast<int>(mode)) + ", key " + std::to_string(key) +
                 ": the unfocused editor is untouched (caret " + std::to_string(editor.cursor_line()) + ":" +
                 std::to_string(editor.cursor_column()) + (editor.dirty() ? ", dirty" : "") + ")");
    }
  }
}

void TestWorkspaceShellDebugBreakpointsKeyboardNavigates() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "main.cpp";
  WriteFile(source, "int a();\nint b();\nint c();\nint d();\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::FocusDebugPaneBreakpoints(shell);
  for (std::size_t line = 0; line < 3; ++line) {
    WorkspaceShellTestAccess::ToggleBreakpointForTest(shell, source, line);
  }
  const std::size_t rows = WorkspaceShellTestAccess::DebugBreakpointRowCount(shell);
  Expect(rows > 1, "the breakpoints fixture should build rows");
  Expect(WorkspaceShellTestAccess::DebugBreakpointsSelectedRow(shell) == 0,
         "the panel should start on its first row");

  Expect(SendKeyDown(shell, SDLK_DOWN, SDL_KMOD_NONE), "Down should be consumed by the panel");
  Expect(WorkspaceShellTestAccess::DebugBreakpointsSelectedRow(shell) == 1,
         "Down should move the panel selection");
  Expect(SendKeyDown(shell, SDLK_END, SDL_KMOD_NONE), "End should be consumed by the panel");
  Expect(WorkspaceShellTestAccess::DebugBreakpointsSelectedRow(shell) ==
             static_cast<int>(rows) - 1,
         "End should select the last row");
  Expect(SendKeyDown(shell, SDLK_HOME, SDL_KMOD_NONE), "Home should be consumed by the panel");
  Expect(WorkspaceShellTestAccess::DebugBreakpointsSelectedRow(shell) == 0,
         "Home should select the first row");

  Expect(SendKeyDown(shell, SDLK_DOWN, SDL_KMOD_NONE), "Down should be consumed by the panel");
  Expect(WorkspaceShellTestAccess::BreakpointEnabledForTest(shell, source, 0),
         "the fixture's first breakpoint starts enabled");
  Expect(SendKeyDown(shell, SDLK_SPACE, SDL_KMOD_NONE), "Space should be consumed by the panel");
  Expect(!WorkspaceShellTestAccess::BreakpointEnabledForTest(shell, source, 0),
         "Space should disable the selected breakpoint, as double-clicking it does");
  Expect(WorkspaceShellTestAccess::FocusIsDebugPane(shell),
         "toggling from the keyboard must leave focus in the pane");

  Expect(SendKeyDown(shell, SDLK_RETURN, SDL_KMOD_NONE), "Enter should be consumed by the panel");
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).path() == source.lexically_normal(),
         "Enter should navigate to the selected breakpoint's file");
  Expect(WorkspaceShellTestAccess::FocusIsDebugPane(shell),
         "navigating from the keyboard must leave focus in the pane");
}

void TestWorkspaceShellSettingsFontPickerScrollbarIsGrabbable() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::OpenSettingsOverlay(shell);

  std::vector<std::string> families;
  for (int i = 0; i < microide::workspace::SettingsOverlayService::kPickerVisibleFamilies + 12;
       ++i) {
    families.push_back("Family " + std::to_string(i));
  }
  WorkspaceShellTestAccess::BeginSettingsFontValueEdit(shell, "editor.font_family", families);

  const auto scrollbar = WorkspaceShellTestAccess::SettingsOverlayPickerScrollbar(shell);
  Expect(scrollbar.has_value(), "an overflowing family list should publish a picker scrollbar");
  Expect(WorkspaceShellTestAccess::SettingsOverlayPickerScroll(shell) == 0,
         "the picker should start unscrolled");
  Expect(SendMouseDown(shell, scrollbar->track.x + scrollbar->track.w * 0.5f,
                       scrollbar->track.y + scrollbar->track.h - 2.0f, SDL_BUTTON_LEFT),
         "clicking the picker scrollbar track should be handled");
  Expect(WorkspaceShellTestAccess::SettingsOverlayPickerScroll(shell) > 0,
         "clicking near the bottom of the picker track should scroll the family list");
  Expect(WorkspaceShellTestAccess::SettingsOverlayPickerScrollbar(shell).has_value(),
         "grabbing the picker scrollbar must leave the dropdown open");
}

void TestWorkspaceShellHelpAboutIsKeyboardAndScrollbarNavigable() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::OpenHelpAboutOverlay(shell);
  const int max_scroll = WorkspaceShellTestAccess::SettingsOverlayMaxScroll(shell);
  Expect(max_scroll > microide::workspace::kListPageStep,
         "help/about should overflow the fixture window by more than one page");

  Expect(SendKeyDown(shell, SDLK_DOWN, SDL_KMOD_NONE), "Down should be consumed by help/about");
  Expect(WorkspaceShellTestAccess::SettingsOverlayScrollRow(shell) == 1,
         "Down should scroll help/about by one entry");
  Expect(SendKeyDown(shell, SDLK_PAGEDOWN, SDL_KMOD_NONE), "PageDown should be consumed");
  Expect(WorkspaceShellTestAccess::SettingsOverlayScrollRow(shell) ==
             1 + microide::workspace::kListPageStep,
         "PageDown should scroll help/about by the shared page step");
  Expect(SendKeyDown(shell, SDLK_END, SDL_KMOD_NONE), "End should be consumed");
  Expect(WorkspaceShellTestAccess::SettingsOverlayScrollRow(shell) == max_scroll,
         "End should scroll help/about to the last entry");
  Expect(SendKeyDown(shell, SDLK_HOME, SDL_KMOD_NONE), "Home should be consumed");
  Expect(WorkspaceShellTestAccess::SettingsOverlayScrollRow(shell) == 0,
         "Home should scroll help/about back to the top");

  const auto scrollbar = WorkspaceShellTestAccess::SettingsOverlayScrollbar(shell);
  Expect(scrollbar.has_value(),
         "an overflowing help/about list should publish a scrollbar on the view model");
  const float track_x = scrollbar->track.x + scrollbar->track.w * 0.5f;
  const float track_y = scrollbar->track.y + scrollbar->track.h - 2.0f;
  Expect(SendMouseDown(shell, track_x, track_y, SDL_BUTTON_LEFT),
         "clicking the help/about scrollbar track should be handled");
  Expect(WorkspaceShellTestAccess::SettingsOverlayScrollRow(shell) > 0,
         "clicking near the bottom of the track should jump the help/about list down");
}

void TestWorkspaceShellHelpAboutFilterNarrowsRows() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::OpenHelpAboutOverlay(shell);

  const std::size_t unfiltered = WorkspaceShellTestAccess::HelpAboutRows(shell).size();
  Expect(unfiltered > 20, "the help/about fixture should have many rows to filter");

  const std::string text = "split";
  for (char character : text) {
    SDL_Event event{};
    event.type = SDL_EVENT_TEXT_INPUT;
    const char buffer[2] = {character, '\0'};
    event.text.text = buffer;
    Expect(shell.HandleEvent(event).handled,
           "typing in help/about should be routed to its filter field");
  }

  const auto rows = WorkspaceShellTestAccess::HelpAboutRows(shell);
  Expect(!rows.empty(), "a filter that matches should keep its matching rows");
  Expect(rows.size() < unfiltered, "a filter should narrow the help/about list");
  Expect(std::all_of(rows.begin(), rows.end(),
                     [](const auto& row) {
                       return row.label.find("Split") != std::string::npos ||
                              row.detail.find("split") != std::string::npos;
                     }),
         "every surviving row should match the needle");

  for (std::size_t i = 0; i < text.size(); ++i) {
    Expect(SendKeyDown(shell, SDLK_BACKSPACE, SDL_KMOD_NONE),
           "backspace should be routed to the help/about filter field");
  }
  Expect(WorkspaceShellTestAccess::HelpAboutRows(shell).size() == unfiltered,
         "clearing the filter should restore every row");

  Expect(SendKeyDown(shell, SDLK_END, SDL_KMOD_NONE), "End should still be consumed");
  Expect(WorkspaceShellTestAccess::SettingsOverlayScrollRow(shell) > 0,
         "End should still jump the help/about list, not move a caret");
}

void TestWorkspaceShellHelpAboutOmitsAuthCommands() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::OpenHelpAboutOverlay(shell);
  const auto rows = WorkspaceShellTestAccess::HelpAboutRows(shell);
  const bool has_auth_reference = std::any_of(
      rows.begin(), rows.end(), [](const auto& row) {
        return row.detail.find("auth-login") != std::string::npos ||
               row.detail.find("auth-refresh") != std::string::npos ||
               row.detail.find("auth-logout") != std::string::npos;
      });
  Expect(!has_auth_reference,
         "help/about rows should not include auth command references");
}

void TestWorkspaceShellHelpAboutShowsBoundKeyChords() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::OpenHelpAboutOverlay(shell);
  const auto rows = WorkspaceShellTestAccess::HelpAboutRows(shell);
  const auto chord_row = std::find_if(rows.begin(), rows.end(), [](const auto& row) {
    return row.detail.find("  ·  ") != std::string::npos;
  });
  Expect(chord_row != rows.end(),
         "help/about rows should show the bound key chord for actions that have a keybinding");
  Expect(chord_row->detail.find("  ·  ") > 0,
         "the bound key chord should be prefixed before the command usage, not empty");
}

void TestWorkspaceShellIgnoredTreeFileActivatesDirectOpenPath() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path ignored_file = root / ".env.local";
  WriteFile(root / ".gitignore", ".env.local\n");
  WriteFile(ignored_file, "TOKEN=abc\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);

  const auto& entries = WorkspaceShellTestAccess::TreeEntries(shell);
  const auto ignored_it = std::find_if(entries.begin(), entries.end(), [&](const auto& entry) {
    return entry.path == ignored_file.lexically_normal();
  });
  Expect(ignored_it != entries.end(),
         "ignored file should remain visible in the project tree");
  Expect(ignored_it != entries.end() && ignored_it->ignored,
         "ignored file should be marked ignored in the tree model");

  Expect(WorkspaceShellTestAccess::SelectTreePath(shell, ignored_file),
         "ignored file should be selectable in the tree");
  const auto activated = WorkspaceShellTestAccess::ActivateTreeSelection(shell);
  Expect(activated.has_value() && activated->lexically_normal() == ignored_file.lexically_normal(),
         "activating an ignored file selection should return a direct-open file path");
}

void TestWorkspaceShellIgnoredDirectoryExpansionMaterializesOneLevel() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path ignored_dir = root / "node_modules";
  const std::filesystem::path nested_dir = ignored_dir / "pkg";
  const std::filesystem::path nested_file = nested_dir / "deep.js";
  const std::filesystem::path immediate_file = ignored_dir / "top.js";
  WriteFile(root / ".gitignore", "node_modules/\n");
  WriteFile(immediate_file, "console.log('top');\n");
  WriteFile(nested_file, "console.log('deep');\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);

  const auto contains_path = [&](const std::filesystem::path& path) {
    const auto& tree_entries = WorkspaceShellTestAccess::TreeEntries(shell);
    return std::any_of(tree_entries.begin(), tree_entries.end(),
                       [&](const auto& entry) { return entry.path == path.lexically_normal(); });
  };

  Expect(contains_path(ignored_dir),
         "ignored directory should be visible before expansion");
  Expect(!contains_path(immediate_file) && !contains_path(nested_file),
         "ignored descendants should remain unmaterialized before expansion");

  Expect(WorkspaceShellTestAccess::SelectTreePath(shell, ignored_dir),
         "ignored directory should be selectable");
  WorkspaceShellTestAccess::ExpandTreeSelection(shell);

  Expect(contains_path(immediate_file),
         "expanding ignored directory should materialize immediate children");
  Expect(contains_path(nested_dir),
         "expanding ignored directory should materialize immediate child directories");
  Expect(!contains_path(nested_file),
         "expanding ignored directory should not recursively materialize deeper descendants");
}

void TestWorkspaceShellHiddenIgnoredDirectoryUsesSameLazyExpansionRules() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path hidden_ignored_dir = root / ".cache";
  const std::filesystem::path nested_dir = hidden_ignored_dir / "pkg";
  const std::filesystem::path nested_file = nested_dir / "deep.bin";
  const std::filesystem::path immediate_file = hidden_ignored_dir / "top.bin";
  WriteFile(root / ".gitignore", ".cache/\n");
  WriteFile(immediate_file, "top\n");
  WriteFile(nested_file, "deep\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);

  const auto contains_path = [&](const std::filesystem::path& path) {
    const auto& tree_entries = WorkspaceShellTestAccess::TreeEntries(shell);
    return std::any_of(tree_entries.begin(), tree_entries.end(),
                       [&](const auto& entry) { return entry.path == path.lexically_normal(); });
  };

  Expect(contains_path(hidden_ignored_dir),
         "hidden ignored directories should be visible like other ignored directories");
  Expect(!contains_path(immediate_file) && !contains_path(nested_file),
         "hidden ignored descendants should remain unmaterialized before expansion");

  Expect(WorkspaceShellTestAccess::SelectTreePath(shell, hidden_ignored_dir),
         "hidden ignored directory should be selectable");
  WorkspaceShellTestAccess::ExpandTreeSelection(shell);

  Expect(contains_path(immediate_file),
         "expanding hidden ignored directories should materialize immediate children");
  Expect(!contains_path(nested_file),
         "expanding hidden ignored directories should still avoid recursive materialization");
}

void TestWorkspaceShellCopySelectionWithContextUsesRelativePathAndLineRange() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source_dir = root / "src";
  const std::filesystem::path source = source_dir / "main.cpp";
  std::filesystem::create_directories(source_dir);
  WriteFile(source, "int main() {\n  int value = 1;\n  return value;\n}\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenFile(shell, source);

  std::string clipboard_text;
  WorkspaceShellTestAccess::SetClipboardTextWriter(
      shell, [&](std::string_view text) {
        clipboard_text = std::string(text);
        return true;
      });

  WorkspaceShellTestAccess::ActiveEditor(shell).MoveCursorTo(1, 2);
  WorkspaceShellTestAccess::ActiveEditor(shell).MoveCursorTo(2, 15, true);

  Expect(WorkspaceShellTestAccess::ExecuteCopySelectionWithContext(shell),
         "copy with context action should execute");
  Expect(clipboard_text ==
             "// context: int main() {\nsrc/main.cpp:2-3\nint value = 1;\n  return value;",
         "copy with context should prepend the enclosing opener, relative path and line range");
}

void TestWorkspaceShellCopySelectionWithContextOutsideProjectRootUsesAbsolutePath() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path outside = temp_dir.path() / "elsewhere" / "stray.cpp";
  std::filesystem::create_directories(root);
  std::filesystem::create_directories(outside.parent_path());
  WriteFile(outside, "int main() {\n  int value = 1;\n}\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenFile(shell, outside);

  std::string clipboard_text;
  WorkspaceShellTestAccess::SetClipboardTextWriter(shell, [&](std::string_view text) {
    clipboard_text = std::string(text);
    return true;
  });

  WorkspaceShellTestAccess::ActiveEditor(shell).MoveCursorTo(1, 5);
  Expect(WorkspaceShellTestAccess::ExecuteCopySelectionWithContext(shell),
         "copy with context should execute for a buffer outside the project root");
  Expect(clipboard_text.find(outside.generic_string()) != std::string::npos,
         "an out-of-root buffer should be labelled with its absolute path, not a "
         "'..'-relative one and not an empty label");
  Expect(clipboard_text.find("..") == std::string::npos,
         "the label must not escape the root with a relative path");
}

void TestWorkspaceShellCopySelectionWithContextWithoutSelectionCopiesCurrentLine() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source_dir = root / "src";
  const std::filesystem::path source = source_dir / "main.cpp";
  std::filesystem::create_directories(source_dir);
  WriteFile(source, "int main() {\n  int value = 1;\n  return value;\n}\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenFile(shell, source);

  std::string clipboard_text;
  WorkspaceShellTestAccess::SetClipboardTextWriter(
      shell, [&](std::string_view text) {
        clipboard_text = std::string(text);
        return true;
      });

  WorkspaceShellTestAccess::ActiveEditor(shell).MoveCursorTo(1, 5);

  Expect(WorkspaceShellTestAccess::ExecuteCopySelectionWithContext(shell),
         "copy with context should execute without a selection");
  Expect(clipboard_text == "// context: int main() {\nsrc/main.cpp:2\n  int value = 1;",
         "copy with context without a selection should copy the current line plus context");
}

void TestWorkspaceShellCopySelectionWithContextNoSelectionReadsSingleLineFromLargeBuffer() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source_dir = root / "src";
  const std::filesystem::path source = source_dir / "big.cpp";
  std::filesystem::create_directories(source_dir);

  std::string content = "void f() {\n";
  for (int i = 0; i < 40; ++i) {
    content += "  step_" + std::to_string(i) + "();\n";
  }
  content += "}\n";
  WriteFile(source, content);

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenFile(shell, source);

  std::string clipboard_text;
  WorkspaceShellTestAccess::SetClipboardTextWriter(
      shell, [&](std::string_view text) {
        clipboard_text = std::string(text);
        return true;
      });

  WorkspaceShellTestAccess::ActiveEditor(shell).MoveCursorTo(21, 4);
  Expect(WorkspaceShellTestAccess::ExecuteCopySelectionWithContext(shell),
         "copy with context should execute on a large buffer without a selection");
  Expect(clipboard_text == "// context: void f() {\nsrc/big.cpp:22\n  step_20();",
         "a no-selection copy on a large buffer must return only the current line + context");
}

void TestWorkspaceShellCopySelectionWithContextOnBlankLineExpandsToEnclosingFold() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source_dir = root / "src";
  const std::filesystem::path source = source_dir / "main.cpp";
  std::filesystem::create_directories(source_dir);
  WriteFile(source, "int main() {\n\n  return 0;\n}\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenFile(shell, source);

  std::string clipboard_text;
  WorkspaceShellTestAccess::SetClipboardTextWriter(
      shell, [&](std::string_view text) {
        clipboard_text = std::string(text);
        return true;
      });

  WorkspaceShellTestAccess::ActiveEditor(shell).MoveCursorTo(1, 0);

  Expect(WorkspaceShellTestAccess::ExecuteCopySelectionWithContext(shell),
         "copy with context should execute on a blank line");
  Expect(clipboard_text == "src/main.cpp:1-4\nint main() {\n\n  return 0;\n}",
         "copy with context on a blank line should expand to the enclosing fold range");
}

void TestWorkspaceShellEditorRightClickOpensSymbolAwareContextMenu() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "main.cpp";
  std::filesystem::create_directories(root);
  WriteFile(source, "int alpha = beta;\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenFile(shell, source);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  const auto metrics = WorkspaceShellTestAccess::ActiveEditorMetrics(shell);
  const float click_x =
      metrics.text_x + WorkspaceShellTestAccess::TextCharWidth(shell) * 12.0f;
  const float click_y = metrics.first_line_y + metrics.line_height * 0.5f;

  SDL_Event event{};
  event.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
  event.button.button = SDL_BUTTON_RIGHT;
  event.button.x = click_x;
  event.button.y = click_y;

  Expect(shell.HandleEvent(event).handled,
         "right-clicking the editor should be handled");
  Expect(WorkspaceShellTestAccess::MenuBarOpen(shell),
         "right-clicking the editor should open a popup menu");
  Expect(WorkspaceShellTestAccess::EditorContextMenuOpen(shell),
         "right-clicking the editor should open the dedicated editor context menu");
  Expect(!WorkspaceShellTestAccess::EditMenuOpen(shell),
         "right-clicking the editor should not light up the top-level Edit menu");
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).cursor_line() == 0 &&
             WorkspaceShellTestAccess::ActiveEditor(shell).cursor_column() == 12,
         "right-clicking a symbol should retarget the caret before opening the context menu");
}

void TestWorkspaceShellEditorDragSelectionTracksPointer() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "main.txt";
  WriteFile(source, "alpha beta\nsecond line\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, source);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);

  const auto metrics = WorkspaceShellTestAccess::ActiveEditorMetrics(shell);
  const float char_width = WorkspaceShellTestAccess::TextCharWidth(shell);
  const float y = metrics.first_line_y + metrics.line_height * 0.5f;
  const float start_x = metrics.text_x + char_width * 0.1f;
  const float end_x = metrics.text_x + char_width * 5.1f;

  Expect(SendMouseDown(shell, start_x, y, SDL_BUTTON_LEFT),
         "pressing inside the editor should start mouse selection");
  Expect(SendMouseMotion(shell, end_x, y, SDL_BUTTON_LMASK),
         "dragging inside the editor should update mouse selection");
  Expect(SendMouseUp(shell, end_x, y, SDL_BUTTON_LEFT),
         "releasing after an editor drag should be handled");
  Expect(WorkspaceShellTestAccess::ActiveEditorHasSelection(shell),
         "dragging across editor text should create a selection");
  Expect(WorkspaceShellTestAccess::ActiveEditorSelectedText(shell) == "alpha",
         "editor drag selection should capture the dragged text range");
}

void TestWorkspaceShellAltClickAddsSecondaryCaret() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "main.txt";
  WriteFile(source, "alpha beta\nsecond line\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, source);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::RenderFrame(shell);

  auto& viewport = WorkspaceShellTestAccess::ActiveEditor(shell);
  viewport.MoveCursorTo(0, 0);

  const auto metrics = WorkspaceShellTestAccess::ActiveEditorMetrics(shell);
  const float char_width = WorkspaceShellTestAccess::TextCharWidth(shell);
  const float y = metrics.first_line_y + metrics.line_height * 0.5f;
  const float click_x = metrics.text_x + char_width * 5.0f;

  const ScopedHeldModifiers alt_mods(shell, SDL_KMOD_ALT);
  const bool handled = SendMouseDown(shell, click_x, y, SDL_BUTTON_LEFT);

  Expect(handled, "Alt+left click inside the editor should be handled");
  Expect(viewport.has_multiple_carets(),
         "Alt+left click should add a secondary caret");
  Expect(!viewport.secondary_carets().empty() &&
             viewport.secondary_carets().front() == microide::editor::TextPosition{0, 0},
         "Alt+left click should preserve the previous primary caret as secondary");
  Expect(viewport.cursor_column() == 5,
         "Alt+left click should move the primary caret to the clicked column");
}

void TestWorkspaceShellAltClickKeepsTheExistingSelection() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "main.txt";
  WriteFile(source, "alpha beta\nsecond line\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, source);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::RenderFrame(shell);

  auto& viewport = WorkspaceShellTestAccess::ActiveEditor(shell);
  viewport.MoveCursorTo(0, 0);
  viewport.MoveCursorTo(0, 5, true);

  const auto metrics = WorkspaceShellTestAccess::ActiveEditorMetrics(shell);
  const float char_width = WorkspaceShellTestAccess::TextCharWidth(shell);
  const float y = metrics.first_line_y + metrics.line_height * 0.5f;

  const ScopedHeldModifiers alt_mods(shell, SDL_KMOD_ALT);
  Expect(SendMouseDown(shell, metrics.text_x + char_width * 8.0f, y, SDL_BUTTON_LEFT),
         "Alt+left click inside the editor should be handled");
  Expect(viewport.cursor_column() == 8 && !viewport.selection_range().has_value(),
         "the clicked position is the new, collapsed primary caret");
  const auto secondaries = viewport.secondary_caret_ranges();
  Expect(secondaries.size() == 1 && secondaries[0].position == microide::editor::TextPosition{0, 5} &&
             secondaries[0].selection_anchor == microide::editor::TextPosition{0, 0},
         "the previous selection survives as a secondary caret with its range");

  Expect(SendMouseDown(shell, metrics.text_x + char_width * 5.0f, y, SDL_BUTTON_LEFT),
         "the second Alt+click is handled");
  Expect(!viewport.has_multiple_carets(), "Alt+click on a secondary caret removes it");
  Expect(viewport.cursor_column() == 8, "the primary stays where it was");
}

void TestWorkspaceShellShiftAltClickAddsColumnCarets() {
  EnsureDummySdlVideoInitialized();
  ResetSdlModStateForTests();

  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "main.txt";
  WriteFile(source, "line0\nline1\nline2\nline3\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, source);
  WorkspaceShellTestAccess::SetSettingValue(shell, "editor.fold.enabled", "false");
  WorkspaceShellTestAccess::SetSettingValue(shell, "editor.fold.sticky_scroll.enabled", "false");
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::RenderFrame(shell);

  auto& viewport = WorkspaceShellTestAccess::ActiveEditor(shell);
  viewport.SetScrollLine(0);
  viewport.SetHorizontalScroll(0);
  viewport.MoveCursorTo(0, 0);
  Expect(viewport.cursor_line() == 0 && viewport.cursor_column() == 0,
         "Shift+Alt column caret fixture should start at the anchor column");

  const auto metrics = WorkspaceShellTestAccess::ActiveEditorMetrics(shell);
  viewport.SetViewportSize(metrics.visible_rows, metrics.visible_columns);
  const float click_y = metrics.first_line_y + metrics.line_height * 2.5f;
  const float click_x = metrics.text_x;
  const int visual_row = static_cast<int>(viewport.scroll_line() + 2);
  const auto preflight_hit = viewport.LogicalPositionForVisualHit(visual_row, 0);
  Expect(preflight_hit.line == 2 && preflight_hit.column == 0,
         "Shift+Alt column caret fixture should target column 0 on line 2");

  const ScopedHeldModifiers shift_alt_mods(shell, static_cast<SDL_Keymod>(SDL_KMOD_ALT | SDL_KMOD_SHIFT));
  const bool handled = SendMouseDown(shell, click_x, click_y, SDL_BUTTON_LEFT);

  Expect(handled, "Shift+Alt+left click inside the editor should be handled");
  Expect(viewport.has_multiple_carets(),
         "Shift+Alt+vertical click should add column carets");
  Expect(viewport.cursor_line() == 2 && viewport.cursor_column() == 0,
         "Shift+Alt+vertical click should move the primary caret to the clicked line");
  Expect(viewport.secondary_carets().size() == 2,
         "Shift+Alt+vertical click should add one secondary caret per other line");
  for (const microide::editor::TextPosition& caret : viewport.secondary_carets()) {
    Expect(caret.column == 0,
           "Shift+Alt+vertical click should place every caret in the anchor column");
  }
  std::vector<microide::editor::TextPosition> secondary_lines = viewport.secondary_carets();
  std::sort(secondary_lines.begin(), secondary_lines.end(),
            [](const microide::editor::TextPosition& lhs,
               const microide::editor::TextPosition& rhs) {
              return lhs.line < rhs.line || (lhs.line == rhs.line && lhs.column < rhs.column);
            });
  Expect(secondary_lines.size() == 2 && secondary_lines[0].line == 0 &&
             secondary_lines[1].line == 1,
         "Shift+Alt+vertical click should cover every line between anchor and target");
}

void TestWorkspaceShellShiftAltClickOffColumnMakesBoxSelection() {
  EnsureDummySdlVideoInitialized();
  ResetSdlModStateForTests();

  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "main.txt";
  WriteFile(source, "alpha beta\nsecond line\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, source);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::RenderFrame(shell);

  auto& viewport = WorkspaceShellTestAccess::ActiveEditor(shell);
  viewport.MoveCursorTo(0, 0);

  const auto metrics = WorkspaceShellTestAccess::ActiveEditorMetrics(shell);
  const float char_width = WorkspaceShellTestAccess::TextCharWidth(shell);
  const float y = metrics.first_line_y + metrics.line_height * 1.5f;
  const float click_x = metrics.text_x + char_width * 5.0f;

  const ScopedHeldModifiers shift_alt_mods(shell, static_cast<SDL_Keymod>(SDL_KMOD_ALT | SDL_KMOD_SHIFT));
  const bool handled = SendMouseDown(shell, click_x, y, SDL_BUTTON_LEFT);

  Expect(handled, "Shift+Alt+off-column click should still be handled");
  Expect(viewport.has_multiple_carets(),
         "Shift+Alt+off-column click should make a multi-line box selection");
  Expect(viewport.cursor_line() == 1 && viewport.cursor_column() == 5,
         "Shift+Alt+off-column click should place the primary caret on the clicked corner");
  const auto primary = viewport.selection_range();
  Expect(primary.has_value() &&
             primary->start == microide::editor::TextPosition{1, 0} &&
             primary->end == microide::editor::TextPosition{1, 5},
         "the primary line should select the box columns 0..5");
  const auto ranges = viewport.secondary_caret_ranges();
  Expect(ranges.size() == 1 && ranges.front().position == microide::editor::TextPosition{0, 5} &&
             ranges.front().selection_anchor ==
                 std::optional<microide::editor::TextPosition>(
                     microide::editor::TextPosition{0, 0}),
         "line 0 should carry a ranged secondary caret selecting columns 0..5");
}

void TestWorkspaceShellShiftAltDragUpdatesBoxSelection() {
  EnsureDummySdlVideoInitialized();
  ResetSdlModStateForTests();

  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "main.txt";
  WriteFile(source, "aaaaaaaa\nbbbbbbbb\ncccccccc\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, source);
  WorkspaceShellTestAccess::SetSettingValue(shell, "editor.fold.enabled", "false");
  WorkspaceShellTestAccess::SetSettingValue(shell, "editor.fold.sticky_scroll.enabled", "false");
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::RenderFrame(shell);

  auto& viewport = WorkspaceShellTestAccess::ActiveEditor(shell);
  viewport.SetScrollLine(0);
  viewport.SetHorizontalScroll(0);
  const auto metrics = WorkspaceShellTestAccess::ActiveEditorMetrics(shell);
  viewport.SetViewportSize(metrics.visible_rows, metrics.visible_columns);
  const float char_width = WorkspaceShellTestAccess::TextCharWidth(shell);

  viewport.MoveCursorTo(0, 2);
  const float press_x = metrics.text_x + char_width * 2.0f;
  const float press_y = metrics.first_line_y + metrics.line_height * 0.5f;
  const ScopedHeldModifiers shift_alt_mods(shell, static_cast<SDL_Keymod>(SDL_KMOD_ALT | SDL_KMOD_SHIFT));
  Expect(SendMouseDown(shell, press_x, press_y, SDL_BUTTON_LEFT),
         "Shift+Alt press should be handled");

  const float drag_x = metrics.text_x + char_width * 6.0f;
  const float drag_y = metrics.first_line_y + metrics.line_height * 2.5f;
  Expect(SendMouseMotion(shell, drag_x, drag_y, SDL_BUTTON_LMASK),
         "Shift+Alt drag motion should be handled");

  Expect(viewport.cursor_line() == 2 && viewport.cursor_column() == 6,
         "the box drag should track the pointer to the caret corner");
  const auto primary = viewport.selection_range();
  Expect(primary.has_value() &&
             primary->start == microide::editor::TextPosition{2, 2} &&
             primary->end == microide::editor::TextPosition{2, 6},
         "the primary line should select the dragged box columns 2..6");
  const auto ranges = viewport.secondary_caret_ranges();
  Expect(ranges.size() == 2, "the box drag should span lines 0 and 1 as secondaries");
  for (const auto& caret : ranges) {
    Expect(caret.selection_anchor.has_value() &&
               caret.selection_anchor->column == 2 && caret.position.column == 6,
           "every dragged secondary line should select columns 2..6");
  }

  Expect(SendMouseUp(shell, drag_x, drag_y, SDL_BUTTON_LEFT),
         "mouse-up should end the box selection gesture");
}

void TestWorkspaceShellHoveredTabShowsRelativePathTooltip() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "src" / "deep" / "main.cpp";
  WriteFile(source, "int main() {\n  return 0;\n}\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenFile(shell, source);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);

  const SDL_FRect tab_rect = WorkspaceShellTestAccess::EditorTabRect(shell, 0);
  SendMouseMotion(shell, tab_rect.x + tab_rect.w * 0.5f,
                                              tab_rect.y + tab_rect.h * 0.5f, 0);
  Expect(WorkspaceShellTestAccess::HoveredTooltipLabel(shell) == "src/deep/main.cpp",
         "hovering a tab should expose the full relative path tooltip");
}

void TestWorkspaceShellWindowMouseLeaveClearsTabTooltip() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "src" / "deep" / "main.cpp";
  WriteFile(source, "int main() {\n  return 0;\n}\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenFile(shell, source);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);

  const SDL_FRect tab_rect = WorkspaceShellTestAccess::EditorTabRect(shell, 0);
  SendMouseMotion(shell, tab_rect.x + tab_rect.w * 0.5f,
                                              tab_rect.y + tab_rect.h * 0.5f, 0);
  Expect(WorkspaceShellTestAccess::HoveredTooltipLabel(shell) == "src/deep/main.cpp",
         "tab tooltip fixture should start with a hovered tab label");

  Expect(SendWindowMouseLeave(shell),
         "window mouse leave should be handled");
  Expect(WorkspaceShellTestAccess::HoveredTooltipLabel(shell).empty(),
         "window mouse leave should clear stale tab tooltip hover state");
}

void TestWorkspaceShellInWindowMouseMoveClearsProjectTabTooltipAndInvalidatesChrome() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path first_root = temp_dir.path() / "first-project";
  const std::filesystem::path second_root = temp_dir.path() / "second-project";
  const std::filesystem::path second_file = second_root / "src" / "deep" / "main.cpp";
  WriteFile(first_root / "README.md", "first\n");
  WriteFile(second_file, "int main() {\n  return 0;\n}\n");

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, first_root, false, false),
         "first project should open");
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, second_root, false, false),
         "second project should open");
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);

  const SDL_FRect project_tab_rect = WorkspaceShellTestAccess::ProjectTabRect(shell, 1);
  SendMouseMotion(shell, project_tab_rect.x + project_tab_rect.w * 0.5f,
                  project_tab_rect.y + project_tab_rect.h * 0.5f, 0);
  const std::string hovered_label_before =
      WorkspaceShellTestAccess::HoveredTooltipLabel(shell);
  Expect(!hovered_label_before.empty(),
         "project tab tooltip fixture should start with a hovered project tab label");
  (void)shell.ConsumePendingRenderInvalidation();

  const SDL_FRect editor_rect = WorkspaceShellTestAccess::ActiveEditorPaneRect(shell);
  SendMouseMotion(shell, editor_rect.x + 20.0f, editor_rect.y + 20.0f, 0);
  Expect(WorkspaceShellTestAccess::HoveredTooltipLabel(shell).empty(),
         "moving inside the window away from project tabs should clear project tab tooltip hover");
}

void TestWorkspaceShellEditorSelectionWritesPrimaryBufferAndMiddleClickPastes() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path file = root / "main.txt";
  WriteFile(file, "hello world\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenFile(shell, file);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);

  std::string primary_selection;
  WorkspaceShellTestAccess::SetPrimarySelectionTextWriter(
      shell, [&](std::string_view text) {
        primary_selection = std::string(text);
        return true;
      });
  WorkspaceShellTestAccess::SetPrimarySelectionTextReader(
      shell, [&]() -> std::optional<std::string> { return primary_selection; });

  const SDL_FRect pane = WorkspaceShellTestAccess::ActiveEditorPaneRect(shell);
  const auto metrics = WorkspaceShellTestAccess::ActiveEditorMetrics(shell);
  const float start_x = metrics.text_x + 1.0f;
  const float end_x = metrics.text_x + WorkspaceShellTestAccess::TextCharWidth(shell) * 5.0f + 1.0f;
  const float y = metrics.first_line_y + std::min(4.0f, pane.h * 0.25f);

  Expect(SendMouseDown(shell, start_x, y, SDL_BUTTON_LEFT),
         "starting an editor drag selection should be handled");
  Expect(SendMouseMotion(shell, end_x, y, SDL_BUTTON_LMASK),
         "dragging an editor selection should be handled");
  Expect(SendMouseUp(shell, end_x, y, SDL_BUTTON_LEFT),
         "releasing an editor selection should be handled");
  Expect(primary_selection == "hello",
         "editor drag selection should update the primary selection buffer");

  const float paste_x = metrics.text_x + WorkspaceShellTestAccess::TextCharWidth(shell) * 11.0f + 1.0f;
  Expect(SendMouseDown(shell, paste_x, y, SDL_BUTTON_MIDDLE),
         "middle-clicking the editor should be handled");
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).lines()[0] == "hello worldhello",
         "middle-clicking the editor should paste the primary selection at the click location");
}

void TestWorkspaceShellTextInputSurfaceTracksEditorOverlayAndPrompt() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path file = root / "main.txt";
  WriteFile(file, "hello\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenFile(shell, file);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);

  Expect(WorkspaceShellTestAccess::TextInputSurfaceIsEditor(shell),
         "the active editor should own text input by default");

  Expect(WorkspaceShellTestAccess::ExecuteFilesFromShortcut(shell),
         "opening the file-finder overlay should be handled");
  Expect(WorkspaceShellTestAccess::TextInputSurfaceIsFileFinder(shell),
         "the file-finder overlay should take text-input ownership when visible");

  WorkspaceShellTestAccess::PrepareRenamePrompt(shell, file, "renamed.txt");
  Expect(WorkspaceShellTestAccess::PromptSurfaceVisible(shell),
         "rename prompt fixture should open the prompt surface");
  Expect(WorkspaceShellTestAccess::TextInputSurfaceIsPromptInput(shell),
         "prompt input should override the underlying overlay or editor text-input surface");
}

void TestWorkspaceShellSidebarModeTabsSwitchView() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "README.md", "hello\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);

  Expect(WorkspaceShellTestAccess::SidebarModeOverflowRect(shell).w > 0.0f,
         "the builtin overflow views should give the mode row an overflow button");

  const auto click_tab = [&](std::string_view id) {
    const SDL_FRect tab = WorkspaceShellTestAccess::SidebarModeTabRect(shell, id);
    Expect(tab.w > 0.0f, "each primary view should expose a clickable mode tab");
    return SendMouseDown(shell, tab.x + tab.w * 0.5f, tab.y + tab.h * 0.5f, SDL_BUTTON_LEFT);
  };

  Expect(click_tab("git"), "clicking the Source Control tab should be handled");
  Expect(!WorkspaceShellTestAccess::MenuBarOpen(shell),
         "switching views via a tab should not open any menu");
  Expect(WorkspaceShellTestAccess::SidebarMode(shell) == WorkspaceShell::SidebarMode::Git,
         "clicking the Source Control tab should activate Git mode");
  Expect(WorkspaceShellTestAccess::FocusIsSidebar(shell),
         "switching views via a tab should keep sidebar focus");

  Expect(click_tab("search"), "clicking the Search tab should be handled");
  Expect(WorkspaceShellTestAccess::SidebarMode(shell) == WorkspaceShell::SidebarMode::Search,
         "clicking the Search tab should activate Search mode");

  Expect(click_tab("tree"), "clicking the Project tab should be handled");
  Expect(WorkspaceShellTestAccess::SidebarMode(shell) == WorkspaceShell::SidebarMode::Tree,
         "clicking the Project tab should activate Tree mode");

  const SDL_FRect overflow = WorkspaceShellTestAccess::SidebarModeOverflowRect(shell);
  Expect(SendMouseDown(shell, overflow.x + overflow.w * 0.5f, overflow.y + overflow.h * 0.5f,
                       SDL_BUTTON_LEFT),
         "clicking the mode-row overflow button should be handled");
  const auto overflow_labels = WorkspaceShellTestAccess::SidebarModeMenuLabels(shell);
  const auto lists = [&](std::string_view label) {
    return std::any_of(overflow_labels.begin(), overflow_labels.end(),
                       [&](const std::string& item) { return item == label; });
  };
  Expect(lists("Problems") && lists("Tests") && lists("Outline"),
         "the mode-row overflow menu should list every view that has no tab");
  const auto problems_rect = WorkspaceShellTestAccess::SidebarModeMenuItemRect(shell, "Problems");
  Expect(problems_rect.has_value(), "the overflow menu should expose a Problems entry rect");
  Expect(SendMouseDown(shell, problems_rect->x + problems_rect->w * 0.5f,
                       problems_rect->y + problems_rect->h * 0.5f, SDL_BUTTON_LEFT),
         "clicking the Problems entry should be handled");
  Expect(WorkspaceShellTestAccess::SidebarMode(shell) == WorkspaceShell::SidebarMode::Problems,
         "choosing Problems from the overflow menu should activate the Problems view");
}

void TestWorkspaceShellProjectTabsDragReorderToEnd() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root_a = temp_dir.path() / "alpha-project";
  const std::filesystem::path root_b = temp_dir.path() / "beta-project";
  const std::filesystem::path root_c = temp_dir.path() / "gamma-project";
  WriteFile(root_a / "README.md", "alpha\n");
  WriteFile(root_b / "README.md", "beta\n");
  WriteFile(root_c / "README.md", "gamma\n");

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root_a, false, false),
         "first project should open");
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root_b, false, false),
         "second project should open");
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root_c, false, false),
         "third project should open");
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);

  const SDL_FRect source_rect = WorkspaceShellTestAccess::ProjectTabRect(shell, 0);
  Expect(SendMouseDown(
             shell, source_rect.x + source_rect.w * 0.5f, source_rect.y + source_rect.h * 0.5f,
             SDL_BUTTON_LEFT),
         "dragging should start from a project tab press");

  const SDL_FRect last_rect = WorkspaceShellTestAccess::ProjectTabRect(shell, 2);
  const float drop_x = last_rect.x + last_rect.w + 12.0f;
  const float drop_y = last_rect.y + last_rect.h * 0.5f;
  Expect(SendMouseMotion(shell, drop_x, drop_y, SDL_BUTTON_LMASK),
         "dragging across the project tab strip should be handled");
  Expect(SendMouseUp(shell, drop_x, drop_y, SDL_BUTTON_LEFT),
         "releasing a dragged project tab should be handled");

  Expect(WorkspaceShellTestAccess::ProjectRoots(shell) ==
             std::vector<std::filesystem::path>{root_b.lexically_normal(),
                                                root_c.lexically_normal(),
                                                root_a.lexically_normal()},
         "dragging a project tab to the end should reorder the project strip");
  Expect(WorkspaceShellTestAccess::ActiveProjectIndex(shell) == 2,
         "dragged project tab should stay active after reordering");
  Expect(WorkspaceShellTestAccess::ProjectRoot(shell) == root_a.lexically_normal(),
         "dragged project should remain the active workspace");
}

void TestWorkspaceShellProjectOpenExistingRootSwitchesWithoutDuplicatingCatalog() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root_a = temp_dir.path() / "alpha-project";
  const std::filesystem::path root_b = temp_dir.path() / "beta-project";
  WriteFile(root_a / "README.md", "alpha\n");
  WriteFile(root_b / "README.md", "beta\n");

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root_a, false, false),
         "first project should open");
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root_b, false, false),
         "second project should open");
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root_a, false, false),
         "reopening an existing project root should succeed");

  Expect(WorkspaceShellTestAccess::ProjectCount(shell) == 2,
         "reopening an existing project should not duplicate the project catalog");
  Expect(WorkspaceShellTestAccess::ActiveProjectIndex(shell) == 0,
         "reopening an existing project should activate its existing project tab");
  Expect(WorkspaceShellTestAccess::ProjectRoot(shell) == root_a.lexically_normal(),
         "reopening an existing project should restore that project as active");
}

void TestWorkspaceShellProjectOpenFailureRestoresPreviousActiveProject() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path readme = root / "README.md";
  WriteFile(readme, "alpha\n");

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "baseline project should open");
  Expect(!WorkspaceShellTestAccess::OpenProjectTab(shell, readme, false, false),
         "opening a file path as a project should fail");

  Expect(WorkspaceShellTestAccess::ProjectCount(shell) == 1,
         "failed project opens should roll back their catalog insertion");
  Expect(WorkspaceShellTestAccess::ActiveProjectIndex(shell) == 0,
         "failed project opens should preserve the previous active project index");
  Expect(WorkspaceShellTestAccess::ProjectRoot(shell) == root.lexically_normal(),
         "failed project opens should restore the previous active workspace");
  Expect(WorkspaceShellTestAccess::OpenTabs(shell).size() == 1 &&
             WorkspaceShellTestAccess::OpenTabs(shell).front().path == readme.lexically_normal(),
         "failed project opens should keep the previous project's tab state intact");
}

void TestWorkspaceShellCloseActiveProjectRestoresAdjacentProject() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root_a = temp_dir.path() / "alpha-project";
  const std::filesystem::path root_b = temp_dir.path() / "beta-project";
  const std::filesystem::path root_c = temp_dir.path() / "gamma-project";
  WriteFile(root_a / "README.md", "alpha\n");
  WriteFile(root_b / "README.md", "beta\n");
  WriteFile(root_c / "README.md", "gamma\n");

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root_a, false, false),
         "first project should open");
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root_b, false, false),
         "second project should open");
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root_c, false, false),
         "third project should open");
  Expect(WorkspaceShellTestAccess::SwitchProject(shell, 1, false),
         "middle project should become active before the close test");

  WorkspaceShellTestAccess::CloseProject(shell, 1);

  Expect(WorkspaceShellTestAccess::ProjectRoots(shell) ==
             std::vector<std::filesystem::path>{root_a.lexically_normal(),
                                                root_c.lexically_normal()},
         "closing the active project should remove only that project from the catalog");
  Expect(WorkspaceShellTestAccess::ActiveProjectIndex(shell) == 1,
         "closing the active middle project should restore the adjacent project at that slot");
  Expect(WorkspaceShellTestAccess::ProjectRoot(shell) == root_c.lexically_normal(),
         "closing the active middle project should activate the adjacent surviving workspace");
}

void TestWorkspaceShellEditorTabsDragReorderBetweenTabs() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path file_a = root / "alpha.cpp";
  const std::filesystem::path file_b = root / "beta.cpp";
  const std::filesystem::path file_c = root / "gamma.cpp";
  WriteFile(file_a, "int alpha() { return 1; }\n");
  WriteFile(file_b, "int beta() { return 2; }\n");
  WriteFile(file_c, "int gamma() { return 3; }\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, file_a),
         "first editor tab should open");
  Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, file_b),
         "second editor tab should open");
  Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, file_c),
         "third editor tab should open");
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);

  const SDL_FRect source_rect = WorkspaceShellTestAccess::EditorTabRect(shell, 0);
  Expect(SendMouseDown(
             shell, source_rect.x + source_rect.w * 0.5f, source_rect.y + source_rect.h * 0.5f,
             SDL_BUTTON_LEFT),
         "dragging should start from an editor tab press");

  const SDL_FRect third_rect = WorkspaceShellTestAccess::EditorTabRect(shell, 2);
  const float drop_x = third_rect.x + 1.0f;
  const float drop_y = third_rect.y + third_rect.h * 0.5f;
  Expect(SendMouseMotion(shell, drop_x, drop_y, SDL_BUTTON_LMASK),
         "dragging across editor tabs should be handled");
  Expect(SendMouseUp(shell, drop_x, drop_y, SDL_BUTTON_LEFT),
         "releasing a dragged editor tab should be handled");

  const auto& tabs = WorkspaceShellTestAccess::OpenTabs(shell);
  Expect(tabs.size() == 3, "editor tab reorder should keep the same tab count");
  Expect(tabs[0].path == file_b.lexically_normal() && tabs[1].path == file_a.lexically_normal() &&
             tabs[2].path == file_c.lexically_normal(),
         "dragging an editor tab between tabs should reorder it into the requested slot");
  Expect(WorkspaceShellTestAccess::ActiveTabIndex(shell) == 1,
         "dragged editor tab should stay active after reordering");
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).path() == file_a.lexically_normal(),
         "dragged editor tab should keep its buffer active");
  const auto visible_titles = WorkspaceShellTestAccess::EditorTabDisplayTitles(shell);
  Expect(visible_titles.size() == 3,
         "tab reorder should not change the number of visible tab labels");
  Expect(visible_titles[0] == "beta.cpp" && visible_titles[1] == "alpha.cpp" &&
             visible_titles[2] == "gamma.cpp",
         "rendered tab labels must follow the reordered open_tabs vector — the "
         "tab_strip_geometry_cache_ must invalidate on every reorder");
}

void TestWorkspaceShellProjectTabRightClickOpensContextMenu() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root_a = temp_dir.path() / "alpha-project";
  const std::filesystem::path root_b = temp_dir.path() / "beta-project";
  WriteFile(root_a / "README.md", "alpha\n");
  WriteFile(root_b / "README.md", "beta\n");

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root_a, false, false), "project a opens");
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root_b, false, false), "project b opens");
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);

  const SDL_FRect tab_rect = WorkspaceShellTestAccess::ProjectTabRect(shell, 0);
  Expect(SendMouseDown(shell, tab_rect.x + tab_rect.w * 0.5f, tab_rect.y + tab_rect.h * 0.5f,
                       SDL_BUTTON_RIGHT),
         "right-clicking a project tab should be handled");
  Expect(WorkspaceShellTestAccess::ProjectTabContextMenuOpen(shell),
         "right-clicking a project tab should open the project tab context menu");
  Expect(WorkspaceShellTestAccess::ActiveProjectIndex(shell) == 0,
         "right-clicking a project tab should retarget the active project before menu actions run");
}

void TestWorkspaceShellEditorTabDragDefersCommitUntilRelease() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path file_a = root / "alpha.cpp";
  const std::filesystem::path file_b = root / "beta.cpp";
  const std::filesystem::path file_c = root / "gamma.cpp";
  WriteFile(file_a, "int alpha() { return 1; }\n");
  WriteFile(file_b, "int beta() { return 2; }\n");
  WriteFile(file_c, "int gamma() { return 3; }\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, file_a), "first tab opens");
  Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, file_b), "second tab opens");
  Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, file_c), "third tab opens");
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);

  const SDL_FRect source_rect = WorkspaceShellTestAccess::EditorTabRect(shell, 0);
  Expect(SendMouseDown(shell, source_rect.x + source_rect.w * 0.5f,
                       source_rect.y + source_rect.h * 0.5f, SDL_BUTTON_LEFT),
         "press starts a drag");

  const SDL_FRect third_rect = WorkspaceShellTestAccess::EditorTabRect(shell, 2);
  const float drop_x = third_rect.x + third_rect.w * 0.75f;
  const float drop_y = third_rect.y + third_rect.h * 0.5f;
  Expect(SendMouseMotion(shell, drop_x, drop_y, SDL_BUTTON_LMASK), "motion handled");

  const auto& mid_tabs = WorkspaceShellTestAccess::OpenTabs(shell);
  Expect(mid_tabs.size() == 3 && mid_tabs[0].path == file_a.lexically_normal() &&
             mid_tabs[1].path == file_b.lexically_normal() &&
             mid_tabs[2].path == file_c.lexically_normal(),
         "tab order must stay unchanged while dragging (deferred commit)");
  Expect(WorkspaceShellTestAccess::TabDrag(shell).dragging,
         "drag should be active mid-gesture");
  Expect(WorkspaceShellTestAccess::TabDrag(shell).reordered,
         "a target past the last tab should mark a pending reorder");

  Expect(SendMouseUp(shell, drop_x, drop_y, SDL_BUTTON_LEFT), "release commits the reorder");
  const auto& tabs = WorkspaceShellTestAccess::OpenTabs(shell);
  Expect(tabs.size() == 3 && tabs[0].path == file_b.lexically_normal() &&
             tabs[1].path == file_c.lexically_normal() &&
             tabs[2].path == file_a.lexically_normal(),
         "release should move the dragged tab to the end in a single commit");
  Expect(WorkspaceShellTestAccess::ActiveTabIndex(shell) == 2,
         "dragged tab stays active after the deferred commit");
  Expect(WorkspaceShellTestAccess::TabDrag(shell).kind == microide::workspace::TabDragKind::None,
         "drag state clears after release");
}

void TestWorkspaceShellEditorTabDragTargetSlotTracksPointer() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path file_a = root / "alpha.cpp";
  const std::filesystem::path file_b = root / "beta.cpp";
  const std::filesystem::path file_c = root / "gamma.cpp";
  WriteFile(file_a, "a\n");
  WriteFile(file_b, "b\n");
  WriteFile(file_c, "c\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, file_a), "tab a opens");
  Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, file_b), "tab b opens");
  Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, file_c), "tab c opens");
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);

  const SDL_FRect source_rect = WorkspaceShellTestAccess::EditorTabRect(shell, 0);
  Expect(SendMouseDown(shell, source_rect.x + source_rect.w * 0.5f,
                       source_rect.y + source_rect.h * 0.5f, SDL_BUTTON_LEFT),
         "press starts a drag");

  const SDL_FRect second_rect = WorkspaceShellTestAccess::EditorTabRect(shell, 1);
  Expect(SendMouseMotion(shell, second_rect.x + 1.0f, second_rect.y + second_rect.h * 0.5f,
                         SDL_BUTTON_LMASK),
         "motion into the second tab's left half is handled");
  Expect(WorkspaceShellTestAccess::TabDrag(shell).target_slot == 1,
         "target slot should track the insertion gap under the pointer");

  const SDL_FRect third_rect = WorkspaceShellTestAccess::EditorTabRect(shell, 2);
  Expect(SendMouseMotion(shell, third_rect.x + third_rect.w + 20.0f,
                         third_rect.y + third_rect.h * 0.5f, SDL_BUTTON_LMASK),
         "motion past the last tab is handled");
  Expect(WorkspaceShellTestAccess::TabDrag(shell).target_slot == 3,
         "dropping past the last tab should target the trailing slot");

  Expect(SendMouseUp(shell, third_rect.x + third_rect.w + 20.0f, third_rect.y + 1.0f,
                     SDL_BUTTON_LEFT),
         "release handled");
}

void TestWorkspaceShellEditorTabDragSeedsSlideAnimation() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path file_a = root / "alpha.cpp";
  const std::filesystem::path file_b = root / "beta.cpp";
  const std::filesystem::path file_c = root / "gamma.cpp";
  WriteFile(file_a, "a\n");
  WriteFile(file_b, "b\n");
  WriteFile(file_c, "c\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, file_a), "tab a opens");
  Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, file_b), "tab b opens");
  Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, file_c), "tab c opens");
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);

  const SDL_FRect source_rect = WorkspaceShellTestAccess::EditorTabRect(shell, 0);
  Expect(SendMouseDown(shell, source_rect.x + source_rect.w * 0.5f,
                       source_rect.y + source_rect.h * 0.5f, SDL_BUTTON_LEFT),
         "press starts a drag on tab 0");

  const SDL_FRect third_rect = WorkspaceShellTestAccess::EditorTabRect(shell, 2);
  Expect(SendMouseMotion(shell, third_rect.x + 1.0f, third_rect.y + third_rect.h * 0.5f,
                         SDL_BUTTON_LMASK),
         "dragging across the strip is handled");

  const auto& slide = WorkspaceShellTestAccess::TabSlide(shell).strips[0];
  Expect(slide.kind == microide::workspace::TabDragKind::Editor,
         "an in-flight editor drag arms the editor tab-slide animation");
  Expect(!WorkspaceShellTestAccess::TabSlide(shell).settling,
         "the slide is in its drag phase, not settling");
  Expect(slide.target.size() == 3, "one slide target per model tab");
  Expect(slide.target[1] < -1.0f,
         "tab 1 targets a leftward offset to fill the dragged tab's vacated slot");

  Expect(SendMouseUp(shell, third_rect.x + 1.0f, third_rect.y + third_rect.h * 0.5f,
                     SDL_BUTTON_LEFT),
         "release commits the reorder");

  const auto& settle = WorkspaceShellTestAccess::TabSlide(shell);
  Expect(settle.strips[0].kind == microide::workspace::TabDragKind::Editor && settle.settling,
         "release hands off to a settle glide that outlives the drag state");
  Expect(WorkspaceShellTestAccess::TabDrag(shell).kind == microide::workspace::TabDragKind::None,
         "the drag state itself clears on release");
}

void TestWorkspaceShellEditorTabDragHomeStillGlides() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path file_a = root / "alpha.cpp";
  const std::filesystem::path file_b = root / "beta.cpp";
  WriteFile(file_a, "a\n");
  WriteFile(file_b, "b\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, file_a), "tab a opens");
  Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, file_b), "tab b opens");
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);

  const SDL_FRect source_rect = WorkspaceShellTestAccess::EditorTabRect(shell, 0);
  const float cx = source_rect.x + source_rect.w * 0.5f;
  const float cy = source_rect.y + source_rect.h * 0.5f;
  Expect(SendMouseDown(shell, cx, cy, SDL_BUTTON_LEFT), "press starts a drag");
  Expect(SendMouseMotion(shell, cx + 10.0f, cy, SDL_BUTTON_LMASK), "small drag is handled");
  Expect(!WorkspaceShellTestAccess::TabDrag(shell).reordered,
         "staying over the source slot marks no pending reorder");

  Expect(SendMouseUp(shell, cx + 10.0f, cy, SDL_BUTTON_LEFT), "release handled");
  const auto& settle = WorkspaceShellTestAccess::TabSlide(shell);
  Expect(settle.settling && settle.strips[0].kind == microide::workspace::TabDragKind::Editor,
         "even a no-reorder drop glides the lifted tab back home instead of snapping");
}

void TestWorkspaceShellEditorTabDragSlotFollowsTheTabNotTheGrabPoint() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path file_a = root / "alpha.cpp";
  const std::filesystem::path file_b = root / "beta.cpp";
  const std::filesystem::path file_c = root / "gamma.cpp";
  WriteFile(file_a, "a\n");
  WriteFile(file_b, "b\n");
  WriteFile(file_c, "c\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, file_a), "tab a opens");
  Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, file_b), "tab b opens");
  Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, file_c), "tab c opens");
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);

  const SDL_FRect source_rect = WorkspaceShellTestAccess::EditorTabRect(shell, 0);
  const SDL_FRect second_rect = WorkspaceShellTestAccess::EditorTabRect(shell, 1);
  Expect(source_rect.w > 24.0f, "the fixture needs tabs wide enough for a grab offset to matter");

  const float grab_x = source_rect.x + 2.0f;
  const float grab_y = source_rect.y + source_rect.h * 0.5f;
  Expect(SendMouseDown(shell, grab_x, grab_y, SDL_BUTTON_LEFT), "press starts a drag");

  const float second_midpoint = second_rect.x + second_rect.w * 0.5f;
  Expect(SendMouseMotion(shell, second_midpoint - 4.0f, grab_y, SDL_BUTTON_LMASK),
         "dragging is handled");
  Expect(WorkspaceShellTestAccess::TabDrag(shell).reordered,
         "the dragged tab's centre passed tab 1's midpoint, so a reorder is pending "
         "even though the cursor has not");

  Expect(SendMouseUp(shell, second_midpoint - 4.0f, grab_y, SDL_BUTTON_LEFT), "release handled");
  const auto& tabs = WorkspaceShellTestAccess::OpenTabs(shell);
  Expect(tabs.size() == 3 && tabs[0].path == file_b.lexically_normal() &&
             tabs[1].path == file_a.lexically_normal(),
         "the drop lands where the dragged tab was drawn, not where the cursor was");
}

void TestWorkspaceShellEditorTabDropCarriesNeighborEaseAcrossTheCommit() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path file_a = root / "alpha.cpp";
  const std::filesystem::path file_b = root / "beta.cpp";
  const std::filesystem::path file_c = root / "gamma.cpp";
  WriteFile(file_a, "a\n");
  WriteFile(file_b, "b\n");
  WriteFile(file_c, "c\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, file_a), "tab a opens");
  Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, file_b), "tab b opens");
  Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, file_c), "tab c opens");
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);

  const SDL_FRect source_rect = WorkspaceShellTestAccess::EditorTabRect(shell, 0);
  const SDL_FRect third_rect = WorkspaceShellTestAccess::EditorTabRect(shell, 2);
  const float cy = source_rect.y + source_rect.h * 0.5f;
  Expect(SendMouseDown(shell, source_rect.x + source_rect.w * 0.5f, cy, SDL_BUTTON_LEFT),
         "press starts a drag on tab 0");
  const float drop_x = third_rect.x + third_rect.w + 20.0f;
  Expect(SendMouseMotion(shell, drop_x, cy, SDL_BUTTON_LMASK), "drag past the last tab");

  const microide::workspace::TabStripSlide during =
      WorkspaceShellTestAccess::TabSlide(shell).strips[0];
  Expect(during.current.size() == 3 && during.target.size() == 3,
         "the drag arms one slide offset per tab");
  const float residual_for_tab_1 = during.current[1] - during.target[1];
  const float residual_for_tab_2 = during.current[2] - during.target[2];

  Expect(SendMouseUp(shell, drop_x, cy, SDL_BUTTON_LEFT), "release commits the reorder");
  const auto& settle_state = WorkspaceShellTestAccess::TabSlide(shell);
  const auto& settle = settle_state.strips[0];
  Expect(settle_state.settling && settle.current.size() == 3,
         "release hands off to a settle glide");
  Expect(std::fabs(settle.current[0] - residual_for_tab_1) < 1e-3f,
         "the first neighbour keeps its unfinished ease instead of teleporting");
  Expect(std::fabs(settle.current[1] - residual_for_tab_2) < 1e-3f,
         "the second neighbour keeps its unfinished ease instead of teleporting");
}

void TestWorkspaceShellEditorTabDragHidesTheHoverTooltip() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path file_a = root / "alpha.cpp";
  const std::filesystem::path file_b = root / "beta.cpp";
  const std::filesystem::path file_c = root / "gamma.cpp";
  WriteFile(file_a, "a\n");
  WriteFile(file_b, "b\n");
  WriteFile(file_c, "c\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, file_a), "tab a opens");
  Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, file_b), "tab b opens");
  Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, file_c), "tab c opens");
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);

  const SDL_FRect source_rect = WorkspaceShellTestAccess::EditorTabRect(shell, 0);
  const float cx = source_rect.x + source_rect.w * 0.5f;
  const float cy = source_rect.y + source_rect.h * 0.5f;
  SendMouseMotion(shell, cx, cy, 0);
  Expect(!WorkspaceShellTestAccess::HoveredTooltipLabel(shell).empty(),
         "hovering a tab shows its path tooltip");

  Expect(SendMouseDown(shell, cx, cy, SDL_BUTTON_LEFT), "press starts a drag");
  const SDL_FRect third_rect = WorkspaceShellTestAccess::EditorTabRect(shell, 2);
  Expect(SendMouseMotion(shell, third_rect.x + 1.0f, cy, SDL_BUTTON_LMASK), "drag is handled");
  Expect(WorkspaceShellTestAccess::TabDrag(shell).dragging, "the drag is live");
  Expect(WorkspaceShellTestAccess::HoveredTooltipLabel(shell).empty(),
         "no tooltip is shown while a tab is being dragged");

  Expect(SendMouseUp(shell, third_rect.x + 1.0f, cy, SDL_BUTTON_LEFT), "release handled");
  Expect(!WorkspaceShellTestAccess::HoveredTooltipLabel(shell).empty(),
         "the tooltip comes back once the drag ends");
}

void TestWorkspaceShellEditorTabDragIsAbandonedByEscapeAndFocusLoss() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path file_a = root / "alpha.cpp";
  const std::filesystem::path file_b = root / "beta.cpp";
  const std::filesystem::path file_c = root / "gamma.cpp";
  WriteFile(file_a, "a\n");
  WriteFile(file_b, "b\n");
  WriteFile(file_c, "c\n");

  const auto open_three = [&](WorkspaceShell& shell) {
    WorkspaceShellTestAccess::SetProjectRoot(shell, root);
    Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, file_a), "tab a opens");
    Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, file_b), "tab b opens");
    Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, file_c), "tab c opens");
    WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  };
  const auto start_drag_past_the_end = [&](WorkspaceShell& shell) {
    const SDL_FRect source_rect = WorkspaceShellTestAccess::EditorTabRect(shell, 0);
    const SDL_FRect third_rect = WorkspaceShellTestAccess::EditorTabRect(shell, 2);
    const float cy = source_rect.y + source_rect.h * 0.5f;
    Expect(SendMouseDown(shell, source_rect.x + source_rect.w * 0.5f, cy, SDL_BUTTON_LEFT),
           "press starts a drag on tab 0");
    Expect(SendMouseMotion(shell, third_rect.x + third_rect.w + 20.0f, cy, SDL_BUTTON_LMASK),
           "drag past the last tab");
    Expect(WorkspaceShellTestAccess::TabDrag(shell).reordered, "a reorder is pending");
  };
  const auto expect_unmoved = [&](WorkspaceShell& shell, const char* how) {
    Expect(WorkspaceShellTestAccess::TabDrag(shell).kind == microide::workspace::TabDragKind::None,
           how);
    const auto& tabs = WorkspaceShellTestAccess::OpenTabs(shell);
    Expect(tabs.size() == 3 && tabs[0].path == file_a.lexically_normal() &&
               tabs[2].path == file_c.lexically_normal(),
           "an abandoned drag commits nothing");
    Expect(WorkspaceShellTestAccess::TabSlide(shell).settling,
           "the lifted tab still glides home rather than snapping back");
  };

  {
    WorkspaceShell shell;
    open_three(shell);
    start_drag_past_the_end(shell);
    Expect(SendKeyDown(shell, SDLK_ESCAPE, SDL_KMOD_NONE), "Escape is handled by the live drag");
    expect_unmoved(shell, "Escape clears the drag state");
  }
  {
    WorkspaceShell shell;
    open_three(shell);
    start_drag_past_the_end(shell);
    SDL_Event focus_lost{};
    focus_lost.type = SDL_EVENT_WINDOW_FOCUS_LOST;
    Expect(shell.HandleEvent(focus_lost).handled, "focus loss is handled");
    expect_unmoved(shell, "losing focus clears the drag state");
  }
}

void TestWorkspaceShellEditorTabDragMovesTabToTheOtherGroup() {
  using microide::workspace::EditorSplitOrientation;
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path file_a = root / "alpha.cpp";
  const std::filesystem::path file_b = root / "beta.cpp";
  WriteFile(file_a, "a\n");
  WriteFile(file_b, "b\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, file_a), "tab a opens");
  Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, file_b), "tab b opens");
  WorkspaceShellTestAccess::SetWindowSize(shell, 1600, 900);
  Expect(WorkspaceShellTestAccess::SplitEditorGroup(shell, EditorSplitOrientation::Vertical),
         "the fixture needs a second editor group");
  Expect(WorkspaceShellTestAccess::FocusOtherEditorGroup(shell), "focus returns to group 0");
  Expect(WorkspaceShellTestAccess::FocusedGroupIndex(shell) == 0, "group 0 owns the drag");
  Expect(WorkspaceShellTestAccess::GroupTabCount(shell, 0) == 2, "group 0 starts with two tabs");
  Expect(WorkspaceShellTestAccess::GroupTabCount(shell, 1) == 1, "group 1 starts with the clone");

  const SDL_FRect source_rect = WorkspaceShellTestAccess::GroupEditorTabRect(shell, 0, 0);
  const SDL_FRect other_strip = WorkspaceShellTestAccess::GroupTabStripRect(shell, 1);
  Expect(source_rect.w > 0.0f && other_strip.w > 0.0f, "both strips have geometry");
  const float grab_x = source_rect.x + source_rect.w * 0.5f;
  const float grab_y = source_rect.y + source_rect.h * 0.5f;
  Expect(SendMouseDown(shell, grab_x, grab_y, SDL_BUTTON_LEFT), "press starts a drag");

  const float drop_x = other_strip.x + other_strip.w - 8.0f;
  const float drop_y = other_strip.y + other_strip.h * 0.5f;
  Expect(SendMouseMotion(shell, drop_x, drop_y, SDL_BUTTON_LMASK), "the drag crosses the divider");
  Expect(WorkspaceShellTestAccess::TabDrag(shell).cross_group(),
         "the pointer over the other group's strip retargets the drop");

  Expect(SendMouseUp(shell, drop_x, drop_y, SDL_BUTTON_LEFT), "release commits the move");
  Expect(WorkspaceShellTestAccess::EditorGroupCount(shell) == 2, "both groups survive the move");
  Expect(WorkspaceShellTestAccess::GroupTabCount(shell, 0) == 1, "the tab left group 0");
  Expect(WorkspaceShellTestAccess::GroupTabCount(shell, 1) == 2, "the tab landed in group 1");
  Expect(WorkspaceShellTestAccess::FocusedGroupIndex(shell) == 1,
         "focus follows the tab into its new group, as it does in VS Code");
  Expect(WorkspaceShellTestAccess::GroupActiveViewport(shell, 1).path() == file_a.lexically_normal(),
         "the moved tab is the destination group's active tab");
}

void TestWorkspaceShellEditorTabDragOutOfLastTabCollapsesTheGroup() {
  using microide::workspace::EditorSplitOrientation;
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path file_a = root / "alpha.cpp";
  WriteFile(file_a, "a\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, file_a), "tab a opens");
  WorkspaceShellTestAccess::SetWindowSize(shell, 1600, 900);
  Expect(WorkspaceShellTestAccess::SplitEditorGroup(shell, EditorSplitOrientation::Vertical),
         "the fixture needs a second editor group");
  Expect(WorkspaceShellTestAccess::FocusedGroupIndex(shell) == 1, "the split focuses the new group");

  const SDL_FRect source_rect = WorkspaceShellTestAccess::GroupEditorTabRect(shell, 1, 0);
  const SDL_FRect other_strip = WorkspaceShellTestAccess::GroupTabStripRect(shell, 0);
  Expect(source_rect.w > 0.0f && other_strip.w > 0.0f, "both strips have geometry");
  Expect(SendMouseDown(shell, source_rect.x + source_rect.w * 0.5f,
                       source_rect.y + source_rect.h * 0.5f, SDL_BUTTON_LEFT),
         "press starts a drag");
  const float drop_x = other_strip.x + other_strip.w - 8.0f;
  const float drop_y = other_strip.y + other_strip.h * 0.5f;
  Expect(SendMouseMotion(shell, drop_x, drop_y, SDL_BUTTON_LMASK), "the drag crosses the divider");
  Expect(SendMouseUp(shell, drop_x, drop_y, SDL_BUTTON_LEFT), "release commits the move");

  Expect(WorkspaceShellTestAccess::EditorGroupCount(shell) == 1,
         "the emptied group collapses instead of staying open with no editors");
  Expect(WorkspaceShellTestAccess::GroupSplitOrientation(shell) == EditorSplitOrientation::None,
         "collapsing the last split group drops the split orientation");
  Expect(WorkspaceShellTestAccess::GroupTabCount(shell, 0) == 2, "both tabs are in the survivor");
  Expect(WorkspaceShellTestAccess::FocusedGroupIndex(shell) == 0,
         "focus re-homes onto the surviving group across the erase");
}

namespace {

void DragEditorTabTo(WorkspaceShell& shell,
                     std::size_t group_index,
                     std::size_t tab_index,
                     float x,
                     float y) {
  const SDL_FRect source_rect =
      WorkspaceShellTestAccess::GroupEditorTabRect(shell, group_index, tab_index);
  Expect(source_rect.w > 0.0f, "the dragged tab needs geometry");
  Expect(SendMouseDown(shell, source_rect.x + source_rect.w * 0.5f,
                       source_rect.y + source_rect.h * 0.5f, SDL_BUTTON_LEFT),
         "press starts a drag");
  Expect(SendMouseMotion(shell, x, y, SDL_BUTTON_LMASK), "the drag reaches the drop point");
}

}

void TestWorkspaceShellEditorTabDragToPaneEdgeSplitsTheGroup() {
  using microide::workspace::EditorBodyDropZone;
  using microide::workspace::EditorSplitOrientation;
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path file_a = root / "alpha.cpp";
  const std::filesystem::path file_b = root / "beta.cpp";
  WriteFile(file_a, "a\n");
  WriteFile(file_b, "b\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, file_a), "tab a opens");
  Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, file_b), "tab b opens");
  WorkspaceShellTestAccess::SetWindowSize(shell, 1600, 900);
  Expect(WorkspaceShellTestAccess::EditorGroupCount(shell) == 1, "one group to start");

  const SDL_FRect pane = WorkspaceShellTestAccess::GroupEditorSurfaceRect(shell, 0);
  Expect(pane.w > 0.0f && pane.h > 0.0f, "the pane has geometry");
  const float drop_x = pane.x + pane.w - 8.0f;
  const float drop_y = pane.y + pane.h * 0.5f;
  DragEditorTabTo(shell, 0, 0, drop_x, drop_y);

  const auto& drag = WorkspaceShellTestAccess::TabDrag(shell);
  Expect(drag.body_drop_zone == EditorBodyDropZone::Right,
         "the right fifth of a pane offers a split-to-the-right drop");
  Expect(drag.body_drop_rect.w > 0.0f && drag.body_drop_rect.x > pane.x,
         "the overlay highlights the half the tab would take");

  Expect(SendMouseUp(shell, drop_x, drop_y, SDL_BUTTON_LEFT), "release commits the split");
  Expect(WorkspaceShellTestAccess::EditorGroupCount(shell) == 2,
         "dropping on a pane edge splits the editor area");
  Expect(WorkspaceShellTestAccess::GroupSplitOrientation(shell) == EditorSplitOrientation::Vertical,
         "a left/right edge drop splits side-by-side");
  Expect(WorkspaceShellTestAccess::FocusedGroupIndex(shell) == 1,
         "focus follows the tab into the group it carved out");
  Expect(WorkspaceShellTestAccess::GroupTabCount(shell, 0) == 1 &&
             WorkspaceShellTestAccess::GroupTabCount(shell, 1) == 1,
         "the dragged tab is the only one in the new group");
  Expect(WorkspaceShellTestAccess::GroupActiveViewport(shell, 1).path() == file_a.lexically_normal(),
         "the new group holds the tab that was dragged");
  Expect(WorkspaceShellTestAccess::GroupActiveViewport(shell, 0).path() == file_b.lexically_normal(),
         "the group it left keeps its remaining buffer");
}

void TestWorkspaceShellEditorTabDragToLeftPaneEdgeSplitsAhead() {
  using microide::workspace::EditorBodyDropZone;
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path file_a = root / "alpha.cpp";
  const std::filesystem::path file_b = root / "beta.cpp";
  WriteFile(file_a, "a\n");
  WriteFile(file_b, "b\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, file_a), "tab a opens");
  Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, file_b), "tab b opens");
  WorkspaceShellTestAccess::SetWindowSize(shell, 1600, 900);

  const SDL_FRect pane = WorkspaceShellTestAccess::GroupEditorSurfaceRect(shell, 0);
  const float drop_x = pane.x + 8.0f;
  const float drop_y = pane.y + pane.h * 0.5f;
  DragEditorTabTo(shell, 0, 0, drop_x, drop_y);
  Expect(WorkspaceShellTestAccess::TabDrag(shell).body_drop_zone == EditorBodyDropZone::Left,
         "the left fifth of a pane offers a split-to-the-left drop");
  Expect(SendMouseUp(shell, drop_x, drop_y, SDL_BUTTON_LEFT), "release commits the split");

  Expect(WorkspaceShellTestAccess::EditorGroupCount(shell) == 2, "the editor area splits");
  Expect(WorkspaceShellTestAccess::FocusedGroupIndex(shell) == 0,
         "a left-edge drop puts the carved group AHEAD of the one it came from");
  Expect(WorkspaceShellTestAccess::GroupActiveViewport(shell, 0).path() == file_a.lexically_normal(),
         "the dragged tab lands on the side the pointer was on");
  Expect(WorkspaceShellTestAccess::GroupActiveViewport(shell, 1).path() == file_b.lexically_normal(),
         "the source group slid right to make room");
}

void TestWorkspaceShellEditorTabDragToBottomPaneEdgeStacks() {
  using microide::workspace::EditorBodyDropZone;
  using microide::workspace::EditorSplitOrientation;
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path file_a = root / "alpha.cpp";
  const std::filesystem::path file_b = root / "beta.cpp";
  WriteFile(file_a, "a\n");
  WriteFile(file_b, "b\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, file_a), "tab a opens");
  Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, file_b), "tab b opens");
  WorkspaceShellTestAccess::SetWindowSize(shell, 1600, 900);

  const SDL_FRect pane = WorkspaceShellTestAccess::GroupEditorSurfaceRect(shell, 0);
  const float drop_x = pane.x + pane.w * 0.5f;
  const float drop_y = pane.y + pane.h - 8.0f;
  DragEditorTabTo(shell, 0, 0, drop_x, drop_y);
  Expect(WorkspaceShellTestAccess::TabDrag(shell).body_drop_zone == EditorBodyDropZone::Bottom,
         "the bottom fifth of a pane offers a split-below drop");
  Expect(SendMouseUp(shell, drop_x, drop_y, SDL_BUTTON_LEFT), "release commits the split");

  Expect(WorkspaceShellTestAccess::EditorGroupCount(shell) == 2, "the editor area splits");
  Expect(WorkspaceShellTestAccess::GroupSplitOrientation(shell) ==
             EditorSplitOrientation::Horizontal,
         "a top/bottom edge drop stacks the groups");
  Expect(WorkspaceShellTestAccess::GroupActiveViewport(shell, 1).path() == file_a.lexically_normal(),
         "the dragged tab lands in the lower group");
}

void TestWorkspaceShellEditorTabDragToOtherPaneBodyMovesIntoThatGroup() {
  using microide::workspace::EditorBodyDropZone;
  using microide::workspace::EditorSplitOrientation;
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path file_a = root / "alpha.cpp";
  const std::filesystem::path file_b = root / "beta.cpp";
  WriteFile(file_a, "a\n");
  WriteFile(file_b, "b\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, file_a), "tab a opens");
  Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, file_b), "tab b opens");
  WorkspaceShellTestAccess::SetWindowSize(shell, 1600, 900);
  Expect(WorkspaceShellTestAccess::SplitEditorGroup(shell, EditorSplitOrientation::Vertical),
         "the fixture needs a second editor group");
  Expect(WorkspaceShellTestAccess::FocusOtherEditorGroup(shell), "focus returns to group 0");
  Expect(WorkspaceShellTestAccess::GroupTabCount(shell, 0) == 2, "group 0 owns both tabs");

  const SDL_FRect other_pane = WorkspaceShellTestAccess::GroupEditorSurfaceRect(shell, 1);
  const float drop_x = other_pane.x + other_pane.w * 0.5f;
  const float drop_y = other_pane.y + other_pane.h * 0.5f;
  DragEditorTabTo(shell, 0, 0, drop_x, drop_y);
  Expect(WorkspaceShellTestAccess::TabDrag(shell).body_drop_zone == EditorBodyDropZone::Center,
         "the middle of another group's pane offers a move-into-it drop");
  Expect(WorkspaceShellTestAccess::TabDrag(shell).body_drop_group_index == 1,
         "the drop targets the pane under the pointer");
  Expect(SendMouseUp(shell, drop_x, drop_y, SDL_BUTTON_LEFT), "release commits the move");

  Expect(WorkspaceShellTestAccess::EditorGroupCount(shell) == 2, "both groups survive");
  Expect(WorkspaceShellTestAccess::GroupTabCount(shell, 0) == 1, "the tab left group 0");
  Expect(WorkspaceShellTestAccess::GroupTabCount(shell, 1) == 2, "the tab landed in group 1");
  Expect(WorkspaceShellTestAccess::GroupActiveViewport(shell, 1).path() == file_a.lexically_normal(),
         "the moved tab is the destination's active tab");
}

void TestWorkspaceShellEditorTabDragOverOwnPaneCenterOffersNoDrop() {
  using microide::workspace::EditorBodyDropZone;
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path file_a = root / "alpha.cpp";
  const std::filesystem::path file_b = root / "beta.cpp";
  WriteFile(file_a, "a\n");
  WriteFile(file_b, "b\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, file_a), "tab a opens");
  Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, file_b), "tab b opens");
  WorkspaceShellTestAccess::SetWindowSize(shell, 1600, 900);

  const SDL_FRect pane = WorkspaceShellTestAccess::GroupEditorSurfaceRect(shell, 0);
  DragEditorTabTo(shell, 0, 0, pane.x + pane.w * 0.5f, pane.y + pane.h * 0.5f);
  Expect(WorkspaceShellTestAccess::TabDrag(shell).body_drop_zone == EditorBodyDropZone::None,
         "dropping a tab in the middle of the pane it already lives in is a no-op");
  Expect(SendMouseUp(shell, pane.x + pane.w * 0.5f, pane.y + pane.h * 0.5f, SDL_BUTTON_LEFT),
         "release ends the gesture");
  Expect(WorkspaceShellTestAccess::EditorGroupCount(shell) == 1, "nothing split");
  Expect(WorkspaceShellTestAccess::GroupTabCount(shell, 0) == 2, "nothing moved");
}

void TestWorkspaceShellEditorTabDragToPaneEdgeSplitsAnAlreadySplitPane() {
  using microide::workspace::EditorBodyDropZone;
  using microide::workspace::EditorSplitOrientation;
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path file_a = root / "alpha.cpp";
  const std::filesystem::path file_b = root / "beta.cpp";
  const std::filesystem::path file_c = root / "gamma.cpp";
  WriteFile(file_a, "a\n");
  WriteFile(file_b, "b\n");
  WriteFile(file_c, "c\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, file_a), "tab a opens");
  Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, file_b), "tab b opens");
  Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, file_c), "tab c opens");
  WorkspaceShellTestAccess::SetWindowSize(shell, 1600, 900);
  Expect(WorkspaceShellTestAccess::SplitEditorGroup(shell, EditorSplitOrientation::Vertical),
         "the fixture needs a second editor group");
  Expect(WorkspaceShellTestAccess::FocusOtherEditorGroup(shell), "focus returns to group 0");

  const SDL_FRect other_pane = WorkspaceShellTestAccess::GroupEditorSurfaceRect(shell, 1);
  const float drop_x = other_pane.x + other_pane.w * 0.5f;
  const float drop_y = other_pane.y + other_pane.h - 8.0f;
  DragEditorTabTo(shell, 0, 0, drop_x, drop_y);
  Expect(WorkspaceShellTestAccess::TabDrag(shell).body_drop_zone == EditorBodyDropZone::Bottom,
         "the bottom fifth of an already-split pane still offers a split drop");
  Expect(WorkspaceShellTestAccess::TabDrag(shell).body_drop_group_index == 1,
         "the drop targets the pane under the pointer");
  Expect(SendMouseUp(shell, drop_x, drop_y, SDL_BUTTON_LEFT), "release commits the split");

  Expect(WorkspaceShellTestAccess::EditorGroupCount(shell) == 3,
         "dropping on a split pane's edge adds a third pane");
  Expect(WorkspaceShellTestAccess::EditorSplit(shell).leaf_count() == 3,
         "the split tree gains a leaf with the group");
  Expect(WorkspaceShellTestAccess::GroupSplitOrientation(shell) == EditorSplitOrientation::Vertical,
         "the outermost split stays side-by-side");
  Expect(WorkspaceShellTestAccess::FocusedGroupIndex(shell) == 2,
         "focus follows the tab into the pane it carved out, after the target pane");
  Expect(WorkspaceShellTestAccess::GroupActiveViewport(shell, 2).path() == file_a.lexically_normal(),
         "the new pane holds the dragged tab");

  const SDL_FRect target = WorkspaceShellTestAccess::GroupEditorSurfaceRect(shell, 1);
  const SDL_FRect carved = WorkspaceShellTestAccess::GroupEditorSurfaceRect(shell, 2);
  Expect(carved.x == target.x && carved.w == target.w,
         "the carved pane shares the column it was dropped into");
  Expect(carved.y > target.y, "a bottom-edge drop stacks the new pane below the target");
}

void TestWorkspaceShellEditorTabDragToPaneEdgeMovesAcrossPanes() {
  using microide::workspace::EditorBodyDropZone;
  using microide::workspace::EditorSplitOrientation;
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path file_a = root / "alpha.cpp";
  const std::filesystem::path file_b = root / "beta.cpp";
  WriteFile(file_a, "a\n");
  WriteFile(file_b, "b\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, file_a), "tab a opens");
  Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, file_b), "tab b opens");
  WorkspaceShellTestAccess::SetWindowSize(shell, 1600, 900);
  Expect(WorkspaceShellTestAccess::SplitEditorGroup(shell, EditorSplitOrientation::Vertical),
         "the fixture needs a second editor group");
  Expect(WorkspaceShellTestAccess::GroupTabCount(shell, 1) == 1, "group 1 has one tab");

  const SDL_FRect target_pane = WorkspaceShellTestAccess::GroupEditorSurfaceRect(shell, 0);
  const float drop_x = target_pane.x + target_pane.w * 0.5f;
  const float drop_y = target_pane.y + 8.0f;
  DragEditorTabTo(shell, 1, 0, drop_x, drop_y);
  Expect(WorkspaceShellTestAccess::TabDrag(shell).body_drop_zone == EditorBodyDropZone::Top,
         "another pane's lone tab may still be dropped on an edge");
  Expect(SendMouseUp(shell, drop_x, drop_y, SDL_BUTTON_LEFT), "release commits the drop");

  Expect(WorkspaceShellTestAccess::EditorGroupCount(shell) == 2,
         "the emptied source pane collapses as the new one appears");
  Expect(WorkspaceShellTestAccess::EditorSplit(shell).leaf_count() == 2,
         "the split tree tracks the collapse");
  Expect(WorkspaceShellTestAccess::GroupSplitOrientation(shell) == EditorSplitOrientation::Horizontal,
         "the surviving split is the stacked one the drop created");
  Expect(WorkspaceShellTestAccess::FocusedGroupIndex(shell) == 0,
         "a top-edge drop seats the pane ahead of the target, and focus follows it");
}

void TestWorkspaceShellEditorSplitDividerDragMovesOnlyItsPair() {
  using microide::workspace::EditorSplitOrientation;
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path file_a = root / "alpha.cpp";
  WriteFile(file_a, "a\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1600, 900);
  Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, file_a), "tab a opens");
  Expect(WorkspaceShellTestAccess::SplitEditorGroup(shell, EditorSplitOrientation::Vertical),
         "split once");
  Expect(WorkspaceShellTestAccess::SplitEditorGroup(shell, EditorSplitOrientation::Vertical),
         "split again for a three-pane row");
  Expect(WorkspaceShellTestAccess::EditorGroupCount(shell) == 3, "three panes");

  const auto dividers = WorkspaceShellTestAccess::EditorSplitDividerRects(shell);
  Expect(dividers.size() == 2, "a three-pane row has two dividers");
  const float width_zero_before = WorkspaceShellTestAccess::GroupEditorSurfaceRect(shell, 0).w;
  const float width_one_before = WorkspaceShellTestAccess::GroupEditorSurfaceRect(shell, 1).w;
  const float width_two_before = WorkspaceShellTestAccess::GroupEditorSurfaceRect(shell, 2).w;
  const SDL_FRect first = dividers[0].rect;

  Expect(SendMouseDown(shell, first.x + first.w * 0.5f, first.y + first.h * 0.5f, SDL_BUTTON_LEFT),
         "pressing the first divider starts a resize");
  Expect(SendMouseMotion(shell, first.x - 120.0f, first.y + first.h * 0.5f, SDL_BUTTON_LMASK),
         "dragging it left narrows the pane to its left");
  Expect(SendMouseUp(shell, first.x - 120.0f, first.y + first.h * 0.5f, SDL_BUTTON_LEFT),
         "release ends the resize");

  const float width_zero_after = WorkspaceShellTestAccess::GroupEditorSurfaceRect(shell, 0).w;
  const float width_one_after = WorkspaceShellTestAccess::GroupEditorSurfaceRect(shell, 1).w;
  const float width_two_after = WorkspaceShellTestAccess::GroupEditorSurfaceRect(shell, 2).w;
  Expect(std::fabs(width_zero_after - (width_zero_before - 120.0f)) < 2.0f,
         "the pane left of the divider gives up exactly the distance dragged");
  Expect(std::fabs(width_one_after - (width_one_before + 120.0f)) < 2.0f,
         "its right-hand neighbour takes exactly that width");
  Expect(std::fabs(width_two_after - width_two_before) < 2.0f,
         "the pane the divider does not touch keeps its width");
}

void TestWorkspaceShellEditorTabDragToPaneEdgeAtCapMovesThePane() {
  using microide::workspace::EditorBodyDropZone;
  using microide::workspace::EditorSplitOrientation;
  using microide::workspace::kMaxEditorGroups;
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path file_a = root / "alpha.cpp";
  WriteFile(file_a, "a\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, file_a), "tab a opens");
  WorkspaceShellTestAccess::SetWindowSize(shell, 3000, 1000);
  const auto focus_group = [&](std::size_t index) {
    while (WorkspaceShellTestAccess::FocusedGroupIndex(shell) != index) {
      Expect(WorkspaceShellTestAccess::FocusOtherEditorGroup(shell), "cycling reaches every pane");
    }
  };
  while (WorkspaceShellTestAccess::EditorGroupCount(shell) < kMaxEditorGroups) {
    const std::size_t generation = WorkspaceShellTestAccess::EditorGroupCount(shell);
    for (std::size_t i = 0; i < generation; ++i) {
      focus_group(i * 2);
      Expect(WorkspaceShellTestAccess::SplitEditorGroup(shell, EditorSplitOrientation::Vertical),
             "splitting below the cap should succeed");
    }
  }
  Expect(WorkspaceShellTestAccess::EditorGroupCount(shell) == kMaxEditorGroups,
         "the fixture should be at the pane cap");
  Expect(!WorkspaceShellTestAccess::SplitEditorGroup(shell, EditorSplitOrientation::Vertical),
         "a further split should be refused at the cap");

  const std::size_t source = WorkspaceShellTestAccess::FocusedGroupIndex(shell);
  const std::size_t target = source == 0 ? 1 : 0;
  Expect(WorkspaceShellTestAccess::GroupTabCount(shell, source) == 1,
         "the source pane should hold exactly one tab");
  const SDL_FRect pane = WorkspaceShellTestAccess::GroupEditorSurfaceRect(shell, target);
  const float drop_x = pane.x + pane.w - 4.0f;
  const float drop_y = pane.y + pane.h * 0.5f;
  DragEditorTabTo(shell, source, 0, drop_x, drop_y);
  Expect(WorkspaceShellTestAccess::TabDrag(shell).body_drop_zone == EditorBodyDropZone::Right,
         "a count-preserving edge drop should still be offered at the cap");
  Expect(SendMouseUp(shell, drop_x, drop_y, SDL_BUTTON_LEFT), "release commits the drop");
  Expect(WorkspaceShellTestAccess::EditorGroupCount(shell) == kMaxEditorGroups,
         "the pane count should be unchanged by a relocation");
  Expect(WorkspaceShellTestAccess::EditorSplit(shell).leaf_count() == kMaxEditorGroups,
         "the split tree should hold one leaf per group after the move");
}

void TestWorkspaceShellEditorTabDragToPaneEdgeRefusesLoneTab() {
  using microide::workspace::EditorBodyDropZone;
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path file_a = root / "alpha.cpp";
  WriteFile(file_a, "a\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, file_a), "tab a opens");
  WorkspaceShellTestAccess::SetWindowSize(shell, 1600, 900);

  const SDL_FRect pane = WorkspaceShellTestAccess::GroupEditorSurfaceRect(shell, 0);
  DragEditorTabTo(shell, 0, 0, pane.x + pane.w - 8.0f, pane.y + pane.h * 0.5f);
  Expect(WorkspaceShellTestAccess::TabDrag(shell).body_drop_zone == EditorBodyDropZone::None,
         "a group with one tab offers no edge split");
  Expect(SendMouseUp(shell, pane.x + pane.w - 8.0f, pane.y + pane.h * 0.5f, SDL_BUTTON_LEFT),
         "release ends the gesture");
  Expect(WorkspaceShellTestAccess::EditorGroupCount(shell) == 1, "nothing split");
}

void TestWorkspaceShellDirectionalEditorGroupFocusAndMove() {
  using microide::workspace::EditorGroupDirection;
  using microide::workspace::EditorSplitOrientation;
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path file_a = root / "alpha.cpp";
  WriteFile(file_a, "a\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, file_a), "tab a opens");
  WorkspaceShellTestAccess::SetWindowSize(shell, 1600, 1000);

  Expect(!WorkspaceShellTestAccess::FocusEditorGroupInDirection(shell, EditorGroupDirection::Right),
         "a single pane has nowhere to go");

  Expect(WorkspaceShellTestAccess::SplitEditorGroup(shell, EditorSplitOrientation::Vertical),
         "the fixture needs a second column");
  Expect(WorkspaceShellTestAccess::SplitEditorGroup(shell, EditorSplitOrientation::Horizontal),
         "the fixture needs the right column stacked");
  Expect(WorkspaceShellTestAccess::EditorGroupCount(shell) == 3, "three panes");

  Expect(WorkspaceShellTestAccess::FocusEditorGroupInDirection(shell, EditorGroupDirection::Left) &&
             WorkspaceShellTestAccess::FocusedGroupIndex(shell) == 0,
         "focus-left from the right column lands on the left one");
  Expect(!WorkspaceShellTestAccess::FocusEditorGroupInDirection(shell, EditorGroupDirection::Left),
         "focus-left from the leftmost column is refused, it does not wrap");
  Expect(WorkspaceShellTestAccess::FocusEditorGroupInDirection(shell, EditorGroupDirection::Right) &&
             WorkspaceShellTestAccess::FocusedGroupIndex(shell) == 1,
         "focus-right lands on the top pane of the right column");
  Expect(WorkspaceShellTestAccess::FocusEditorGroupInDirection(shell, EditorGroupDirection::Down) &&
             WorkspaceShellTestAccess::FocusedGroupIndex(shell) == 2,
         "focus-down lands on the pane below");
  Expect(!WorkspaceShellTestAccess::FocusEditorGroupInDirection(shell, EditorGroupDirection::Down),
         "there is nothing below the bottom pane");

  Expect(WorkspaceShellTestAccess::MoveEditorGroupInDirection(shell, EditorGroupDirection::Left),
         "the bottom-right pane can move left");
  Expect(WorkspaceShellTestAccess::EditorGroupCount(shell) == 3,
         "a move must not add or drop a pane");
  Expect(WorkspaceShellTestAccess::EditorSplit(shell).leaf_count() == 3,
         "the tree keeps one leaf per group across the move");
  Expect(WorkspaceShellTestAccess::FocusedGroupIndex(shell) == 0,
         "the moved pane keeps focus at its new ordinal");
  const SDL_FRect moved = WorkspaceShellTestAccess::GroupEditorSurfaceRect(shell, 0);
  const SDL_FRect neighbour = WorkspaceShellTestAccess::GroupEditorSurfaceRect(shell, 1);
  Expect(moved.x < neighbour.x, "the moved pane is now the leftmost column");

  Expect(SendKeyDown(shell, SDLK_K, SDL_KMOD_CTRL), "Ctrl+K arms the editor chord");
  Expect(SendKeyDown(shell, SDLK_RIGHT, SDL_KMOD_CTRL),
         "Ctrl+K Ctrl+Right is handled as focus-group-right");
  Expect(WorkspaceShellTestAccess::FocusedGroupIndex(shell) == 1,
         "Ctrl+K Ctrl+Right focuses the pane to the right");
  Expect(SendKeyDown(shell, SDLK_K, SDL_KMOD_CTRL), "Ctrl+K arms the editor chord again");
  Expect(SendKeyDown(shell, SDLK_LEFT, SDL_KMOD_CTRL | SDL_KMOD_SHIFT),
         "Ctrl+K Ctrl+Shift+Left is handled as move-group-left");
  Expect(WorkspaceShellTestAccess::FocusedGroupIndex(shell) == 0 &&
             WorkspaceShellTestAccess::EditorGroupCount(shell) == 3,
         "Ctrl+K Ctrl+Shift+Left moves the focused pane left without changing the count");
}

void TestWorkspaceShellCrossGroupDragAnimatesBothStrips() {
  using microide::workspace::EditorSplitOrientation;
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path file_a = root / "alpha.cpp";
  const std::filesystem::path file_b = root / "beta.cpp";
  const std::filesystem::path file_c = root / "gamma.cpp";
  WriteFile(file_a, "a\n");
  WriteFile(file_b, "b\n");
  WriteFile(file_c, "c\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, file_a), "tab a opens");
  Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, file_b), "tab b opens");
  Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, file_c), "tab c opens");
  WorkspaceShellTestAccess::SetWindowSize(shell, 1600, 900);
  Expect(WorkspaceShellTestAccess::SplitEditorGroup(shell, EditorSplitOrientation::Vertical),
         "the fixture needs a second editor group");
  Expect(WorkspaceShellTestAccess::FocusOtherEditorGroup(shell), "focus returns to group 0");

  const SDL_FRect source_rect = WorkspaceShellTestAccess::GroupEditorTabRect(shell, 0, 0);
  const SDL_FRect other_strip = WorkspaceShellTestAccess::GroupTabStripRect(shell, 1);
  Expect(SendMouseDown(shell, source_rect.x + source_rect.w * 0.5f,
                       source_rect.y + source_rect.h * 0.5f, SDL_BUTTON_LEFT),
         "press starts a drag on group 0's first tab");
  const float drop_x = other_strip.x + 4.0f;
  const float drop_y = other_strip.y + other_strip.h * 0.5f;
  Expect(SendMouseMotion(shell, drop_x, drop_y, SDL_BUTTON_LMASK), "the drag crosses the divider");

  const auto& slide = WorkspaceShellTestAccess::TabSlide(shell);
  Expect(slide.strips[0].kind == microide::workspace::TabDragKind::Editor &&
             slide.strips[0].group_index == 0,
         "slot 0 animates the source group's strip");
  Expect(slide.strips[1].kind == microide::workspace::TabDragKind::Editor &&
             slide.strips[1].group_index == 1,
         "slot 1 animates the destination group's strip");
  Expect(slide.strips[0].target.size() == 3 && slide.strips[1].target.size() == 1,
         "each strip's targets are sized to its OWN tab list");
  Expect(slide.strips[0].target[1] < -1.0f,
         "the source strip closes ranks over the lifted tab (no gap opens there)");
  Expect(slide.strips[1].target[0] > 1.0f,
         "the destination strip opens a gap ahead of the tab being dropped in front of it");

  Expect(SendMouseMotion(shell, source_rect.x + source_rect.w * 0.5f,
                         source_rect.y + source_rect.h * 0.5f, SDL_BUTTON_LMASK),
         "the drag comes back to its own strip");
  Expect(!WorkspaceShellTestAccess::TabDrag(shell).cross_group(),
         "the drop targets the source group again");
  Expect(WorkspaceShellTestAccess::TabSlide(shell).strips[1].idle(),
         "the destination strip's animation is retired when the drag leaves it");
}

void TestWorkspaceShellEditorTabDragAutoScrollsOverflowingStrip() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  for (int i = 0; i < 10; ++i) {
    const std::filesystem::path file =
        root / ("file-" + std::to_string(i) + "-with-a-very-long-name.cpp");
    WriteFile(file, "int value() { return 1; }\n");
    Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, file), "editor tab fixture opens");
  }
  WorkspaceShellTestAccess::SetWindowSize(shell, 640, 720);
  WorkspaceShellTestAccess::ActivateTab(shell, 9);
  const int scrolled_index = WorkspaceShellTestAccess::EditorTabScrollIndex(shell);
  Expect(scrolled_index > 0, "activating the last tab scrolls the overflowing strip");

  const SDL_FRect strip = WorkspaceShellTestAccess::GroupTabStripRect(shell, 0);
  const SDL_FRect source_rect =
      WorkspaceShellTestAccess::EditorTabRect(shell, static_cast<std::size_t>(scrolled_index));
  const float cy = source_rect.y + source_rect.h * 0.5f;
  Expect(SendMouseDown(shell, source_rect.x + source_rect.w * 0.5f, cy, SDL_BUTTON_LEFT),
         "press starts a drag on the first visible tab");
  const int pressed_index = WorkspaceShellTestAccess::EditorTabScrollIndex(shell);
  Expect(pressed_index > 0, "the strip is still scrolled after the press");
  Expect(SendMouseMotion(shell, strip.x + 2.0f, cy, SDL_BUTTON_LMASK),
         "dragging to the strip's left edge is handled");
  Expect(WorkspaceShellTestAccess::EditorTabScrollIndex(shell) < pressed_index,
         "holding a drag at the left edge walks the overflowing strip back toward the start");
  Expect(WorkspaceShellTestAccess::TabDrag(shell).autoscroll_direction == -1,
         "the auto-scroll stays armed so a still pointer keeps stepping from the animation tick");

  Expect(SendMouseUp(shell, strip.x + 2.0f, cy, SDL_BUTTON_LEFT), "release handled");
}

void TestWorkspaceShellOutputTabReorderMovesActiveChannel() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetOutputChannelTabs(shell, {"build", "lint", "run"}, "build");
  Expect(WorkspaceShellTestAccess::MoveActiveOutputTabTo(shell, 2),
         "reordering the active output channel tab succeeds");
  Expect(WorkspaceShellTestAccess::OutputChannelTabOrder(shell) ==
             std::vector<std::string>{"lint", "run", "build"},
         "the active output channel should move to the requested slot");

  WorkspaceShellTestAccess::SetOutputChannelTabs(shell, {"build", "lint"}, "run");
  Expect(WorkspaceShellTestAccess::MoveActiveOutputTabTo(shell, 0),
         "reordering an unpinned active output channel succeeds");
  Expect(WorkspaceShellTestAccess::OutputChannelTabOrder(shell) ==
             std::vector<std::string>{"run", "build", "lint"},
         "an unpinned active output channel should be pinned then moved to the slot");
}

void TestReorderActiveHelperMovesAndGuards() {
  using microide::workspace::ReorderActive;
  std::vector<int> v{10, 20, 30, 40};
  std::size_t active = 0;
  Expect(ReorderActive(v, active, 2), "moving 0 -> 2 succeeds");
  Expect((v == std::vector<int>{20, 30, 10, 40}) && active == 2, "element moves down, active follows");

  active = 3;
  Expect(ReorderActive(v, active, 1), "moving 3 -> 1 succeeds");
  Expect((v == std::vector<int>{20, 40, 30, 10}) && active == 1, "element moves up, active follows");

  active = 1;
  Expect(ReorderActive(v, active, 1), "no-op move returns true");
  Expect((v == std::vector<int>{20, 40, 30, 10}) && active == 1, "no-op leaves vector and active");

  active = 5;
  Expect(!ReorderActive(v, active, 0), "out-of-range active is rejected");
  active = 0;
  Expect(!ReorderActive(v, active, 9), "out-of-range target is rejected");
}

void TestWorkspaceShellProjectTabWheelScrollsStrip() {
  TemporaryDirectory temp_dir;
  WorkspaceShell shell;

  for (int i = 0; i < 8; ++i) {
    const std::filesystem::path root =
        temp_dir.path() / ("project-" + std::to_string(i) + "-with-a-long-name");
    WriteFile(root / "README.md", "root\n");
    Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
           "project fixture should open");
  }
  WorkspaceShellTestAccess::SetWindowSize(shell, 640, 720);
  Expect(WorkspaceShellTestAccess::SwitchProject(shell, 0, false),
         "first project should become active before the wheel scroll test");
  const SDL_FRect first_rect = WorkspaceShellTestAccess::ProjectTabRect(shell, 0);

  SDL_Event event{};
  event.type = SDL_EVENT_MOUSE_WHEEL;
  event.wheel.integer_y = -2;
  event.wheel.y = -2.0f;
  event.wheel.mouse_x = first_rect.x + first_rect.w * 0.5f;
  event.wheel.mouse_y = first_rect.y + first_rect.h * 0.5f;

  Expect(shell.HandleEvent(event).handled, "mouse wheel over the project strip should be handled");
  Expect(WorkspaceShellTestAccess::ProjectTabScrollIndex(shell) == 2,
         "mouse wheel over the project strip should update the project strip scroll index");
}

void TestWorkspaceShellEditorTabWheelScrollsStrip() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);

  for (int i = 0; i < 10; ++i) {
    const std::filesystem::path file =
        root / ("file-" + std::to_string(i) + "-with-a-very-long-name.cpp");
    WriteFile(file, "int value() { return 1; }\n");
    Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, file),
           "editor tab fixture should open");
  }
  WorkspaceShellTestAccess::SetWindowSize(shell, 640, 720);
  WorkspaceShellTestAccess::ActivateTab(shell, 0);
  const SDL_FRect first_rect = WorkspaceShellTestAccess::EditorTabRect(shell, 0);

  SDL_Event event{};
  event.type = SDL_EVENT_MOUSE_WHEEL;
  event.wheel.integer_y = -3;
  event.wheel.y = -3.0f;
  event.wheel.mouse_x = first_rect.x + first_rect.w * 0.5f;
  event.wheel.mouse_y = first_rect.y + first_rect.h * 0.5f;

  Expect(shell.HandleEvent(event).handled, "mouse wheel over the editor strip should be handled");
  Expect(WorkspaceShellTestAccess::EditorTabScrollIndex(shell) == 3,
         "mouse wheel over the editor strip should update the editor strip scroll index");
}

void TestWorkspaceShellProjectWatcherReloadDoesNotContinuouslyRearm() {
  static const bool initialized = SDL_Init(SDL_INIT_VIDEO);
  Expect(initialized, "SDL video initialization should succeed for watcher event tests");

  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "README.md", "root\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::RegisterLifecycleWakeEvents(shell);
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "project watcher fixture should open the project");
  const auto idle_delay_before = shell.NextAnimationDelayMs();
  Expect(!idle_delay_before.has_value() || *idle_delay_before > 0,
         "idle project watchers should not schedule a zero-delay wake when no change is pending");

  WriteFile(root / "watched.txt", "changed\n");
  Expect(WaitForProjectReload(shell, std::chrono::seconds(1)),
         "project watcher reload should detect filesystem changes");
  bool settled = false;
  for (int attempt = 0; attempt < 10; ++attempt) {
    if (!WorkspaceShellTestAccess::ReloadProjectIfFilesChanged(shell, true)) {
      settled = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  Expect(settled,
         "project watcher reload should settle quickly instead of continuously rearming");
  const auto idle_delay_after = shell.NextAnimationDelayMs();
  Expect(!idle_delay_after.has_value() || *idle_delay_after > 0,
         "project watcher reload should settle without a zero-delay wake after refresh");
}

void TestWorkspaceShellEmptyDirectoryCreationRefreshesTree() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "README.md", "root\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::RegisterLifecycleWakeEvents(shell);
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "empty-directory fixture should open the project");
  for (int attempt = 0; attempt < 20; ++attempt) {
    if (!WorkspaceShellTestAccess::ReloadProjectIfFilesChanged(shell, false)) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  const auto tree_has = [&shell](std::string_view name) {
    for (const project::TreeEntry& entry : WorkspaceShellTestAccess::TreeEntries(shell)) {
      if (entry.path.filename().string() == name) {
        return true;
      }
    }
    return false;
  };
  Expect(!tree_has("fresh-dir"), "the directory does not exist yet");

  std::error_code create_error;
  std::filesystem::create_directory(root / "fresh-dir", create_error);
  Expect(!create_error, "empty-directory fixture should create the directory");

  Expect(WaitUntil([&] { return tree_has("fresh-dir"); }, std::chrono::seconds(5),
                   std::chrono::milliseconds(10),
                   [&] { WorkspaceShellTestAccess::ReloadProjectIfFilesChanged(shell, false); }),
         "creating an empty directory should refresh the sidebar tree");
}

void TestWorkspaceShellTruncatedIndexBatchNotifiesOnce() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "README.md", "root\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::RegisterLifecycleWakeEvents(shell);
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "truncated-index fixture should open the project");

  const auto truncation_notices = [&shell]() {
    std::size_t count = 0;
    for (const auto& notification : WorkspaceShellTestAccess::ActiveNotifications(shell)) {
      if (notification.message.find("too large to index") != std::string::npos) {
        ++count;
      }
    }
    return count;
  };
  Expect(truncation_notices() == 0, "a small project should raise no truncation notice");

  platform::IndexUpdateBatch truncated;
  truncated.is_initial = true;
  truncated.truncated = true;
  Expect(WorkspaceShellTestAccess::DispatchFileIndexWatcherBatchForTesting(shell,
                                                                          std::move(truncated)),
         "truncated-index fixture should have a live file index watcher");
  WorkspaceShellTestAccess::ReloadProjectIfFilesChanged(shell, false);
  Expect(truncation_notices() == 1,
         "a truncated initial index batch should surface exactly one notice");

  WorkspaceShellTestAccess::ReloadProjectIfFilesChanged(shell, false);
  Expect(truncation_notices() == 1, "the truncation notice should not repeat per drain");

  const auto& rows = WorkspaceShellTestAccess::ActiveNotifications(shell);
  Expect(rows.size() == 1 && rows.front().sticky, "the truncation notice is sticky");
  WorkspaceShellTestAccess::ExpireNotificationsAt(shell,
                                                  workspace::NotificationService::DurationMs() * 100);
  Expect(truncation_notices() == 1, "a sticky state notice does not expire on the timer");

  const std::filesystem::path other_root = temp_dir.path() / "other-project";
  WriteFile(other_root / "README.md", "other\n");
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, other_root, false, false),
         "the fixture should open a second project");
  Expect(truncation_notices() == 0,
         "restarting the index watcher clears the notice about the old index");
}

void TestWorkspaceShellAProjectWhoseFolderIsDeletedSaysSo() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "doomed";
  const std::filesystem::path other = temp_dir.path() / "other";
  WriteFile(root / "README.md", "root\n");
  WriteFile(other / "README.md", "other\n");
  WorkspaceShell shell;
  WorkspaceShellTestAccess::RegisterLifecycleWakeEvents(shell);
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, other, false, false), "the other project");
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false), "the doomed one, in front");
  Expect(WaitForFileIndexPath(shell, std::filesystem::path("README.md"), true, std::chrono::seconds(5)), "indexed");
  const auto row = [&]() -> const workspace::NotificationService::Notification* {
    for (const auto& note : WorkspaceShellTestAccess::ActiveNotifications(shell)) {
      if (note.key == workspace::kProjectRootMissingNotificationKey) {
        return &note;
      }
    }
    return nullptr;
  };
  Expect(row() == nullptr, "no row while the folder is there");
  std::filesystem::remove_all(root);
  Expect(WaitUntil(
             [&] {
               (void)WorkspaceShellTestAccess::ReloadProjectIfFilesChanged(shell, false);
               return row() != nullptr;
             },
             std::chrono::seconds(10)),
         "the watcher's batch raises the row");
  Expect(row()->sticky && row()->message.find(root.string()) != std::string::npos, "sticky, naming the folder");
  WriteFile(root / "BACK.md", "back\n");
  SDL_Event focus{};
  focus.type = SDL_EVENT_WINDOW_FOCUS_GAINED;
  (void)shell.HandleEvent(focus);
  Expect(row() == nullptr, "the folder is back: the row goes");
  Expect(WaitForFileIndexPath(shell, std::filesystem::path("BACK.md"), true, std::chrono::seconds(5)),
         "and the folder is indexed again");
  std::filesystem::remove_all(root);
  Expect(WaitUntil(
             [&] {
               (void)WorkspaceShellTestAccess::ReloadProjectIfFilesChanged(shell, false);
               return row() != nullptr;
             },
             std::chrono::seconds(10)),
         "gone again: the restarted watcher sees it");
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, other, false, false), "switch to the other project");
  Expect(row() == nullptr, "which is there: the row goes");
}

void TestWorkspaceShellProjectWatcherIgnoresGitignoredDirectories() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / ".gitignore", "node_modules/\n");
  WriteFile(root / "README.md", "root\n");
  WriteFile(root / "node_modules" / "pkg" / "index.js", "module.exports = 1;\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::RegisterLifecycleWakeEvents(shell);
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "ignored-directory watcher fixture should open the project");

  Expect(WaitForFileIndexPath(shell, std::filesystem::path("README.md"), true,
                              std::chrono::seconds(5)),
         "the initial file-index batch should land before the gitignore assertion");
  for (int attempt = 0; attempt < 50; ++attempt) {
    if (!WorkspaceShellTestAccess::ReloadProjectIfFilesChanged(shell, false)) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  WriteFile(root / "node_modules" / "pkg" / "index.js", "module.exports = 2;\n");
  Expect(!WorkspaceShellTestAccess::ReloadProjectIfFilesChanged(shell, true),
         "project watcher should ignore gitignored directory changes");

  WriteFile(root / "watched.txt", "changed\n");
  Expect(WaitForForcedProjectChange(shell, std::chrono::seconds(2)),
         "project watcher should still detect visible project changes");
}

void TestWorkspaceShellProjectWatcherIgnoresGitMetadataLockfiles() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / ".git" / "HEAD", "ref: refs/heads/main\n");
  WriteFile(root / "README.md", "root\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::RegisterLifecycleWakeEvents(shell);
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "git-metadata watcher fixture should open the project");
  for (int attempt = 0; attempt < 20; ++attempt) {
    if (!WorkspaceShellTestAccess::ReloadProjectIfFilesChanged(shell, false)) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  WriteFile(root / ".git" / "index.lock", "lock\n");
  Expect(!WaitForProjectReload(shell, std::chrono::milliseconds(400)),
         "project watcher should ignore .git lockfile churn");
  Expect(!WaitForFileIndexPath(shell, std::filesystem::path(".git/index.lock"), true,
                               std::chrono::milliseconds(50)),
         "file index should not include .git lockfiles");

  WriteFile(root / "watched.txt", "changed\n");
  Expect(WaitForProjectReload(shell, std::chrono::milliseconds(1000)),
         "project watcher should still detect visible project changes after ignoring git locks");
}

void TestWorkspaceShellWatcherIgnoresGitignoredIncrementalCreate() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / ".gitignore", "*.log\n");
  WriteFile(root / "README.md", "root\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::RegisterLifecycleWakeEvents(shell);
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "gitignore watcher fixture should open the project");
  for (int attempt = 0; attempt < 20; ++attempt) {
    if (!WorkspaceShellTestAccess::ReloadProjectIfFilesChanged(shell, false)) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  WriteFile(root / "debug.log", "noise\n");
  WriteFile(root / "visible.cpp", "int main(){ return 0; }\n");
  Expect(WaitForProjectReload(shell, std::chrono::milliseconds(1000)),
         "watcher should observe the newly created files");
  Expect(WaitForFileIndexPath(shell, std::filesystem::path("visible.cpp"), true,
                              std::chrono::milliseconds(1000)),
         "a non-ignored file created via an incremental event should be indexed");
  Expect(!WorkspaceShellTestAccess::FileIndexContainsPath(shell, std::filesystem::path("debug.log")),
         "a gitignored file created via an incremental event must not enter the index");
}

void TestWorkspaceShellFileIndexUpdatesDoNotReloadCleanBuffers() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "README.md", "root\n");

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "index-only reload fixture should open the project");
  for (int attempt = 0; attempt < 20; ++attempt) {
    if (!WorkspaceShellTestAccess::ReloadProjectIfFilesChanged(shell, false)) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  WorkspaceShellTestAccess::ResetReloadCleanOpenBuffersFromDiskInvocationCount(shell);
  WorkspaceShellTestAccess::SetFileIndexHasPendingChanges(shell, true);

  Expect(WorkspaceShellTestAccess::ReloadProjectIfFilesChanged(shell, false),
         "index-only watcher updates should still refresh project state");
  Expect(WorkspaceShellTestAccess::ReloadCleanOpenBuffersFromDiskInvocationCount(shell) == 0,
         "file index updates alone should not reopen clean editor buffers from disk");
}

void TestWorkspaceShellFileFinderReflectsFileIndexUpdates() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path readme = root / "README.md";
  const std::filesystem::path added_file = root / "new-indexed.cpp";
  WriteFile(readme, "root\n");

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "file-index update fixture should open the project");

  Expect(WorkspaceShellTestAccess::ExecuteFilesFromShortcut(shell),
         "file finder should open on demand");
  Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "new-indexed"),
         "typing a missing file query should be handled");
  Expect(WorkspaceShellTestAccess::FileFinderResultCount(shell) == 0,
         "file finder should have no result before the file exists");
  Expect(SendKeyDown(shell, SDLK_ESCAPE, SDL_KMOD_NONE),
         "Escape should close the overlay after the missing file query");

  WriteFile(added_file, "int value() { return 7; }\n");
  Expect(WaitForProjectReload(shell, std::chrono::milliseconds(1000)),
         "project reload should observe a newly added file via the file index watcher");
  Expect(WaitForFileIndexPath(shell, std::filesystem::path("new-indexed.cpp"), true,
                              std::chrono::milliseconds(1000)),
         "file index should include the newly added file");

  Expect(WorkspaceShellTestAccess::ExecuteFilesFromShortcut(shell),
         "file finder should open after a file-index update");
  Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "new-indexed"),
         "typing the added file query should be handled");
  Expect(WorkspaceShellTestAccess::FileFinderResultCount(shell) >= 1,
         "file finder should include the newly indexed file");
  const auto selected_path = WorkspaceShellTestAccess::FileFinderSelectedPath(shell);
  Expect(selected_path.has_value() &&
             selected_path->lexically_normal() ==
                 std::filesystem::path("new-indexed.cpp"),
         "file finder should select the newly indexed file path");
  Expect(SendKeyDown(shell, SDLK_RETURN, SDL_KMOD_NONE),
         "pressing Enter should open the newly indexed file");
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).path() == added_file.lexically_normal(),
         "opening from file finder should land on the newly indexed file");

  std::error_code remove_error;
  std::filesystem::remove(added_file, remove_error);
  Expect(!remove_error, "fixture cleanup should remove the indexed file");
  Expect(WaitForProjectReload(shell, std::chrono::milliseconds(1000)),
         "project reload should observe file deletion via the file index watcher");
  Expect(WaitForFileIndexPath(shell, std::filesystem::path("new-indexed.cpp"), false,
                              std::chrono::milliseconds(1000)),
         "file index should remove the deleted file");

  Expect(WorkspaceShellTestAccess::ExecuteFilesFromShortcut(shell),
         "file finder should still open after file deletion");
  Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "new-indexed"),
         "typing the deleted file query should be handled");
  Expect(WorkspaceShellTestAccess::FileFinderResultCount(shell) == 0,
         "file finder should no longer list the deleted file");
}

void TestWorkspaceShellFileFinderUsesMaintainedIndexWithoutProjectScan() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "README.md", "root\n");
  WriteFile(root / "src" / "finder_target.cpp", "int target() { return 1; }\n");

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "file finder no-scan fixture should open the project");
  Expect(WaitForFileIndexPath(shell, std::filesystem::path("src/finder_target.cpp"), true,
                              std::chrono::milliseconds(1000)),
         "file finder no-scan fixture should wait for file index initialization");

  util::ResetPerformanceCounters();
  Expect(WorkspaceShellTestAccess::ExecuteFilesFromShortcut(shell),
         "file finder no-scan fixture should open file finder");
  Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "finder_target"),
         "file finder no-scan fixture should accept query input");
  Expect(WorkspaceShellTestAccess::FileFinderResultCount(shell) >= 1,
         "file finder no-scan fixture should resolve indexed matches");
  Expect(SendKeyDown(shell, SDLK_ESCAPE, SDL_KMOD_NONE),
         "file finder no-scan fixture should close the overlay");
  Expect(WorkspaceShellTestAccess::ExecuteFilesFromShortcut(shell),
         "file finder no-scan fixture should reopen file finder");
  Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "README"),
         "file finder no-scan fixture should accept a second query");
  Expect(WorkspaceShellTestAccess::FileFinderResultCount(shell) >= 1,
         "file finder no-scan fixture should resolve indexed matches on query refresh");
  Expect(util::ReadPerformanceCounter(
             util::PerfCounterId::ProjectFileScannerCollectProjectFilesCalls) == 0,
         "opening and querying file finder after index readiness should not trigger project scans");
}

void TestWorkspaceShellProjectSearchUsesMaintainedIndexWithoutProjectScan() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "README.md", "root\n");
  WriteFile(root / "src" / "search_target.cpp", "needle search_target\n");

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "project search no-scan fixture should open the project");
  Expect(WaitForFileIndexPath(shell, std::filesystem::path("src/search_target.cpp"), true,
                              std::chrono::milliseconds(1000)),
         "project search no-scan fixture should wait for file index initialization");

  util::ResetPerformanceCounters();
  WorkspaceShellTestAccess::ShowSearchSidebar(shell, "needle", false);
  Expect(WaitForProjectSearchCompletion(shell, std::chrono::milliseconds(2000)),
         "project search no-scan fixture should complete search");
  Expect(!WorkspaceShellTestAccess::ProjectSearchResults(shell).empty(),
         "project search no-scan fixture should find indexed matches");
  WorkspaceShellTestAccess::ShowSearchSidebar(shell, "search_target", false);
  Expect(WaitForProjectSearchCompletion(shell, std::chrono::milliseconds(2000)),
         "project search no-scan fixture should complete search query refresh");
  Expect(!WorkspaceShellTestAccess::ProjectSearchResults(shell).empty(),
         "project search no-scan fixture should keep matches on query refresh");
  WorkspaceShellTestAccess::ShowSearchSidebar(shell, "needle", false);
  Expect(WaitForProjectSearchCompletion(shell, std::chrono::milliseconds(2000)),
         "project search no-scan fixture should restart search before replace-all");
  WorkspaceShellTestAccess::ReplaceAllProjectSearchMatches(shell);
  Expect(WaitForProjectSearchCompletion(shell, std::chrono::milliseconds(2000)),
         "project search no-scan fixture should refresh after replace-all");
  Expect(WorkspaceShellTestAccess::ProjectSearchResults(shell).empty(),
         "replace-all should update project search results after indexed replacements");
  Expect(util::ReadPerformanceCounter(
             util::PerfCounterId::ProjectFileScannerCollectProjectFilesCalls) == 0,
         "project search start/query refresh/replace-all after index readiness should not trigger project scans");
}

void TestWorkspaceShellReplaceAllAbortsWithFeedbackPastAggregateCap() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "notes.txt";
  WriteFile(source, "needle needle\n");

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "replace-all abort fixture should open the project");
  Expect(WaitForFileIndexPath(shell, std::filesystem::path("notes.txt"), true,
                              std::chrono::milliseconds(1000)),
         "replace-all abort fixture should wait for file index initialization");

  WorkspaceShellTestAccess::ShowSearchSidebar(shell, "needle", false);
  Expect(WaitForProjectSearchCompletion(shell, std::chrono::milliseconds(2000)),
         "replace-all abort fixture should complete search");
  Expect(!WorkspaceShellTestAccess::ProjectSearchResults(shell).empty(),
         "replace-all abort fixture should find matches");

  WorkspaceShellTestAccess::SetReplaceAllAggregateCapBytes(shell, 1);
  WorkspaceShellTestAccess::ReplaceAllProjectSearchMatches(shell);

  Expect(!WorkspaceShellTestAccess::ProjectSearchError(shell).empty(),
         "replace-all past the aggregate cap must surface an error, not fail silently");
  Expect(ReadFile(source) == "needle needle\n",
         "an aborted replace-all must not have written any file");
}

void TestWorkspaceShellReplaceAllNamesAFileItCouldNotWrite() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "a.txt", "needle\n");
  WriteFile(root / "locked" / "b.txt", "needle\n");
  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false), "open the project");
  Expect(WaitForFileIndexPath(shell, std::filesystem::path("locked/b.txt"), true, std::chrono::milliseconds(1000)),
         "indexed");
  WorkspaceShellTestAccess::ShowSearchSidebar(shell, "needle", false);
  Expect(WaitForProjectSearchCompletion(shell, std::chrono::milliseconds(2000)), "search completes");
  std::filesystem::permissions(root / "locked",
                               std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec);
  if (::access((root / "locked").c_str(), W_OK) == 0) {
    std::filesystem::permissions(root / "locked", std::filesystem::perms::owner_all);
    return;
  }
  WorkspaceShellTestAccess::ReplaceAllProjectSearchMatches(shell);
  std::filesystem::permissions(root / "locked", std::filesystem::perms::owner_all);
  const std::string error = WorkspaceShellTestAccess::ProjectSearchError(shell);
  Expect(error.find("1 could not be written (locked/b.txt: permission denied)") != std::string::npos,
         "the error names the file and the cause: " + error);
  Expect(ReadFile(root / "a.txt") != "needle\n" && ReadFile(root / "locked" / "b.txt") == "needle\n",
         "the writable file is replaced, the other untouched");
}

void TestWorkspaceShellReplaceAllReplacesInADirtyOpenBuffer() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "notes.txt";
  const std::filesystem::path other = root / "other.txt";
  WriteFile(source, "needle\n");
  WriteFile(other, "needle elsewhere\n");

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "replace-all dirty fixture should open the project");
  Expect(WaitForFileIndexSize(shell, 2, std::chrono::milliseconds(1500)),
         "replace-all dirty fixture should index both files");

  WorkspaceShellTestAccess::ShowSearchSidebar(shell, "needle", false);
  Expect(WaitForProjectSearchCompletion(shell, std::chrono::milliseconds(2000)),
         "replace-all dirty fixture should complete search");

  WorkspaceShellTestAccess::OpenFile(shell, source);
  WorkspaceShellTestAccess::ActiveEditor(shell).InsertText("x");
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).dirty(),
         "precondition: the open matched file is dirty");

  WorkspaceShellTestAccess::ReplaceAllProjectSearchMatches(shell);

  const auto& editor = WorkspaceShellTestAccess::ActiveEditor(shell);
  Expect(editor.lines().LineView(0) == "x" && editor.dirty(),
         "the dirty buffer is replaced in place and stays dirty, its edit kept: " +
             std::string(editor.lines().LineView(0)));
  Expect(ReadFile(source) == "needle\n", "the disk under the unsaved buffer is not written");
  Expect(ReadFile(other) == " elsewhere\n", "the closed file is replaced on disk: " + ReadFile(other));
  Expect(WorkspaceShellTestAccess::ProjectSearchError(shell).empty(),
         "nothing is refused: " + WorkspaceShellTestAccess::ProjectSearchError(shell));
}

void TestWorkspaceShellAContentlessReopenedTabEditsNothing() {
  if (::geteuid() == 0) {
    return;
  }
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path file = root / "crlf.txt";
  WriteFile(root / "README.md", "# readme\n");
  WriteFile(file, "one\r\ntwo\r\n");

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false), "open the project");
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "open crlf.txt"), "open crlf.txt");
  const auto& group = WorkspaceShellTestAccess::CurrentProjectState(shell).focused_group();
  WorkspaceShellTestAccess::CloseTab(shell, group.active_tab_index);
  std::filesystem::permissions(file, std::filesystem::perms::none);
  WorkspaceShellTestAccess::ExecuteCommandLine(shell, "reopen-closed-editor");
  const std::string welcome_before = group.welcome_surface.viewport.SerializeText();
  const bool welcome_dirty_before = group.welcome_surface.viewport.dirty();
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  (void)DispatchEvents(shell, {TextInputEvent("typed-into-the-wrong-buffer"), KeyDownEvent(SDLK_RETURN)});
  (void)WorkspaceShellTestAccess::ExecuteCommandLine(shell, "type typed-into-the-wrong-buffer");
  std::filesystem::permissions(file, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);

  Expect(group.has_active_tab() && group.active_tab().path.filename() == "crlf.txt",
         "the reopened tab is active, naming its file");
  const editor::TextViewport* active = WorkspaceShellTestAccess::ActiveNavigableViewport(shell);
  Expect(active != &group.welcome_surface.viewport, "the active editor is not the welcome surface's view");
  Expect(active == nullptr || active->path().filename() == "crlf.txt",
         "it is the tab's own: " + (active ? active->path().string() : std::string("none")));
  Expect(group.welcome_surface.viewport.SerializeText() == welcome_before &&
             group.welcome_surface.viewport.dirty() == welcome_dirty_before,
         "typing does not reach the welcome surface's view behind the tab: '" +
             group.welcome_surface.viewport.SerializeText() + "'");
  for (const auto& tab : group.open_tabs) {
    Expect(!tab.editor_state.has_value() || !tab.editor_state->viewport.dirty(),
           "nor any open buffer: " + tab.title);
  }
}

void TestWorkspaceShellReplaceAllKeepsAUtf8ByteOrderMark() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::string bom = "\xEF\xBB\xBF";
  WriteFile(root / "bom.txt", bom + "a needle here\n");
  WriteFile(root / "plain.txt", "a needle there\n");

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false), "open the project");
  Expect(WaitForFileIndexSize(shell, 2, std::chrono::milliseconds(1500)), "index both files");
  WorkspaceShellTestAccess::ShowSearchSidebar(shell, "needle", false);
  Expect(WaitForProjectSearchCompletion(shell, std::chrono::milliseconds(2000)), "the search completes");
  WorkspaceShellTestAccess::ReplaceAllProjectSearchMatches(shell);
  const bool replaced = WaitUntil(
      [&root]() {
        return ReadFile(root / "bom.txt").find("needle") == std::string::npos &&
               ReadFile(root / "plain.txt").find("needle") == std::string::npos;
      },
      std::chrono::seconds(2), std::chrono::milliseconds(5),
      [&shell]() { WorkspaceShellTestAccess::ConsumeProjectSearchUpdates(shell); });
  Expect(replaced, "replace-all rewrites both files");
  const std::string with_bom = ReadFile(root / "bom.txt");
  Expect(with_bom == bom + "a  here\n", "the BOM file keeps its mark and changes only the match, got " +
                                               std::to_string(with_bom.size()) + " bytes");
  Expect(ReadFile(root / "plain.txt") == "a  there\n", "a file without one gains none");
}

void TestWorkspaceShellReplaceAllReadsOnlyMatchedFiles() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "match_a.txt", "needle here\n");
  WriteFile(root / "match_b.txt", "a needle line\n");
  constexpr int kDecoyCount = 30;
  for (int i = 0; i < kDecoyCount; ++i) {
    WriteFile(root / ("decoy_" + std::to_string(i) + ".txt"), "hay only\n");
  }

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "replace-all matched-only fixture should open the project");
  Expect(WaitForFileIndexSize(shell, static_cast<std::size_t>(kDecoyCount) + 2,
                              std::chrono::milliseconds(1500)),
         "replace-all matched-only fixture should index the whole tree");

  WorkspaceShellTestAccess::ShowSearchSidebar(shell, "needle", false);
  Expect(WaitForProjectSearchCompletion(shell, std::chrono::milliseconds(2000)),
         "replace-all matched-only fixture should complete the search");
  Expect(!WorkspaceShellTestAccess::ProjectSearchTruncated(shell),
         "a two-file search must not truncate");
  Expect(WorkspaceShellTestAccess::ProjectSearchResults(shell).size() == 2,
         "the search should find exactly the two matching files");

  const std::uint64_t candidates_before =
      util::ReadPerformanceCounter(util::PerfCounterId::SearchProjectReplaceCandidateFiles);
  const std::uint64_t reads_before =
      util::ReadPerformanceCounter(util::PerfCounterId::SearchProjectReplaceFilesRead);
  WorkspaceShellTestAccess::ReplaceAllProjectSearchMatches(shell);
  const bool replaced = WaitUntil(
      [&root]() {
        return ReadFile(root / "match_a.txt").find("needle") == std::string::npos &&
               ReadFile(root / "match_b.txt").find("needle") == std::string::npos;
      },
      std::chrono::seconds(2), std::chrono::milliseconds(5),
      [&shell]() { WorkspaceShellTestAccess::ConsumeProjectSearchUpdates(shell); });
  Expect(replaced, "replace-all should rewrite both matched files");
  Expect(WaitForProjectSearchCompletion(shell, std::chrono::milliseconds(2000)),
         "replace-all should settle its trailing refresh");
  const std::uint64_t candidates =
      util::ReadPerformanceCounter(util::PerfCounterId::SearchProjectReplaceCandidateFiles) -
      candidates_before;
  const std::uint64_t reads =
      util::ReadPerformanceCounter(util::PerfCounterId::SearchProjectReplaceFilesRead) -
      reads_before;

  Expect(candidates == 2,
         "replace-all must take the cached-results fast path, not the whole-catalog fallback");
  Expect(reads == 2, "replace-all must read only the two matched files, not the whole project");

  Expect(ReadFile(root / "match_a.txt").find("needle") == std::string::npos,
         "the first matched file should have its needle replaced");
  Expect(ReadFile(root / "match_b.txt").find("needle") == std::string::npos,
         "the second matched file should have its needle replaced");
  Expect(ReadFile(root / "decoy_0.txt") == "hay only\n",
         "a no-match decoy file must be left untouched");
}

void TestWorkspaceShellReplaceAllFallsBackWhenResultsTruncated() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  constexpr int kMatchFiles = 210;
  for (int i = 0; i < kMatchFiles; ++i) {
    WriteFile(root / ("m_" + std::to_string(i) + ".txt"), "needle\n");
  }

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "truncated replace-all fixture should open the project");
  Expect(WaitForFileIndexSize(shell, static_cast<std::size_t>(kMatchFiles),
                              std::chrono::milliseconds(10000)),
         "truncated replace-all fixture should index the whole match set");

  const std::uint64_t revision_before =
      WorkspaceShellTestAccess::ProjectSearchResultsRevision(shell);
  WorkspaceShellTestAccess::ShowSearchSidebar(shell, "needle", false);
  Expect(WaitForProjectSearchResults(shell, revision_before, std::chrono::milliseconds(15000)),
         "truncated replace-all fixture should complete the search");
  const std::size_t candidate_files = WorkspaceShellTestAccess::ProjectSearchTotalFiles(shell);
  const std::size_t indexed_files = WorkspaceShellTestAccess::FileIndexSize(shell);
  Expect(candidate_files >= static_cast<std::size_t>(kMatchFiles),
         "the search must have been given every match file as a candidate (candidates=" +
             std::to_string(candidate_files) + ", indexed=" + std::to_string(indexed_files) +
             ", wanted>=" + std::to_string(kMatchFiles) +
             ", index_incomplete=" +
             (WorkspaceShellTestAccess::ProjectSearchIndexIncomplete(shell) ? "yes" : "no") + ")");
  Expect(WorkspaceShellTestAccess::ProjectSearchTruncated(shell),
         "more matches than the display cap must mark the results truncated (candidates=" +
             std::to_string(candidate_files) + ", results=" +
             std::to_string(WorkspaceShellTestAccess::ProjectSearchResults(shell).size()) + ")");

  WorkspaceShellTestAccess::ReplaceAllProjectSearchMatches(shell);

  for (int i = 0; i < kMatchFiles; ++i) {
    Expect(ReadFile(root / ("m_" + std::to_string(i) + ".txt")).find("needle") ==
               std::string::npos,
           "truncated replace-all must still rewrite every matching file");
  }
}

void TestWorkspaceShellForcedIndexRefreshDoesNotDropABatchThatLandedDuringTheScan() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "README.md", "root\n");

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "forced-refresh ordering fixture should open the project");
  Expect(WaitForFileIndexSize(shell, 1, std::chrono::milliseconds(5000)),
         "the project's own initial scan lands");
  Expect(WorkspaceShellTestAccess::QuiesceFileIndexWatcherForTesting(shell),
         "the live watcher is quiesced");

  const std::uint64_t version_at_dispatch =
      WorkspaceShellTestAccess::ProjectFileIndexVersion(shell);
  std::vector<project::ProjectFile> scanned_without_injected =
      project::FileIndex::ScanFiles(root, false, {}, nullptr);

  const std::filesystem::path relative_injected = std::filesystem::path("src/injected.cpp");
  WriteFile(root / relative_injected, "needle\n");
  (void)WorkspaceShellTestAccess::ApplyFileIndexBatchForTesting(
      shell, BuildInjectedCreateBatch(root, relative_injected));
  Expect(WorkspaceShellTestAccess::ProjectFileIndexContains(shell, relative_injected),
         "the batch must put the injected entry in the index to begin with");
  Expect(WorkspaceShellTestAccess::ProjectFileIndexVersion(shell) != version_at_dispatch,
         "a batch that changed the index must move its version, or the guard has "
         "nothing to compare");

  WorkspaceShellTestAccess::ApplyForcedFileIndexRefreshForTesting(
      shell, root, std::move(scanned_without_injected), version_at_dispatch);
  Expect(WorkspaceShellTestAccess::ProjectFileIndexContains(shell, relative_injected),
         "a rescan that predates a watcher batch must not delete what the batch added");

  WriteFile(root / "second.txt", "second\n");
  const std::uint64_t current_version =
      WorkspaceShellTestAccess::ProjectFileIndexVersion(shell);
  WorkspaceShellTestAccess::ApplyForcedFileIndexRefreshForTesting(
      shell, root, project::FileIndex::ScanFiles(root, false, {}, nullptr), current_version);
  Expect(WorkspaceShellTestAccess::ProjectFileIndexContains(shell, "second.txt"),
         "an up-to-date rescan must still be applied");
  Expect(WorkspaceShellTestAccess::ProjectFileIndexContains(shell, relative_injected),
         "and it still sees the injected file, which is on disk");
}

void TestWorkspaceShellSearchRerunOnAFileChangeKeepsTheListAndSelection() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  for (const char* name : {"a.cpp", "b.cpp", "c.cpp", "d.cpp", "e.cpp"}) {
    WriteFile(root / name, std::string("// needle in ") + name + "\n");
  }
  WriteFile(root / "other.txt", "nothing here\n");

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "search re-run fixture should open the project");
  Expect(WaitForFileIndexSize(shell, 6, std::chrono::milliseconds(5000)),
         "search re-run fixture should finish its initial scan");
  Expect(WorkspaceShellTestAccess::QuiesceFileIndexWatcherForTesting(shell),
         "search re-run fixture should quiesce its watcher");
  while (WorkspaceShellTestAccess::ReloadProjectIfFilesChanged(shell, false)) {
  }

  const std::uint64_t first_revision = WorkspaceShellTestAccess::ProjectSearchResultsRevision(shell);
  WorkspaceShellTestAccess::ShowSearchSidebar(shell, "needle", false);
  Expect(WaitForProjectSearchResults(shell, first_revision, std::chrono::milliseconds(5000)),
         "the first search should complete");
  Expect(WorkspaceShellTestAccess::ProjectSearchResults(shell).size() == 5,
         "the fixture has five matches");
  WorkspaceShellTestAccess::SetProjectSearchSelectedIndex(shell, 3);
  WorkspaceShellTestAccess::SetSidebarScrollRow(shell, 2);
  const std::filesystem::path selected_path =
      WorkspaceShellTestAccess::ProjectSearchResults(shell)[3].relative_path;

  WriteFile(root / "aa.cpp", "// needle, new\n");
  platform::IndexUpdateBatch batch = BuildInjectedCreateBatch(root, "aa.cpp");
  Expect(WorkspaceShellTestAccess::DispatchFileIndexWatcherBatchForTesting(shell, std::move(batch)),
         "the change should be delivered");
  const std::uint64_t before_rerun = WorkspaceShellTestAccess::ProjectSearchResultsRevision(shell);
  Expect(WorkspaceShellTestAccess::ReloadProjectIfFilesChanged(shell, false),
         "the change should be applied");

  Expect(WorkspaceShellTestAccess::ProjectSearchResults(shell).size() == 5 &&
             WorkspaceShellTestAccess::ProjectSearchSelectedIndex(shell) == 3 &&
             WorkspaceShellTestAccess::SidebarScrollRow(shell) == 2,
         "while the re-run is in flight the list on screen stays as it was");

  Expect(WaitForProjectSearchResults(shell, before_rerun, std::chrono::milliseconds(5000)),
         "the re-run should complete");
  const auto& results = WorkspaceShellTestAccess::ProjectSearchResults(shell);
  Expect(results.size() == 6, "the re-run picks up the new match");
  const std::size_t selected = WorkspaceShellTestAccess::ProjectSearchSelectedIndex(shell);
  Expect(selected < results.size() && results[selected].relative_path == selected_path,
         "the selection follows the same match, not the same row number");
  Expect(WorkspaceShellTestAccess::SidebarScrollRow(shell) == 2,
         "a re-run caused by a file change does not scroll the list to the top");
}

void TestWorkspaceShellInjectedFileIndexBatchUpdatesFinderAndSearch() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "README.md", "root\n");

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "injected file-index batch fixture should open the project");
  Expect(WaitForFileIndexSize(shell, 1, std::chrono::milliseconds(5000)),
         "injected file-index batch fixture should finish its initial project scan");

  const std::filesystem::path relative_injected = std::filesystem::path("src/injected.cpp");
  const std::filesystem::path absolute_injected = root / relative_injected;
  WriteFile(absolute_injected, "needle\n");
  const platform::IndexUpdateBatch create_batch =
      BuildInjectedCreateBatch(root, relative_injected);
  const bool create_changed =
      WorkspaceShellTestAccess::ApplyFileIndexBatchForTesting(shell, create_batch);
  Expect(WorkspaceShellTestAccess::ProjectFileIndexContains(shell, relative_injected),
         "injected create batch should leave the new entry in the file index");
  if (create_changed) {
    Expect(WorkspaceShellTestAccess::ReloadProjectIfFilesChanged(shell, false),
           "injected create batch should flow through project reload plumbing");
  } else {
    WorkspaceShellTestAccess::ReloadProjectIfFilesChanged(shell, false);
  }

  Expect(WorkspaceShellTestAccess::ExecuteFilesFromShortcut(shell),
         "injected create batch fixture should open file finder");
  WorkspaceShellTestAccess::SetFileFinderQuery(shell, "injected");
  Expect(WorkspaceShellTestAccess::FileFinderResultCount(shell) >= 1,
         "injected create batch should surface new entries in file finder");

  WorkspaceShellTestAccess::ShowSearchSidebar(shell, "needle", false);
  Expect(WaitForProjectSearchCompletion(shell, std::chrono::milliseconds(2000)),
         "injected create batch fixture should complete project search");
  Expect(!WorkspaceShellTestAccess::ProjectSearchResults(shell).empty(),
         "injected create batch should surface new entries in project search");

  std::error_code remove_error;
  std::filesystem::remove(absolute_injected, remove_error);
  Expect(!remove_error, "injected delete batch fixture should remove injected file from disk");
  const platform::IndexUpdateBatch delete_batch = BuildInjectedDeleteBatch(relative_injected);
  const bool delete_changed =
      WorkspaceShellTestAccess::ApplyFileIndexBatchForTesting(shell, delete_batch);
  if (!delete_changed) {
    WorkspaceShellTestAccess::SetFileFinderQuery(shell, "injected");
  }
  Expect(delete_changed || WorkspaceShellTestAccess::FileFinderResultCount(shell) == 0,
         "injected delete batch should mutate the file index");
  Expect(WorkspaceShellTestAccess::ReloadProjectIfFilesChanged(shell, false),
         "injected delete batch should flow through project reload plumbing");

  Expect(WorkspaceShellTestAccess::ExecuteFilesFromShortcut(shell),
         "injected delete batch fixture should reopen file finder");
  WorkspaceShellTestAccess::SetFileFinderQuery(shell, "injected");
  Expect(WorkspaceShellTestAccess::FileFinderResultCount(shell) == 0,
         "injected delete batch should remove entries from file finder");

  WorkspaceShellTestAccess::ShowSearchSidebar(shell, "needle", false);
  Expect(WaitForProjectSearchCompletion(shell, std::chrono::milliseconds(2000)),
         "injected delete batch fixture should complete project search");
  Expect(WorkspaceShellTestAccess::ProjectSearchResults(shell).empty(),
         "injected delete batch should remove entries from project search");
}

void TestWorkspaceShellUntitledTabFloodIsBounded() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "src" / "main.cpp", "int main() { return 0; }\n");

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "project fixture should open");

  const std::size_t flood = microide::workspace::kMaxOpenTabsPerGroup + 200;
  for (std::size_t i = 0; i < flood; ++i) {
    RunCommandLine(shell, "tab");
  }
  Expect(WorkspaceShellTestAccess::OpenTabs(shell).size() <=
             microide::workspace::kMaxOpenTabsPerGroup,
         "untitled-tab flood must be bounded by the per-group tab ceiling");
}

void TestWorkspaceShellUntitledTabAtTheCeilingIsRejected() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "src" / "main.cpp", "int main() { return 0; }\n");

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "project fixture should open");

  while (WorkspaceShellTestAccess::OpenTabs(shell).size() <
         microide::workspace::kMaxOpenTabsPerGroup) {
    Expect(RunCommandLine(shell, "tab"), "filling the group must keep succeeding");
  }
  Expect(!RunCommandLine(shell, "tab"),
         "a `tab` the group has no room for must be reported as a failure");
  Expect(WorkspaceShellTestAccess::OpenTabs(shell).size() ==
             microide::workspace::kMaxOpenTabsPerGroup,
         "the refused `tab` must not have added a tab");
}

void TestWorkspaceShellOpenAtTheTabCeilingIsRejected() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path target = root / "target.txt";
  WriteFile(root / "src" / "main.cpp", "int main() { return 0; }\n");
  WriteFile(target, "target contents\n");

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "project fixture should open");

  while (WorkspaceShellTestAccess::OpenTabs(shell).size() <
         microide::workspace::kMaxOpenTabsPerGroup) {
    Expect(RunCommandLine(shell, "tab"), "filling the group must keep succeeding");
  }
  const std::size_t filled = WorkspaceShellTestAccess::OpenTabs(shell).size();

  Expect(!RunCommandLine(shell, "open " + target.string()),
         "an open the group has no room for must be reported as a failure, not as "
         "success with the previous tab still active");
  Expect(WorkspaceShellTestAccess::OpenTabs(shell).size() == filled,
         "the refused open must not have added a tab");
}

void TestWorkspaceShellRevealOpensAndCentresTheLine() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  std::string body;
  for (int i = 1; i <= 300; ++i) {
    body += "row " + std::to_string(i) + "\n";
  }
  WriteFile(root / "src" / "long.txt", body);
  WriteFile(root / "README.md", "root\n");
  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);

  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "reveal src/long.txt 150:3"),
         "reveal is accepted for an existing file");
  const microide::editor::TextViewport& viewport =
      WorkspaceShellTestAccess::GroupActiveViewport(shell, 0);
  Expect(viewport.path().filename() == "long.txt", "reveal opened the file");
  Expect(viewport.cursor_line() == 149 && viewport.cursor_column() == 2,
         "the caret lands on the 1-based line:col");
  const_cast<microide::editor::TextViewport&>(viewport).SetViewportSize(30, 120);
  Expect(viewport.scroll_line() == 149 - 15,
         "the line is centred once the new tab has its real size");

  Expect(!WorkspaceShellTestAccess::ExecuteCommandLine(shell, "reveal src/long.txt"),
         "reveal without a line is rejected");
  Expect(!WorkspaceShellTestAccess::ExecuteCommandLine(shell, "reveal src/long.txt 0"),
         "reveal with a non-positive line is rejected");
}

void TestWorkspaceShellTabToGroupMovesTheActiveTabAcrossPanes() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "a.txt", "a\n");
  WriteFile(root / "b.txt", "b\n");
  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::OpenFile(shell, root / "a.txt");
  Expect(WorkspaceShellTestAccess::SplitEditorGroup(
             shell, microide::workspace::EditorSplitOrientation::Vertical),
         "split the editor");
  WorkspaceShellTestAccess::OpenFile(shell, root / "b.txt");
  Expect(WorkspaceShellTestAccess::EditorGroupCount(shell) == 2 &&
             WorkspaceShellTestAccess::FocusedGroupIndex(shell) == 1,
         "two panes, the second focused");
  const std::size_t before_left = WorkspaceShellTestAccess::GroupTabCount(shell, 0);
  const std::size_t before_right = WorkspaceShellTestAccess::GroupTabCount(shell, 1);

  Expect(!WorkspaceShellTestAccess::ExecuteCommandLine(shell, "tab-to-group 0"),
         "group 0 is not a group: the numbering is the one focus-group uses");
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "tab-to-group 1"),
         "tab-to-group into the other pane is accepted");
  Expect(WorkspaceShellTestAccess::GroupTabCount(shell, 0) == before_left + 1,
         "the target pane gained the tab");
  Expect(WorkspaceShellTestAccess::EditorGroupCount(shell) == 1 ||
             WorkspaceShellTestAccess::GroupTabCount(shell, 1) == before_right - 1,
         "the source pane lost it (or collapsed when emptied)");

  Expect(!WorkspaceShellTestAccess::ExecuteCommandLine(shell, "tab-to-group 9"),
         "a group that does not exist is rejected");
  Expect(!WorkspaceShellTestAccess::ExecuteCommandLine(shell, "tab-to-group x"),
         "a non-numeric group is rejected");
}

}

void TestWorkspaceShellEditorGroupSplitFocusCloseSemantics() {
  using microide::workspace::EditorSplitOrientation;
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path file_a = root / "a.txt";
  const std::filesystem::path file_b = root / "b.txt";
  std::string many_lines;
  for (int i = 0; i < 200; ++i) {
    many_lines += "line " + std::to_string(i) + "\n";
  }
  WriteFile(file_a, many_lines);
  WriteFile(file_b, "only b\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1000, 700);
  WorkspaceShellTestAccess::OpenFile(shell, file_a);

  WorkspaceShellTestAccess::SetGroupScrollLine(shell, 0, 40);

  Expect(WorkspaceShellTestAccess::SplitEditorGroup(shell, EditorSplitOrientation::Vertical),
         "split-right should succeed when an editor tab is active");
  Expect(WorkspaceShellTestAccess::EditorGroupCount(shell) == 2,
         "splitting should create a second editor group");
  Expect(WorkspaceShellTestAccess::FocusedGroupIndex(shell) == 1,
         "splitting should focus the new group");
  Expect(WorkspaceShellTestAccess::GroupSplitOrientation(shell) == EditorSplitOrientation::Vertical,
         "split-right should set the vertical (side-by-side) orientation");
  Expect(WorkspaceShellTestAccess::GroupActiveViewport(shell, 1).path() == file_a.lexically_normal(),
         "the split clone should start on the same document");
  Expect(WorkspaceShellTestAccess::GroupActiveViewport(shell, 0).line_count() ==
             WorkspaceShellTestAccess::GroupActiveViewport(shell, 1).line_count(),
         "both groups should observe the same shared buffer contents");

  WorkspaceShellTestAccess::SetGroupScrollLine(shell, 1, 5);
  Expect(WorkspaceShellTestAccess::GroupActiveViewport(shell, 0).scroll_line() == 40,
         "scrolling the second group must not disturb the first group's scroll");
  Expect(WorkspaceShellTestAccess::GroupActiveViewport(shell, 1).scroll_line() == 5,
         "the second group should keep its own scroll position");

  WorkspaceShellTestAccess::OpenFile(shell, file_b);
  Expect(WorkspaceShellTestAccess::GroupActiveViewport(shell, 0).path() == file_a.lexically_normal(),
         "the first group should still show the original document");
  Expect(WorkspaceShellTestAccess::GroupActiveViewport(shell, 1).path() == file_b.lexically_normal(),
         "the second group should show the newly opened document");

  Expect(WorkspaceShellTestAccess::FocusOtherEditorGroup(shell),
         "focus-other-group should switch focus when two groups exist");
  Expect(WorkspaceShellTestAccess::FocusedGroupIndex(shell) == 0, "focus should move to group 0");
  Expect(WorkspaceShellTestAccess::GroupActiveViewport(shell, 0).scroll_line() == 40,
         "group 0 scroll should survive a focus switch (guards the original scroll-reset bug)");

  Expect(WorkspaceShellTestAccess::SplitEditorGroup(shell, EditorSplitOrientation::Horizontal),
         "splitting a pane of an existing split should succeed");
  Expect(WorkspaceShellTestAccess::EditorGroupCount(shell) == 3,
         "splitting a second time should add a third editor group");
  Expect(WorkspaceShellTestAccess::FocusedGroupIndex(shell) == 1,
         "the pane carved below group 0 should be focused");
  Expect(WorkspaceShellTestAccess::GroupSplitOrientation(shell) == EditorSplitOrientation::Vertical,
         "a nested split should not change the outermost orientation");

  Expect(WorkspaceShellTestAccess::CloseEditorGroup(shell),
         "close-group should succeed while more than one group exists");
  Expect(WorkspaceShellTestAccess::EditorGroupCount(shell) == 2,
         "closing a group should drop exactly that pane");
  Expect(WorkspaceShellTestAccess::CloseEditorGroup(shell),
         "close-group should succeed while a second group exists");
  Expect(WorkspaceShellTestAccess::EditorGroupCount(shell) == 1,
         "closing the last extra group should collapse back to one group");
  Expect(WorkspaceShellTestAccess::FocusedGroupIndex(shell) == 0,
         "the surviving group should become focused");
  Expect(WorkspaceShellTestAccess::GroupSplitOrientation(shell) == EditorSplitOrientation::None,
         "collapsing should clear the split orientation");
  Expect(!WorkspaceShellTestAccess::FocusOtherEditorGroup(shell),
         "focus-other-group should be a no-op with a single group");
  Expect(!WorkspaceShellTestAccess::CloseEditorGroup(shell),
         "close-group should be a no-op with a single group");
}

void TestWorkspaceShellSplitContextMenuAvailabilityAndTreeOpen() {
  using microide::workspace::TreeContextTargetKind;
  using ActionId = WorkspaceShell::ActionId;

  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path file_a = root / "a.txt";
  const std::filesystem::path file_b = root / "b.txt";
  WriteFile(file_a, "alpha\n");
  WriteFile(file_b, "beta\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1000, 700);

  Expect(!WorkspaceShellTestAccess::IsActionEnabled(shell, ActionId::SplitEditorRight),
         "split-right should be disabled with no active editor");

  WorkspaceShellTestAccess::OpenFile(shell, file_a);
  Expect(WorkspaceShellTestAccess::IsActionEnabled(shell, ActionId::SplitEditorRight),
         "split-right should be enabled with one editor group");
  Expect(WorkspaceShellTestAccess::IsActionEnabled(shell, ActionId::SplitEditorDown),
         "split-down should be enabled with one editor group");

  WorkspaceShellTestAccess::OpenTreeContextMenuForPath(shell, TreeContextTargetKind::File, file_b);
  Expect(WorkspaceShellTestAccess::ExecuteContextMenuAction(shell, ActionId::SplitEditorRight),
         "tree Split Right should execute");
  Expect(WorkspaceShellTestAccess::EditorGroupCount(shell) == 2,
         "tree Split Right should create a second group");
  Expect(WorkspaceShellTestAccess::FocusedGroupIndex(shell) == 1,
         "the new split group should be focused");
  Expect(WorkspaceShellTestAccess::GroupActiveViewport(shell, 1).path() == file_b.lexically_normal(),
         "the tree-split group should show the right-clicked file");
  Expect(WorkspaceShellTestAccess::GroupActiveViewport(shell, 0).path() == file_a.lexically_normal(),
         "the original group should keep its own file");

  Expect(WorkspaceShellTestAccess::IsActionEnabled(shell, ActionId::SplitEditorRight),
         "split-right should stay enabled while the editor area is below its cap");
  Expect(WorkspaceShellTestAccess::IsActionEnabled(shell, ActionId::SplitEditorDown),
         "split-down should stay enabled while the editor area is below its cap");
  while (WorkspaceShellTestAccess::EditorGroupCount(shell) < microide::workspace::kMaxEditorGroups) {
    Expect(WorkspaceShellTestAccess::SplitEditorGroup(
               shell, microide::workspace::EditorSplitOrientation::Vertical),
           "splitting below the cap should succeed");
  }
  Expect(!WorkspaceShellTestAccess::IsActionEnabled(shell, ActionId::SplitEditorRight),
         "split-right should be disabled at the pane cap");
  Expect(!WorkspaceShellTestAccess::IsActionEnabled(shell, ActionId::SplitEditorDown),
         "split-down should be disabled at the pane cap");
}

void TestWorkspaceShellRevealInFileTreeFromTabMenu() {
  using ActionId = WorkspaceShell::ActionId;
  using SidebarMode = WorkspaceShell::SidebarMode;

  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path nested = root / "src" / "deep" / "main.cpp";
  WriteFile(nested, "int main() { return 0; }\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1000, 700);
  WorkspaceShellTestAccess::OpenFile(shell, nested);
  Expect(WorkspaceShellTestAccess::IsActionEnabled(shell, ActionId::RevealInFileTree),
         "reveal-in-tree should be enabled for a project-backed active tab");

  Expect(WorkspaceShellTestAccess::SelectTreePath(shell, root / "src"),
         "src folder should be selectable in the tree");
  WorkspaceShellTestAccess::CollapseTreeSelection(shell);
  WorkspaceShellTestAccess::ShowSearchSidebar(shell, "", false);
  Expect(WorkspaceShellTestAccess::SidebarMode(shell) != SidebarMode::Tree,
         "precondition: sidebar is not on the Tree view before reveal");

  Expect(WorkspaceShellTestAccess::ExecuteMenuAction(shell, ActionId::RevealInFileTree),
         "reveal-in-tree should execute from the tab menu");
  Expect(WorkspaceShellTestAccess::SidebarVisible(shell),
         "reveal should make the sidebar visible");
  Expect(WorkspaceShellTestAccess::SidebarMode(shell) == SidebarMode::Tree,
         "reveal should switch the sidebar to the Tree view");
  Expect(WorkspaceShellTestAccess::SelectedTreePath(shell) == nested.lexically_normal(),
         "reveal should force-expand ancestors and select the revealed file");
}

void TestWorkspaceShellOpenBufferViewCountsAcrossGroups() {
  using microide::workspace::EditorSplitOrientation;
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path file_a = root / "a.txt";
  const std::filesystem::path file_b = root / "b.txt";
  WriteFile(file_a, "alpha\n");
  WriteFile(file_b, "beta\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1000, 700);
  WorkspaceShellTestAccess::OpenFile(shell, file_a);

  const std::string key_a = file_a.lexically_normal().generic_string();
  const std::string key_b = file_b.lexically_normal().generic_string();

  {
    const auto counts = WorkspaceShellTestAccess::OpenBufferViewCounts(shell);
    Expect(counts.at(key_a) == 1, "a single open tab counts as one view");
    Expect(WorkspaceShellTestAccess::CountOpenBufferViews(shell, file_a) == 1,
           "the map and the single-path count agree for one view");
  }

  Expect(WorkspaceShellTestAccess::SplitEditorGroup(shell, EditorSplitOrientation::Vertical),
         "split-right should succeed when an editor tab is active");
  {
    const auto counts = WorkspaceShellTestAccess::OpenBufferViewCounts(shell);
    Expect(counts.at(key_a) == 2, "a buffer viewed by both groups counts as two views");
    Expect(WorkspaceShellTestAccess::CountOpenBufferViews(shell, file_a) == 2,
           "the map and the single-path count agree for a shared buffer");
  }

  WorkspaceShellTestAccess::OpenFile(shell, file_b);
  {
    const auto counts = WorkspaceShellTestAccess::OpenBufferViewCounts(shell);
    Expect(counts.at(key_a) == 2, "a.txt is still viewed by both groups");
    Expect(counts.at(key_b) == 1, "b.txt has a single view in the second group");
  }

  Expect(WorkspaceShellTestAccess::CloseEditorGroup(shell),
         "close-group should succeed while a second group exists");
  Expect(WorkspaceShellTestAccess::EditorGroupCount(shell) == 1,
         "closing a group collapses back to one group");
  {
    const auto counts = WorkspaceShellTestAccess::OpenBufferViewCounts(shell);
    Expect(counts.at(key_a) == 1, "the surviving group keeps its only view of a.txt");
    Expect(counts.find(key_b) == counts.end(), "b.txt has no remaining views after the group closes");
    Expect(WorkspaceShellTestAccess::CountOpenBufferViews(shell, file_b) == 0,
           "the single-path count also reports b.txt fully closed");
  }
}

void TestWorkspaceShellRegexReplaceAllAcrossFiles() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "a.txt", "foo_alpha and foo_beta\n");
  WriteFile(root / "b.txt", "foo_gamma\n");
  WriteFile(root / "c.txt", "nothing here\n");

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "regex replace-all fixture should open the project");
  Expect(WaitForFileIndexSize(shell, 3, std::chrono::milliseconds(1000)),
         "regex replace-all fixture should wait for the whole tree to be indexed");

  WorkspaceShellTestAccess::SetProjectSearchPatternModeRegex(shell, true);
  WorkspaceShellTestAccess::ShowSearchSidebar(shell, "foo_(\\w+)", false);
  Expect(WaitForProjectSearchCompletion(shell, std::chrono::milliseconds(2000)),
         "regex search should complete");
  Expect(!WorkspaceShellTestAccess::ProjectSearchResults(shell).empty(),
         "regex search should find matches across files");
  Expect(WorkspaceShellTestAccess::ProjectSearchCanReplaceAll(shell),
         "regex mode should now permit replace-all");

  WorkspaceShellTestAccess::SetProjectSearchReplaceText(shell, "bar_$1");
  WorkspaceShellTestAccess::ReplaceAllProjectSearchMatches(shell);
  Expect(WaitForProjectSearchCompletion(shell, std::chrono::milliseconds(2000)),
         "regex replace-all should refresh the search after writing");

  Expect(ReadFile(root / "a.txt") == "bar_alpha and bar_beta\n",
         "every capture-group match on a line should be replaced");
  Expect(ReadFile(root / "b.txt") == "bar_gamma\n",
         "matches in other files should be replaced too");
  Expect(ReadFile(root / "c.txt") == "nothing here\n",
         "a file with no match must be left untouched");
}

void TestWorkspaceShellRegexReplaceAllInvalidReplacementAborts() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "a.txt";
  WriteFile(source, "foo_alpha\n");

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "invalid-replacement fixture should open the project");
  Expect(WaitForFileIndexPath(shell, std::filesystem::path("a.txt"), true,
                              std::chrono::milliseconds(1000)),
         "invalid-replacement fixture should wait for file index initialization");

  WorkspaceShellTestAccess::SetProjectSearchPatternModeRegex(shell, true);
  WorkspaceShellTestAccess::ShowSearchSidebar(shell, "foo_(\\w+)", false);
  Expect(WaitForProjectSearchCompletion(shell, std::chrono::milliseconds(2000)),
         "invalid-replacement fixture should complete search");

  WorkspaceShellTestAccess::SetProjectSearchReplaceText(shell, "bar_\\q");
  WorkspaceShellTestAccess::ReplaceAllProjectSearchMatches(shell);

  Expect(!WorkspaceShellTestAccess::ProjectSearchError(shell).empty(),
         "an invalid replacement pattern must surface an error");
  Expect(ReadFile(source) == "foo_alpha\n",
         "an aborted regex replace-all must not write any file");
}

void RegisterWorkspaceShellProjectTests(std::vector<TestCase>& tests) {
  AddTest(tests, "WorkspaceShell/SettingsPickerRowsBelowTheCardStillPick",
          TestWorkspaceShellSettingsPickerRowsBelowTheCardStillPick);
  AddTest(tests, "WorkspaceShell/RevealOpensAndCentresTheLine",
          TestWorkspaceShellRevealOpensAndCentresTheLine);
  AddTest(tests, "WorkspaceShell/TabToGroupMovesTheActiveTabAcrossPanes",
          TestWorkspaceShellTabToGroupMovesTheActiveTabAcrossPanes);
  AddTest(tests, "WorkspaceShell/RegexReplaceAllAcrossFiles",
          TestWorkspaceShellRegexReplaceAllAcrossFiles);
  AddTest(tests, "WorkspaceShell/RegexReplaceAllInvalidReplacementAborts",
          TestWorkspaceShellRegexReplaceAllInvalidReplacementAborts);
  AddTest(tests, "WorkspaceShell/OpenBufferViewCountsAcrossGroups",
          TestWorkspaceShellOpenBufferViewCountsAcrossGroups);
  AddTest(tests, "WorkspaceShell/UntitledTabFloodIsBounded",
          TestWorkspaceShellUntitledTabFloodIsBounded);
  AddTest(tests, "WorkspaceShell/OpenAtTheTabCeilingIsRejected",
          TestWorkspaceShellOpenAtTheTabCeilingIsRejected);
  AddTest(tests, "WorkspaceShell/UntitledTabAtTheCeilingIsRejected",
          TestWorkspaceShellUntitledTabAtTheCeilingIsRejected);
  AddTest(tests, "WorkspaceShell/ProjectOpenMenuUsesNativePickerSelection",
          TestWorkspaceShellProjectOpenMenuUsesNativePickerSelection);
  AddTest(tests, "WorkspaceShell/NativeDialogsAreOffInTests",
          TestWorkspaceShellNativeDialogsAreOffInTests);
  AddTest(tests, "WorkspaceShell/ProjectOpenCommandUsesNativePickerAtActiveProjectRoot",
          TestWorkspaceShellProjectOpenCommandUsesNativePickerAtActiveProjectRoot);
  AddTest(tests, "WorkspaceShell/ProjectOpenMenuFallsBackToTypedPathWhenNativePickerFails",
          TestWorkspaceShellProjectOpenMenuFallsBackToTypedPathWhenNativePickerFails);
  AddTest(tests, "WorkspaceShell/ValueCommandsFromTheMenuSeedTheCommandPalette",
          TestWorkspaceShellValueCommandsFromTheMenuSeedTheCommandPalette);
  AddTest(tests, "WorkspaceShell/TreeRowDragAndDrop", TestWorkspaceShellTreeRowDragAndDrop);
  AddTest(tests, "WorkspaceShell/BareArgumentCommandsOfferTheirChoices",
          TestWorkspaceShellBareArgumentCommandsOfferTheirChoices);
  AddTest(tests, "WorkspaceShell/TreeMoveRepaintsEveryPaneShowingTheFile",
          TestWorkspaceShellTreeMoveRepaintsEveryPaneShowingTheFile);
  AddTest(tests, "WorkspaceShell/TreeExpandRepaintsSidebarWhenCursorChanges",
          TestWorkspaceShellTreeExpandRepaintsSidebarWhenCursorChanges);
  AddTest(tests, "WorkspaceShell/TreeWheelRepaintsSidebarWhenCursorChanges",
          TestWorkspaceShellTreeWheelRepaintsSidebarWhenCursorChanges);
  AddTest(tests, "WorkspaceShell/TreeRepaintsWhenUnrelatedDamageIsQueued",
          TestWorkspaceShellTreeRepaintsWhenUnrelatedDamageIsQueued);
  AddTest(tests, "WorkspaceShell/ProjectOpenMaterializesTreeGitBadgesAfterFirstPaint",
          TestWorkspaceShellProjectOpenMaterializesTreeGitBadgesAfterFirstPaint);
  AddTest(tests, "WorkspaceShell/ProjectOpenDirectoryTreeRefreshDoesNotBlockOnGitStatuses",
          TestWorkspaceShellProjectOpenDirectoryTreeRefreshDoesNotBlockOnGitStatuses);
  AddTest(tests, "WorkspaceShell/TerminalWakeDoesNotForceProjectScan",
          TestWorkspaceShellTerminalWakeDoesNotForceProjectScan);
  AddTest(tests, "WorkspaceShell/AutomaticGitRefreshKeepsTreeBadgesClean",
          TestWorkspaceShellAutomaticGitRefreshKeepsTreeBadgesClean);
  AddTest(tests, "WorkspaceShell/StatusBarShowsSourceControlState",
          TestWorkspaceShellStatusBarShowsSourceControlState);
  AddTest(tests, "WorkspaceShell/TerminalWakeRefreshesStatusBarAfterCommit",
          TestWorkspaceShellTerminalWakeRefreshesStatusBarAfterCommit);
  AddTest(tests, "WorkspaceShell/TerminalWakeClearsTreeGitBadgesAfterCommit",
          TestWorkspaceShellTerminalWakeClearsTreeGitBadgesAfterCommit);
  AddTest(tests, "WorkspaceShell/GitSidebarRefreshDispatchIsNonBlocking",
          TestWorkspaceShellGitSidebarRefreshDispatchIsNonBlocking);
  AddTest(tests, "WorkspaceShell/ProjectSwitchDiscardsStaleGitSidebarRefreshResult",
          TestWorkspaceShellProjectSwitchDiscardsStaleGitSidebarRefreshResult);
  AddTest(tests, "WorkspaceShell/EmptyDirectoryCreationRefreshesTree",
          TestWorkspaceShellEmptyDirectoryCreationRefreshesTree);
  AddTest(tests, "WorkspaceShell/TruncatedIndexBatchNotifiesOnce",
          TestWorkspaceShellTruncatedIndexBatchNotifiesOnce);
  AddTest(tests, "WorkspaceShell/AProjectWhoseFolderIsDeletedSaysSo", TestWorkspaceShellAProjectWhoseFolderIsDeletedSaysSo);
  AddTest(tests, "WorkspaceShell/ProjectWatcherIgnoresGitignoredDirectories",
          TestWorkspaceShellProjectWatcherIgnoresGitignoredDirectories);
  AddTest(tests, "WorkspaceShell/ProjectWatcherIgnoresGitMetadataLockfiles",
          TestWorkspaceShellProjectWatcherIgnoresGitMetadataLockfiles);
  AddTest(tests, "WorkspaceShell/WatcherIgnoresGitignoredIncrementalCreate",
          TestWorkspaceShellWatcherIgnoresGitignoredIncrementalCreate);
  AddTest(tests, "WorkspaceShell/FileIndexUpdatesDoNotReloadCleanBuffers",
          TestWorkspaceShellFileIndexUpdatesDoNotReloadCleanBuffers);
  AddTest(tests, "WorkspaceShell/FileFinderReflectsFileIndexUpdates",
          TestWorkspaceShellFileFinderReflectsFileIndexUpdates);
  AddTest(tests, "WorkspaceShell/FileFinderUsesMaintainedIndexWithoutProjectScan",
          TestWorkspaceShellFileFinderUsesMaintainedIndexWithoutProjectScan);
  AddTest(tests, "WorkspaceShell/ProjectSearchUsesMaintainedIndexWithoutProjectScan",
          TestWorkspaceShellProjectSearchUsesMaintainedIndexWithoutProjectScan);
  AddTest(tests, "WorkspaceShell/ForcedIndexRefreshDoesNotDropABatchThatLandedDuringTheScan",
          TestWorkspaceShellForcedIndexRefreshDoesNotDropABatchThatLandedDuringTheScan);
  AddTest(tests, "WorkspaceShell/SearchRerunOnAFileChangeKeepsTheListAndSelection",
          TestWorkspaceShellSearchRerunOnAFileChangeKeepsTheListAndSelection);
  AddTest(tests, "WorkspaceShell/InjectedFileIndexBatchUpdatesFinderAndSearch",
          TestWorkspaceShellInjectedFileIndexBatchUpdatesFinderAndSearch);
  AddTest(tests, "WorkspaceShell/ReplaceAllNamesAFileItCouldNotWrite",
          TestWorkspaceShellReplaceAllNamesAFileItCouldNotWrite);
  AddTest(tests, "WorkspaceShell/ReplaceAllAbortsWithFeedbackPastAggregateCap",
          TestWorkspaceShellReplaceAllAbortsWithFeedbackPastAggregateCap);
  AddTest(tests, "WorkspaceShell/ReplaceAllReplacesInADirtyOpenBuffer",
          TestWorkspaceShellReplaceAllReplacesInADirtyOpenBuffer);
  AddTest(tests, "WorkspaceShell/AContentlessReopenedTabEditsNothing", TestWorkspaceShellAContentlessReopenedTabEditsNothing);
  AddTest(tests, "WorkspaceShell/ReplaceAllKeepsAUtf8ByteOrderMark", TestWorkspaceShellReplaceAllKeepsAUtf8ByteOrderMark);
  AddTest(tests, "WorkspaceShell/ReplaceAllReadsOnlyMatchedFiles",
          TestWorkspaceShellReplaceAllReadsOnlyMatchedFiles);
  AddTest(tests, "WorkspaceShell/ReplaceAllFallsBackWhenResultsTruncated",
          TestWorkspaceShellReplaceAllFallsBackWhenResultsTruncated);
  AddTest(tests, "WorkspaceShell/UnknownCommandKeepsPromptOpenWithFeedback",
          TestWorkspaceShellUnknownCommandKeepsPromptOpenWithFeedback);
  AddTest(tests, "WorkspaceShell/CommandReportsMissingProjectInsteadOfSilentNoOp",
          TestWorkspaceShellCommandReportsMissingProjectInsteadOfSilentNoOp);
  AddTest(tests, "WorkspaceShell/OpenCommandRequiresPath",
          TestWorkspaceShellOpenCommandRequiresPath);
  AddTest(tests, "WorkspaceShell/ProjectNextAndPrevCommandsCycleProjects",
          TestWorkspaceShellProjectNextAndPrevCommandsCycleProjects);
  AddTest(tests, "WorkspaceShell/AProjectLeftUnderAPromptGetsItsFocusBack",
          TestWorkspaceShellAProjectLeftUnderAPromptGetsItsFocusBack);
  AddTest(tests, "WorkspaceShell/ProjectSwitchPreservesProjectScopedCommandState",
          TestWorkspaceShellProjectSwitchPreservesProjectScopedCommandState);
  AddTest(tests, "WorkspaceShell/ProjectSwitchReloadsCleanBuffersChangedInTheBackground",
          TestWorkspaceShellProjectSwitchReloadsCleanBuffersChangedInTheBackground);
  AddTest(tests, "WorkspaceShell/ProjectSwitchPreservesSearchSidebarSurfaceState",
          TestWorkspaceShellProjectSwitchPreservesSearchSidebarSurfaceState);
  AddTest(tests, "WorkspaceShell/ProjectSwitchClearsTransientInteractionState",
          TestWorkspaceShellProjectSwitchClearsTransientInteractionState);
  AddTest(tests, "WorkspaceShell/SettingsScrollbarsReleaseAlike",
          TestWorkspaceShellSettingsScrollbarsReleaseAlike);
  AddTest(tests, "WorkspaceShell/ProjectOpenShowsDefaultTerminalPanel",
          TestWorkspaceShellProjectOpenShowsDefaultTerminalPanel);
  AddTest(tests, "WorkspaceShell/TermCommandRequestsBottomPanelRedraw",
          TestWorkspaceShellTermCommandRequestsBottomPanelRedraw);
  AddTest(tests, "WorkspaceShell/ProjectOpenSchedulesNoIdleWatcherTick",
          TestWorkspaceShellProjectOpenSchedulesNoIdleWatcherTick);
  AddTest(tests, "WorkspaceShell/SidebarWidthCommandParsesTypedRequests",
          TestWorkspaceShellSidebarWidthCommandParsesTypedRequests);
  AddTest(tests, "WorkspaceShell/MergeCommandResolvesRelativePaths",
          TestWorkspaceShellMergeCommandResolvesRelativePaths);
  AddTest(tests, "WorkspaceShell/TabMoveCommandSupportsRelativeOffsets",
          TestWorkspaceShellTabMoveCommandSupportsRelativeOffsets);
  AddTest(tests, "WorkspaceShell/TabMoveCommandSupportsRelativeForwardOffset",
          TestWorkspaceShellTabMoveCommandSupportsRelativeForwardOffset);
  AddTest(tests, "WorkspaceShell/SplitDoesNotClobberAnotherGroupsDirtyTab",
          TestWorkspaceShellSplitDoesNotClobberAnotherGroupsDirtyTab);
  AddTest(tests, "WorkspaceShell/SecondViewOfAnOpenFileSharesItsBuffer",
          TestWorkspaceShellSecondViewOfAnOpenFileSharesItsBuffer);
  AddTest(tests, "WorkspaceShell/RandomTabAndGroupOperationsKeepInvariants",
          TestWorkspaceShellRandomTabAndGroupOperationsKeepInvariants);
  AddTest(tests, "WorkspaceShell/BreadcrumbIsPerPane", TestWorkspaceShellBreadcrumbIsPerPane);
  AddTest(tests, "WorkspaceShell/SplitPaneRevealsItsNewTab",
          TestWorkspaceShellSplitPaneRevealsItsNewTab);
  AddTest(tests, "WorkspaceShell/FileFinderOpensIntoTheFocusedPane",
          TestWorkspaceShellFileFinderOpensIntoTheFocusedPane);
  AddTest(tests, "WorkspaceShell/SplitStopsAtTheGroupCap",
          TestWorkspaceShellSplitStopsAtTheGroupCap);
  AddTest(tests, "WorkspaceShell/WheelScrollsPaneUnderPointer",
          TestWorkspaceShellWheelScrollsPaneUnderPointer);
  AddTest(tests, "WorkspaceShell/GotoAndJumpCommandsUseTypedNavigationRequests",
          TestWorkspaceShellGotoAndJumpCommandsUseTypedNavigationRequests);
  AddTest(tests, "WorkspaceShell/GotoRejectsNonPositiveLine",
          TestWorkspaceShellGotoRejectsNonPositiveLine);
  AddTest(tests, "WorkspaceShell/GlobalCommandsApplyTypedRequests",
          TestWorkspaceShellGlobalCommandsApplyTypedRequests);
  AddTest(tests, "WorkspaceShell/CommandPaletteTabCompletion",
          TestWorkspaceShellCommandPaletteTabCompletion);
  AddTest(tests, "WorkspaceShell/OverlayWheelScrollsWithoutMovingSelection",
          TestWorkspaceShellOverlayWheelScrollsWithoutMovingSelection);
  AddTest(tests, "WorkspaceShell/CommandPaletteHomeEndEditsQuery",
          TestWorkspaceShellCommandPaletteHomeEndEditsQuery);
  AddTest(tests, "WorkspaceShell/WheelDoesNotStealKeyboardFocus",
          TestWorkspaceShellWheelDoesNotStealKeyboardFocus);
  AddTest(tests, "WorkspaceShell/WheelStepMatchesEditorAcrossSurfaces",
          TestWorkspaceShellWheelStepMatchesEditorAcrossSurfaces);
  AddTest(tests, "WorkspaceShell/CommandPaletteRunsCommandLineVsFuzzyPick",
          TestWorkspaceShellCommandPaletteRunsCommandLineVsFuzzyPick);
  AddTest(tests, "WorkspaceShell/CtrlNOpensUntitledTab",
          TestWorkspaceShellCtrlNOpensUntitledTab);
  AddTest(tests, "WorkspaceShell/VsCodeAlignedShortcutsDispatch",
          TestWorkspaceShellVsCodeAlignedShortcutsDispatch);
  AddTest(tests, "WorkspaceShell/FilesShortcutEscapeRestoresSidebarFocus",
          TestWorkspaceShellFilesShortcutEscapeRestoresSidebarFocus);
  AddTest(tests, "WorkspaceShell/FilesShortcutEscapeRestoresEditorFocusOnWelcome",
          TestWorkspaceShellFilesShortcutEscapeRestoresEditorFocusOnWelcome);
  AddTest(tests, "WorkspaceShell/WelcomeTabUsesLeftEdgeHitArea",
          TestWorkspaceShellWelcomeTabUsesLeftEdgeHitArea);
  AddTest(tests, "WorkspaceShell/FilesShortcutOpensMatchedFileAfterDeferredIndexCacheBuild",
          TestWorkspaceShellFilesShortcutOpensMatchedFileAfterDeferredIndexCacheBuild);
  AddTest(tests, "WorkspaceShell/ProjectOpenFromWelcomeInvalidatesCachedLayout",
          TestWorkspaceShellProjectOpenFromWelcomeInvalidatesCachedLayout);
  AddTest(tests, "WorkspaceShell/ResolvedKeybindingsAreCachedUntilInputsChange",
          TestWorkspaceShellResolvedKeybindingsAreCachedUntilInputsChange);
  AddTest(tests, "WorkspaceShell/ReopeningCleanTabDoesNotReloadUnrelatedTabs",
          TestWorkspaceShellReopeningCleanTabDoesNotReloadUnrelatedTabs);
  AddTest(tests, "WorkspaceShell/ReopeningCleanTabPicksUpExternalEdits",
          TestWorkspaceShellReopeningCleanTabPicksUpExternalEdits);
  AddTest(tests, "WorkspaceShell/OverlayOutsideClickRestoresPrimaryFocus",
          TestWorkspaceShellOverlayOutsideClickRestoresPrimaryFocus);
  AddTest(tests, "WorkspaceShell/TreeCollapseAllowsOpenDescendantsAndReselectReveal",
          TestWorkspaceShellTreeCollapseAllowsOpenDescendantsAndReselectReveal);
  AddTest(tests, "WorkspaceShell/CtrlOOpensNativeFilePicker",
          TestWorkspaceShellCtrlOOpensNativeFilePicker);
  AddTest(tests, "WorkspaceShell/TreeSidebarSupportsPageAndHomeEndKeys",
          TestWorkspaceShellTreeSidebarSupportsPageAndHomeEndKeys);
  AddTest(tests, "WorkspaceShell/F6CyclesEveryVisibleSurface",
          TestWorkspaceShellF6CyclesEveryVisibleSurface);
  AddTest(tests, "WorkspaceShell/CtrlTabStepsRecentEditors",
          TestWorkspaceShellCtrlTabStepsRecentEditors);
  AddTest(tests, "WorkspaceShell/DoubleClickResetsResizeDividers",
          TestWorkspaceShellDoubleClickResetsResizeDividers);
  AddTest(tests, "WorkspaceShell/TreeScrollDoesNotSnapToSelectionDuringRender",
          TestWorkspaceShellTreeScrollDoesNotSnapToSelectionDuringRender);
  AddTest(tests, "WorkspaceShell/TabSwitchSelectsActiveTreePath",
          TestWorkspaceShellTabSwitchSelectsActiveTreePath);
  AddTest(tests, "WorkspaceShell/TreeCollapseButtonCollapsesAllOpenDirectories",
          TestWorkspaceShellTreeCollapseButtonCollapsesAllOpenDirectories);
  AddTest(tests, "WorkspaceShell/TreeHeaderCompactsBeforeButtonsOverlap",
          TestWorkspaceShellTreeHeaderCompactsBeforeButtonsOverlap);
  AddTest(tests, "WorkspaceShell/TabSizeSettingAppliesImmediately",
          TestWorkspaceShellTabSizeSettingAppliesImmediately);
  AddTest(tests, "WorkspaceShell/TabSizeSettingStaysVisibleAfterRestart",
          TestWorkspaceShellTabSizeSettingStaysVisibleAfterRestart);
  AddTest(tests, "WorkspaceShell/FontSizeSettingAppliesImmediately",
          TestWorkspaceShellFontSizeSettingAppliesImmediately);
  AddTest(tests, "WorkspaceShell/FontSizeIsProjectScopedAndPersists",
          TestWorkspaceShellFontSizeIsProjectScopedAndPersists);
  AddTest(tests, "WorkspaceShell/CommandTabSizeStaysVisibleAfterRestart",
          TestWorkspaceShellCommandTabSizeStaysVisibleAfterRestart);
  AddTest(tests, "WorkspaceShell/AutoCloseToggleUpdatesViewportContract",
          TestWorkspaceShellAutoCloseToggleUpdatesViewportContract);
  AddTest(tests, "WorkspaceShell/AutoCloseToggleUpdatesAllTabContracts",
          TestWorkspaceShellAutoCloseToggleUpdatesAllTabContracts);
  AddTest(tests, "WorkspaceShell/LanguageOverrideSettingAppliesLive",
          TestWorkspaceShellLanguageOverrideSettingAppliesLive);
  AddTest(tests, "WorkspaceShell/SameLanguageTabsShareOneContractView",
          TestWorkspaceShellSameLanguageTabsShareOneContractView);
  AddTest(tests, "WorkspaceShell/TabSizeChangeAppliesToAllTabsWithoutContractRebuild",
          TestWorkspaceShellTabSizeChangeAppliesToAllTabsWithoutContractRebuild);
  AddTest(tests, "WorkspaceShell/TabKeyIndentsMultiLineSelection",
          TestWorkspaceShellTabKeyIndentsMultiLineSelection);
  AddTest(tests, "WorkspaceShell/TabKeyOnSingleLineInsertsTabCharacter",
          TestWorkspaceShellTabKeyOnSingleLineInsertsTabCharacter);
  AddTest(tests, "WorkspaceShell/SaveAsAndBuffersForPathsThatDoNotExistYet",
          TestWorkspaceShellSaveAsAndBuffersForPathsThatDoNotExistYet);
  AddTest(tests, "WorkspaceShell/EditorScrollbarUsesVisualRows",
          TestWorkspaceShellEditorScrollbarUsesVisualRows);
  AddTest(tests, "WorkspaceShell/EditorGestureEndsWhenItsBufferLeavesTheFront",
          TestWorkspaceShellEditorGestureEndsWhenItsBufferLeavesTheFront);
  AddTest(tests, "WorkspaceShell/TextDragDoesNotArmWithSeveralCarets",
          TestWorkspaceShellTextDragDoesNotArmWithSeveralCarets);
  AddTest(tests, "WorkspaceShell/TextDragRefusesAStaleSourceRange",
          TestWorkspaceShellTextDragRefusesAStaleSourceRange);
  AddTest(tests, "WorkspaceShell/CollapsedFoldExpandingVerbsAreExactlyTheDocumentedSet",
          TestWorkspaceShellCollapsedFoldExpandingVerbsAreExactlyTheDocumentedSet);
  AddTest(tests, "WorkspaceShell/NoActionNoticesSoftWrap",
          TestWorkspaceShellNoActionNoticesSoftWrap);
  AddTest(tests, "WorkspaceShell/AddCursorChordsAreBound",
          TestWorkspaceShellAddCursorChordsAreBound);
  AddTest(tests, "WorkspaceShell/NoActionEditsAnUnfocusedSplitGroup",
          TestWorkspaceShellNoActionEditsAnUnfocusedSplitGroup);
  AddTest(tests, "WorkspaceShell/NoActionEditsABackgroundBuffer",
          TestWorkspaceShellNoActionEditsABackgroundBuffer);
  AddTest(tests, "WorkspaceShell/ColumnSelectReanchorsAfterAForeignCaretMove",
          TestWorkspaceShellColumnSelectReanchorsAfterAForeignCaretMove);
  AddTest(tests, "WorkspaceShell/ForeignEditEndsTheSnippetSession",
          TestWorkspaceShellForeignEditEndsTheSnippetSession);
  AddTest(tests, "WorkspaceShell/TypingInAPlaceholderKeepsTheSnippetSession",
          TestWorkspaceShellTypingInAPlaceholderKeepsTheSnippetSession);
  AddTest(tests, "WorkspaceShell/EveryActionKeepsBreakpointsOnTheirLine",
          TestWorkspaceShellEveryActionKeepsBreakpointsOnTheirLine);
  AddTest(tests, "WorkspaceShell/ABackgroundBufferEditKeepsItsBreakpointsOnTheirLine",
          TestWorkspaceShellABackgroundBufferEditKeepsItsBreakpointsOnTheirLine);
  AddTest(tests, "WorkspaceShell/AddCursorAtNextMatchCountsEveryOccurrence",
          TestWorkspaceShellAddCursorAtNextMatchCountsEveryOccurrence);
  AddTest(tests, "WorkspaceShell/AddCursorAtNextMatchWalksForwardEachPress",
          TestWorkspaceShellAddCursorAtNextMatchWalksForwardEachPress);
  AddTest(tests, "WorkspaceShell/PasteSpreadsOneLinePerCaret",
          TestWorkspaceShellPasteSpreadsOneLinePerCaret);
  AddTest(tests, "WorkspaceShell/EmptySelectionCopyPastesAsAWholeLine",
          TestWorkspaceShellEmptySelectionCopyPastesAsAWholeLine);
  AddTest(tests, "WorkspaceShell/AddCursorAtAllMatchesFromFindWidgetFocusesTheEditor",
          TestWorkspaceShellAddCursorAtAllMatchesFromFindWidgetFocusesTheEditor);
  AddTest(tests, "WorkspaceShell/ShapingCapabilityTogglesGateExecutorCommandsAndIndentTab",
          TestWorkspaceShellShapingCapabilityTogglesGateExecutorCommandsAndIndentTab);
  AddTest(tests, "WorkspaceShell/CodeActionMenuIsCentered",
          TestWorkspaceShellCodeActionMenuIsCentered);
  AddTest(tests, "WorkspaceShell/SettingsOverlayRightClickDoesNotOpenEditorContextMenu",
          TestWorkspaceShellSettingsOverlayRightClickDoesNotOpenEditorContextMenu);
  AddTest(tests, "WorkspaceShell/VsCodeParityBindings", TestWorkspaceShellVsCodeParityBindings);
  AddTest(tests, "WorkspaceShell/AltF8StepsDiagnostics", TestWorkspaceShellAltF8StepsDiagnostics);
  AddTest(tests, "WorkspaceShell/EscapeCancelsTheSelection",
          TestWorkspaceShellEscapeCancelsTheSelection);
  AddTest(tests, "WorkspaceShell/CtrlArrowsScrollAndCtrlLSelectsLines",
          TestWorkspaceShellCtrlArrowsScrollAndCtrlLSelectsLines);
  AddTest(tests, "WorkspaceShell/SettingsOverlayTrapsKeyboardInput",
          TestWorkspaceShellSettingsOverlayTrapsKeyboardInput);
  AddTest(tests, "WorkspaceShell/SettingsOverlayWheelScrolls",
          TestWorkspaceShellSettingsOverlayWheelScrolls);
  AddTest(tests, "WorkspaceShell/SettingsClickOnTheHeaderMissesTheRowScrolledBeneathIt",
          TestWorkspaceShellSettingsClickOnTheHeaderMissesTheRowScrolledBeneathIt);
  AddTest(tests, "WorkspaceShell/DebugBreakpointsKeyboardNavigates",
          TestWorkspaceShellDebugBreakpointsKeyboardNavigates);
  AddTest(tests, "WorkspaceShell/DebugPaneKeysDoNotReachTheEditor",
          TestWorkspaceShellDebugPaneKeysDoNotReachTheEditor);
  AddTest(tests, "WorkspaceShell/DebugCallStackKeyboardNavigates",
          TestWorkspaceShellDebugCallStackKeyboardNavigates);
  AddTest(tests, "WorkspaceShell/DebugPaneKeyboardRevealsAndPages",
          TestWorkspaceShellDebugPaneKeyboardRevealsAndPages);
  AddTest(tests, "WorkspaceShell/SettingsFontPickerScrollbarIsGrabbable",
          TestWorkspaceShellSettingsFontPickerScrollbarIsGrabbable);
  AddTest(tests, "WorkspaceShell/HelpAboutFilterNarrowsRows",
          TestWorkspaceShellHelpAboutFilterNarrowsRows);
  AddTest(tests, "WorkspaceShell/HelpAboutIsKeyboardAndScrollbarNavigable",
          TestWorkspaceShellHelpAboutIsKeyboardAndScrollbarNavigable);
  AddTest(tests, "WorkspaceShell/HelpAboutOmitsAuthCommands",
          TestWorkspaceShellHelpAboutOmitsAuthCommands);
  AddTest(tests, "WorkspaceShell/HelpAboutShowsBoundKeyChords",
          TestWorkspaceShellHelpAboutShowsBoundKeyChords);
  AddTest(tests, "WorkspaceShell/IgnoredTreeFileActivatesDirectOpenPath",
          TestWorkspaceShellIgnoredTreeFileActivatesDirectOpenPath);
  AddTest(tests, "WorkspaceShell/IgnoredDirectoryExpansionMaterializesOneLevel",
          TestWorkspaceShellIgnoredDirectoryExpansionMaterializesOneLevel);
  AddTest(tests, "WorkspaceShell/HiddenIgnoredDirectoryUsesSameLazyExpansionRules",
          TestWorkspaceShellHiddenIgnoredDirectoryUsesSameLazyExpansionRules);
  AddTest(tests, "WorkspaceShell/CopySelectionWithContextUsesRelativePathAndLineRange",
          TestWorkspaceShellCopySelectionWithContextUsesRelativePathAndLineRange);
  AddTest(tests, "WorkspaceShell/CopySelectionWithContextOutsideProjectRootUsesAbsolutePath",
          TestWorkspaceShellCopySelectionWithContextOutsideProjectRootUsesAbsolutePath);
  AddTest(tests, "WorkspaceShell/CopySelectionWithContextWithoutSelectionCopiesCurrentLine",
          TestWorkspaceShellCopySelectionWithContextWithoutSelectionCopiesCurrentLine);
  AddTest(tests,
          "WorkspaceShell/CopySelectionWithContextNoSelectionReadsSingleLineFromLargeBuffer",
          TestWorkspaceShellCopySelectionWithContextNoSelectionReadsSingleLineFromLargeBuffer);
  AddTest(tests, "WorkspaceShell/CopySelectionWithContextOnBlankLineExpandsToEnclosingFold",
          TestWorkspaceShellCopySelectionWithContextOnBlankLineExpandsToEnclosingFold);
  AddTest(tests, "WorkspaceShell/EditorRightClickOpensSymbolAwareContextMenu",
          TestWorkspaceShellEditorRightClickOpensSymbolAwareContextMenu);
  AddTest(tests, "WorkspaceShell/EditorDragSelectionTracksPointer",
          TestWorkspaceShellEditorDragSelectionTracksPointer);
  AddTest(tests, "WorkspaceShell/AltClickAddsSecondaryCaret",
          TestWorkspaceShellAltClickAddsSecondaryCaret);
  AddTest(tests, "WorkspaceShell/AltClickKeepsTheExistingSelection",
          TestWorkspaceShellAltClickKeepsTheExistingSelection);
  AddTest(tests, "WorkspaceShell/ShiftAltClickAddsColumnCarets",
          TestWorkspaceShellShiftAltClickAddsColumnCarets);
  AddTest(tests, "WorkspaceShell/ShiftAltClickOffColumnMakesBoxSelection",
          TestWorkspaceShellShiftAltClickOffColumnMakesBoxSelection);
  AddTest(tests, "WorkspaceShell/ShiftAltDragUpdatesBoxSelection",
          TestWorkspaceShellShiftAltDragUpdatesBoxSelection);
  AddTest(tests, "WorkspaceShell/HoveredTabShowsRelativePathTooltip",
          TestWorkspaceShellHoveredTabShowsRelativePathTooltip);
  AddTest(tests, "WorkspaceShell/WindowMouseLeaveClearsTabTooltip",
          TestWorkspaceShellWindowMouseLeaveClearsTabTooltip);
  AddTest(tests, "WorkspaceShell/InWindowMouseMoveClearsProjectTabTooltipAndInvalidatesChrome",
          TestWorkspaceShellInWindowMouseMoveClearsProjectTabTooltipAndInvalidatesChrome);
  AddTest(tests, "WorkspaceShell/EditorSelectionWritesPrimaryBufferAndMiddleClickPastes",
          TestWorkspaceShellEditorSelectionWritesPrimaryBufferAndMiddleClickPastes);
  AddTest(tests, "WorkspaceShell/TextInputSurfaceTracksEditorOverlayAndPrompt",
          TestWorkspaceShellTextInputSurfaceTracksEditorOverlayAndPrompt);
  AddTest(tests, "WorkspaceShell/SidebarModeTabsSwitchView",
          TestWorkspaceShellSidebarModeTabsSwitchView);
  AddTest(tests, "WorkspaceShell/ProjectOpenExistingRootSwitchesWithoutDuplicatingCatalog",
          TestWorkspaceShellProjectOpenExistingRootSwitchesWithoutDuplicatingCatalog);
  AddTest(tests, "WorkspaceShell/ProjectOpenFailureRestoresPreviousActiveProject",
          TestWorkspaceShellProjectOpenFailureRestoresPreviousActiveProject);
  AddTest(tests, "WorkspaceShell/CloseActiveProjectRestoresAdjacentProject",
          TestWorkspaceShellCloseActiveProjectRestoresAdjacentProject);
  AddTest(tests, "WorkspaceShell/ProjectTabsDragReorderToEnd",
          TestWorkspaceShellProjectTabsDragReorderToEnd);
  AddTest(tests, "WorkspaceShell/EditorTabsDragReorderBetweenTabs",
          TestWorkspaceShellEditorTabsDragReorderBetweenTabs);
  AddTest(tests, "WorkspaceShell/ProjectTabRightClickOpensContextMenu",
          TestWorkspaceShellProjectTabRightClickOpensContextMenu);
  AddTest(tests, "WorkspaceShell/EditorTabDragDefersCommitUntilRelease",
          TestWorkspaceShellEditorTabDragDefersCommitUntilRelease);
  AddTest(tests, "WorkspaceShell/EditorTabDragTargetSlotTracksPointer",
          TestWorkspaceShellEditorTabDragTargetSlotTracksPointer);
  AddTest(tests, "WorkspaceShell/EditorTabDragSeedsSlideAnimation",
          TestWorkspaceShellEditorTabDragSeedsSlideAnimation);
  AddTest(tests, "WorkspaceShell/EditorTabDragSlotFollowsTheTabNotTheGrabPoint",
          TestWorkspaceShellEditorTabDragSlotFollowsTheTabNotTheGrabPoint);
  AddTest(tests, "WorkspaceShell/EditorTabDropCarriesNeighborEaseAcrossTheCommit",
          TestWorkspaceShellEditorTabDropCarriesNeighborEaseAcrossTheCommit);
  AddTest(tests, "WorkspaceShell/EditorTabDragMovesTabToTheOtherGroup",
          TestWorkspaceShellEditorTabDragMovesTabToTheOtherGroup);
  AddTest(tests, "WorkspaceShell/EditorTabDragOutOfLastTabCollapsesTheGroup",
          TestWorkspaceShellEditorTabDragOutOfLastTabCollapsesTheGroup);
  AddTest(tests, "WorkspaceShell/EditorTabDragToPaneEdgeSplitsTheGroup",
          TestWorkspaceShellEditorTabDragToPaneEdgeSplitsTheGroup);
  AddTest(tests, "WorkspaceShell/EditorTabDragToLeftPaneEdgeSplitsAhead",
          TestWorkspaceShellEditorTabDragToLeftPaneEdgeSplitsAhead);
  AddTest(tests, "WorkspaceShell/EditorTabDragToBottomPaneEdgeStacks",
          TestWorkspaceShellEditorTabDragToBottomPaneEdgeStacks);
  AddTest(tests, "WorkspaceShell/EditorTabDragToOtherPaneBodyMovesIntoThatGroup",
          TestWorkspaceShellEditorTabDragToOtherPaneBodyMovesIntoThatGroup);
  AddTest(tests, "WorkspaceShell/EditorTabDragOverOwnPaneCenterOffersNoDrop",
          TestWorkspaceShellEditorTabDragOverOwnPaneCenterOffersNoDrop);
  AddTest(tests, "WorkspaceShell/EditorTabDragToPaneEdgeAtCapMovesThePane",
          TestWorkspaceShellEditorTabDragToPaneEdgeAtCapMovesThePane);
  AddTest(tests, "WorkspaceShell/EditorTabDragToPaneEdgeRefusesLoneTab",
          TestWorkspaceShellEditorTabDragToPaneEdgeRefusesLoneTab);
  AddTest(tests, "WorkspaceShell/EditorTabDragToPaneEdgeSplitsAnAlreadySplitPane",
          TestWorkspaceShellEditorTabDragToPaneEdgeSplitsAnAlreadySplitPane);
  AddTest(tests, "WorkspaceShell/EditorTabDragToPaneEdgeMovesAcrossPanes",
          TestWorkspaceShellEditorTabDragToPaneEdgeMovesAcrossPanes);
  AddTest(tests, "WorkspaceShell/EditorSplitDividerDragMovesOnlyItsPair",
          TestWorkspaceShellEditorSplitDividerDragMovesOnlyItsPair);
  AddTest(tests, "WorkspaceShell/DirectionalEditorGroupFocusAndMove",
          TestWorkspaceShellDirectionalEditorGroupFocusAndMove);
  AddTest(tests, "WorkspaceShell/CrossGroupDragAnimatesBothStrips",
          TestWorkspaceShellCrossGroupDragAnimatesBothStrips);
  AddTest(tests, "WorkspaceShell/EditorTabDragAutoScrollsOverflowingStrip",
          TestWorkspaceShellEditorTabDragAutoScrollsOverflowingStrip);
  AddTest(tests, "WorkspaceShell/EditorTabDragIsAbandonedByEscapeAndFocusLoss",
          TestWorkspaceShellEditorTabDragIsAbandonedByEscapeAndFocusLoss);
  AddTest(tests, "WorkspaceShell/EditorTabDragHidesTheHoverTooltip",
          TestWorkspaceShellEditorTabDragHidesTheHoverTooltip);
  AddTest(tests, "WorkspaceShell/EditorTabDragHomeStillGlides",
          TestWorkspaceShellEditorTabDragHomeStillGlides);
  AddTest(tests, "WorkspaceShell/OutputTabReorderMovesActiveChannel",
          TestWorkspaceShellOutputTabReorderMovesActiveChannel);
  AddTest(tests, "WorkspaceShell/ReorderActiveHelperMovesAndGuards",
          TestReorderActiveHelperMovesAndGuards);
  AddTest(tests, "WorkspaceShell/ProjectTabWheelScrollsStrip",
          TestWorkspaceShellProjectTabWheelScrollsStrip);
  AddTest(tests, "WorkspaceShell/EditorTabWheelScrollsStrip",
          TestWorkspaceShellEditorTabWheelScrollsStrip);
  AddTest(tests, "WorkspaceShell/ProjectWatcherReloadDoesNotContinuouslyRearm",
          TestWorkspaceShellProjectWatcherReloadDoesNotContinuouslyRearm);
  AddTest(tests, "WorkspaceShell/EditorGroupSplitFocusCloseSemantics",
          TestWorkspaceShellEditorGroupSplitFocusCloseSemantics);
  AddTest(tests, "WorkspaceShell/SplitContextMenuAvailabilityAndTreeOpen",
          TestWorkspaceShellSplitContextMenuAvailabilityAndTreeOpen);
  AddTest(tests, "WorkspaceShell/RevealInFileTreeFromTabMenu",
          TestWorkspaceShellRevealInFileTreeFromTabMenu);
  AddTest(tests, "WorkspaceShell/ForcedFileIndexRefreshRunsOffThreadAndPicksUpNewFiles",
          TestWorkspaceShellForcedFileIndexRefreshRunsOffThreadAndPicksUpNewFiles);
  AddTest(tests, "WorkspaceShell/DirtyPromptSurvivesTabShiftWhileOpen",
          TestWorkspaceShellDirtyPromptSurvivesTabShiftWhileOpen);
  AddTest(tests, "WorkspaceShell/DirtyPromptClosesWithItsTab", TestWorkspaceShellDirtyPromptClosesWithItsTab);
}

}
