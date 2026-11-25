#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace microide::util {

inline bool IsConflictStartLine(std::string_view line) { return line.starts_with("<<<<<<<"); }
inline bool IsConflictBaseLine(std::string_view line) { return line.starts_with("|||||||"); }
inline bool IsConflictEndLine(std::string_view line) { return line.starts_with(">>>>>>>"); }
inline bool IsConflictSeparatorLine(std::string_view line) {
  if (!line.empty() && line.back() == '\r') {
    line.remove_suffix(1);
  }
  return line.size() >= 7 && line.find_first_not_of('=') == std::string_view::npos;
}

bool ContainsCompleteConflictMarkers(std::string_view text);
bool StagedDiffIntroducesConflictMarker(std::string_view diff);

std::optional<std::size_t> FirstConflictMarkerLine(std::span<const std::string> lines);

struct ConflictMarkerScan {
  bool complete = false;
  std::optional<std::size_t> first_marker_line;
};

template <typename LinesLike>
ConflictMarkerScan ScanConflictMarkers(const LinesLike& lines) {
  bool has_ours = false;
  bool has_base = false;
  bool has_theirs = false;
  std::optional<std::size_t> first_line;
  const std::size_t count = lines.size();
  for (std::size_t i = 0; i < count; ++i) {
    const std::string_view line = lines[i];
    if (IsConflictStartLine(line)) {
      has_ours = true;
      if (!first_line.has_value()) {
        first_line = i;
      }
    } else if (IsConflictSeparatorLine(line)) {
      has_base = true;
    } else if (IsConflictEndLine(line)) {
      has_theirs = true;
    }
  }
  return ConflictMarkerScan{has_ours && has_base && has_theirs, first_line};
}

}
