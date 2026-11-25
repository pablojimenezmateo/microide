#include "workspace/git/CompareTabLoad.h"

#include <algorithm>
#include <utility>

#include "project/GitCompareService.h"
#include "util/TextFileIO.h"
#include "workspace/render/CompareVisibleLayoutCache.h"

namespace microide::workspace {

std::size_t CountCompareTextLines(const std::string_view text) {
  return text.empty() ? 0
                      : static_cast<std::size_t>(std::count(text.begin(), text.end(), '\n')) + 1;
}

void RunCompareTabLoad(const CompareTabLoadRequest& request, CompareTabLoadResult& result) {
  result.built_options = request.build_options;
  if (request.collect_review_files == compare::CompareReviewMode::Commit) {
    result.review_files = project::CollectGitCommitChangedFiles(request.root, *request.launcher, request.left_ref);
  } else if (request.collect_review_files == compare::CompareReviewMode::Branch) {
    std::vector<std::filesystem::path>& paths = result.review_files.emplace();
    for (const project::GitBranchFileEntry& entry :
         project::CollectGitBranchOutgoingFiles(request.root, *request.launcher, request.left_ref)) {
      paths.push_back(entry.relative_path);
    }
  }

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
  std::string right_bytes;
  if (request.right_is_working_tree()) {
    util::TextFileReadResult working = util::ReadTextFileClassified(request.path);
    if (working.is_error()) {
      return;
    }
    right_exists = working.status != util::TextFileReadStatus::Missing;
    right_bytes = std::move(working.content);
  } else {
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

  compare::BuildCompareModelInto(
      result.model, result.left,
      compare::MakeCompareText(result.right.SerializeDocumentText(result.right.line_ending())),
      result.built_options);
}

CompareRevisionSides ReadCompareRevisionSides(const CompareTabRefreshRequest& request) {
  CompareRevisionSides sides;
  if (!request.left_held) {
    sides.left = project::ReadGitFileAtCommit(request.root, *request.launcher, request.left_path, request.left_ref);
  }
  if (!request.right_ref.empty() && request.right_ref != "WORKTREE") {
    sides.right =
        project::ReadGitFileAtCommit(request.root, *request.launcher, request.right_path, request.right_ref);
  }
  return sides;
}

void MarkCompareModelBuilt(CompareTabState& compare_tab, const compare::CompareBuildOptions& built,
                           const std::size_t left_line_count) {
  ++compare_tab.model_revision;
  compare_tab.visible_layouts.model_revision = compare_tab.model_revision;
  ResetCompareVisibleLayoutCache(compare_tab);
  compare_tab.derived_right_content_revision = compare_tab.right_viewport.content_revision();
  compare_tab.derived_right_line_ending = compare_tab.right_viewport.line_ending();
  compare_tab.derived_left_content = compare_tab.left_content;
  compare_tab.derived_left_line_count = left_line_count;
  compare_tab.derived_ignore_whitespace = built.ignore_whitespace;
  compare_tab.derived_syntax = built.syntax;
  compare_tab.derived_fingerprint_valid = true;
  const auto index_side = [&](std::vector<std::uint32_t>& line_rows, int compare::CompareRow::* line) {
    const std::vector<compare::CompareRow>& rows = compare_tab.model.rows;
    std::size_t line_count = 0;
    for (std::size_t row = rows.size(); row-- > 0;) {
      if (rows[row].*line > 0) {
        line_count = static_cast<std::size_t>(rows[row].*line);
        break;
      }
    }
    line_rows.assign(line_count, 0);
    for (std::size_t row = 0; row < rows.size(); ++row) {
      const int number = rows[row].*line;
      if (number > 0 && static_cast<std::size_t>(number) <= line_count) {
        line_rows[static_cast<std::size_t>(number - 1)] = static_cast<std::uint32_t>(row);
      }
    }
  };
  index_side(compare_tab.left_line_rows, &compare::CompareRow::left_line);
  index_side(compare_tab.right_line_rows, &compare::CompareRow::right_line);
  compare_tab.token_windows_stale = true;
}

}
