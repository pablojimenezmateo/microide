#include "compare/CompareModel.h"
#include "editor/SyntaxHighlighter.h"
#include "project/GitCompareService.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::size_t kLargeCompareByteThreshold = 512 * 1024;
constexpr std::size_t kLargeCompareRowThreshold = 6000;

std::optional<std::string> ReadFileText(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return std::nullopt;
  }
  std::ostringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

std::vector<std::string> SplitSyntaxLines(std::string_view text) {
  std::vector<std::string> lines;
  std::size_t start = 0;
  while (start < text.size()) {
    const std::size_t newline = text.find('\n', start);
    if (newline == std::string_view::npos) {
      lines.emplace_back(text.substr(start));
      break;
    }
    lines.emplace_back(text.substr(start, newline - start));
    start = newline + 1;
  }
  return lines;
}

bool ShouldSyntaxHighlight(std::string_view left_content,
                           std::string_view right_content,
                           const microide::compare::CompareModel& model) {
  return left_content.size() + right_content.size() <= kLargeCompareByteThreshold &&
         model.rows.size() <= kLargeCompareRowThreshold;
}

std::size_t HighlightCompareRows(const std::filesystem::path& path,
                                 std::string_view left_content,
                                 std::string_view right_content,
                                 const microide::compare::CompareModel& model) {
  const auto left_lines = SplitSyntaxLines(left_content);
  const auto right_lines = SplitSyntaxLines(right_content);
  auto left_state = microide::editor::SyntaxHighlighter::InitialState(path, left_lines);
  auto right_state = microide::editor::SyntaxHighlighter::InitialState(path, right_lines);

  std::size_t highlighted_rows = 0;
  for (const auto& row : model.rows) {
    const bool reuse_tokens =
        row.kind == microide::compare::CompareRowKind::Unchanged && row.left_line > 0 &&
        row.right_line > 0 && row.left_text == row.right_text &&
        left_state.definition_id == right_state.definition_id &&
        left_state.region_id == right_state.region_id;
    if (reuse_tokens) {
      auto highlighted =
          microide::editor::SyntaxHighlighter::HighlightLine(row.left_text, path, left_state);
      left_state = highlighted.end_state;
      right_state = highlighted.end_state;
      ++highlighted_rows;
      continue;
    }
    if (row.left_line > 0) {
      auto highlighted =
          microide::editor::SyntaxHighlighter::HighlightLine(row.left_text, path, left_state);
      left_state = highlighted.end_state;
      ++highlighted_rows;
    }
    if (row.right_line > 0) {
      auto highlighted =
          microide::editor::SyntaxHighlighter::HighlightLine(row.right_text, path, right_state);
      right_state = highlighted.end_state;
      ++highlighted_rows;
    }
  }
  return highlighted_rows;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3 || argc > 5) {
    std::cerr << "usage: microide_diff_bench <repo-root> <file> [left-ref] [right-ref|WORKTREE]\n";
    return 1;
  }

  const std::filesystem::path repo_root = std::filesystem::path(argv[1]).lexically_normal();
  std::filesystem::path file_path = std::filesystem::path(argv[2]);
  const std::string left_ref = argc >= 4 ? argv[3] : "HEAD";
  const std::string right_ref = argc >= 5 ? argv[4] : "WORKTREE";
  if (file_path.is_relative()) {
    file_path = (repo_root / file_path).lexically_normal();
  } else {
    file_path = file_path.lexically_normal();
  }

  const auto read_start = std::chrono::steady_clock::now();
  const auto left = microide::project::ReadGitFileAtCommit(repo_root, file_path, left_ref);
  if (!left.has_value()) {
    std::cerr << "failed to read left side from git ref " << left_ref << '\n';
    return 1;
  }
  std::optional<std::string> right_content;
  if (right_ref == "WORKTREE") {
    right_content = ReadFileText(file_path).value_or("");
  } else {
    const auto right = microide::project::ReadGitFileAtCommit(repo_root, file_path, right_ref);
    if (!right.has_value()) {
      std::cerr << "failed to read right side from git ref " << right_ref << '\n';
      return 1;
    }
    right_content = right->exists ? right->content : "";
  }
  const std::string left_content = left->exists ? left->content : "";
  const auto read_end = std::chrono::steady_clock::now();

  const auto diff_start = std::chrono::steady_clock::now();
  const auto model = microide::compare::BuildCompareModel(left_content, *right_content);
  const auto diff_end = std::chrono::steady_clock::now();

  bool syntax_highlighted = false;
  std::size_t highlighted_rows = 0;
  const auto highlight_start = std::chrono::steady_clock::now();
  if (ShouldSyntaxHighlight(left_content, *right_content, model)) {
    syntax_highlighted = true;
    highlighted_rows = HighlightCompareRows(file_path, left_content, *right_content, model);
  }
  const auto highlight_end = std::chrono::steady_clock::now();

  const auto read_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(read_end - read_start).count();
  const auto diff_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(diff_end - diff_start).count();
  const auto highlight_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(highlight_end - highlight_start).count();

  std::cout << "file: " << file_path << '\n';
  std::cout << "left-ref: " << left_ref << '\n';
  std::cout << "right-ref: " << right_ref << '\n';
  std::cout << "left-bytes: " << left_content.size() << '\n';
  std::cout << "right-bytes: " << right_content->size() << '\n';
  std::cout << "rows: " << model.rows.size() << '\n';
  std::cout << "hunks: " << model.hunks.size() << '\n';
  std::cout << "read-ms: " << read_ms << '\n';
  std::cout << "diff-ms: " << diff_ms << '\n';
  std::cout << "highlight-ms: " << highlight_ms << '\n';
  std::cout << "syntax-highlighted: " << (syntax_highlighted ? "yes" : "no") << '\n';
  std::cout << "highlighted-rows: " << highlighted_rows << '\n';
  return 0;
}
