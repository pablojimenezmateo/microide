#include "compare/CompareSemanticMetadata.h"

#include <algorithm>

#include "util/StringUtil.h"

namespace microide::compare {

namespace {

bool LooksBinary(std::string_view text) {
  if (text.empty()) {
    return false;
  }
  std::size_t non_text = 0;
  for (unsigned char byte : text) {
    if (byte == 0) {
      return true;
    }
    if (byte < 0x09 || (byte > 0x0d && byte < 0x20)) {
      ++non_text;
    }
  }
  return non_text * 20 > text.size();
}

bool SubmodulePointerContent(std::string_view text, std::string* oid) {
  if (text.size() != 41 || text[0] != '-' || text.back() != '\n') {
    return false;
  }
  const std::string_view body = text.substr(1, 40);
  if (!std::all_of(body.begin(), body.end(), [](char ch) {
        return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
      })) {
    return false;
  }
  if (oid != nullptr) {
    *oid = std::string(body);
  }
  return true;
}

bool LineEndingOnlyChange(std::string_view left, std::string_view right) {
  if (left == right) {
    return false;
  }
  const std::string normalized_left = util::NormalizeLineEndings(left);
  const std::string normalized_right = util::NormalizeLineEndings(right);
  return normalized_left == normalized_right;
}

}  // namespace

CompareSemanticFileMetadata InferCompareSemanticFileMetadata(
    const CompareSemanticMetadataInput& input) {
  CompareSemanticFileMetadata metadata;
  metadata.new_path = input.path;
  metadata.old_path = input.old_path.value_or(input.path);
  metadata.old_executable = input.old_executable;
  metadata.new_executable = input.new_executable;

  if (input.git_entry.has_value()) {
    const auto& entry = *input.git_entry;
    if (entry.kind == project::GitRepositoryEntryKind::Renamed && entry.old_path.has_value()) {
      metadata.renamed = true;
      metadata.old_path = entry.old_path->relative_path;
      metadata.new_path = entry.path.relative_path;
    }
  } else if (input.old_path.has_value() && input.old_path != input.path) {
    metadata.renamed = true;
    metadata.old_path = *input.old_path;
  }

  metadata.mode_changed = metadata.old_executable != metadata.new_executable;

  std::string left_oid;
  std::string right_oid;
  if (SubmodulePointerContent(input.left_content, &left_oid) &&
      SubmodulePointerContent(input.right_content, &right_oid)) {
    metadata.file_kind = CompareSemanticFileKind::Submodule;
    metadata.submodule_pointer_changed = left_oid != right_oid;
    metadata.old_submodule_oid = std::move(left_oid);
    metadata.new_submodule_oid = std::move(right_oid);
    return metadata;
  }

  if (LooksBinary(input.left_content) || LooksBinary(input.right_content)) {
    metadata.file_kind = CompareSemanticFileKind::Binary;
    return metadata;
  }

  metadata.line_ending_only = LineEndingOnlyChange(input.left_content, input.right_content);
  return metadata;
}

}  // namespace microide::compare
