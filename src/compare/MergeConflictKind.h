#pragma once

#include <optional>
#include <string_view>

#include "project/GitRepositoryState.h"

namespace microide::compare {

enum class MergeFileConflictKind {
  BothModified,
  BothAdded,
  BothDeleted,
  DeletedByUs,
  DeletedByThem,
  AddedByUs,
  AddedByThem,
  RenameRename,
  RenameDelete,
  FileDirectory,
  Binary,
  Submodule,
  Mode,
  LineEndingHeavy,
  Unknown,
};

struct MergeFileConflictMetadata {
  MergeFileConflictKind kind = MergeFileConflictKind::Unknown;
  bool text_hunks_available = true;
  bool requires_existence_choice = false;
  std::string summary;
};

struct MergeConflictClassificationInput {
  const project::GitRepositoryEntry* repository_entry = nullptr;
  bool base_exists = true;
  bool incoming_exists = true;
  bool current_exists = true;
  std::string_view base_content;
  std::string_view incoming_content;
  std::string_view current_content;
};

MergeFileConflictMetadata ClassifyMergeFileConflict(const MergeConflictClassificationInput& input);
const char* MergeFileConflictKindLabel(MergeFileConflictKind kind);
bool MergeContentLooksBinary(std::string_view content);
bool MergeContentIsLineEndingHeavy(std::string_view base,
                                    std::string_view incoming,
                                    std::string_view current);

}  // namespace microide::compare
