#pragma once

#include <filesystem>
#include <optional>
#include <string_view>

#include "compare/CompareReviewTypes.h"
#include "project/GitRepositoryState.h"

namespace microide::compare {

struct CompareSemanticMetadataInput {
  std::filesystem::path path;
  // Views, not owned strings. The compare refresh path passes whole-file buffers
  // here; taking them by value copied both files on every call, and the classifier
  // only ever reads them.
  std::string_view left_content;
  std::string_view right_content;
  std::optional<project::GitRepositoryEntry> git_entry;
  std::optional<std::filesystem::path> old_path;
  bool old_executable = false;
  bool new_executable = false;
};

CompareSemanticFileMetadata InferCompareSemanticFileMetadata(
    const CompareSemanticMetadataInput& input);

}  // namespace microide::compare
