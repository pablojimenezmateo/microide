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
  // Upper bound on the generated patch text. A whole-file or huge-selection patch
  // copy/stage would otherwise allocate an arbitrarily large string on the UI/apply
  // path; over budget, generation returns nullopt (surfaced as "no patch") instead
  // of building it. 0 disables the budget. TD-2026-07-17A-099.
  std::size_t max_patch_bytes = 64u * 1024 * 1024;  // 64 MiB
};

// The PatchChangeSpan that model rows [first_row, last_row] of a HEAD-vs-
// working-tree model describe.
PatchChangeSpan ChangeSpanForRows(const compare::CompareModel& model,
                                  std::size_t first_row,
                                  std::size_t last_row);

// The patch git needs to bring `current_text` (the index) to the state where
// `span` — a change between `head_text` and `worktree_text` — is applied
// (`stage`) or reverted (`unstage`). Empty when the change is already in that
// state; nullopt when the span's lines are no longer intact in `current_text`
// (partially staged, or edited since), which the caller reports.
struct StagingPatchOutcome {
  std::optional<std::string> patch;
  bool already_applied = false;
  bool span_not_intact = false;
};
// `head_exists`/`index_exists`/`worktree_exists`: whether the file exists on
// each side (an absent side reads as "" but is a creation/deletion, not an
// empty file — see CompareBuildOptions).
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

}  // namespace microide::project
