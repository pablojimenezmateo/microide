#pragma once

#include <SDL3/SDL.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "compare/CompareModel.h"
#include "compare/MergeModel.h"
#include "editor/EditorViewRenderer.h"
#include "editor/HighlightPrefetchService.h"
#include "editor/TextViewport.h"
#include "plugin/PluginHost.h"
#include "platform/FileIndexWatcher.h"
#include "project/DirectoryTree.h"
#include "project/FileFinder.h"
#include "project/GitBlameService.h"
#include "project/ProjectBackgroundExecutor.h"
#include "project/FileIndex.h"
#include "project/GitRepositoryMetadataTracker.h"
#include "project/ProjectChangeCoalescer.h"
#include "project/GitCompareService.h"
#include "project/ProjectSearchService.h"
#include "render/SurfaceTextureCache.h"
#include "render/TextRenderer.h"
#include "render/Theme.h"
#include "terminal/TerminalSession.h"
#include "workspace/CompareInput.h"
#include "workspace/EditorPreferenceSettings.h"
#include "workspace/HoverTooltip.h"
#include "workspace/ProjectReplaceOutcome.h"
#include "workspace/render/SingleLineViewMetrics.h"
#include "workspace/WorkspaceLayout.h"
#include "workspace/actions/WorkspaceActionAvailability.h"
#include "workspace/actions/WorkspaceActionTypes.h"
#include "workspace/WorkspaceContext.h"
#include "workspace/registries/WorkspaceKeybindingRegistry.h"
#include "workspace/registries/WorkspaceFormatterRegistry.h"
#include "workspace/WorkspaceSaveParticipants.h"
#include "workspace/registries/WorkspaceCompletionRegistry.h"
#include "workspace/registries/WorkspaceCodeActionRegistry.h"
#include "workspace/registries/WorkspaceToolRegistry.h"
#include "workspace/lsp/WorkspaceLspManager.h"
#include "workspace/WorkspaceTestController.h"
#include "workspace/registries/WorkspaceScmRegistry.h"
#include "workspace/registries/WorkspaceAnnotationRegistry.h"
#include "workspace/registries/WorkspaceThemeRegistry.h"
#include "workspace/registries/WorkspaceFileIconRegistry.h"
#include "workspace/PluginEditorEventTracker.h"
#include "workspace/WorkspaceVirtualDocument.h"
#include "workspace/WorkspaceEventResult.h"
#include "workspace/services/AssistService.h"
#include "workspace/control/ControlChannelService.h"
#include "workspace/control/ControlSpec.h"
#include "workspace/debug/DebugService.h"
#include "workspace/lsp/LspService.h"
#include "workspace/services/EditorBlameOverlayService.h"
#include "workspace/state/WorkspaceInteractionState.h"
#include "workspace/state/WorkspaceMenuState.h"
#include "workspace/registries/WorkspaceMenuRegistry.h"
#include "workspace/services/LayoutModeService.h"
#include "workspace/git/GitRepositoryService.h"
#include "workspace/git/CommitWorkflowService.h"
#include "workspace/git/GitOperationService.h"
#include "workspace/render/NotificationLayout.h"
#include "workspace/services/NotificationService.h"
#include "workspace/git/PatchApplyService.h"
#include "workspace/persistence/PersistenceService.h"
#include "workspace/persistence/RecentsService.h"
#include "workspace/services/SettingsOverlayService.h"
#include "workspace/persistence/SettingsStore.h"
#include "workspace/services/StatusBarModelService.h"
#include "workspace/services/StatusBarService.h"
#include "workspace/services/TabStripService.h"
#include "workspace/services/TerminalFindService.h"
#include "workspace/WorkspaceTabStripChrome.h"
#include "workspace/persistence/WorkspacePersistenceFormat.h"
#include "workspace/WorkspaceOutputChannels.h"
#include "workspace/WorkspaceLanguageContract.h"
#include "workspace/WorkspacePluginReloadRequest.h"
#include "workspace/WorkspacePluginRuntime.h"
#include "workspace/WorkspaceProjectFileMonitor.h"
#include "workspace/state/WorkspaceProjectDialogState.h"
#include "workspace/state/WorkspaceProjectState.h"
#include "workspace/debug/DebugPaneRegistry.h"
#include "workspace/debug/DebugPaneService.h"
#include "workspace/WorkspaceProjectSearchRuntime.h"
#include "workspace/state/WorkspacePromptState.h"
#include "workspace/render/RenderViewModelBuilder.h"
#include "workspace/shell/WorkspaceRootView.h"
#include "workspace/registries/WorkspaceStatusRegistry.h"
#include "workspace/state/WorkspaceSidebarState.h"
#include "workspace/WorkspaceStartupOptions.h"
#include "workspace/WorkspaceTerminalSelection.h"
#include "workspace/state/WorkspaceTextInputState.h"

namespace microide::workspace {

struct WorkspaceTabTextModel;
class WorkspaceActionContext;
class PromptSurfaceService;
class ProjectCatalogService;
class PersistenceCoordinator;
class CommandLineCoordinator;
class MenuCoordinator;
class KeyInputCoordinator;
class TextInputCoordinator;
class EditorTabService;
class TabCoordinator;
class PathMutationCoordinator;
class LifecycleCoordinator;
class DirtyPromptCoordinator;
class CompareInteractionCoordinator;
class DiffTabCoordinator;
class CompareMergeService;
class TerminalPanelService;
class SidebarCoordinator;
class SidebarService;
class ChromeMouseCoordinator;
class EditorMouseCoordinator;
class CompareMouseCoordinator;
class MergeMouseCoordinator;
class TabMouseCoordinator;
class SidebarMouseCoordinator;
class PanelMouseCoordinator;
class DebugPaneMouseCoordinator;

// Memoizes WorkspaceShell::BreadcrumbLabel across paints. The label is a path-derived
// string rebuilt every chrome frame otherwise; cached against the inputs that shape it
// so a repaint that changed none reuses it (TD-2026-07-17A-023). Namespace-scoped (not
// nested in WorkspaceShell) to keep WorkspaceShellMembers.inc within its size budget.
struct BreadcrumbLabelCache {
  bool valid = false;
  int mode = 0;  // 0 editor, 1 compare, 2 merge
  bool placeholder = false;
  std::filesystem::path root;
  std::filesystem::path path;
  std::string left_label;
  std::string right_label;
  std::string label;
};

class WorkspaceShell {
#include "workspace/shell/WorkspaceShellMembers.inc"
};

}  // namespace microide::workspace
