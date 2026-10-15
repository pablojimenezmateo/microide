#include "TestSupport.h"
#include "TestSupportSdl.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "WorkspaceShellEventHelpers.h"
#include "support/RetainedRedraw.h"
#include "support/SoftwareCanvas.h"
#include "util/JsonValue.h"
#include "workspace/control/ControlProtocol.h"
#include "workspace/control/ControlUiQuery.h"
#include "workspace/shell/WorkspaceShellTestAccess.h"

namespace microide::tests {
namespace {

using microide::workspace::ControlUiElements;
using microide::workspace::WorkspaceShell;
using TestAccess = microide::workspace::WorkspaceShell::TestAccess;

struct UiElement {
  std::string name;
  SDL_FRect rect{};
  std::optional<bool> active;
  std::optional<bool> selected;
};

std::vector<UiElement> Elements(const util::JsonValue& ui) {
  std::vector<UiElement> out;
  for (const util::JsonValue& entry : ui["elements"].AsArray()) {
    UiElement element;
    element.name = entry["name"].AsString();
    const auto& rect = entry["rect"].AsArray();
    element.rect = SDL_FRect{static_cast<float>(rect[0].AsInt()), static_cast<float>(rect[1].AsInt()),
                             static_cast<float>(rect[2].AsInt()), static_cast<float>(rect[3].AsInt())};
    if (entry.HasKey("active")) {
      element.active = entry["active"].AsBool();
    }
    if (entry.HasKey("selected")) {
      element.selected = entry["selected"].AsBool();
    }
    out.push_back(std::move(element));
  }
  return out;
}

std::optional<UiElement> Find(const util::JsonValue& ui, std::string_view name) {
  for (UiElement& element : Elements(ui)) {
    if (element.name == name) {
      return element;
    }
  }
  return std::nullopt;
}

void ExpectElement(const util::JsonValue& ui, std::string_view name) {
  Expect(Find(ui, name).has_value(), "the ui query should name " + std::string(name));
}

struct UiFixture {
  static constexpr int kWidth = 1280;
  static constexpr int kHeight = 720;

  UiFixture() {
    EnsureDummySdlVideoInitialized();
    root = temp_dir.path() / "project";
    WriteFile(root / "alpha.cpp", "int alpha = 1;\n");
    WriteFile(root / "beta.cpp", "int beta = 2;\n");
    WriteFile(root / "src" / "gamma.cpp", "int gamma = 3;\n");
    Expect(TestAccess::OpenProjectTab(shell, root, false, false), "the ui fixture should open its project");
    TestAccess::SetWindowSize(shell, kWidth, kHeight);
    TestAccess::OpenSingleEditorTab(shell, root / "alpha.cpp");
    TestAccess::OpenFileInNewTab(shell, root / "beta.cpp");
    TestAccess::ShowTreeSidebar(shell);
    TestAccess::NoteLayoutInputsChanged(shell);
    shell.Render(canvas.renderer(), kWidth, kHeight);
    (void)shell.ConsumePendingRenderInvalidation();
  }

  // A real click at the element's center, its redraw folded as the app loop does.
  void ClickCenter(const UiElement& element) {
    const float x = element.rect.x + element.rect.w / 2.0f;
    const float y = element.rect.y + element.rect.h / 2.0f;
    (void)DispatchEvents(shell, {MouseMotionEvent(x, y),
                                 MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_DOWN, x, y),
                                 MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_UP, x, y)});
    shell.Render(canvas.renderer(), kWidth, kHeight);
    (void)shell.ConsumePendingRenderInvalidation();
  }

  TemporaryDirectory temp_dir;
  std::filesystem::path root;
  WorkspaceShell shell;
  SoftwareCanvas canvas{kWidth, kHeight};
};

void TestUiQueryIsRegistered() {
  bool found = false;
  for (const std::string_view verb : workspace::ControlQueryVerbs()) {
    found = found || verb == "ui";
  }
  Expect(found, "`ui` should be a control query verb");
}

void TestUiQueryNamesTheWorkspaceChrome() {
  UiFixture fixture;
  const util::JsonValue ui = TestAccess::ControlUi(fixture.shell);
  Expect(ui["window"]["width"].AsInt() == UiFixture::kWidth, "the ui query should report the window");
  Expect(ui["focus"].AsString() == "sidebar",
         "the ui query should report the focus target (the tree, just shown): " +
             ui["focus"].AsString());
  Expect(ui["sidebar"]["visible"].AsBool() && ui["sidebar"]["view"].AsString() == "tree",
         "the ui query should report the sidebar view");
  Expect(!ui["menu"]["open"].AsBool(), "no menu should be open");
  Expect(ui["surfaces"]["editorSurface"].IsArray(), "the ui query should report surface rects");
  for (const std::string_view name :
       {"menu:File", "menu:Edit", "menu:Help", "tab:0/alpha.cpp", "tab:0/beta.cpp",
        "sidebar-view:tree", "sidebar-view:search", "row:tree/alpha.cpp", "row:tree/src",
        "button:sidebar/Collapse", "status:line-column", "divider:sidebar"}) {
    ExpectElement(ui, name);
  }
  const auto beta = Find(ui, "tab:0/beta.cpp");
  Expect(beta.has_value() && beta->active == true, "the active tab should be marked active");
}

void TestUiQueryHitsWhatItNames() {
  UiFixture fixture;
  const auto edit = Find(TestAccess::ControlUi(fixture.shell), "menu:Edit");
  Expect(edit.has_value(), "the Edit menu should be on the bar");
  fixture.ClickCenter(*edit);
  Expect(TestAccess::ActiveMenuId(fixture.shell) == WorkspaceShell::MenuId::Edit &&
             TestAccess::MenuBarOpen(fixture.shell),
         "a click at the center of menu:Edit should open the Edit menu");
  const util::JsonValue open = TestAccess::ControlUi(fixture.shell);
  Expect(open["menu"]["open"].AsBool() && open["menu"]["menu"].AsString() == "Edit",
         "the ui query should report the open menu");
  ExpectElement(open, "menu-item:Edit/Undo");
  // Escape closes it again before the next click.
  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_ESCAPE)});

  const auto alpha_tab = Find(TestAccess::ControlUi(fixture.shell), "tab:0/alpha.cpp");
  Expect(alpha_tab.has_value() && alpha_tab->active == false, "alpha.cpp should be an inactive tab");
  fixture.ClickCenter(*alpha_tab);
  const auto after_tab = Find(TestAccess::ControlUi(fixture.shell), "tab:0/alpha.cpp");
  Expect(after_tab.has_value() && after_tab->active == true,
         "a click at the center of a tab should activate that tab");

  const auto row = Find(TestAccess::ControlUi(fixture.shell), "row:tree/beta.cpp");
  Expect(row.has_value() && row->selected == false, "beta.cpp should be an unselected tree row");
  fixture.ClickCenter(*row);
  const auto after_row = Find(TestAccess::ControlUi(fixture.shell), "row:tree/beta.cpp");
  Expect(after_row.has_value() && after_row->selected == true,
         "a click at the center of a tree row should select that row");
}

void TestUiQueryIsReadOnly() {
  UiFixture fixture;
  const std::string first = util::SerializeJson(TestAccess::ControlUi(fixture.shell));
  const std::string second = util::SerializeJson(TestAccess::ControlUi(fixture.shell));
  Expect(first == second, "repeated ui queries with no input between should be identical");
  Expect(!fixture.shell.ConsumePendingRenderInvalidation().HasAnyRedraw(),
         "answering the ui query should not request a redraw");
}

void TestUiNamesAreMadeUnique() {
  ControlUiElements elements;
  workspace::AddControlUiElement(elements, "tab", "0/main.cpp", SDL_FRect{0, 0, 10, 10});
  workspace::AddControlUiElement(elements, "tab", "0/util.cpp", SDL_FRect{10, 0, 10, 10});
  workspace::AddControlUiElement(elements, "tab", "0/main.cpp", SDL_FRect{20, 0, 10, 10});
  workspace::AddControlUiElement(elements, "tab", "0/main.cpp", SDL_FRect{30, 0, 10, 10});
  workspace::MakeControlUiNamesUnique(elements);
  Expect(elements[0].name == "tab:0/main.cpp" && elements[1].name == "tab:0/util.cpp" &&
             elements[2].name == "tab:0/main.cpp#2" && elements[3].name == "tab:0/main.cpp#3",
         "later elements sharing a name should get #2, #3 suffixes in visual order");
}

}  // namespace

void RegisterControlUiQueryTests(std::vector<TestCase>& tests) {
  AddTest(tests, "ControlUiQuery/IsRegistered", TestUiQueryIsRegistered);
  AddTest(tests, "ControlUiQuery/NamesTheWorkspaceChrome", TestUiQueryNamesTheWorkspaceChrome);
  AddTest(tests, "ControlUiQuery/HitsWhatItNames", TestUiQueryHitsWhatItNames);
  AddTest(tests, "ControlUiQuery/IsReadOnly", TestUiQueryIsReadOnly);
  AddTest(tests, "ControlUiQuery/NamesAreMadeUnique", TestUiNamesAreMadeUnique);
}

}  // namespace microide::tests
