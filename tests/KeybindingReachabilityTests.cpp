#include "TestSupport.h"
#include "TestSupportSdl.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "WorkspaceShellEventHelpers.h"
#include "workspace/registries/WorkspaceKeybindingRegistry.h"
#include "workspace/shell/WorkspaceShellTestAccess.h"

namespace microide::tests {
namespace {

using microide::workspace::ActionId;
using microide::workspace::KeybindingContext;
using microide::workspace::ResolvedKeybinding;
using microide::workspace::WorkspaceShell;
using TestAccess = microide::workspace::WorkspaceShell::TestAccess;

enum class Focus {
  Editor,
  Tree,
  SearchView,
  GitView,
  OutputPanel,
  Terminal,
  DebugPane,
  QuickOpen,
};

constexpr Focus kFocusTargets[] = {Focus::Editor,      Focus::Tree,     Focus::SearchView,
                                   Focus::GitView,     Focus::OutputPanel, Focus::Terminal,
                                   Focus::DebugPane,   Focus::QuickOpen};

std::string_view FocusName(Focus focus) {
  switch (focus) {
    case Focus::Editor: return "editor";
    case Focus::Tree: return "tree";
    case Focus::SearchView: return "search view";
    case Focus::GitView: return "git view";
    case Focus::OutputPanel: return "output panel";
    case Focus::Terminal: return "terminal";
    case Focus::DebugPane: return "debug pane";
    case Focus::QuickOpen: return "quick open";
  }
  return "?";
}

KeybindingContext ContextOf(Focus focus) {
  switch (focus) {
    case Focus::Editor: return KeybindingContext::Editor;
    case Focus::Tree:
    case Focus::SearchView:
    case Focus::GitView: return KeybindingContext::Sidebar;
    case Focus::Terminal: return KeybindingContext::Terminal;
    case Focus::OutputPanel:
    case Focus::DebugPane:
    case Focus::QuickOpen: return KeybindingContext::Global;
  }
  return KeybindingContext::Global;
}

bool HasCtrl(const ResolvedKeybinding& binding) {
  return (binding.modifiers & SDL_KMOD_CTRL) != 0;
}

bool OnlyCtrl(const ResolvedKeybinding& binding) {
  return (binding.modifiers & (SDL_KMOD_SHIFT | SDL_KMOD_ALT | SDL_KMOD_GUI)) == 0 &&
         HasCtrl(binding);
}

std::optional<std::string_view> Gate(const ResolvedKeybinding& binding, Focus focus) {
  const bool single_line_field = focus == Focus::SearchView || focus == Focus::QuickOpen;
  if (single_line_field && HasCtrl(binding) && binding.key == SDLK_V) {
    return "a focused text field takes Ctrl+V as paste";
  }
  if (single_line_field && OnlyCtrl(binding) &&
      (binding.key == SDLK_A || binding.key == SDLK_C || binding.key == SDLK_X)) {
    return "a focused text field owns Ctrl+A/C/X";
  }
  const bool editor_text_shortcut =
      binding.action == ActionId::SelectAll || binding.action == ActionId::Undo ||
      binding.action == ActionId::Redo || binding.action == ActionId::CopySelection ||
      binding.action == ActionId::CutSelection || binding.action == ActionId::PasteClipboard ||
      binding.action == ActionId::Search || binding.action == ActionId::ReplaceInBuffer;
  if (editor_text_shortcut && focus != Focus::Editor) {
    return "editor text shortcut outside the editor";
  }
  if (focus == Focus::QuickOpen && binding.action == ActionId::Tab && binding.args.empty() &&
      binding.command_name.empty()) {
    return "Tab belongs to an open overlay";
  }
  if (focus == Focus::QuickOpen && !binding.command_name.empty()) {
    return "command-name bindings wait for the overlay to close";
  }
  if (focus == Focus::QuickOpen && binding.leader_key != SDLK_UNKNOWN) {
    return "a chord's leader does not arm under an overlay";
  }
  return std::nullopt;
}

struct FocusFixture {
  explicit FocusFixture(Focus focus) : focus(focus) {
    EnsureDummySdlVideoInitialized();
    root = temp_dir.path() / "project";
    WriteFile(root / "alpha.cpp", "int alpha = 1;\nint beta = 2;\n");
    Expect(TestAccess::OpenProjectTab(shell, root, false, false),
           "the keybinding fixture should open its project");
    TestAccess::SetWindowSize(shell, 1280, 720);
    TestAccess::OpenSingleEditorTab(shell, root / "alpha.cpp");
    if (focus == Focus::Terminal) {
      TestAccess::OpenTerminalViaService(shell);
    }
    if (focus == Focus::OutputPanel) {
      TestAccess::ShowOutputChannelWithLines(shell, "reachability", 3);
    }
    TestAccess::KeyDispatchProbe(shell).record_only = true;
  }

  ~FocusFixture() { TestAccess::KeyDispatchProbe(shell).record_only = false; }

  void Apply() {
    while (TestAccess::PromptSurfaceVisible(shell)) {
      (void)shell.HandleEvent(KeyDownEvent(SDLK_ESCAPE, SDL_KMOD_NONE));
    }
    switch (focus) {
      case Focus::Editor:
        TestAccess::SetFocusEditor(shell);
        break;
      case Focus::Tree:
        TestAccess::ShowTreeSidebar(shell);
        TestAccess::SetFocusSidebar(shell);
        break;
      case Focus::SearchView:
        TestAccess::ShowSearchSidebar(shell, "");
        break;
      case Focus::GitView:
        TestAccess::ShowGitSidebar(shell);
        TestAccess::SetFocusSidebar(shell);
        break;
      case Focus::OutputPanel:
        TestAccess::ShowOutputChannelWithLines(shell, "reachability", 0);
        TestAccess::SetFocusPanel(shell);
        break;
      case Focus::Terminal:
        TestAccess::SetPanelContentTerminal(shell);
        TestAccess::SetFocusPanel(shell);
        break;
      case Focus::DebugPane:
        TestAccess::FocusDebugPaneCallStack(shell);
        break;
      case Focus::QuickOpen:
        if (!TestAccess::FocusIsOverlay(shell)) {
          TestAccess::OpenCommandPalette(shell);
        }
        break;
    }
  }

  std::string Press(const ResolvedKeybinding& binding) {
    Apply();
    if (binding.leader_key != SDLK_UNKNOWN) {
      (void)shell.HandleEvent(KeyDownEvent(binding.leader_key, binding.leader_modifiers));
    }
    return PressKey(binding.key, binding.modifiers);
  }
  std::string Press(SDL_Keycode key, SDL_Keymod modifiers) {
    Apply();
    return PressKey(key, modifiers);
  }
  std::string PressKey(SDL_Keycode key, SDL_Keymod modifiers) {
    (void)shell.HandleEvent(KeyDownEvent(key, modifiers));
    (void)shell.ConsumePendingRenderInvalidation();
    const ResolvedKeybinding* last = TestAccess::KeyDispatchProbe(shell).last;
    return last == nullptr ? std::string{} : last->id;
  }

  Focus focus;
  TemporaryDirectory temp_dir;
  std::filesystem::path root;
  WorkspaceShell shell;
};

void TestEveryBuiltinBindingDispatchesWhereItsRegistrationSays() {
  std::vector<std::string> failures;
  std::size_t checked = 0;
  for (const Focus focus : kFocusTargets) {
    FocusFixture fixture(focus);
    fixture.Apply();
    Expect(TestAccess::KeyDispatchProbe(fixture.shell).record_only,
           "the matrix must not run the actions it presses");
    const KeybindingContext context = ContextOf(focus);
    const std::vector<ResolvedKeybinding>& bindings =
        TestAccess::ResolvedKeybindingsForTest(fixture.shell);
    for (const ResolvedKeybinding& binding : bindings) {
      if (binding.from_plugin) {
        continue;
      }
      fixture.Apply();
      const auto enabled = [&](workspace::ActionId action) {
        return TestAccess::IsActionEnabled(fixture.shell, action);
      };
      const ResolvedKeybinding* first =
          binding.leader_key == SDLK_UNKNOWN
              ? workspace::FindKeybinding(bindings, binding.key, binding.modifiers, context, enabled)
              : workspace::FindChordKeybinding(bindings, binding.leader_key, binding.leader_modifiers,
                                               binding.key, binding.modifiers, context, enabled);
      const bool registered_here = first == &binding;
      const std::optional<std::string_view> gate = Gate(binding, focus);
      const bool expected = registered_here && !gate.has_value();
      const std::string dispatched = fixture.Press(binding);
      ++checked;
      if (expected && dispatched != binding.id) {
        failures.push_back(binding.id + " should dispatch from the " + std::string(FocusName(focus)) +
                           " but " +
                           (dispatched.empty() ? std::string("nothing did") : dispatched + " did"));
      } else if (!expected && dispatched == binding.id) {
        failures.push_back(binding.id + " should not dispatch from the " +
                           std::string(FocusName(focus)) + " (" +
                           std::string(gate.value_or("not registered for this context")) +
                           ") but did");
      }
    }
  }
  Expect(checked >= 8 * 80, "the matrix should press every built-in binding from every focus target");
  std::string report;
  for (const std::string& failure : failures) {
    report += "\n  " + failure;
  }
  Expect(failures.empty(), "keybinding reachability mismatches:" + report);
}

void TestDispatchProbeSeesAGlobalShortcut() {
  FocusFixture fixture(Focus::Tree);
  Expect(fixture.Press(SDLK_F, static_cast<SDL_Keymod>(SDL_KMOD_CTRL | SDL_KMOD_SHIFT)) ==
             "project-search",
         "Ctrl+Shift+F from the tree should dispatch project-search");
  Expect(fixture.Press(SDLK_F, SDL_KMOD_CTRL).empty(),
         "Ctrl+F from the tree should dispatch nothing (find is an editor text shortcut)");
}

}

void RegisterKeybindingReachabilityTests(std::vector<TestCase>& tests) {
  AddTest(tests, "KeybindingReachability/DispatchProbeSeesAGlobalShortcut",
          TestDispatchProbeSeesAGlobalShortcut);
  AddTest(tests, "KeybindingReachability/EveryBuiltinBindingDispatchesWhereItsRegistrationSays",
          TestEveryBuiltinBindingDispatchesWhereItsRegistrationSays);
}

}
