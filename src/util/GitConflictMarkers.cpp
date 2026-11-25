#include "util/GitConflictMarkers.h"

namespace microide::util {

bool ContainsCompleteConflictMarkers(std::string_view text) {
  bool start = false;
  bool separator = false;
  bool end = false;
  std::size_t pos = 0;
  while (pos <= text.size() && !(start && separator && end)) {
    const std::size_t eol = text.find('\n', pos);
    const std::size_t line_end = eol == std::string_view::npos ? text.size() : eol;
    const std::string_view line = text.substr(pos, line_end - pos);
    start = start || IsConflictStartLine(line);
    separator = separator || IsConflictSeparatorLine(line);
    end = end || IsConflictEndLine(line);
    if (eol == std::string_view::npos) {
      break;
    }
    pos = eol + 1;
  }
  return start && separator && end;
}

bool StagedDiffIntroducesConflictMarker(std::string_view diff) {
  std::size_t pos = 0;
  while (pos < diff.size()) {
    const std::size_t eol = diff.find('\n', pos);
    const std::size_t line_end = eol == std::string_view::npos ? diff.size() : eol;
    const std::string_view line = diff.substr(pos, line_end - pos);
    if (!line.empty() && line.front() == '+') {
      const std::string_view content = line.substr(1);
      if (IsConflictStartLine(content) || IsConflictEndLine(content)) {
        return true;
      }
    }
    if (eol == std::string_view::npos) {
      break;
    }
    pos = eol + 1;
  }
  return false;
}

std::optional<std::size_t> FirstConflictMarkerLine(std::span<const std::string> lines) {
  for (std::size_t i = 0; i < lines.size(); ++i) {
    if (IsConflictStartLine(lines[i])) {
      return i;
    }
  }
  return std::nullopt;
}

}
