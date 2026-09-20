#include "TestSupport.h"

#include "support/SoftwareCanvas.h"

#include "workspace/registries/WorkspaceMenuRegistry.h"
#include "workspace/shell/WorkspaceShellTestAccess.h"

#include "WorkspaceShellEventHelpers.h"

#include <SDL3/SDL.h>

#include <filesystem>
#include <string>
#include <vector>

// The menu POPUP paint path. WorkspaceShellRenderMenus.cpp draws three popups --
// the menu-bar menu (plus its submenu), the menu-bar overflow popup, and the tree
// context menu -- through one shared `draw_popup_body` lambda. Every one of them
// is reachable in production only through chrome mouse/keyboard events at real
// window coordinates, so the TU sat at 7.50% line coverage: the menu STATE was
// well covered by WorkspaceShellMenuTests, and the draw loop above it was never
// entered once.
//
// The popups are drawn at several window widths on purpose. The overflow popup
// only has items when the menu bar is too narrow to fit every menu, and the
// popup-rect computations flip their anchor near the window edges -- so a single
// wide window would paint the menu-bar popup and silently skip the other two.

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

// Paints one frame and proves the shared popup body actually laid out rows.
// Two guards, because they fail for different reasons: the state guard catches a
// test that forgot to open anything, and the row-count guard catches a popup that
// was open but whose body was skipped (a nullopt popup rect, an empty item list).
// A frame with no popup at all still paints and still comes out opaque, so a
// pixel assertion on its own proves nothing here.
void PaintPopupBody(WorkspaceShell& shell, SoftwareCanvas& canvas, const char* what) {
  Expect(WorkspaceShellTestAccess::AnyMenuPopupOpen(shell), what);
  WorkspaceShellTestAccess::ClearPaintedPopupRowsForTest(shell);
  WorkspaceShellTestAccess::RenderFrameWithRenderer(shell, canvas.renderer());
  Expect(WorkspaceShellTestAccess::LastPaintedPopupRowCount(shell) > 0,
         "the popup body should have laid out at least one row");
}

// The overflow popup has its own draw loop rather than the shared body, so it
// gets the state guard only.
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

  // Every menu-bar menu, so the item mix (checkables, disabled rows, separators,
  // accelerators) is covered rather than whichever one happens to be first.
  const MenuId menus[] = {MenuId::File,  MenuId::Edit,  MenuId::View,
                          MenuId::Go,    MenuId::Git,   MenuId::Terminal,
                          MenuId::Debug, MenuId::Help};
  for (const MenuId menu : menus) {
    WorkspaceShellTestAccess::OpenMenuBarMenuForTest(shell, menu);
    PaintPopupBody(shell, canvas, "opening a menu-bar menu should open the menu bar");

    // The hovered/active row is a separate fill from the plain row, and a
    // negative index (nothing active) is the default the loop above already hit.
    WorkspaceShellTestAccess::SetActiveMenuItemIndexForTest(shell, 1);
    WorkspaceShellTestAccess::RenderFrameWithRenderer(shell, canvas.renderer());

    // An index past the end must not read out of bounds; the row loop is driven
    // by the item list, not by the active index, so this is the regression guard
    // for anyone who makes it drive the other way round.
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
  // A submenu paints on TOP of its parent: both popup bodies run in one frame,
  // which is the branch a single-popup test never reaches.
  WorkspaceShellTestAccess::OpenSubmenuForTest(shell, MenuId::SidebarMode,
                                               SDL_FRect{200.0f, 30.0f, 180.0f, 22.0f});
  PaintPopupBody(shell, canvas, "a submenu should paint over its open parent menu");

  // Anchored hard against the right edge, where the submenu rect flips side.
  WorkspaceShellTestAccess::OpenSubmenuForTest(
      shell, MenuId::SidebarMode,
      SDL_FRect{static_cast<float>(kWidth) - 20.0f, 30.0f, 180.0f, 22.0f});
  WorkspaceShellTestAccess::RenderFrameWithRenderer(shell, canvas.renderer());
}

void TestOverflowPopupPaints() {
  // The overflow popup exists only when the menu bar cannot fit every menu, so it
  // needs a narrow window -- at 1400px wide there is nothing to overflow and this
  // popup would paint an empty body.
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = MakeProject(root);
  constexpr int kWidth = 420;
  SoftwareCanvas canvas(kWidth, kWindowHeight);

  WorkspaceShell shell;
  OpenShell(shell, root, source, kWidth);

  Expect(WorkspaceShellTestAccess::MenuBarOverflowItemCount(shell) > 0,
         "a narrow window must actually overflow the menu bar, or this paints nothing");

  WorkspaceShellTestAccess::OpenMenuOverflowPopupForTest(
      shell, SDL_FRect{static_cast<float>(kWidth) - 40.0f, 2.0f, 28.0f, 24.0f});
  PaintWithAPopup(shell, canvas, "the overflow popup should be open");

  // With a row highlighted. The overflow rows' hovered fill reads the active
  // index, which a motion over the popup writes — and so does the keyboard.
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

  // Each target kind produces a different item list, including ones where some
  // rows are disabled -- the disabled fill is its own branch.
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
  // A window too small to hold a popup at all. The popup-rect helpers return
  // nullopt or a clamped rect here, and the draw loop must cope with both.
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

      WorkspaceShellTestAccess::OpenMenuOverflowPopupForTest(
          shell, SDL_FRect{static_cast<float>(width) - 10.0f, 2.0f, 28.0f, 24.0f});
      WorkspaceShellTestAccess::RenderFrameWithRenderer(shell, canvas.renderer());

      WorkspaceShellTestAccess::OpenTreeContextMenuForPath(shell, TreeContextTargetKind::File,
                                                           source);
      WorkspaceShellTestAccess::RenderFrameWithRenderer(shell, canvas.renderer());
    }
  }
}

}  // namespace

void RegisterWorkspaceShellRenderMenusTests(std::vector<TestCase>& tests) {
  AddTest(tests, "WorkspaceShellRenderMenus/MenuBarPopupsPaint", TestMenuBarPopupsPaint);
  AddTest(tests, "WorkspaceShellRenderMenus/SubmenuPopupPaints", TestSubmenuPopupPaints);
  AddTest(tests, "WorkspaceShellRenderMenus/OverflowPopupPaints", TestOverflowPopupPaints);
  AddTest(tests, "WorkspaceShellRenderMenus/TreeContextMenuPopupPaints",
          TestTreeContextMenuPopupPaints);
  AddTest(tests, "WorkspaceShellRenderMenus/PaintsAtDegenerateWindowSizes",
          TestMenuPopupsPaintAtDegenerateWindowSizes);
}

}  // namespace microide::tests
