#include "compare/MergeConflictKind.h"

#include <algorithm>

#include "util/StringUtil.h"

namespace microide::compare {
namespace {

bool ContentHasNulByte(std::string_view content) {
  return content.find('\0') != std::string_view::npos;
}

// Strips all carriage returns (CRLF/CR -> LF); shared with the rest of the text
// layer via util::NormalizeLineEndings.
using util::NormalizeLineEndings;

MergeFileConflictKind KindFromGitConflict(project::GitConflictKind git_kind,
                                          const project::GitRepositoryEntry* entry) {
  // A conflicted gitlink outranks every content kind: there is no text to
  // three-way merge, so reporting it as both-modified would offer hunks for a
  // file that is really a commit pointer.
  if (entry != nullptr && entry->submodule && entry->conflicted) {
    return MergeFileConflictKind::Submodule;
  }
  // A D/F conflict next: the path is a directory on disk, so there is no file to
  // three-way merge and the existence choice is between a file and a tree. This
  // is checked AFTER the submodule case on purpose — a submodule checkout is also
  // a directory, and reporting it as file/directory would lose the more specific
  // (and more actionable) submodule message.
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
    // A D/F conflict has a directory on one side: there is no text to three-way
    // merge, so offering hunks would present an empty diff with live actions.
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
      return "Current side deleted the file; incoming side modified it.";
    case MergeFileConflictKind::DeletedByThem:
      return "Incoming side deleted the file; current side modified it.";
    case MergeFileConflictKind::AddedByUs:
      return "Current side added this file.";
    case MergeFileConflictKind::AddedByThem:
      return "Incoming side added this file.";
    case MergeFileConflictKind::RenameRename:
      return "Both sides renamed this path differently.";
    case MergeFileConflictKind::RenameDelete:
      return "One side renamed the file and the other deleted or moved it.";
    case MergeFileConflictKind::FileDirectory:
      return "One side changed the path to a file and the other to a directory.";
    case MergeFileConflictKind::Binary:
      return "Git reported a binary conflict; text hunk actions are disabled.";
    case MergeFileConflictKind::Submodule:
      return "Submodule conflict; resolve in the submodule checkout.";
    case MergeFileConflictKind::Mode:
      return "File mode conflict; review executable and permission bits.";
    case MergeFileConflictKind::LineEndingHeavy:
      return "Sides differ mainly by line endings or whitespace.";
    case MergeFileConflictKind::Unknown:
      return "Unclassified merge conflict.";
  }
  return "Unclassified merge conflict.";
}

}  // namespace

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
  if (incoming_only_endings || current_only_endings) {
    return true;
  }
  return normalized_incoming == normalized_current && incoming != normalized_incoming &&
         current != normalized_current;
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
      // No base: a side that has the file added it — this is not a delete.
      kind = MergeFileConflictKind::AddedByUs;
    } else if (!input.base_exists && input.incoming_exists && !input.current_exists) {
      kind = MergeFileConflictKind::AddedByThem;
    } else if (input.incoming_exists && input.current_exists) {
      kind = MergeFileConflictKind::BothModified;
    }
  }

  // Only downgrade content-diff conflicts to Binary. Existence-choice kinds
  // (deleted-by-us/them, both-deleted, rename-delete) must keep their kind so the
  // resolver still offers the keep-vs-delete choice the user needs; overwriting
  // them with Binary would suppress that choice and disable resolution.
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

}  // namespace microide::compare
