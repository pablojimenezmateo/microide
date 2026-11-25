#pragma once

#include <memory>
#include <optional>

#include "workspace/actions/WorkspaceActionAvailability.h"

namespace microide::workspace {

class CommandLineCoordinator;
class CompareMergeService;
class CompareReviewService;
class CompareMouseCoordinator;
class ChromeMouseCoordinator;
class DebugPaneMouseCoordinator;
class DebugPaneService;
class EditorMouseCoordinator;
class TabCoordinator;
using EditorTabService = TabCoordinator;
class KeyInputCoordinator;
class LifecycleCoordinator;
class MenuCoordinator;
class MergeMouseCoordinator;
class PanelMouseCoordinator;
class PersistenceCoordinator;
class ProjectCatalogService;
class PromptSurfaceService;
class SidebarMouseCoordinator;
class SidebarCoordinator;
using SidebarService = SidebarCoordinator;
class TabMouseCoordinator;
class TerminalPanelService;
class RemoteHostService;
class TextInputCoordinator;
class WorkspaceActionContext;

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
  std::unique_ptr<CompareReviewService> compare_review_service;
  std::unique_ptr<PromptSurfaceService> prompt_surface_service;
  std::unique_ptr<TerminalPanelService> terminal_panel_service;
  std::unique_ptr<RemoteHostService> remote_host_service;
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

}
