#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "editor/RuntimeSyntaxRegistry.h"

namespace microide::editor::runtime_syntax {

std::vector<RuntimeSyntaxDefinitionData> LoadDefinitionsFromDirectories(
    const std::vector<std::filesystem::path>& directories,
    std::vector<std::string>* errors = nullptr);
std::uint64_t DefinitionSourceFingerprint(
    const std::vector<std::filesystem::path>& directories);

}  // namespace microide::editor::runtime_syntax
