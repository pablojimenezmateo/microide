#pragma once

#include <filesystem>
#include <optional>
#include <string>

#include "compare/CompareModel.h"
#include "project/PatchApplyTypes.h"

namespace microide::project {

struct PatchGenerationOptions {
  std::size_t context_lines = 3;
  // Upper bound on the generated patch text. A whole-file or huge-selection patch
  // copy/stage would otherwise allocate an arbitrarily large string on the UI/apply
  // path; over budget, generation returns nullopt (surfaced as "no patch") instead
  // of building it. 0 disables the budget. TD-2026-07-17A-099.
  std::size_t max_patch_bytes = 64u * 1024 * 1024;  // 64 MiB
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
