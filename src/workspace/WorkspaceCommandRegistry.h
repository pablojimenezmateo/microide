#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "workspace/WorkspaceActionTypes.h"

namespace microide::workspace {

std::span<const ActionSpec> WorkspaceCommandSpecs();
const ActionSpec* FindWorkspaceActionSpec(ActionId id);
const ActionSpec* FindWorkspaceActionByCommand(std::string_view command_name);
const std::vector<std::string>& WorkspaceCommandNames();
std::vector<std::string> WorkspaceDocumentedCommandUsages();

}  // namespace microide::workspace
