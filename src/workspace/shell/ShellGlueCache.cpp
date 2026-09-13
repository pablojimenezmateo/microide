#include "workspace/shell/ShellGlueCache.h"

// The unique_ptr members are declared over forward-declared types, so the
// constructor and destructor are defined here, where every type is complete.
#include "workspace/actions/WorkspaceActionServices.h"
#include "workspace/coordinators/WorkspaceChromeMouseCoordinator.h"
#include "workspace/coordinators/WorkspaceCommandLineCoordinator.h"
#include "workspace/coordinators/WorkspaceCompareMouseCoordinator.h"
#include "workspace/coordinators/WorkspaceEditorMouseCoordinator.h"
#include "workspace/coordinators/WorkspaceKeyInputCoordinator.h"
#include "workspace/coordinators/WorkspaceLifecycleCoordinator.h"
#include "workspace/coordinators/WorkspaceMenuCoordinator.h"
#include "workspace/coordinators/WorkspaceMergeMouseCoordinator.h"
#include "workspace/coordinators/WorkspacePanelMouseCoordinator.h"
#include "workspace/coordinators/WorkspaceSidebarMouseCoordinator.h"
#include "workspace/coordinators/WorkspaceTabMouseCoordinator.h"
#include "workspace/coordinators/WorkspaceTextInputCoordinator.h"
#include "workspace/debug/DebugPaneMouseCoordinator.h"
#include "workspace/debug/DebugPaneService.h"
#include "workspace/persistence/WorkspacePersistenceCoordinator.h"
#include "workspace/services/CompareMergeService.h"
#include "workspace/services/EditorTabService.h"
#include "workspace/services/ProjectCatalogService.h"
#include "workspace/services/PromptSurfaceService.h"
#include "workspace/services/SidebarService.h"
#include "workspace/services/TerminalPanelService.h"

namespace microide::workspace {

ShellGlueCache::ShellGlueCache() = default;
ShellGlueCache::~ShellGlueCache() = default;

}  // namespace microide::workspace
