#include "compare/MergeConflictKind.h"

#include <algorithm>

#include "util/StringUtil.h"

namespace microide::compare {
namespace {

bool ContentHasNulByte(std::string_view content) {
  return content.find('\0') != std::string_view::npos;
}

using util::NormalizeLineEndings;

MergeFileConflictKind KindFromGitConflict(project::GitConflictKind git_kind,
                                          const project::GitRepositoryEntry* entry) {
  if (entry != nullptr && entry->submodule && entry->conflicted) {
    return MergeFileConflictKind::Submodule;
  }
  if (entry != nullptr && entry->conflicted && entry->path_is_directory) {
    return MergeFileConflictKind::FileDirectory;
  }
  if (entry != nullptr && entry->kind == project::GitRepositoryEntryKind::Renamed && entry->conflicted) {
    if (entry->old_path.has_value()) {
      return MergeFileConflictKind::RenameRename;
    }
    return MergeFileConflictKind::RenameDelete;
  }

  switch (git_kind) {
    case project::GitConflictKind::BothModified:
      return MergeFileConflictKind::BothModified;
    case project::GitConflictKind::BothAdded:
      return MergeFileConflictKind::BothAdded;
    case project::GitConflictKind::BothDeleted:
      return MergeFileConflictKind::BothDeleted;
    case project::GitConflictKind::DeletedByUs:
      return MergeFileConflictKind::DeletedByUs;
    case project::GitConflictKind::DeletedByThem:
      return MergeFileConflictKind::DeletedByThem;
    case project::GitConflictKind::AddedByUs:
      return MergeFileConflictKind::AddedByUs;
    case project::GitConflictKind::AddedByThem:
      return MergeFileConflictKind::AddedByThem;
    case project::GitConflictKind::None:
    case project::GitConflictKind::Unknown:
      break;
  }
  return MergeFileConflictKind::Unknown;
}

bool RequiresExistenceChoice(MergeFileConflictKind kind) {
  switch (kind) {
    case MergeFileConflictKind::DeletedByUs:
    case MergeFileConflictKind::DeletedByThem:
    case MergeFileConflictKind::BothDeleted:
    case MergeFileConflictKind::RenameDelete:
      return true;
    default:
      return false;
  }
}

bool TextHunksAvailable(MergeFileConflictKind kind) {
  switch (kind) {
    case MergeFileConflictKind::Binary:
    case MergeFileConflictKind::Submodule:
    case MergeFileConflictKind::FileDirectory:
    case MergeFileConflictKind::BothDeleted:
      return false;
    default:
      return true;
  }
}

std::string BuildSummary(MergeFileConflictKind kind) {
  switch (kind) {
    case MergeFileConflictKind::BothModified:
      return "Both sides modified this file.";
    case MergeFileConflictKind::BothAdded:
      return "Both sides added this file.";
    case MergeFileConflictKind::BothDeleted:
      return "Both sides deleted this file.";
    case MergeFileConflictKind::DeletedByUs:
      return "Local side deleted the file; remote side modified it.";
    case MergeFileConflictKind::DeletedByThem:
      return "Remote side deleted the file; local side modified it.";
    case MergeFileConflictKind::AddedByUs:
      return "Local side added this file.";
    case MergeFileConflictKind::AddedByThem:
      return "Remote side added this file.";
    case MergeFileConflictKind::RenameRename:
      return "Both sides renamed this path differently.";
    case MergeFileConflictKind::RenameDelete:
      return "One side renamed the file and the other deleted or moved it.";
    case MergeFileConflictKind::FileDirectory:
      return "One side changed the path to a file and the other to a directory.";
    case MergeFileConflictKind::Binary:
      return "Binary conflict: keep a whole side with Accept Local or Accept Remote.";
    case MergeFileConflictKind::Submodule:
      return "Submodule conflict; resolve in the submodule checkout.";
    case MergeFileConflictKind::LineEndingHeavy:
      return "Sides differ only by line endings.";
    case MergeFileConflictKind::Unknown:
      return "Unclassified merge conflict.";
  }
  return "Unclassified merge conflict.";
}

}

bool MergeContentLooksBinary(std::string_view content) {
  return ContentHasNulByte(content);
}

bool MergeContentIsLineEndingHeavy(std::string_view base,
                                    std::string_view incoming,
                                    std::string_view current) {
  const std::string normalized_base = NormalizeLineEndings(base);
  const std::string normalized_incoming = NormalizeLineEndings(incoming);
  const std::string normalized_current = NormalizeLineEndings(current);
  if (normalized_base == normalized_incoming && normalized_base == normalized_current) {
    return incoming != normalized_incoming || current != normalized_current ||
           base != normalized_base;
  }
  const bool incoming_only_endings =
      normalized_incoming == normalized_base && incoming != normalized_incoming;
  const bool current_only_endings =
      normalized_current == normalized_base && current != normalized_current;
  if (incoming_only_endings && current_only_endings) {
    return true;
  }
  return normalized_incoming == normalized_current &&
         (incoming != normalized_incoming || current != normalized_current);
}

MergeFileConflictMetadata ClassifyMergeFileConflict(
    const MergeConflictClassificationInput& input) {
  MergeFileConflictKind kind = MergeFileConflictKind::Unknown;
  if (input.repository_entry != nullptr) {
    kind = KindFromGitConflict(input.repository_entry->conflict_kind, input.repository_entry);
  }

  if (kind == MergeFileConflictKind::Unknown) {
    if (!input.base_exists && input.incoming_exists && input.current_exists) {
      kind = MergeFileConflictKind::BothAdded;
    } else if (input.base_exists && !input.incoming_exists && input.current_exists) {
      kind = MergeFileConflictKind::DeletedByThem;
    } else if (input.base_exists && input.incoming_exists && !input.current_exists) {
      kind = MergeFileConflictKind::DeletedByUs;
    } else if (input.base_exists && !input.incoming_exists && !input.current_exists) {
      kind = MergeFileConflictKind::BothDeleted;
    } else if (!input.base_exists && !input.incoming_exists && input.current_exists) {
      kind = MergeFileConflictKind::AddedByUs;
    } else if (!input.base_exists && input.incoming_exists && !input.current_exists) {
      kind = MergeFileConflictKind::AddedByThem;
    } else if (input.incoming_exists && input.current_exists) {
      kind = MergeFileConflictKind::BothModified;
    }
  }

  if (!RequiresExistenceChoice(kind) &&
      (MergeContentLooksBinary(input.base_content) ||
       MergeContentLooksBinary(input.incoming_content) ||
       MergeContentLooksBinary(input.current_content))) {
    kind = MergeFileConflictKind::Binary;
  } else if (kind == MergeFileConflictKind::BothModified &&
             MergeContentIsLineEndingHeavy(input.base_content, input.incoming_content,
                                           input.current_content)) {
    kind = MergeFileConflictKind::LineEndingHeavy;
  }

  return MergeFileConflictMetadata{
      .kind = kind,
      .text_hunks_available = TextHunksAvailable(kind),
      .requires_existence_choice = RequiresExistenceChoice(kind),
      .summary = BuildSummary(kind),
  };
}

}
