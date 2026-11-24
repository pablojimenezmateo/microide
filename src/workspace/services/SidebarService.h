#pragma once

#include "workspace/coordinators/WorkspaceSidebarCoordinator.h"

namespace microide::workspace {

// The workspace's sidebar service (openspec workspace-architecture): sidebar mode,
// refresh, open-or-select. SidebarCoordinator implements it directly; a separate
// class that forwarded each of its methods one-to-one added nothing.
using SidebarService = SidebarCoordinator;

}  // namespace microide::workspace
