#pragma once

#include "workspace/coordinators/WorkspaceTabCoordinator.h"

namespace microide::workspace {

// The workspace's editor-tab service (openspec workspace-architecture): the tab
// list, active tab, splits, dirty state, save/load and view restore, and the
// owner of the active editor viewport. TabCoordinator implements it directly; a
// separate class that forwarded each of its methods one-to-one added nothing.
using EditorTabService = TabCoordinator;

}  // namespace microide::workspace
