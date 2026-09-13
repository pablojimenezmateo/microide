#pragma once

#include <memory>
#include <optional>

#include "workspace/actions/WorkspaceActionAvailability.h"

namespace microide::workspace {

class CommandLineCoordinator;
class CompareMergeService;
class CompareMouseCoordinator;
class ChromeMouseCoordinator;
class DebugPaneMouseCoordinator;
class DebugPaneService;
class EditorMouseCoordinator;
class EditorTabService;
class KeyInputCoordinator;
class LifecycleCoordinator;
class MenuCoordinator;
class MergeMouseCoordinator;
class PanelMouseCoordinator;
class PersistenceCoordinator;
class ProjectCatalogService;
class PromptSurfaceService;
class SidebarMouseCoordinator;
class SidebarService;
class TabMouseCoordinator;
class TerminalPanelService;
class TextInputCoordinator;
class WorkspaceActionContext;

// The shell's lazily built, then permanently reused, glue objects.
//
// Named for what it is rather than WorkspaceShellGlue: the architecture lint
// caps WorkspaceShell*.cpp companion translation units, and this one carries no
// shell behavior -- only a defaulted constructor and destructor, defined where
// the forward-declared coordinator types are complete.
//
// Every member here is a coordinator, service or action context: references into
// WorkspaceShell state plus an Operations struct of tens of std::functions whose
// lambdas capture only the shell, and no per-call state of its own. Each used to
// be rebuilt at every call of its Make* factory — which for the mouse, key and
// active-viewport paths meant per event, per frame, or several times per frame.
// KeyInputCoordinator alone is 149 std::functions and ran on every keystroke.
//
// They live in one holder rather than as 21 loose shell members so the shell's
// declaration surface does not grow by one line per cached object, and so the
// lifetime rule has one place to be stated: WorkspaceShell owns this by
// unique_ptr and declares it AFTER the state these objects reference, so they
// are destroyed first. Forward declarations keep the heavy coordinator headers
// out of WorkspaceShell.h; the shell's constructor and destructor TUs include
// them.
struct ShellGlueCache {
  std::optional<ActionAvailability> action_availability;
  std::unique_ptr<WorkspaceActionContext> action_context;
  std::unique_ptr<KeyInputCoordinator> key_input_coordinator;
  std::unique_ptr<ChromeMouseCoordinator> chrome_mouse_coordinator;
  std::unique_ptr<EditorMouseCoordinator> editor_mouse_coordinator;
  std::unique_ptr<SidebarMouseCoordinator> sidebar_mouse_coordinator;
  std::unique_ptr<PanelMouseCoordinator> panel_mouse_coordinator;
  std::unique_ptr<TabMouseCoordinator> tab_mouse_coordinator;
  std::unique_ptr<TextInputCoordinator> text_input_coordinator;
  std::unique_ptr<MenuCoordinator> menu_coordinator;
  std::unique_ptr<EditorTabService> editor_tab_service;
  std::unique_ptr<SidebarService> sidebar_service;
  std::unique_ptr<CompareMergeService> compare_merge_service;
  std::unique_ptr<PromptSurfaceService> prompt_surface_service;
  std::unique_ptr<TerminalPanelService> terminal_panel_service;
  std::unique_ptr<ProjectCatalogService> project_catalog_service;
  std::unique_ptr<DebugPaneService> debug_pane_service;
  std::unique_ptr<PersistenceCoordinator> persistence_coordinator;
  std::unique_ptr<MergeMouseCoordinator> merge_mouse_coordinator;
  std::unique_ptr<CompareMouseCoordinator> compare_mouse_coordinator;
  std::unique_ptr<DebugPaneMouseCoordinator> debug_pane_mouse_coordinator;
  std::unique_ptr<CommandLineCoordinator> command_line_coordinator;
  std::unique_ptr<LifecycleCoordinator> lifecycle_coordinator;

  ShellGlueCache();
  ~ShellGlueCache();
};

}  // namespace microide::workspace
