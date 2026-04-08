#include "project/GitStatusService.h"

#include <array>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>

#include "util/StartupTrace.h"

namespace microide::project {

namespace {

std::string EscapeShellArg(std::string_view text) {
  std::string escaped;
  escaped.reserve(text.size() + 8);
  for (char c : text) {
    if (c == '\'') {
      escaped += "'\\''";
    } else {
      escaped.push_back(c);
    }
  }
  return escaped;
}

bool HasGitMarker(const std::filesystem::path& root) {
  return std::filesystem::exists(root / ".git");
}

bool StatusUsesTargetPath(std::string_view code) {
  return code.find('R') != std::string_view::npos || code.find('C') != std::string_view::npos;
}

int GitStatusPriority(GitFileStatus status) {
  switch (status) {
    case GitFileStatus::Deleted:
      return 4;
    case GitFileStatus::Modified:
      return 3;
    case GitFileStatus::Added:
      return 2;
    case GitFileStatus::Untracked:
      return 1;
    case GitFileStatus::Clean:
    default:
      return 0;
  }
}

GitFileStatus CombineGitStatus(GitFileStatus current, GitFileStatus next) {
  return GitStatusPriority(next) > GitStatusPriority(current) ? next : current;
}

GitFileStatus StatusFromPorcelainCode(std::string_view code) {
  if (code == "??") {
    return GitFileStatus::Untracked;
  }
  if (code.find('D') != std::string_view::npos) {
    return GitFileStatus::Deleted;
  }
  if (code.find('A') != std::string_view::npos || code.find('C') != std::string_view::npos) {
    return GitFileStatus::Added;
  }
  if (code.find_first_of("MRTU") != std::string_view::npos) {
    return GitFileStatus::Modified;
  }
  return GitFileStatus::Clean;
}

void RecordGitStatus(std::unordered_map<std::string, GitFileStatus>& statuses,
                     std::filesystem::path relative_path,
                     GitFileStatus status) {
  relative_path = relative_path.lexically_normal();
  const std::string normalized = relative_path.string();
  if (!normalized.empty() && normalized != ".") {
    statuses[normalized] = CombineGitStatus(statuses[normalized], status);
  }

  std::filesystem::path dir = relative_path.parent_path();
  while (!dir.empty() && dir != ".") {
    const std::string key = dir.lexically_normal().string();
    statuses[key] = CombineGitStatus(statuses[key], status);
    const auto next = dir.parent_path();
    if (next == dir) {
      break;
    }
    dir = next;
  }
}

std::unordered_map<std::string, GitFileStatus> ParseGitPorcelainStatus(std::string_view output) {
  std::unordered_map<std::string, GitFileStatus> statuses;

  std::size_t offset = 0;
  while (offset < output.size()) {
    const std::size_t end = output.find('\0', offset);
    const std::size_t current_end = end == std::string_view::npos ? output.size() : end;
    const std::string_view entry = output.substr(offset, current_end - offset);
    offset = current_end == output.size() ? output.size() : current_end + 1;

    if (entry.size() < 4) {
      continue;
    }

    const std::string_view code = entry.substr(0, 2);
    std::string path(entry.substr(3));
    if (path.empty()) {
      continue;
    }

    if (StatusUsesTargetPath(code) && offset < output.size()) {
      const std::size_t target_end = output.find('\0', offset);
      const std::size_t resolved_end =
          target_end == std::string_view::npos ? output.size() : target_end;
      const std::string_view target = output.substr(offset, resolved_end - offset);
      if (!target.empty()) {
        path = std::string(target);
      }
      offset = resolved_end == output.size() ? output.size() : resolved_end + 1;
    }

    RecordGitStatus(statuses, std::filesystem::path(path), StatusFromPorcelainCode(code));
  }

  return statuses;
}

std::string ReadCommandOutput(const std::string& command) {
  std::string output;
  FILE* pipe = popen(command.c_str(), "r");
  if (pipe == nullptr) {
    return output;
  }

  std::array<char, 4096> buffer{};
  while (true) {
    const std::size_t bytes_read = fread(buffer.data(), 1, buffer.size(), pipe);
    if (bytes_read > 0) {
      output.append(buffer.data(), bytes_read);
    }
    if (bytes_read < buffer.size()) {
      break;
    }
  }

  const int status = pclose(pipe);
  if (status != 0) {
    return {};
  }

  return output;
}

}  // namespace

std::unordered_map<std::string, GitFileStatus> CollectGitStatuses(
    const std::filesystem::path& root) {
  util::StartupTrace::Scope trace_scope("CollectGitStatuses");
  if (root.empty() || !HasGitMarker(root)) {
    return {};
  }

  const std::string command =
      "git -C '" + EscapeShellArg(root.lexically_normal().string()) +
      "' status --porcelain=v1 -z --untracked-files=all 2>/dev/null";
  const std::string output = ReadCommandOutput(command);
  if (output.empty()) {
    return {};
  }

  return ParseGitPorcelainStatus(output);
}

}  // namespace microide::project
