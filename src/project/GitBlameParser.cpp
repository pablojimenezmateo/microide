#include "project/GitBlameService.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "util/Parse.h"
#include "util/StringUtil.h"

namespace microide::project {
namespace {

bool IsHexCommitPrefix(std::string_view line) {
  // A blame-incremental header starts with the full object id followed by a
  // space (or end-of-line): 40 hex chars in a SHA-1 repo, 64 in a SHA-256 repo
  // (extensions.objectFormat=sha256). Match the whole leading hex run and accept
  // either width rather than hardcoding 40, which silently rejected every header
  // in a SHA-256 repo and produced no blame attributions at all.
  std::size_t hex_len = 0;
  while (hex_len < line.size()) {
    const char ch = line[hex_len];
    const bool is_digit = ch >= '0' && ch <= '9';
    const bool is_hex_lower = ch >= 'a' && ch <= 'f';
    if (!is_digit && !is_hex_lower) {
      break;
    }
    ++hex_len;
  }
  if (hex_len != 40 && hex_len != 64) {
    return false;
  }
  return hex_len == line.size() || line[hex_len] == ' ';
}

struct CommitMetadata {
  std::string author;
  std::string summary;
  std::int64_t author_time = 0;
  bool boundary = false;
};

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
      const std::vector<std::string_view> fields = util::SplitAsciiWhitespace(line);
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
      // Defense-in-depth: the caller always bounds the blame `-L` window (≤512
      // lines), so git self-limits these fields. But they are attacker-tunable
      // (a compromised/buggy git, or a future unbounded caller), and downstream
      // GitBlameService inserts one map entry per line in [result_line,
      // result_line + line_count). Clamp both the start line and the count so an
      // absurd value cannot drive unbounded map growth (OOM) or wrap `result_line
      // + offset` past SIZE_MAX. 1M is orders of magnitude past any real hunk.
      constexpr std::size_t kMaxAttributionLineCount = 1'000'000;
      current_result_line = std::min<std::size_t>(*parsed_result - 1, kMaxAttributionLineCount);
      current_line_count = std::min<std::size_t>(*parsed_count, kMaxAttributionLineCount);
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
