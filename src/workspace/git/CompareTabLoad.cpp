#include "workspace/git/CompareTabLoad.h"

#include <algorithm>
#include <utility>

#include "project/GitCompareService.h"
#include "workspace/render/CompareVisibleLayoutCache.h"

namespace microide::workspace {

std::size_t CountCompareTextLines(const std::string_view text) {
  return text.empty() ? 0
                      : static_cast<std::size_t>(std::count(text.begin(), text.end(), '\n')) + 1;
}

void RunCompareTabLoad(const CompareTabLoadRequest& request, std::string right_bytes,
                       CompareTabLoadResult& result) {
  result.built_options = request.build_options;

  std::optional<project::GitFileContentAtCommit> left = project::ReadGitFileAtCommit(
      request.root, *request.launcher, request.path, request.left_ref);
  if (!left.has_value() || left->truncated) {
    return;
  }
  result.left_ok = true;
  result.built_options.left_exists = left->exists;
  result.left = compare::MakeCompareText(left->exists ? std::move(left->content) : std::string());
  result.left_line_count = CountCompareTextLines(*result.left);

  bool right_exists = true;
  if (!request.right_is_working_tree()) {
    std::optional<project::GitFileContentAtCommit> right = project::ReadGitFileAtCommit(
        request.root, *request.launcher, request.path, request.right_ref);
    if (!right.has_value() || right->truncated) {
      return;
    }
    right_exists = right->exists;
    right_bytes = right->exists ? std::move(right->content) : std::string();
  }
  result.right_read = true;
  if (!result.right.AdoptFileContent(request.path, std::move(right_bytes))) {
    return;
  }
  result.right_ok = true;
  result.built_options.right_exists = right_exists;

  // The same serialize the derived-state refresh does before a rebuild, so the
  // model is byte-for-byte the one a synchronous open would have built.
  compare::BuildCompareModelInto(
      result.model, result.left,
      compare::MakeCompareText(result.right.SerializeDocumentText(result.right.line_ending())),
      result.built_options);
}

void MarkCompareModelBuilt(CompareTabState& compare_tab, const bool built_ignore_whitespace,
                           const std::size_t left_line_count) {
  ++compare_tab.model_revision;
  compare_tab.visible_layouts.model_revision = compare_tab.model_revision;
  ResetCompareVisibleLayoutCache(compare_tab);
  compare_tab.derived_right_content_revision = compare_tab.right_viewport.content_revision();
  compare_tab.derived_right_line_ending = compare_tab.right_viewport.line_ending();
  compare_tab.derived_left_content = compare_tab.left_content;
  compare_tab.derived_left_line_count = left_line_count;
  compare_tab.derived_ignore_whitespace = built_ignore_whitespace;
  compare_tab.derived_fingerprint_valid = true;
}

}  // namespace microide::workspace
