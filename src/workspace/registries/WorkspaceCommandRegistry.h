#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "workspace/actions/WorkspaceActionTypes.h"

namespace microide::workspace {

std::span<const ActionSpec> WorkspaceCommandSpecs();
const ActionSpec* FindWorkspaceActionSpec(ActionId id);
const ActionSpec* FindWorkspaceActionByCommand(std::string_view command_name);
const std::vector<std::string>& WorkspaceCommandNames();
std::vector<std::string> WorkspaceDocumentedCommandUsages();

// Whether a command's usage (`git-create-branch <name> [start-point]`) names an
// argument it cannot run without: a `<...>` outside every `[...]`. A command that
// has a bare form (Go to Line's prompt, Open File's dialog) writes its argument
// optional, so this is also what the usage line promises a reader.
bool CommandUsageRequiresArgument(std::string_view usage);

}  // namespace microide::workspace
