#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "compare/CompareModel.h"
#include "project/PatchApplyTypes.h"

namespace microide::project {

struct PatchGenerationOptions {
  std::size_t context_lines = 3;
  std::size_t max_patch_bytes = 64u * 1024 * 1024;
};

PatchChangeSpan ChangeSpanForRows(const compare::CompareModel& model,
                                  std::size_t first_row,
                                  std::size_t last_row);

struct StagingPatchOutcome {
  std::optional<std::string> patch;
  bool already_applied = false;
  bool span_not_intact = false;
};
StagingPatchOutcome BuildStagingPatch(std::string_view head_text,
                                      std::string_view current_index_text,
                                      std::string_view worktree_text,
                                      const PatchChangeSpan& span,
                                      bool stage,
                                      const std::filesystem::path& relative_path,
                                      bool head_exists = true,
                                      bool index_exists = true,
                                      bool worktree_exists = true,
                                      const PatchGenerationOptions& options = {});

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

}
