#include "TestSupport.h"

#include "WorkspaceShellTestAccess.h"

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace microide::tests {
namespace {

using microide::workspace::WorkspaceShell;
using microide::workspace::WorkspaceShellTestAccess;

void TestWorkspaceShellMenuBarOmitsRemovedMenus() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);

  const std::vector<std::string> labels = WorkspaceShellTestAccess::VisibleMenuBarLabels(shell);
  Expect(std::find(labels.begin(), labels.end(), "Project") == labels.end(),
         "menu bar should omit the removed Project menu");
  Expect(std::find(labels.begin(), labels.end(), "Terminal") == labels.end(),
         "menu bar should omit the removed Terminal menu");
  Expect(std::find(labels.begin(), labels.end(), "Help") == labels.end(),
         "menu bar should omit the removed Help menu");
}

void TestWorkspaceShellFileCloseAllTabsClosesOpenEditorTabs() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path left = root / "left.txt";
  const std::filesystem::path right = root / "right.txt";
  WriteFile(left, "left\n");
  WriteFile(right, "right\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenFile(shell, left);
  Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, right),
         "close-all fixture should open a second editor tab");

  Expect(WorkspaceShellTestAccess::OpenTabs(shell).size() == 2,
         "close-all fixture should start with two tabs");
  Expect(WorkspaceShellTestAccess::ExecuteCloseAllTabs(shell),
         "close all tabs action should execute");
  Expect(WorkspaceShellTestAccess::OpenTabs(shell).empty(),
         "close all tabs should close every clean editor tab");
}

void TestWorkspaceShellDoubleClickTitleBarRequestsMaximizeToggle() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::SetWindowChromeEnabled(shell, true);

  Expect(WorkspaceShellTestAccess::WindowHitTest(shell, 640.0f, 10.0f) == SDL_HITTEST_NORMAL,
         "empty title-bar hit testing should stay normal so mouse clicks reach the shell");
  Expect(WorkspaceShellTestAccess::WindowDragRegionContains(shell, 640.0f, 10.0f),
         "empty title-bar space should still be eligible for window dragging");
  Expect(WorkspaceShellTestAccess::HandleMouseButtonDown(shell, 640.0f, 10.0f, SDL_BUTTON_LEFT, 2),
         "double-clicking an empty title-bar region should be handled");
  Expect(WorkspaceShellTestAccess::ConsumeWindowAction(shell) ==
             WorkspaceShell::WindowAction::ToggleMaximize,
         "double-clicking the title bar should request the same maximize toggle as the chrome button");
}

void TestWorkspaceShellFullscreenStateDisablesResizableFrameHitTest() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::SetWindowChromeEnabled(shell, true, false, true);

  Expect(WorkspaceShellTestAccess::WindowHitTest(shell, 1.0f, 1.0f) == SDL_HITTEST_NORMAL,
         "fullscreen chrome state should not expose resize hit targets");
}

void TestWorkspaceShellWindowPresentationStateUpdatesChromeAndSize() {
  WorkspaceShell shell;
  shell.SetWindowPresentationState(WorkspaceShell::WindowPresentationState{
      .logical_width = 1280,
      .logical_height = 720,
      .scale_x = 1.5f,
      .scale_y = 1.25f,
      .chrome =
          WorkspaceShell::WindowChromeState{
              .custom_enabled = true,
              .maximized = true,
              .fullscreen = false,
          },
  });

  Expect(WorkspaceShellTestAccess::WindowHitTest(shell, 1.0f, 1.0f) == SDL_HITTEST_NORMAL,
         "maximized presentation state should disable resize hit targets");
  Expect(WorkspaceShellTestAccess::WindowDragRegionContains(shell, 640.0f, 10.0f),
         "presentation state should keep the title bar draggable");
}

void TestWorkspaceShellMenuBarHoverSwitchesActiveMenu() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);

  const auto file_rect = WorkspaceShellTestAccess::MenuBarItemRect(shell, "File");
  const auto edit_rect = WorkspaceShellTestAccess::MenuBarItemRect(shell, "Edit");
  Expect(file_rect.has_value(), "menu hover fixture should expose a File menu item");
  Expect(edit_rect.has_value(), "menu hover fixture should expose an Edit menu item");

  Expect(WorkspaceShellTestAccess::HandleMouseButtonDown(
             shell, file_rect->x + file_rect->w * 0.5f, file_rect->y + file_rect->h * 0.5f,
             SDL_BUTTON_LEFT),
         "clicking the File menu should be handled");
  Expect(WorkspaceShellTestAccess::FileMenuOpen(shell),
         "clicking the File menu should open the File popup");

  Expect(WorkspaceShellTestAccess::HandleMouseMotion(
             shell, edit_rect->x + edit_rect->w * 0.5f, edit_rect->y + edit_rect->h * 0.5f, 0),
         "hovering another menu while the menu bar is open should be handled");
  Expect(WorkspaceShellTestAccess::EditMenuOpen(shell),
         "hovering the Edit menu should switch the active popup");
}

void TestWorkspaceShellEditorTabRightClickOpensContextMenu() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path left = root / "left.txt";
  const std::filesystem::path right = root / "right.txt";
  WriteFile(left, "left\n");
  WriteFile(right, "right\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::OpenFile(shell, left);
  Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, right),
         "tab context-menu fixture should open a second editor tab");

  const SDL_FRect tab_rect = WorkspaceShellTestAccess::EditorTabRect(shell, 0);
  Expect(WorkspaceShellTestAccess::HandleMouseButtonDown(
             shell, tab_rect.x + tab_rect.w * 0.5f, tab_rect.y + tab_rect.h * 0.5f,
             SDL_BUTTON_RIGHT),
         "right-clicking an editor tab should be handled");
  Expect(WorkspaceShellTestAccess::EditorTabContextMenuOpen(shell),
         "right-clicking an editor tab should open the editor tab context menu");
  Expect(WorkspaceShellTestAccess::ActiveTabIndex(shell) == 0,
         "right-clicking an editor tab should retarget the active tab before menu actions run");
}

void TestWorkspaceShellTabContextActionsCloseAdjacentTabs() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path one = root / "one.txt";
  const std::filesystem::path two = root / "two.txt";
  const std::filesystem::path three = root / "three.txt";
  WriteFile(one, "one\n");
  WriteFile(two, "two\n");
  WriteFile(three, "three\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenFile(shell, one);
  Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, two),
         "tab close fixture should open the middle tab");
  Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, three),
         "tab close fixture should open the final tab");

  WorkspaceShellTestAccess::ActivateTab(shell, 1);
  Expect(WorkspaceShellTestAccess::ExecuteCloseTabsToRight(shell),
         "close tabs to the right should execute");
  Expect(WorkspaceShellTestAccess::OpenTabs(shell).size() == 2,
         "close tabs to the right should remove tabs after the active tab");
  Expect(WorkspaceShellTestAccess::TabDisplayTitle(shell, 1).find("two.txt") != std::string::npos,
         "close tabs to the right should keep the active tab in place");

  Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, three),
         "tab close fixture should reopen the right-side tab");
  WorkspaceShellTestAccess::ActivateTab(shell, 1);
  Expect(WorkspaceShellTestAccess::ExecuteCloseTabsToLeft(shell),
         "close tabs to the left should execute");
  Expect(WorkspaceShellTestAccess::OpenTabs(shell).size() == 2,
         "close tabs to the left should remove tabs before the active tab");
  Expect(WorkspaceShellTestAccess::TabDisplayTitle(shell, 0).find("two.txt") != std::string::npos,
         "close tabs to the left should keep the active tab after compaction");

  WorkspaceShellTestAccess::ActivateTab(shell, 0);
  Expect(WorkspaceShellTestAccess::ExecuteCloseOtherTabs(shell),
         "close other tabs should execute");
  Expect(WorkspaceShellTestAccess::OpenTabs(shell).size() == 1,
         "close other tabs should keep only the active tab");
  Expect(WorkspaceShellTestAccess::TabDisplayTitle(shell, 0).find("two.txt") != std::string::npos,
         "close other tabs should preserve the selected tab");
}

}  // namespace

void RegisterWorkspaceShellChromeTests(std::vector<TestCase>& tests) {
  AddTest(tests, "WorkspaceShell/MenuBarOmitsRemovedMenus",
          TestWorkspaceShellMenuBarOmitsRemovedMenus);
  AddTest(tests, "WorkspaceShell/MenuBarHoverSwitchesActiveMenu",
          TestWorkspaceShellMenuBarHoverSwitchesActiveMenu);
  AddTest(tests, "WorkspaceShell/EditorTabRightClickOpensContextMenu",
          TestWorkspaceShellEditorTabRightClickOpensContextMenu);
  AddTest(tests, "WorkspaceShell/TabContextActionsCloseAdjacentTabs",
          TestWorkspaceShellTabContextActionsCloseAdjacentTabs);
  AddTest(tests, "WorkspaceShell/FileCloseAllTabsClosesOpenEditorTabs",
          TestWorkspaceShellFileCloseAllTabsClosesOpenEditorTabs);
  AddTest(tests, "WorkspaceShell/DoubleClickTitleBarRequestsMaximizeToggle",
          TestWorkspaceShellDoubleClickTitleBarRequestsMaximizeToggle);
  AddTest(tests, "WorkspaceShell/FullscreenStateDisablesResizableFrameHitTest",
          TestWorkspaceShellFullscreenStateDisablesResizableFrameHitTest);
  AddTest(tests, "WorkspaceShell/WindowPresentationStateUpdatesChromeAndSize",
          TestWorkspaceShellWindowPresentationStateUpdatesChromeAndSize);
}

}  // namespace microide::tests
