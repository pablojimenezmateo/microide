#pragma once

#include <SDL3/SDL.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "compare/CompareModel.h"
#include "compare/MergeModel.h"
#include "editor/EditorViewRenderer.h"
#include "editor/TextViewport.h"
#include "plugin/PluginHost.h"
#include "platform/FileIndexWatcher.h"
#include "project/DirectoryTree.h"
#include "project/FileFinder.h"
#include "project/GitBlameService.h"
#include "project/FileIndex.h"
#include "project/GitCompareService.h"
#include "project/ProjectSearchService.h"
#include "render/TextRenderer.h"
#include "render/Theme.h"
#include "terminal/TerminalSession.h"
#include "workspace/WorkspaceLayout.h"
#include "workspace/WorkspaceActionAvailability.h"
#include "workspace/WorkspaceActionTypes.h"
#include "workspace/WorkspaceContext.h"
#include "workspace/WorkspaceFormatterRegistry.h"
#include "workspace/WorkspaceSaveParticipants.h"
#include "workspace/WorkspaceCompletionRegistry.h"
#include "workspace/WorkspaceCodeActionRegistry.h"
#include "workspace/WorkspaceTaskRegistry.h"
#include "workspace/WorkspaceTaskRuntime.h"
#include "workspace/WorkspaceToolRegistry.h"
#include "workspace/WorkspaceToolDownloader.h"
#include "workspace/WorkspaceDapManager.h"
#include "workspace/WorkspaceLspManager.h"
#include "workspace/WorkspaceTestController.h"
#include "workspace/WorkspaceScmRegistry.h"
#include "workspace/WorkspaceAnnotationRegistry.h"
#include "workspace/WorkspaceAuthProvider.h"
#include "workspace/WorkspaceSecretStorage.h"
#include "workspace/WorkspaceVirtualDocument.h"
#include "workspace/WorkspaceReviewComments.h"
#include "workspace/WorkspaceAiProvider.h"
#include "workspace/WorkspaceInlineCompletion.h"
#include "workspace/WorkspaceConversation.h"
#include "workspace/WorkspaceExternalAgent.h"
#include "workspace/WorkspaceMcpTool.h"
#include "workspace/WorkspaceAiContext.h"
#include "workspace/WorkspaceProviderBridge.h"
#include "workspace/WorkspaceEventResult.h"
#include "workspace/WorkspaceInteractionState.h"
#include "workspace/WorkspaceMenuState.h"
#include "workspace/WorkspaceMenuRegistry.h"
#include "workspace/PersistenceService.h"
#include "workspace/WorkspacePersistenceFormat.h"
#include "workspace/WorkspaceOutputChannels.h"
#include "workspace/WorkspacePluginReloadRequest.h"
#include "workspace/WorkspacePluginRuntime.h"
#include "workspace/WorkspaceProjectFileMonitor.h"
#include "workspace/WorkspaceProjectDialogState.h"
#include "workspace/WorkspaceProjectState.h"
#include "workspace/WorkspaceProjectSearchRuntime.h"
#include "workspace/WorkspacePromptState.h"
#include "workspace/WorkspaceRootView.h"
#include "workspace/WorkspaceStatusRegistry.h"
#include "workspace/WorkspaceSidebarState.h"
#include "workspace/WorkspaceTerminalSelection.h"
#include "workspace/WorkspaceTextInputState.h"

namespace microide::workspace {

class WorkspaceActionContext;
class PromptSurfaceService;
class ProjectCatalogService;
class PersistenceCoordinator;
class CommandPromptCoordinator;
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

class WorkspaceShell {
#include "workspace/WorkspaceShellMembers.inc"
};

}  // namespace microide::workspace
