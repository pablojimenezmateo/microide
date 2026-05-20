#include "project/GitBlameService.h"

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "util/Parse.h"

namespace microide::project {
namespace {

bool IsHexCommitPrefix(std::string_view line) {
  if (line.size() < 40) {
    return false;
  }
  for (std::size_t i = 0; i < 40; ++i) {
    const char ch = line[i];
    const bool is_digit = ch >= '0' && ch <= '9';
    const bool is_hex_lower = ch >= 'a' && ch <= 'f';
    if (!is_digit && !is_hex_lower) {
      return false;
    }
  }
  return line.size() == 40 || line[40] == ' ';
}

struct CommitMetadata {
  std::string author;
  std::string summary;
  std::int64_t author_time = 0;
  bool boundary = false;
};

std::vector<std::string_view> SplitFields(std::string_view line) {
  std::vector<std::string_view> fields;
  std::size_t offset = 0;
  while (offset < line.size()) {
    while (offset < line.size() && line[offset] == ' ') {
      ++offset;
    }
    if (offset >= line.size()) {
      break;
    }
    const std::size_t end = line.find(' ', offset);
    if (end == std::string_view::npos) {
      fields.push_back(line.substr(offset));
      break;
    }
    fields.push_back(line.substr(offset, end - offset));
    offset = end + 1;
  }
  return fields;
}

}  // namespace

std::vector<GitBlameAttribution> ParseGitBlameIncrementalOutput(std::string_view output) {
  std::unordered_map<std::string, CommitMetadata> commit_metadata;
  std::vector<GitBlameAttribution> attributions;

  std::string current_commit;
  CommitMetadata current_metadata;
  std::size_t current_result_line = 0;
  std::size_t current_line_count = 0;
  bool in_entry = false;

  std::size_t offset = 0;
  while (offset <= output.size()) {
    const std::size_t end = output.find('\n', offset);
    const std::size_t line_end = end == std::string_view::npos ? output.size() : end;
    std::string_view line = output.substr(offset, line_end - offset);
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1);
    }
    offset = end == std::string_view::npos ? output.size() + 1 : end + 1;

    if (line.empty()) {
      continue;
    }

    if (IsHexCommitPrefix(line)) {
      const std::vector<std::string_view> fields = SplitFields(line);
      if (fields.size() < 4) {
        continue;
      }

      current_commit = std::string(fields[0]);
      const auto parsed_result = util::ParseSize(fields[2]);
      const auto parsed_count = util::ParseSize(fields[3]);
      if (!parsed_result.has_value() || !parsed_count.has_value() || *parsed_result == 0 ||
          *parsed_count == 0) {
        in_entry = false;
        continue;
      }
      current_result_line = *parsed_result - 1;
      current_line_count = *parsed_count;
      current_metadata = commit_metadata[current_commit];
      in_entry = true;
      continue;
    }

    if (!in_entry) {
      continue;
    }

    if (line.starts_with("author ")) {
      current_metadata.author = std::string(line.substr(7));
      continue;
    }
    if (line.starts_with("author-time ")) {
      if (const auto parsed = util::ParseInt64(line.substr(12)); parsed.has_value()) {
        current_metadata.author_time = *parsed;
      }
      continue;
    }
    if (line.starts_with("summary ")) {
      current_metadata.summary = std::string(line.substr(8));
      continue;
    }
    if (line == "boundary") {
      current_metadata.boundary = true;
      continue;
    }
    if (line.starts_with("filename ")) {
      commit_metadata[current_commit] = current_metadata;
      attributions.push_back(GitBlameAttribution{
          .commit_id = current_commit,
          .author = current_metadata.author,
          .summary = current_metadata.summary,
          .author_time = current_metadata.author_time,
          .result_line = current_result_line,
          .line_count = current_line_count,
          .boundary = current_metadata.boundary,
      });
      in_entry = false;
      continue;
    }
  }

  return attributions;
}

}  // namespace microide::project
