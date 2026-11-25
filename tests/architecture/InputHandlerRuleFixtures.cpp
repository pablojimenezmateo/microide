#include "architecture/ArchitectureRuleFixtures.h"

#include "TestSupport.h"
#include "architecture/WorkspaceServiceArchitectureRules.h"

#include <filesystem>

namespace microide::tests::architecture {

void RunWheelFocusRuleFixtures() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path();
  std::filesystem::create_directories(root / "src/workspace");
  const std::filesystem::path coordinator = root / "src/workspace/SomeMouseCoordinator.cpp";

  WriteFile(coordinator,
            "bool C::HandleWheel(const SDL_Event& e, int t) {\n"
            "  Scroll(-t);\n"
            "  state_.surface.focus = FocusTarget::Editor;\n"
            "  return true;\n"
            "}\n");
  Expect(!CheckWheelHandlersDoNotMoveFocus(root).violations.empty(),
         "a wheel handler that assigns surface.focus must be flagged");

  WriteFile(coordinator,
            "bool C::HandleWheel(const SDL_Event& e, int t) {\n"
            "  if (state_.surface.focus == FocusTarget::Panel) { return false; }\n"
            "  Scroll(-t);\n"
            "  return true;\n"
            "}\n");
  const RuleResult clean = CheckWheelHandlersDoNotMoveFocus(root);
  Expect(clean.violations.empty() && clean.missing_targets.empty(),
         "a wheel handler that only reads surface.focus must pass");

  WriteFile(coordinator,
            "bool C::HandleWheel(const SDL_Event& e, int t) {\n"
            "  Scroll(-t);\n"
            "  return true;\n"
            "}\n"
            "bool C::HandleButtonDown(const SDL_Event& e) {\n"
            "  state_.surface.focus = FocusTarget::Panel;\n"
            "  return true;\n"
            "}\n");
  Expect(CheckWheelHandlersDoNotMoveFocus(root).violations.empty(),
         "a click handler taking focus must not be flagged by the wheel rule");

  TemporaryDirectory empty_dir;
  std::filesystem::create_directories(empty_dir.path() / "src/workspace");
  Expect(!CheckWheelHandlersDoNotMoveFocus(empty_dir.path()).violations.empty(),
         "a scan with no wheel handlers must report a violation, not pass vacuously");
}

void RunQueuedModifiersRuleFixtures() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path();
  std::filesystem::create_directories(root / "src/workspace/shell");
  const std::filesystem::path dispatcher = root / "src/workspace/WorkspaceEventOrchestrator.cpp";
  const std::filesystem::path handler = root / "src/workspace/shell/Mouse.cpp";
  WriteFile(dispatcher, "void D(){ state_.key_modifiers = SDL_GetModState(); }\n");

  WriteFile(handler, "void U(){ Finish((SDL_GetModState() & SDL_KMOD_CTRL) != 0); }\n");
  Expect(CheckPointerHandlersReadQueuedModifiers(root).violations.size() == 1,
         "a handler reading SDL_GetModState() must be flagged");

  WriteFile(handler,
            "// never SDL_GetModState() here\n"
            "void U(){ Finish((interaction_state_.key_modifiers & SDL_KMOD_CTRL) != 0); }\n");
  const RuleResult clean = CheckPointerHandlersReadQueuedModifiers(root);
  Expect(clean.violations.empty() && clean.missing_targets.empty(),
         "the queued modifiers and a comment naming the live read must pass");

  WriteFile(dispatcher, "void D(){}\n");
  Expect(!CheckPointerHandlersReadQueuedModifiers(root).violations.empty(),
         "a dispatcher without its resync must report a moved target");
  std::filesystem::remove(dispatcher);
  Expect(!CheckPointerHandlersReadQueuedModifiers(root).missing_targets.empty(),
         "a missing dispatcher must be reported, not pass");
}

}
