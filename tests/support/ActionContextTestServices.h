#pragma once

// Every service WorkspaceActionContext::Services names, built over empty state,
// for a test that drives the action context without a WorkspaceShell. Each one
// has default (empty) Operations, so an action that reaches a service the test
// did not mean to exercise is a quiet no-op, not a crash.

#include <string>
#include <vector>

#include "render/Theme.h"
#include "workspace/WorkspaceContext.h"
#include "workspace/WorkspaceOutputChannels.h"
#include "workspace/WorkspacePluginRuntime.h"
#include "workspace/actions/WorkspaceActionServices.h"
#include "workspace/coordinators/WorkspaceCompareInteractionCoordinator.h"
#include "workspace/coordinators/WorkspaceDiffTabCoordinator.h"
#include "workspace/coordinators/WorkspaceSidebarCoordinator.h"
#include "workspace/coordinators/WorkspaceTabCoordinator.h"
#include "workspace/coordinators/WorkspaceTextInputCoordinator.h"
#include "workspace/debug/DebugPaneService.h"
#include "workspace/debug/DebugService.h"
#include "workspace/persistence/SettingsStore.h"
#include "workspace/persistence/WorkspacePersistenceCoordinator.h"
#include "workspace/services/AssistService.h"
#include "workspace/services/CompareMergeService.h"
#include "workspace/services/EditorTabService.h"
#include "workspace/services/NotificationService.h"
#include "workspace/services/QuickPickService.h"
#include "workspace/services/SidebarService.h"
#include "workspace/services/TerminalPanelService.h"

namespace microide::tests {

struct ActionContextTestServices {
  ActionContextTestServices(workspace::ProjectCatalogState& catalog,
                            workspace::ProjectWorkspaceState& state, float& ui_scale)
      : terminal_panel(state, {}),
        editor_tabs(workspace::TabCoordinator(catalog, state, {})),
        text_input(state, prompts, menus, text_input_state, {}),
        persistence(workspace_context, theme, colorschemes, theme_names, ui_scale, settings, {}),
        sidebar(workspace::SidebarCoordinator(state, prompts, menus, project_root, plugin_runtime, {})),
        compare_merge(workspace::DiffTabCoordinator(state, {}),
                      workspace::CompareInteractionCoordinator(state, {})),
        debug_pane(state, {}) {}

  workspace::WorkspaceActionContext::Services Services() {
    return {.assist = assist,
            .debug = debug,
            .terminal_panel = terminal_panel,
            .editor_tabs = editor_tabs,
            .text_input = text_input,
            .quick_pick = quick_pick,
            .notifications = notifications,
            .persistence = persistence,
            .output_channels = output_channels,
            .sidebar = sidebar,
            .compare_merge = compare_merge,
            .debug_pane = debug_pane};
  }

  workspace::PromptState prompts;
  workspace::MenuSurfaceState menus;
  workspace::TextInputState text_input_state;
  workspace::WorkspaceContext workspace_context;
  render::Theme theme;
  std::vector<std::string> colorschemes;
  render::ThemeNameCatalog theme_names;
  workspace::SettingsStore settings;
  std::filesystem::path project_root;
  workspace::WorkspacePluginRuntime plugin_runtime;
  workspace::WorkspaceOutputChannels output_channels;

  workspace::AssistService assist;
  workspace::DebugService debug;
  workspace::TerminalPanelService terminal_panel;
  workspace::EditorTabService editor_tabs;
  workspace::TextInputCoordinator text_input;
  workspace::QuickPickService quick_pick;
  workspace::NotificationService notifications;
  workspace::PersistenceCoordinator persistence;
  workspace::SidebarService sidebar;
  workspace::CompareMergeService compare_merge;
  workspace::DebugPaneService debug_pane;
};

}  // namespace microide::tests
