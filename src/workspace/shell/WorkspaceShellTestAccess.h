#pragma once

#include "workspace/actions/WorkspaceActionCoordinator.h"
#include "workspace/render/CompareMergeRender.h"
#include "workspace/services/CompareMergeService.h"
#include "workspace/git/CompareTabReview.h"
#include "workspace/coordinators/WorkspaceCommandLineCoordinator.h"
#include "workspace/coordinators/WorkspaceKeyInputCoordinator.h"
#include "workspace/WorkspaceLayout.h"
#include "workspace/coordinators/WorkspaceMenuCoordinator.h"
#include "workspace/git/GitSidebarHeaderLayout.h"
#include "workspace/ProjectSearchPanelLayout.h"
#include "workspace/persistence/WorkspacePersistenceCoordinator.h"
#include "workspace/render/RenderViewModelBuilder.h"
#include "workspace/SettingFlags.h"
#include "workspace/render/WorkspaceShellRenderPrimitives.h"
#include "workspace/shell/WorkspaceShell.h"
#include "workspace/coordinators/WorkspaceTextInputCoordinator.h"

#include "editor/SnippetEngine.h"
#include "util/PerformanceCounters.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <functional>
#include <future>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace microide::workspace {

#ifdef MICROIDE_TESTING

struct WorkspaceShell::TestAccess {
#include "workspace/testaccess/WorkspaceShellTestAccessProjectEditor.inc"
#include "workspace/testaccess/WorkspaceShellTestAccessActionsIntegrations.inc"
#include "workspace/testaccess/WorkspaceShellTestAccessLayoutRendering.inc"
#include "workspace/testaccess/WorkspaceShellTestAccessStateMenus.inc"
};

#endif

}  // namespace microide::workspace
