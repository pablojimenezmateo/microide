#pragma once

#include "workspace/WorkspaceActionCoordinator.h"
#include "workspace/WorkspaceCommandPromptCoordinator.h"
#include "workspace/WorkspaceLayout.h"
#include "workspace/WorkspacePersistenceCoordinator.h"
#include "workspace/RenderViewModelBuilder.h"
#include "workspace/WorkspaceShellRenderPrimitives.h"
#include "workspace/WorkspaceShell.h"
#include "workspace/WorkspaceTextInputCoordinator.h"

#include "editor/SnippetEngine.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <functional>
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
