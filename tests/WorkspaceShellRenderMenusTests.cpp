#include "TestSupport.h"

#include "support/SoftwareCanvas.h"

#include "workspace/registries/WorkspaceMenuRegistry.h"
#include "workspace/shell/WorkspaceShellTestAccess.h"

#include "WorkspaceShellEventHelpers.h"

#include <SDL3/SDL.h>

#include <filesystem>
#include <string>
#include <vector>

namespace microide::tests {
namespace {

using WorkspaceShellTestAccess = microide::workspace::WorkspaceShell::TestAccess;
using microide::workspace::TreeContextTargetKind;
using microide::workspace::WorkspaceShell;
using MenuId = WorkspaceShell::MenuId;

constexpr int kWindowHeight = 800;

std::filesystem::path MakeProject(const std::filesystem::path& root) {
  std::filesystem::create_directories(root / "sub");
  const std::filesystem::path source = root / "main.cpp";
  WriteFile(source, "int main() { return 0; }\n");
  WriteFile(root / "sub" / "other.cpp", "void other() {}\n");
  return source;
}

void OpenShell(WorkspaceShell& shell, const std::filesystem::path& root,
               const std::filesystem::path& source, int width) {
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, width, kWindowHeight);
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, source);
}

void PaintPopupBody(WorkspaceShell& shell, SoftwareCanvas& canvas, const char* what) {
  Expect(WorkspaceShellTestAccess::AnyMenuPopupOpen(shell), what);
  WorkspaceShellTestAccess::ClearPaintedPopupRowsForTest(shell);
  WorkspaceShellTestAccess::RenderFrameWithRenderer(shell, canvas.renderer());
  Expect(WorkspaceShellTestAccess::LastPaintedPopupRowCount(shell) > 0,
         "the popup body should have laid out at least one row");
}

void PaintWithAPopup(WorkspaceShell& shell, SoftwareCanvas& canvas, const char* what) {
  Expect(WorkspaceShellTestAccess::AnyMenuPopupOpen(shell), what);
  WorkspaceShellTestAccess::RenderFrameWithRenderer(shell, canvas.renderer());
}

void TestMenuBarPopupsPaint() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = MakeProject(root);
  constexpr int kWidth = 1400;
  SoftwareCanvas canvas(kWidth, kWindowHeight);

  WorkspaceShell shell;
  OpenShell(shell, root, source, kWidth);

  const MenuId menus[] = {MenuId::File,     MenuId::Edit,  MenuId::View, MenuId::Go,   MenuId::Git,
                          MenuId::Remote, MenuId::Terminal, MenuId::Debug, MenuId::Help};
  for (const MenuId menu : menus) {
    WorkspaceShellTestAccess::OpenMenuBarMenuForTest(shell, menu);
    PaintPopupBody(shell, canvas, "opening a menu-bar menu should open the menu bar");

    WorkspaceShellTestAccess::SetActiveMenuItemIndexForTest(shell, 1);
    WorkspaceShellTestAccess::RenderFrameWithRenderer(shell, canvas.renderer());

    WorkspaceShellTestAccess::SetActiveMenuItemIndexForTest(shell, 9999);
    WorkspaceShellTestAccess::RenderFrameWithRenderer(shell, canvas.renderer());
  }
}

void TestSubmenuPopupPaints() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = MakeProject(root);
  constexpr int kWidth = 1400;
  SoftwareCanvas canvas(kWidth, kWindowHeight);

  WorkspaceShell shell;
  OpenShell(shell, root, source, kWidth);

  WorkspaceShellTestAccess::OpenMenuBarMenuForTest(shell, MenuId::View);
  WorkspaceShellTestAccess::OpenSubmenuForTest(shell, MenuId::SidebarMode,
                                               SDL_FRect{200.0f, 30.0f, 180.0f, 22.0f});
  PaintPopupBody(shell, canvas, "a submenu should paint over its open parent menu");

  WorkspaceShellTestAccess::OpenSubmenuForTest(
      shell, MenuId::SidebarMode,
      SDL_FRect{static_cast<float>(kWidth) - 20.0f, 30.0f, 180.0f, 22.0f});
  WorkspaceShellTestAccess::RenderFrameWithRenderer(shell, canvas.renderer());
}

void TestOverflowPopupPaints() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = MakeProject(root);
  constexpr int kWidth = 420;
  SoftwareCanvas canvas(kWidth, kWindowHeight);

  WorkspaceShell shell;
  OpenShell(shell, root, source, kWidth);

  Expect(WorkspaceShellTestAccess::MenuBarOverflowItemCount(shell) > 0,
         "a narrow window must actually overflow the menu bar, or this paints nothing");

  WorkspaceShellTestAccess::OpenMenuOverflowPopupForTest(shell);
  PaintWithAPopup(shell, canvas, "the overflow popup should be open");

  const auto popup = WorkspaceShellTestAccess::MenuOverflowPopupRect(shell);
  Expect(popup.has_value(), "an open overflow popup should have a rect");
  Expect(SendMouseMotion(shell, popup->x + popup->w * 0.5f, popup->y + 10.0f, 0),
         "a motion inside the overflow popup should be consumed by it");
  Expect(WorkspaceShellTestAccess::MenuOverflowPopupActiveIndex(shell) == 0,
         "a motion over the first overflow row should highlight it");
  WorkspaceShellTestAccess::RenderFrameWithRenderer(shell, canvas.renderer());
}

void TestTreeContextMenuPopupPaints() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = MakeProject(root);
  constexpr int kWidth = 1400;
  SoftwareCanvas canvas(kWidth, kWindowHeight);

  WorkspaceShell shell;
  OpenShell(shell, root, source, kWidth);

  const struct {
    TreeContextTargetKind target;
    std::filesystem::path path;
  } cases[] = {
      {TreeContextTargetKind::File, source},
      {TreeContextTargetKind::Directory, root / "sub"},
      {TreeContextTargetKind::Root, root},
      {TreeContextTargetKind::Background, root},
  };
  for (const auto& entry : cases) {
    WorkspaceShellTestAccess::OpenTreeContextMenuForPath(shell, entry.target, entry.path);
    PaintPopupBody(shell, canvas, "the tree context menu should be open");
    WorkspaceShellTestAccess::SetTreeContextMenuActiveItemIndexForTest(shell, 0);
    WorkspaceShellTestAccess::RenderFrameWithRenderer(shell, canvas.renderer());
  }
}

void TestMenuPopupsPaintAtDegenerateWindowSizes() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = MakeProject(root);

  for (const int width : {60, 120, 240}) {
    for (const int height : {40, 120}) {
      SoftwareCanvas canvas(width, height);
      WorkspaceShell shell;
      OpenShell(shell, root, source, width);
      WorkspaceShellTestAccess::SetWindowSize(shell, width, height);

      WorkspaceShellTestAccess::OpenMenuBarMenuForTest(shell, MenuId::File);
      WorkspaceShellTestAccess::RenderFrameWithRenderer(shell, canvas.renderer());

      WorkspaceShellTestAccess::OpenMenuOverflowPopupForTest(shell);
      WorkspaceShellTestAccess::RenderFrameWithRenderer(shell, canvas.renderer());

      WorkspaceShellTestAccess::OpenTreeContextMenuForPath(shell, TreeContextTargetKind::File,
                                                           source);
      WorkspaceShellTestAccess::RenderFrameWithRenderer(shell, canvas.renderer());
    }
  }
}

}

void RegisterWorkspaceShellRenderMenusTests(std::vector<TestCase>& tests) {
  AddTest(tests, "WorkspaceShellRenderMenus/MenuBarPopupsPaint", TestMenuBarPopupsPaint);
  AddTest(tests, "WorkspaceShellRenderMenus/SubmenuPopupPaints", TestSubmenuPopupPaints);
  AddTest(tests, "WorkspaceShellRenderMenus/OverflowPopupPaints", TestOverflowPopupPaints);
  AddTest(tests, "WorkspaceShellRenderMenus/TreeContextMenuPopupPaints",
          TestTreeContextMenuPopupPaints);
  AddTest(tests, "WorkspaceShellRenderMenus/PaintsAtDegenerateWindowSizes",
          TestMenuPopupsPaintAtDegenerateWindowSizes);
}

}
