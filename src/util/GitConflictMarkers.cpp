#include "util/GitConflictMarkers.h"

namespace microide::util {

bool ContainsCompleteConflictMarkers(std::string_view text) {
  return text.find("<<<<<<<") != std::string_view::npos &&
         text.find("=======") != std::string_view::npos &&
         text.find(">>>>>>>") != std::string_view::npos;
}

bool StagedDiffIntroducesConflictMarker(std::string_view diff) {
  // Scan unified-diff output (e.g. `git diff --cached`) for a conflict marker
  // introduced by the staged changes. Only *added* lines (a leading '+') count:
  // a marker on a context or removed line is pre-existing or being resolved, not
  // introduced now. The marker must be line-anchored to the unambiguous
  // `<<<<<<<` / `>>>>>>>` sigils; a bare `=======` run is a common section
  // divider (banner comments in many languages) and matching it as a substring
  // caused spurious "still contains conflict markers" commit blocks. A real
  // leaked conflict always carries the angle sigils, so dropping `=======` from
  // the predicate loses no genuine detection.
  std::size_t pos = 0;
  while (pos < diff.size()) {
    const std::size_t eol = diff.find('\n', pos);
    const std::size_t line_end = eol == std::string_view::npos ? diff.size() : eol;
    const std::string_view line = diff.substr(pos, line_end - pos);
    // Added content lines start with a single '+'; the `+++ b/path` file header
    // starts with `+++` and never matches the 7-char sigils below.
    if (!line.empty() && line.front() == '+') {
      const std::string_view content = line.substr(1);
      if (content.starts_with("<<<<<<<") || content.starts_with(">>>>>>>")) {
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
    if (lines[i].starts_with("<<<<<<<")) {
      return i;
    }
  }
  return std::nullopt;
}

}  // namespace microide::util
