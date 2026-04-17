#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "editor/RuntimeSyntaxRegistry.h"

namespace microide::editor::runtime_syntax {

std::vector<RuntimeSyntaxDefinitionData> LoadDefinitionsFromDirectories(
    const std::vector<std::filesystem::path>& directories,
    std::vector<std::string>* errors = nullptr);

}  // namespace microide::editor::runtime_syntax
