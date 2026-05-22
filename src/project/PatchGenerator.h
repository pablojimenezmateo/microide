#pragma once

#include <filesystem>
#include <optional>
#include <string>

#include "compare/CompareModel.h"
#include "project/PatchApplyTypes.h"

namespace microide::project {

struct PatchGenerationOptions {
  std::size_t context_lines = 3;
};

std::optional<std::string> GenerateComparePatch(const compare::CompareModel& model,
                                                const std::filesystem::path& relative_path,
                                                int hunk_index,
                                                const PatchGenerationOptions& options = {});

std::optional<std::string> GenerateComparePatchForRows(
    const compare::CompareModel& model,
    const std::filesystem::path& relative_path,
    std::size_t first_model_row,
    std::size_t last_model_row,
    const PatchGenerationOptions& options = {});

std::optional<PatchLineSelection> PatchLineSelectionFromModelRows(
    const compare::CompareModel& model,
    std::size_t first_model_row,
    std::size_t last_model_row);

bool PatchLineSelectionHasChanges(const compare::CompareModel& model,
                                  const PatchLineSelection& selection);

}  // namespace microide::project
