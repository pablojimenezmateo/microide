#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "workspace/WorkspaceShell.h"

namespace microide::workspace {

std::span<const WorkspaceShell::ActionSpec> WorkspaceCommandSpecs();
const WorkspaceShell::ActionSpec* FindWorkspaceActionSpec(WorkspaceShell::ActionId id);
const WorkspaceShell::ActionSpec* FindWorkspaceActionByCommand(std::string_view command_name);
const std::vector<std::string>& WorkspaceCommandNames();
std::vector<std::string> WorkspaceDocumentedCommandUsages();

}  // namespace microide::workspace
