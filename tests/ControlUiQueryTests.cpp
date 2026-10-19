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
  Expect(ui["modal"].IsNull(), "no modal should be up");
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

// Rects are in layout units; window.scale says how many window coordinates one is.
// Without it a driver pointing at ui.scale 1.5 clicked two thirds of the way to
// every row and ran the row above (tools/qa menu-clicks, "scaled" layout).
void TestUiQueryReportsTheWindowScale() {
  UiFixture fixture;
  auto state = fixture.shell.CurrentWindowPresentationState().value_or(WorkspaceShell::WindowPresentationState{});
  state.window_per_unit = SDL_FPoint{1.5f, 1.25f};
  fixture.shell.SetWindowPresentationState(state);
  const util::JsonValue ui = TestAccess::ControlUi(fixture.shell);
  const auto& scale = ui["window"]["scale"].AsArray();
  Expect(scale.size() == 2 && scale[0].AsDouble() == 1.5 && scale[1].AsDouble() == 1.25,
         "the ui query should report window coordinates per layout unit");
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

// A modal owns the keyboard and the pointer, so a driver must see it to dismiss it:
// the indent status segment opens Settings, which a QA run otherwise mistakes for
// nothing having happened.
void TestUiQueryReportsTheModal() {
  UiFixture fixture;
  TestAccess::OpenSettingsOverlay(fixture.shell);
  Expect(TestAccess::ControlUi(fixture.shell)["modal"].AsString() == "settings",
         "the ui query should report the Settings overlay as the modal");
  TestAccess::OpenHelpAboutOverlay(fixture.shell);
  Expect(TestAccess::ControlUi(fixture.shell)["modal"].AsString() == "help",
         "the ui query should report Help/About as the modal");
}

void TestUiQueryIsReadOnly() {
  UiFixture fixture;
  const std::string first = util::SerializeJson(TestAccess::ControlUi(fixture.shell));
  const std::string second = util::SerializeJson(TestAccess::ControlUi(fixture.shell));
  Expect(first == second, "repeated ui queries with no input between should be identical");
  Expect(!fixture.shell.ConsumePendingRenderInvalidation().HasAnyRedraw(),
         "answering the ui query should not request a redraw");
}

// Typing in the file tree navigates, as in VS Code's explorer, and bare letters
// run nothing: `d` used to open the compare picker and `r` to refresh, so a query
// typed into the tree by mistake ran commands (TD-2026-10-15-342).
void TestTypingInTheTreeNavigates() {
  UiFixture fixture;
  TestAccess::SetFocusSidebar(fixture.shell);
  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_B)});
  const auto beta = Find(TestAccess::ControlUi(fixture.shell), "row:tree/beta.cpp");
  Expect(beta.has_value() && beta->selected == true, "typing b should select beta.cpp");
  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_D)});
  const util::JsonValue ui = TestAccess::ControlUi(fixture.shell);
  Expect(!ui["overlay"]["visible"].AsBool(), "d in the tree should not open the compare picker");
  Expect(Find(ui, "row:tree/beta.cpp")->selected == true, "and with no row starting with d, the selection stays");
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

// The `buffer` query reads the live viewport: its text, every caret with its
// selection (primary first), and the collapsed folds. tools/qa's editing flows
// assert against it instead of saving and reading the file back.
void TestBufferQueryReportsTextCaretsAndFolds() {
  UiFixture fixture;
  WriteFile(fixture.root / "fold.cpp", "int f() {\n  return 1;\n}\nint g;\n");
  TestAccess::OpenFileInNewTab(fixture.shell, fixture.root / "fold.cpp");
  TestAccess::SetFocusEditor(fixture.shell);
  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_END, SDL_KMOD_SHIFT)});
  util::JsonValue buffer = TestAccess::ControlQuery(fixture.shell, "buffer");
  Expect(buffer["available"].AsBool() && buffer["kind"].AsString() == "editor",
         "the buffer query should describe the active editor");
  Expect(buffer["text"].AsString() == "int f() {\n  return 1;\n}\nint g;\n",
         "the buffer query should report the text: " + buffer["text"].AsString());
  Expect(buffer["selectedText"].AsString() == "int f() {",
         "Shift+End should select the first line: " + buffer["selectedText"].AsString());
  const util::JsonValue& primary = buffer["cursors"][std::size_t{0}];
  Expect(primary["line"].AsInt() == 1 && primary["column"].AsInt() == 10 &&
             primary["anchorColumn"].AsInt() == 1 && !primary["empty"].AsBool(),
         "the primary caret should carry its anchor");
  (void)DispatchEvents(fixture.shell,
                       {KeyDownEvent(SDLK_DOWN, static_cast<SDL_Keymod>(SDL_KMOD_CTRL | SDL_KMOD_ALT))});
  buffer = TestAccess::ControlQuery(fixture.shell, "buffer");
  Expect(buffer["cursors"].AsArray().size() == 2,
         "Ctrl+Alt+Down should add a second caret the query reports");
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "fold-all"), "fold-all should run");
  buffer = TestAccess::ControlQuery(fixture.shell, "buffer");
  Expect(!buffer["folded"].AsArray().empty() &&
             buffer["folded"][std::size_t{0}][std::size_t{0}].AsInt() == 1,
         "fold-all should leave the query reporting the fold at line 1");
}

// The list overlays name their rows, and a click at a row's center runs it.
void TestUiQueryNamesOverlayRows() {
  UiFixture fixture;
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "files"), "files should open the finder");
  TestAccess::SetFileFinderQuery(fixture.shell, "gamma");
  util::JsonValue ui = TestAccess::ControlUi(fixture.shell);
  Expect(ui["overlay"]["query"].AsString() == "gamma", "the ui query should report the overlay query");
  Expect(ui["overlay"]["count"].AsInt() == 1, "one file should match gamma");
  const auto row = Find(ui, "overlay-row:src/gamma.cpp");
  Expect(row.has_value() && row->selected == true, "the match should be a selected overlay row");
  fixture.ClickCenter(*row);
  Expect(TestAccess::ControlUi(fixture.shell)["overlay"]["visible"].AsBool() == false,
         "clicking the row should run it and close the finder");
  Expect(Find(TestAccess::ControlUi(fixture.shell), "tab:0/gamma.cpp").has_value(),
         "clicking the row should open gamma.cpp");
}

// The unsaved-changes prompt names its buttons; clicking Cancel keeps the tab.
void TestUiQueryNamesPromptButtons() {
  UiFixture fixture;
  TestAccess::ActiveEditor(fixture.shell).InsertText("x");
  TestAccess::RequestCloseTab(fixture.shell, 1);
  const util::JsonValue ui = TestAccess::ControlUi(fixture.shell);
  Expect(ui["modal"].AsString() == "unsaved-prompt", "closing a dirty tab should raise the unsaved prompt");
  Expect(ui["prompt"]["kind"].AsString() == "unsaved", "the prompt block should say which prompt");
  bool found_cancel = false;
  for (const UiElement& element : Elements(ui)) {
    found_cancel = found_cancel || element.name == "button:prompt/Cancel";
  }
  Expect(found_cancel, "the unsaved prompt should name its Cancel button");
  fixture.ClickCenter(*Find(ui, "button:prompt/Cancel"));
  Expect(TestAccess::ControlUi(fixture.shell)["modal"].IsNull(), "Cancel should dismiss the prompt");
}

// Settings rows are named by setting id, with their controls; clicking a
// checkbox toggles the setting.
void TestUiQueryNamesSettingsRows() {
  UiFixture fixture;
  TestAccess::OpenSettingsOverlay(fixture.shell);
  const util::JsonValue ui = TestAccess::ControlUi(fixture.shell);
  ExpectElement(ui, "field:settings/filter");
  std::optional<UiElement> checkbox;
  std::string id;
  for (const UiElement& element : Elements(ui)) {
    if (!checkbox && element.name.starts_with("button:setting/") && element.name.ends_with("/checkbox")) {
      checkbox = element;
      id = element.name.substr(std::string_view("button:setting/").size());
      id.resize(id.size() - std::string_view("/checkbox").size());
    }
  }
  Expect(checkbox.has_value(), "the Settings overlay should name a checkbox control");
  if (!checkbox) {
    return;
  }
  const auto before = TestAccess::GetSettingValue(fixture.shell, id);
  fixture.ClickCenter(*checkbox);
  const auto after = TestAccess::GetSettingValue(fixture.shell, id);
  Expect(before != after, "clicking " + checkbox->name + " should toggle " + id);
}

// The ui query's action receipt names what a key ran, by command name, so a
// driver can tell a dead shortcut (nothing ran) from one taken by another
// binding (something else ran).
void TestUiQueryReportsTheLastAction() {
  UiFixture fixture;
  const std::int64_t before = TestAccess::ControlUi(fixture.shell)["actions"]["dispatched"].AsInt();
  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_B, SDL_KMOD_CTRL)});
  const util::JsonValue actions = TestAccess::ControlUi(fixture.shell)["actions"];
  Expect(actions["dispatched"].AsInt() == before + 1, "Ctrl+B should dispatch one action");
  Expect(actions["last"].AsString() == "sidebar-toggle" && actions["source"].AsString() == "shortcut",
         "the receipt should name sidebar-toggle from a shortcut, got " + actions["last"].AsString() +
             " from " + actions["source"].AsString());
}

// `key` and `pointer` are input, not actions: the receipt names what the key or
// click ran, once, and nothing when it ran nothing. Counted as actions, the
// Wayland run of the `keys` sweep read "the terminal owns this key, but key ran"
// for every key the terminal took, and two actions for every menu row clicked.
void TestInjectedInputLeavesTheReceiptToWhatItRan() {
  UiFixture fixture;
  const std::int64_t before = TestAccess::ControlUi(fixture.shell)["actions"]["dispatched"].AsInt();
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "key ctrl+b"), "key ctrl+b should run");
  util::JsonValue actions = TestAccess::ControlUi(fixture.shell)["actions"];
  Expect(actions["dispatched"].AsInt() == before + 1 && actions["last"].AsString() == "sidebar-toggle" &&
             actions["source"].AsString() == "shortcut",
         "`key ctrl+b` should leave one receipt, sidebar-toggle from a shortcut: " +
             std::to_string(actions["dispatched"].AsInt() - before) + " x " + actions["last"].AsString());
  TestAccess::SetFocusEditor(fixture.shell);
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "key x"), "key x should run");
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "pointer move 600 400"), "pointer move should run");
  actions = TestAccess::ControlUi(fixture.shell)["actions"];
  Expect(actions["dispatched"].AsInt() == before + 1,
         "typing a letter and moving the pointer run no action: " +
             std::to_string(actions["dispatched"].AsInt() - before) + " x " + actions["last"].AsString());
}

// The debug pane's mode tabs and rows are named with the rects its mouse path
// hit-tests: a click on `debug-tab:watch` switches the mode, and a click on a
// breakpoint row goes to the breakpoint (QA flows drive the pane this way).
void TestUiQueryNamesTheDebugPane() {
  UiFixture fixture;
  TestAccess::SetSettingValue(fixture.shell, "debug.enabled", "true");
  TestAccess::ToggleBreakpointForTest(fixture.shell, fixture.root / "alpha.cpp", 0);
  TestAccess::FocusDebugPaneBreakpoints(fixture.shell);
  TestAccess::NoteLayoutInputsChanged(fixture.shell);
  fixture.shell.Render(fixture.canvas.renderer(), UiFixture::kWidth, UiFixture::kHeight);
  (void)fixture.shell.ConsumePendingRenderInvalidation();

  const util::JsonValue ui = TestAccess::ControlUi(fixture.shell);
  const auto breakpoints_tab = Find(ui, "debug-tab:breakpoints");
  Expect(breakpoints_tab.has_value() && breakpoints_tab->active == true,
         "the ui query should name the active debug-tab:breakpoints");
  std::optional<UiElement> row;
  for (const util::JsonValue& entry : ui["elements"].AsArray()) {
    if (entry["name"].AsString().starts_with("debug-row:breakpoints/") &&
        entry["label"].AsString().find("alpha.cpp") != std::string::npos) {
      row = Find(ui, entry["name"].AsString());
    }
  }
  Expect(row.has_value(), "the ui query should name the alpha.cpp breakpoint row");

  if (row) {
    fixture.ClickCenter(*row);
    const auto tab = Find(TestAccess::ControlUi(fixture.shell), "tab:0/alpha.cpp");
    Expect(tab.has_value() && tab->active == true,
           "a click on the named breakpoint row should go to alpha.cpp");
  }
  const auto watch_tab = Find(TestAccess::ControlUi(fixture.shell), "debug-tab:watch");
  Expect(watch_tab.has_value(), "the ui query should name debug-tab:watch");
  if (watch_tab) {
    fixture.ClickCenter(*watch_tab);
    const auto after = Find(TestAccess::ControlUi(fixture.shell), "debug-tab:watch");
    Expect(after.has_value() && after->active == true,
           "a click on debug-tab:watch should switch the pane to Watch");
  }
}

// A toast is an element (`notification:<index>`, the message as its label) whether
// or not it has buttons, and a click on it dismisses it.
void TestUiQueryNamesToasts() {
  UiFixture fixture;
  (void)DispatchEvents(fixture.shell, {KeyDownEvent(SDLK_F8, SDL_KMOD_NONE)});  // "No problems"
  fixture.shell.Render(fixture.canvas.renderer(), UiFixture::kWidth, UiFixture::kHeight);
  (void)fixture.shell.ConsumePendingRenderInvalidation();
  const auto toast = Find(TestAccess::ControlUi(fixture.shell), "notification:0");
  Expect(toast.has_value(), "the ui query should name the toast F8 raised");
  if (toast) {
    fixture.ClickCenter(*toast);
    Expect(!Find(TestAccess::ControlUi(fixture.shell), "notification:0").has_value(),
           "a click on the toast should dismiss it");
  }
}

// A large editor font is the editor's: the chrome keeps its size. One renderer
// drew both, so `editor.font_size 20` set every menu title, tab, tree row and
// status segment in a font taller than its fixed row and the text overflowed --
// clipped descenders, and stale glyph rows under a hovered context-menu row
// (QA sweeps-matrix, "light theme, large font").
void TestLargeEditorFontKeepsTheChromeSize() {
  UiFixture fixture;
  const float chrome_line = TestAccess::ChromeTextLineHeight(fixture.shell);
  const float editor_line = TestAccess::TextLineHeight(fixture.shell);
  Expect(TestAccess::SetSettingValue(fixture.shell, "editor.font_size", "24"), "set the editor font size");
  fixture.shell.Render(fixture.canvas.renderer(), UiFixture::kWidth, UiFixture::kHeight);
  (void)fixture.shell.ConsumePendingRenderInvalidation();
  Expect(TestAccess::TextLineHeight(fixture.shell) > editor_line * 1.5f,
         "the editor font grows: " + std::to_string(TestAccess::TextLineHeight(fixture.shell)));
  Expect(TestAccess::ChromeTextLineHeight(fixture.shell) == chrome_line,
         "the chrome font keeps its size: " + std::to_string(TestAccess::ChromeTextLineHeight(fixture.shell)) +
             " (was " + std::to_string(chrome_line) + ")");
  const util::JsonValue ui = TestAccess::ControlUi(fixture.shell);
  const auto row = Find(ui, "row:tree/alpha.cpp");
  Expect(row.has_value() && TestAccess::ChromeTextLineHeight(fixture.shell) <= row->rect.h + 1.0f,
         "a tree row still fits its text");
  // The editor resolves a click with the font it draws: column 6 of line 1.
  const util::JsonValue& pane = ui["editors"][std::size_t{0}];
  Expect(pane["lineHeight"].AsDouble() > static_cast<double>(editor_line) * 1.5,
         "the ui query reports the editor's own line height");
  TestAccess::SetFocusEditor(fixture.shell);
  const double x = pane["textX"].AsDouble() + 5.3 * pane["charWidth"].AsDouble();
  const double y = pane["firstLineY"].AsDouble() + 0.5 * pane["lineHeight"].AsDouble();
  Expect(TestAccess::ExecuteCommandLine(fixture.shell,
                                        "pointer click " + std::to_string(x) + " " + std::to_string(y)),
         "click the editor text");
  const util::JsonValue buffer = TestAccess::ControlQuery(fixture.shell, "buffer");
  const util::JsonValue& caret = buffer["cursors"][std::size_t{0}];
  Expect(caret["line"].AsInt() == 1 && caret["column"].AsInt() == 6,
         "the click lands where the editor font put column 6: line " +
             std::to_string(caret["line"].AsInt()) + " column " + std::to_string(caret["column"].AsInt()));
}

// What `pointer` and `key` change reaches the screen: they hand the shell an event
// from inside the control command, and the damage that nested dispatch returned
// was dropped -- so the frame after a control click repainted only what was still
// pending, and the newly active tab kept its old look (QA on Wayland, `chrome`).
void TestInjectedInputRepaintsWhatItChanged() {
  UiFixture fixture;
  const auto alpha_tab = Find(TestAccess::ControlUi(fixture.shell), "tab:0/alpha.cpp");
  Expect(alpha_tab.has_value() && alpha_tab->active == false, "alpha.cpp should be an inactive tab");
  const std::string x = std::to_string(alpha_tab->rect.x + alpha_tab->rect.w / 2.0f);
  const std::string y = std::to_string(alpha_tab->rect.y + alpha_tab->rect.h / 2.0f);
  for (const std::string& line : {"pointer click " + x + " " + y, std::string("key ctrl+end"),
                                  std::string("key z")}) {
    Expect(TestAccess::ExecuteCommandLine(fixture.shell, line), "should run: " + line);
    // The app loop: the control event's own result is what the shell has pending.
    const workspace::RenderInvalidation redraw = fixture.shell.ConsumePendingRenderInvalidation();
    const render::PixelDiffResult diff = PaintAndDiffWithFullRepaint(
        fixture.shell, fixture.canvas, UiFixture::kWidth, UiFixture::kHeight, redraw,
        [&] { TestAccess::SetCaretBlinkEpochMs(fixture.shell, SDL_GetTicks()); });
    Expect(diff.count == 0, line + " should repaint what it changed: " + DescribePixelDiff(diff));
  }
}

// `key` is a key press with the text a keyboard types: an agent answering a TUI's
// `[y/N]` with `key y` got nothing, since every text surface reads the text event
// and the command sent only the press and the release.
void TestKeyCommandTypesPrintableKeys() {
  UiFixture fixture;
  TestAccess::SetFocusEditor(fixture.shell);
  for (const char* line : {"key ctrl+end", "key y", "key shift+n", "key space", "key ctrl+a"}) {
    Expect(TestAccess::ExecuteCommandLine(fixture.shell, line), std::string("should run: ") + line);
  }
  const util::JsonValue buffer = TestAccess::ControlQuery(fixture.shell, "buffer");
  Expect(buffer["text"].AsString() == "int beta = 2;\nyN ",
         "printable keys should type, shifted and plain, and a chord should not: " +
             buffer["text"].AsString());
}

// `pointer` is the mouse for a driver with no window-system input (a Wayland
// session, an agent): it lands where `ui` says, in the same layout units, and
// runs what a real click there would.
void TestPointerCommandClicksWhatTheUiNames() {
  UiFixture fixture;
  const auto center = [](const UiElement& element) {
    return std::to_string(element.rect.x + element.rect.w / 2.0f) + " " +
           std::to_string(element.rect.y + element.rect.h / 2.0f);
  };
  const auto edit = Find(TestAccess::ControlUi(fixture.shell), "menu:Edit");
  Expect(edit.has_value(), "the Edit menu should be on the bar");
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "pointer click " + center(*edit)),
         "pointer click should run");
  Expect(TestAccess::ActiveMenuId(fixture.shell) == WorkspaceShell::MenuId::Edit &&
             TestAccess::MenuBarOpen(fixture.shell),
         "pointer click at menu:Edit should open the Edit menu");
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "key escape"), "key escape should run");
  Expect(!TestAccess::MenuBarOpen(fixture.shell), "key escape should close the menu");

  const auto alpha_tab = Find(TestAccess::ControlUi(fixture.shell), "tab:0/alpha.cpp");
  Expect(alpha_tab.has_value() && alpha_tab->active == false, "alpha.cpp should be an inactive tab");
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "pointer click " + center(*alpha_tab)),
         "pointer click on a tab should run");
  const auto after = Find(TestAccess::ControlUi(fixture.shell), "tab:0/alpha.cpp");
  Expect(after.has_value() && after->active == true, "pointer click on a tab should activate it");

  // A middle click closes a tab: the button argument reaches the handler.
  const auto beta_tab = Find(TestAccess::ControlUi(fixture.shell), "tab:0/beta.cpp");
  Expect(beta_tab.has_value(), "beta.cpp should still have a tab");
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "pointer click " + center(*beta_tab) + " middle"),
         "pointer middle click should run");
  Expect(!Find(TestAccess::ControlUi(fixture.shell), "tab:0/beta.cpp").has_value(),
         "a middle click on a tab should close it");

  // Modifiers are held for the click only.
  Expect(TestAccess::ExecuteCommandLine(fixture.shell, "pointer ctrl+shift+move 10 10"),
         "a modifier prefix should parse");
  Expect(SDL_GetModState() == SDL_KMOD_NONE, "pointer should release the modifiers it held");

  for (const char* bad : {"pointer", "pointer click 1", "pointer fling 1 1", "pointer click x 1",
                          "pointer click 1 1 fourth", "pointer click 1 1 left 9", "pointer wheel 1 1 0",
                          "pointer hyper+click 1 1"}) {
    Expect(!TestAccess::ExecuteCommandLine(fixture.shell, bad), std::string("should reject: ") + bad);
  }
}

}  // namespace

void RegisterControlUiQueryTests(std::vector<TestCase>& tests) {
  AddTest(tests, "ControlUiQuery/NamesToasts", TestUiQueryNamesToasts);
  AddTest(tests, "ControlUiQuery/IsRegistered", TestUiQueryIsRegistered);
  AddTest(tests, "ControlUiQuery/ReportsTheWindowScale", TestUiQueryReportsTheWindowScale);
  AddTest(tests, "ControlUiQuery/NamesTheDebugPane", TestUiQueryNamesTheDebugPane);
  AddTest(tests, "ControlUiQuery/NamesTheWorkspaceChrome", TestUiQueryNamesTheWorkspaceChrome);
  AddTest(tests, "ControlUiQuery/HitsWhatItNames", TestUiQueryHitsWhatItNames);
  AddTest(tests, "ControlUiQuery/IsReadOnly", TestUiQueryIsReadOnly);
  AddTest(tests, "ControlUiQuery/ReportsTheModal", TestUiQueryReportsTheModal);
  AddTest(tests, "ControlUiQuery/TypingInTheTreeNavigates", TestTypingInTheTreeNavigates);
  AddTest(tests, "ControlUiQuery/NamesAreMadeUnique", TestUiNamesAreMadeUnique);
  AddTest(tests, "ControlUiQuery/BufferReportsTextCaretsAndFolds",
          TestBufferQueryReportsTextCaretsAndFolds);
  AddTest(tests, "ControlUiQuery/NamesOverlayRows", TestUiQueryNamesOverlayRows);
  AddTest(tests, "ControlUiQuery/NamesPromptButtons", TestUiQueryNamesPromptButtons);
  AddTest(tests, "ControlUiQuery/NamesSettingsRows", TestUiQueryNamesSettingsRows);
  AddTest(tests, "ControlUiQuery/ReportsTheLastAction", TestUiQueryReportsTheLastAction);
  AddTest(tests, "ControlUiQuery/InjectedInputLeavesTheReceiptToWhatItRan",
          TestInjectedInputLeavesTheReceiptToWhatItRan);
  AddTest(tests, "ControlUiQuery/KeyCommandTypesPrintableKeys", TestKeyCommandTypesPrintableKeys);
  AddTest(tests, "ControlUiQuery/LargeEditorFontKeepsTheChromeSize", TestLargeEditorFontKeepsTheChromeSize);
  AddTest(tests, "ControlUiQuery/InjectedInputRepaintsWhatItChanged", TestInjectedInputRepaintsWhatItChanged);
  AddTest(tests, "ControlUiQuery/PointerCommandClicksWhatTheUiNames",
          TestPointerCommandClicksWhatTheUiNames);
}

}  // namespace microide::tests
